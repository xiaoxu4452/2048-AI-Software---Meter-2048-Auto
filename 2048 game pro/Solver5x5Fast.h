#ifndef SOLVER_5X5_FAST_H
#define SOLVER_5X5_FAST_H

// ============================================================================
// Solver5x5Fast - 5x5 (25 cells) 2048 AI solver
//   Based on the official ziap/2048-ai 4x4 Zig algorithm, adapted for the odd-size 5x5 board
//
// 5x5 specifics (odd size, 25 cells):
//   - 5 cells per row; the snake path naturally turns at the edges, the middle column is the hub
//   - Many empty cells (up to 25), large chance-layer branching -> MAX_SAMPLE=20 approximates full evaluation
//   - High corner concentration -> Formation uses small 2-5 cell patterns (aligned with ziap's 24 patterns)
//
// Core design (aligned with official ziap):
//   1. 128-bit board: 5 rows x uint32_t, 5 bits per cell (rank 0..31)
//   2. Hand-unrolled 5-element slide-and-merge (no 2^25 big table)
//   3. Evaluation: official ziap params (LOST=200000, MONO=47, SUM=11, MERGES=700, EMPTY=270)
//      Row/column independent scoring, bidirectional MONO (no fixed snake; corner switching allowed)
//   4. Formation snake protection: 24 patterns (2x2 corner + extension / corner L / corner + 1 neighbor)
//      Dynamically detects big-tile formations at board corners, filters moves that break them -> tiles stay put
//      cutoff = lowest high rank + 1 (aligned with ziap), MIN_RANK=9 (512)
//   5. Zobrist hashing + transposition table (2^22=64MB, adopt-on-formation-change)
//   6. Expectimax + iterative deepening: depth 3..10, returns current best on timeout, <=60ms/move
//      No probability pruning (aligned with official); node count controlled by iterative deepening + timeout
//   7. BFS adaptive depth: estimated from the empty-cell count (5x5 BFS is too large)
//   8. Chance layer: depth<=3 spawns both 2 and 4, depth>=4 spawns only 2
// ============================================================================

#if defined(_MSC_VER)
#  pragma inline_recursion(on)
#  pragma inline_depth(255)
#endif

#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <chrono>
#include <memory>
#include <random>
#include <mutex>
#include <algorithm>
#include <string>

class Solver5x5Fast {
public:
    using BoardMatrix = std::vector<std::vector<int>>;
    struct MoveScore { int dir; float score; bool safe; bool allowed; };

private:
    static constexpr int N = 5;                       // 5×5
    static constexpr int CELLS = N * N;               // 25
    static constexpr uint32_t CELL_MASK = 0x1Fu;       // 5 bit
    static constexpr int CELL_BITS = 5;
    static constexpr uint32_t ROW_MASK = (1u << (CELL_BITS * N)) - 1u; // 25 bit

    // ---- 128-bit board: 5 rows x 25 bits ----
    struct Board {
        uint32_t r[N];
    };

    static __forceinline bool boardsEq(const Board& a, const Board& b) {
        return a.r[0] == b.r[0] && a.r[1] == b.r[1] && a.r[2] == b.r[2]
            && a.r[3] == b.r[3] && a.r[4] == b.r[4];
    }
    static __forceinline int getCell(const Board& b, int r, int c) {
        return (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
    }
    static __forceinline void setCell(Board& b, int r, int c, uint32_t v) {
        uint32_t sh = (uint32_t)(c * CELL_BITS);
        b.r[r] = (b.r[r] & ~(CELL_MASK << sh)) | ((v & CELL_MASK) << sh);
    }

    // ========================================================================
    //  Hand-unrolled 5-element slide-and-merge (LEFT direction)
    //  row: 5 cells x 5 bits, cell j at bits [j*5, j*5+5)
    //  Returns the new row + scoreGain (sum of new tile values produced by merges)
    //  Fixed 5-iteration loop + __forceinline -> fully unrolled by MSVC, no 2^25 table
    // ========================================================================
    static __forceinline uint32_t moveRowLeft(uint32_t row, int& scoreGain) {
        uint32_t c0 = row & CELL_MASK;
        uint32_t c1 = (row >> 5) & CELL_MASK;
        uint32_t c2 = (row >> 10) & CELL_MASK;
        uint32_t c3 = (row >> 15) & CELL_MASK;
        uint32_t c4 = (row >> 20) & CELL_MASK;

        // 1) Compact non-zeros to the left (fixed 5 passes, compiler unrolls)
        uint32_t cmp[5] = {0, 0, 0, 0, 0};
        int w = 0;
        if (c0) cmp[w++] = c0;
        if (c1) cmp[w++] = c1;
        if (c2) cmp[w++] = c2;
        if (c3) cmp[w++] = c3;
        if (c4) cmp[w++] = c4;

        // 2) Merge adjacent equal values left to right (each tile merges at most once, standard 2048 rules)
        uint32_t res[5] = {0, 0, 0, 0, 0};
        int out = 0, i = 0;
        scoreGain = 0;
        while (i < w) {
            if (i + 1 < w && cmp[i] == cmp[i + 1]) {
                uint32_t merged = cmp[i] + 1;          // rank+1 (2^rank → 2^(rank+1))
                res[out++] = merged;
                scoreGain += mergeTileValue(cmp[i]);    // new tile value = 2^(rank+1)
                i += 2;
            } else {
                res[out++] = cmp[i];
                i++;
            }
        }
        return res[0] | (res[1] << 5) | (res[2] << 10) | (res[3] << 15) | (res[4] << 20);
    }

    // Real face value of a merged tile (for scoreGain): rank r + rank r -> 2^(r+1)
    static __forceinline int mergeTileValue(uint32_t rank) {
        // rank in [1,31]; 1<<(rank+1) overflows at rank=31 -> clamp
        if (rank >= 31) return 0x7FFFFFFF;
        return 1 << (rank + 1);
    }

    static __forceinline uint32_t reverseRow(uint32_t row) {
        uint32_t c0 = row & CELL_MASK;
        uint32_t c1 = (row >> 5) & CELL_MASK;
        uint32_t c2 = (row >> 10) & CELL_MASK;
        uint32_t c3 = (row >> 15) & CELL_MASK;
        uint32_t c4 = (row >> 20) & CELL_MASK;
        return c4 | (c3 << 5) | (c2 << 10) | (c1 << 15) | (c0 << 20);
    }

    static __forceinline void transpose(const Board& in, Board& out) {
        // out[c] = 5 cells of column c packed (cell r at bit r*5)
        out.r[0] = ((in.r[0]      ) & CELL_MASK)
                 | (((in.r[1]      ) & CELL_MASK) << 5)
                 | (((in.r[2]      ) & CELL_MASK) << 10)
                 | (((in.r[3]      ) & CELL_MASK) << 15)
                 | (((in.r[4]      ) & CELL_MASK) << 20);
        out.r[1] = ((in.r[0] >> 5  ) & CELL_MASK)
                 | (((in.r[1] >> 5  ) & CELL_MASK) << 5)
                 | (((in.r[2] >> 5  ) & CELL_MASK) << 10)
                 | (((in.r[3] >> 5  ) & CELL_MASK) << 15)
                 | (((in.r[4] >> 5  ) & CELL_MASK) << 20);
        out.r[2] = ((in.r[0] >> 10 ) & CELL_MASK)
                 | (((in.r[1] >> 10 ) & CELL_MASK) << 5)
                 | (((in.r[2] >> 10 ) & CELL_MASK) << 10)
                 | (((in.r[3] >> 10 ) & CELL_MASK) << 15)
                 | (((in.r[4] >> 10 ) & CELL_MASK) << 20);
        out.r[3] = ((in.r[0] >> 15 ) & CELL_MASK)
                 | (((in.r[1] >> 15 ) & CELL_MASK) << 5)
                 | (((in.r[2] >> 15 ) & CELL_MASK) << 10)
                 | (((in.r[3] >> 15 ) & CELL_MASK) << 15)
                 | (((in.r[4] >> 15 ) & CELL_MASK) << 20);
        out.r[4] = ((in.r[0] >> 20 ) & CELL_MASK)
                 | (((in.r[1] >> 20 ) & CELL_MASK) << 5)
                 | (((in.r[2] >> 20 ) & CELL_MASK) << 10)
                 | (((in.r[3] >> 20 ) & CELL_MASK) << 15)
                 | (((in.r[4] >> 20 ) & CELL_MASK) << 20);
    }

    // dir: 0=LEFT, 1=RIGHT, 2=UP, 3=DOWN (same convention as SolverAI2048)
    static __forceinline bool moveBoard(const Board& in, int dir, Board& out, int& gain) {
        gain = 0;
        if (dir == 0) {            // LEFT
            for (int r = 0; r < N; ++r) {
                int g; out.r[r] = moveRowLeft(in.r[r], g); gain += g;
            }
        } else if (dir == 1) {     // RIGHT = reverse + left + reverse
            for (int r = 0; r < N; ++r) {
                int g;
                uint32_t rev = reverseRow(in.r[r]);
                uint32_t m = moveRowLeft(rev, g);
                out.r[r] = reverseRow(m); gain += g;
            }
        } else if (dir == 2) {     // UP = transpose + left + transpose
            Board t; transpose(in, t);
            for (int r = 0; r < N; ++r) {
                int g; t.r[r] = moveRowLeft(t.r[r], g); gain += g;
            }
            transpose(t, out);
        } else {                    // DOWN = transpose + right + transpose
            Board t; transpose(in, t);
            for (int r = 0; r < N; ++r) {
                int g;
                uint32_t rev = reverseRow(t.r[r]);
                uint32_t m = moveRowLeft(rev, g);
                t.r[r] = reverseRow(m); gain += g;
            }
            transpose(t, out);
        }
        return !boardsEq(in, out);
    }

    // ========================================================================
    //  Formation system (parallel multi-formation protection - big-snake stabilization)
    //  Old: single Formation; a big tile spanning two corners (e.g. 4096 TL + 16384 BR) mismatched -> none
    //  New: 4 independent corner formations active in parallel, each corner detected independently.
    //       A global snake-gradient term (soft constraint) is added, so even when formation=none
    //       big tiles are still pushed toward a corner snake path (matching official ziap's corner switching)
    // ========================================================================
    struct Formation {
        uint32_t cellMask;   // 25 bits: which cells belong to the formation (bit i = cell[i/N][i%N])
        int      cutoff;     // minimum rank for a tile to count in the formation (>= this value)
        bool     active;

        Formation() : cellMask(0), cutoff(0), active(false) {}

        bool intact(const Board& b) const {
            if (!active) return true;
            for (int i = 0; i < CELLS; ++i) {
                if (cellMask & (1u << i)) {
                    int r = i / N, c = i % N;
                    int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
                    if (rk < cutoff) return false;
                }
            }
            return true;
        }

        double value(const Board& b) const {
            if (!active) return 0;
            double v = 0;
            for (int i = 0; i < CELLS; ++i) {
                if (cellMask & (1u << i)) {
                    int r = i / N, c = i % N;
                    int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
                    if (rk > 0) v += (double)rk;
                }
            }
            return v;
        }
    };

    // Parallel multi-formation protection: one per corner (TL/TR/BL/BR)
    // intact requires all 4 to hold (each corner protects its own big tiles)
    struct MultiFormation {
        Formation corners[4];  // 0=TL, 1=TR, 2=BL, 3=BR
        bool anyActive = false;

        bool intact(const Board& b) const {
            if (!anyActive) return true;
            for (int i = 0; i < 4; ++i) {
                if (corners[i].active && !corners[i].intact(b)) return false;
            }
            return true;
        }

        // multi-formation same check (for TT cache adopt; structural only)
        static bool equal(const MultiFormation& a, const MultiFormation& b) {
            if (a.anyActive != b.anyActive) return false;
            for (int i = 0; i < 4; ++i) {
                if (a.corners[i].active != b.corners[i].active) return false;
                if (a.corners[i].active) {
                    if (a.corners[i].cellMask != b.corners[i].cellMask) return false;
                    if (a.corners[i].cutoff   != b.corners[i].cutoff)   return false;
                }
            }
            return true;
        }
    };

    // ========================================================================
    //  Formation patterns (ported from ziap 4x4 Zig, adapted to 5x5)
    //  During search, detect corner big-tile formations and filter moves that break them
    //  This is the essence of 4x4 "dynamic snake" - the snake can switch corners, not a fixed path
    //
    //  5x5 specifics (odd size):
    //    - 5 cells per row; the snake path turns naturally at edges
    //    - The middle column (col 2) is the snake's hub, no special protection needed
    //    - Corner concentration is higher than 4x4 -> patterns can be smaller (2-5 cells)
    //
    //  Bitmask: bit i = cell[i/5][i%5], built with shift expressions to avoid hand-computed hex errors
    //  Aligned with ziap's 24 patterns: 2x2 corner (4) + 2x2+extension (8) + corner L (4) + corner+1 neighbor (8)
    // ========================================================================

    // 2x2 corner block (4 patterns, 4 cells each)
    static constexpr uint32_t FORM_2X2_TL = (1u<<0)|(1u<<1)|(1u<<5)|(1u<<6);
    static constexpr uint32_t FORM_2X2_TR = (1u<<3)|(1u<<4)|(1u<<8)|(1u<<9);
    static constexpr uint32_t FORM_2X2_BL = (1u<<15)|(1u<<16)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_2X2_BR = (1u<<18)|(1u<<19)|(1u<<23)|(1u<<24);

    // 2x2 corner block + 1 extension (8 patterns, 5 cells each)
    static constexpr uint32_t FORM_2X2_TL_R = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<5)|(1u<<6);
    static constexpr uint32_t FORM_2X2_TL_D = (1u<<0)|(1u<<1)|(1u<<5)|(1u<<6)|(1u<<10);
    static constexpr uint32_t FORM_2X2_TR_L = (1u<<2)|(1u<<3)|(1u<<4)|(1u<<8)|(1u<<9);
    static constexpr uint32_t FORM_2X2_TR_D = (1u<<3)|(1u<<4)|(1u<<8)|(1u<<9)|(1u<<13);
    static constexpr uint32_t FORM_2X2_BL_R = (1u<<15)|(1u<<16)|(1u<<17)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_2X2_BL_U = (1u<<10)|(1u<<15)|(1u<<16)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_2X2_BR_L = (1u<<17)|(1u<<18)|(1u<<19)|(1u<<23)|(1u<<24);
    static constexpr uint32_t FORM_2X2_BR_U = (1u<<13)|(1u<<18)|(1u<<19)|(1u<<23)|(1u<<24);

    // Corner L (4 patterns, 3 cells each: corner + row neighbor + column neighbor)
    static constexpr uint32_t FORM_L_TL = (1u<<0)|(1u<<1)|(1u<<5);
    static constexpr uint32_t FORM_L_TR = (1u<<3)|(1u<<4)|(1u<<9);
    static constexpr uint32_t FORM_L_BL = (1u<<15)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_L_BR = (1u<<19)|(1u<<23)|(1u<<24);

    // Corner + 1 neighbor (8 patterns, 2 cells each)
    static constexpr uint32_t FORM_C_TL_R = (1u<<0)|(1u<<1);
    static constexpr uint32_t FORM_C_TL_D = (1u<<0)|(1u<<5);
    static constexpr uint32_t FORM_C_TR_L = (1u<<3)|(1u<<4);
    static constexpr uint32_t FORM_C_TR_D = (1u<<4)|(1u<<9);
    static constexpr uint32_t FORM_C_BL_R = (1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_C_BL_U = (1u<<15)|(1u<<20);
    static constexpr uint32_t FORM_C_BR_L = (1u<<23)|(1u<<24);
    static constexpr uint32_t FORM_C_BR_U = (1u<<19)|(1u<<24);

    static constexpr uint32_t FORMATION_PATTERNS[] = {
        // 2x2 + 1 extension (8, matched first - largest protection)
        FORM_2X2_TL_R, FORM_2X2_TL_D, FORM_2X2_TR_L, FORM_2X2_TR_D,
        FORM_2X2_BL_R, FORM_2X2_BL_U, FORM_2X2_BR_L, FORM_2X2_BR_U,
        // 2x2 corner block (4)
        FORM_2X2_TL, FORM_2X2_TR, FORM_2X2_BL, FORM_2X2_BR,
        // Corner L (4)
        FORM_L_TL, FORM_L_TR, FORM_L_BL, FORM_L_BR,
        // Corner + 1 neighbor (8, matched last - smallest protection, easiest to satisfy)
        FORM_C_TL_R, FORM_C_TL_D, FORM_C_TR_L, FORM_C_TR_D,
        FORM_C_BL_R, FORM_C_BL_U, FORM_C_BR_L, FORM_C_BR_U,
    };
    static constexpr int FORMATION_COUNT = 24;

    // Aligned with official ziap: MIN_RANK=9 (512), 5x5 uses the same value
    static constexpr int FORMATION_MIN_RANK = 9;

    // Per-corner pattern subset indices (grouped as TL/TR/BL/BR in FORMATION_PATTERNS)
    // 6 patterns per corner: 2x2+extension(2) + 2x2 corner(1) + corner L(1) + corner+1 neighbor(2)
    static constexpr int CORNER_PATTERNS[4][6] = {
        // TL (0):  FORM_2X2_TL_R(0), FORM_2X2_TL_D(1), FORM_2X2_TL(8), FORM_L_TL(12), FORM_C_TL_R(16), FORM_C_TL_D(17)
        {0, 1, 8, 12, 16, 17},
        // TR (1):  FORM_2X2_TR_L(2), FORM_2X2_TR_D(3), FORM_2X2_TR(9), FORM_L_TR(13), FORM_C_TR_L(18), FORM_C_TR_D(19)
        {2, 3, 9, 13, 18, 19},
        // BL (2):  FORM_2X2_BL_R(4), FORM_2X2_BL_U(5), FORM_2X2_BL(10), FORM_L_BL(14), FORM_C_BL_R(20), FORM_C_BL_U(21)
        {4, 5, 10, 14, 20, 21},
        // BR (3):  FORM_2X2_BR_L(6), FORM_2X2_BR_U(7), FORM_2X2_BR(11), FORM_L_BR(15), FORM_C_BR_L(22), FORM_C_BR_U(23)
        {6, 7, 11, 15, 22, 23}
    };

    // Per-corner independent detectFormation (official ziap logic, but matches only this corner's pattern subset)
    // cornerIdx: 0=TL, 1=TR, 2=BL, 3=BR
    Formation detectFormationForCorner(const Board& b, int cornerIdx) const {
        // 1) Collect tiles with rank>=MIN_RANK near this corner for present/repeated stats
        //    Also allows the global cutoff to use a corner+global combined view - for simplicity,
        //    global stats are used here (consistent with official), matching only this corner's subset
        uint32_t present = 0;   // bit i = rank i appears at least once
        uint32_t repeated = 0;  // bit i = rank i appears 2+ times
        for (int i = 0; i < CELLS; ++i) {
            int r = i / N, c = i % N;
            int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
            if (rk >= FORMATION_MIN_RANK) {
                uint32_t bit = 1u << rk;
                repeated |= present & bit;
                present |= bit;
            }
        }
        if (present == 0) return Formation();

        // 2) cutoff = lowest present high rank + 1
        int lowestRank = FORMATION_MIN_RANK;
        for (int i = FORMATION_MIN_RANK; i < 32; ++i) {
            if (present & (1u << i)) { lowestRank = i; break; }
        }
        int cutoff = std::min(lowestRank + 1, 31);

        // 3) A repeated rank >= cutoff -> no formation (equal tiles can merge)
        uint32_t cutoffMask = 0;
        for (int i = cutoff; i < 32; ++i) {
            if (present & (1u << i)) cutoffMask |= (1u << i);
        }
        if (repeated & cutoffMask) return Formation();

        // 4) big = bitmask of all cells with rank >= cutoff
        uint32_t big = 0;
        for (int i = 0; i < CELLS; ++i) {
            int r = i / N, c = i % N;
            int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
            if (rk >= cutoff) big |= (1u << i);
        }

        // 5) Pick the best among this corner's 6-pattern subset (key: each corner matches its own patterns)
        Formation best;
        double bestValue = 0;
        const int* patterns = CORNER_PATTERNS[cornerIdx];
        for (int p = 0; p < 6; ++p) {
            uint32_t pattern = FORMATION_PATTERNS[patterns[p]];
            if ((big & pattern) != pattern) continue;
            uint32_t seen = 0;
            double val = 0;
            for (int i = 0; i < CELLS; ++i) {
                if (pattern & (1u << i)) {
                    int r = i / N, c = i % N;
                    int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
                    if (rk > 0) {
                        uint32_t bit = 1u << rk;
                        if (!(seen & bit)) { val += 1.0; seen |= bit; }
                    }
                }
            }
            if (val > bestValue) {
                bestValue = val;
                best.cellMask = pattern;
                best.cutoff = cutoff;
                best.active = true;
            }
        }
        return best;
    }

    // Parallel multi-formation detection: each of the 4 corners runs detectFormation independently, all active together
    //    Fixes the single-formation mismatch (none) when big tiles span two corners (e.g. 4096 TL + 16384 BR)
    MultiFormation detectMultiFormation(const Board& b) const {
        MultiFormation mf;
        mf.anyActive = false;
        for (int i = 0; i < 4; ++i) {
            mf.corners[i] = detectFormationForCorner(b, i);
            if (mf.corners[i].active) mf.anyActive = true;
        }
        return mf;
    }

    // ========================================================================
    //  Evaluation function:
    //    A. Official ziap formula (row/column independent MONO+SUM+MERGES+EMPTY)
    //    B. Global snake-gradient term (soft constraint, 5x5 odd-size) - when formation=none
    //       it still pushes big tiles onto a standard snake path to one of the 4 corners
    //    C. Corner concentration - the top 3 tiles should gather in the same corner (weighted)
    // ========================================================================
    static constexpr double LOST_PENALTY  = 200000.0;
    static constexpr double MONO_WEIGHT   = 47.0;
    static constexpr double SUM_WEIGHT    = 11.0;
    static constexpr double MERGES_WEIGHT = 700.0;
    static constexpr double EMPTY_WEIGHT  = 270.0;
    static constexpr double MONO_POWER    = 4.0;
    static constexpr double SUM_POWER     = 3.5;
    // Soft-constraint weights (extra for 5x5, matching ziap's dynamic-snake gradient)
    //   Weight calibration: the official formula (~1.5-3M per game) must dominate,
    //      soft terms <= 20-30% of official, acting as tiebreakers when MONO/MERGES are close
    //      Old 12/200: SNAKE on par with official, drowning out MONO/MERGES merge-chain logic -> erratic play
    //      New 4/500: SNAKE ~ 10-20% of official (corner preference), CORNER strengthened -> big tiles stay
    static constexpr double SNAKE_WEIGHT  = 4.0;
    static constexpr double CORNER_WEIGHT = 500.0;

    // Snake weight gradients for the 4 corners (boustrophedon, zig-zag from the peak corner)
    //   Gradient: rank_value * snake[row][col], taking the max over the 4 paths
    //   Each path weights 5x5 from 32 down to 0; the largest tile naturally lands on the highest weight
    static inline double s_snakeTL[5][5];  // TL start: r0c0 = max weight, row0 ->, row1 <-, ...
    static inline double s_snakeTR[5][5];  // TR start: r0c4 = max
    static inline double s_snakeBL[5][5];  // BL start: r4c0 = max
    static inline double s_snakeBR[5][5];  // BR start: r4c4 = max

    // Precompute
    static inline double s_sumPow[33];
    static inline double s_monoPow[33];
    static inline uint8_t s_strategicRank[CELLS];
    static inline std::once_flag s_once;

    static void initSnakeGradients() {
        auto fillSnake = [](double g[5][5], int startR, int startC, bool rowMajor) {
            // Boustrophedon: from startR,startC, turn at the end of each row/column
            // Weights from 31.0 down to 7.0 (span 24 over 25 cells)
            int order[25];
            int idx = 0;
            if (rowMajor) {
                // Row-wise snake
                bool leftToRight = (startC == 0);
                for (int r = 0; r < 5; ++r) {
                    int rr = (startR == 4) ? (4 - r) : r;  // bottom-up or top-down
                    if (leftToRight) {
                        for (int c = 0; c < 5; ++c) order[idx++] = rr * 5 + c;
                    } else {
                        for (int c = 4; c >= 0; --c) order[idx++] = rr * 5 + c;
                    }
                    leftToRight = !leftToRight;
                }
            } else {
                // Column-wise snake (unused, kept for extension)
                bool topDown = (startR == 0);
                for (int c = 0; c < 5; ++c) {
                    int cc = (startC == 4) ? (4 - c) : c;
                    if (topDown) {
                        for (int r = 0; r < 5; ++r) order[idx++] = r * 5 + cc;
                    } else {
                        for (int r = 4; r >= 0; --r) order[idx++] = r * 5 + cc;
                    }
                    topDown = !topDown;
                }
            }
            // Descending weights: 31 -> 7
            for (int k = 0; k < 25; ++k) {
                double w = 31.0 - ((double)k * 24.0) / 24.0;  // 25 cells, k=0 -> 31, k=24 -> 7
                int pos = order[k];
                int r = pos / 5, c = pos % 5;
                g[r][c] = w;
            }
        };
        fillSnake(s_snakeTL, 0, 0, true);  // TL start: top-down, first row left-to-right
        fillSnake(s_snakeTR, 0, 4, true);  // TR start: top-down, first row right-to-left
        fillSnake(s_snakeBL, 4, 0, true);  // BL start: bottom-up, first (bottom) row left-to-right
        fillSnake(s_snakeBR, 4, 4, true);  // BR start: bottom-up, first (bottom) row right-to-left
    }

    // Single row/column evaluation (ziap formula, identical to 4x4)
    __forceinline double evalLine(const int cells[5]) const {
        double sum = 0;
        int empty = 0, merges = 0;
        int prev = 0, counter = 0;
        for (int i = 0; i < N; ++i) {
            int rank = cells[i];
            sum += s_sumPow[rank];
            if (rank == 0) { empty++; continue; }
            if (prev == rank) counter++;
            else if (counter > 0) { merges += 1 + counter; counter = 0; }
            prev = rank;
        }
        if (counter > 0) merges += 1 + counter;
        // Monotonicity: left-descending vs right-descending, take the smaller
        double mono_left = 0, mono_right = 0;
        for (int i = 1; i < N; ++i) {
            double l = s_monoPow[cells[i-1]];
            double r = s_monoPow[cells[i]];
            if (cells[i-1] > cells[i]) mono_left += l - r;
            else mono_right += r - l;
        }
        return LOST_PENALTY
             + EMPTY_WEIGHT  * (double)empty
             + MERGES_WEIGHT * (double)merges
             - MONO_WEIGHT   * std::min(mono_left, mono_right)
             - SUM_WEIGHT    * sum;
    }

    double evaluate(const Board& b) const {
        double total = 0;
        // Row evaluation
        for (int r = 0; r < N; ++r) {
            int cells[5];
            for (int c = 0; c < N; ++c)
                cells[c] = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
            total += evalLine(cells);
        }
        // Column evaluation
        for (int c = 0; c < N; ++c) {
            int cells[5];
            int sh = c * CELL_BITS;
            for (int r = 0; r < N; ++r)
                cells[r] = (int)((b.r[r] >> sh) & CELL_MASK);
            total += evalLine(cells);
        }
        // B. Global snake gradient (max over 4 paths, dynamically picks the best corner snake)
        //    score = sum(rank^2.0 * snake[r][c]) - bigger ranks must land on bigger weights
        //    rank^2 instead of rank: amplifies 16384 vs 4096, preventing big-tile drift
        double snakeScore[4] = {0, 0, 0, 0};
        int maxRank1 = 0, maxRank2 = 0, maxRank3 = 0;
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 5; ++c) {
                int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
                if (rk == 0) continue;
                double rkPow = (double)rk * (double)rk;  // rank^2
                snakeScore[0] += rkPow * s_snakeTL[r][c];
                snakeScore[1] += rkPow * s_snakeTR[r][c];
                snakeScore[2] += rkPow * s_snakeBL[r][c];
                snakeScore[3] += rkPow * s_snakeBR[r][c];
                // Also track the top 3 ranks (for term C concentration)
                if (rk > maxRank1) { maxRank3 = maxRank2; maxRank2 = maxRank1; maxRank1 = rk; }
                else if (rk > maxRank2) { maxRank3 = maxRank2; maxRank2 = rk; }
                else if (rk > maxRank3) { maxRank3 = rk; }
            }
        }
        double bestSnake = snakeScore[0];
        for (int i = 1; i < 4; ++i) if (snakeScore[i] > bestSnake) bestSnake = snakeScore[i];
        total += SNAKE_WEIGHT * bestSnake;

        // C. Corner concentration: top-3-rank tiles should all be near the same corner
        //    Find top3 positions, measure how consistent their weights are across the 4 paths
        if (maxRank1 >= 7) {  // activate only at rank>=7 (128) to avoid small-tile noise
            double pathTop3[4] = {0, 0, 0, 0};
            double norm = 0;
            for (int r = 0; r < 5; ++r) {
                for (int c = 0; c < 5; ++c) {
                    int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
                    if (rk == maxRank1 || rk == maxRank2 || rk == maxRank3) {
                        double w = (double)rk * (double)rk;  // weight by rank^2
                        pathTop3[0] += w * s_snakeTL[r][c];
                        pathTop3[1] += w * s_snakeTR[r][c];
                        pathTop3[2] += w * s_snakeBL[r][c];
                        pathTop3[3] += w * s_snakeBR[r][c];
                        norm += w * 31.0;  // theoretical max (every cell at 31.0)
                    }
                }
            }
            if (norm > 0) {
                double bestPath = pathTop3[0];
                for (int i = 1; i < 4; ++i) if (pathTop3[i] > bestPath) bestPath = pathTop3[i];
                // ratio = actual / theoretical best (all at start), closer to 1.0 is better
                double cornerRatio = bestPath / norm;  // [0, 1]
                total += CORNER_WEIGHT * (cornerRatio * 100.0);  // scaled up to a suitable magnitude
            }
        }

        return total;
    }

    // ========================================================================
    //  Zobrist hashing (read-only shared, initialized once at program level)
    // ========================================================================
    static inline uint64_t s_zobrist[CELLS][32];

    static void initStatic() {
        std::call_once(s_once, []() {
            std::mt19937_64 rng(0x2048550ULL);
            for (int i = 0; i < CELLS; ++i)
                for (int v = 0; v < 32; ++v)
                    s_zobrist[i][v] = rng();
            // Precompute power table (original ziap 4x4 weights)
            s_sumPow[0] = 0.0; s_monoPow[0] = 0.0;
            for (int i = 1; i < 33; ++i) {
                s_sumPow[i]  = std::pow((double)i, SUM_POWER);
                s_monoPow[i] = std::pow((double)i, MONO_POWER);
            }
            // Strategic sampling priority: corner=0, edge=1, center=2
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    int idx = r * N + c;
                    bool isCorner = (r == 0 || r == N - 1) && (c == 0 || c == N - 1);
                    bool isEdge = (r == 0 || r == N - 1 || c == 0 || c == N - 1);
                    s_strategicRank[idx] = isCorner ? 0 : (isEdge ? 1 : 2);
                }
            // 4 snake gradients (new for 5x5)
            initSnakeGradients();
        });
    }

    __forceinline uint64_t zobristHash(const Board& b) const {
        uint64_t h = 0;
        for (int i = 0; i < CELLS; ++i) {
            int r = i / N, c = i % N;
            int v = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
            if (v) h ^= s_zobrist[i][v];
        }
        return h;
    }

    // ========================================================================
    //  Transposition table: 2^22 entries x 16 bytes = 64MB per instance
    //  thread_local instance -> single-threaded access -> lock-free, race-free
    //  Stores exact (board, depth) -> expected score; reuse when entry.depth >= reqDepth
    //
    //  Root-cause fix for "getting worse over time":
    //    Old: TT memset-cleared on formation change -> all experience discarded, cold start every step
    //        = every search is a cold start, fewer hits as steps grow, score drops
    //    New: TT never cleared. Zobrist hash uniquely represents (board+rank); depth-first replacement
    //        keeps high-precision results at various depths. Formation changes do not affect hash
    //        correctness, since the hash depends only on the actual board. Experience accumulates,
    //        more hits -> deeper search -> smarter over time
    // ========================================================================
    static constexpr int TT_BITS = 22;
    static constexpr size_t TT_SIZE = (size_t)1 << TT_BITS;
    static constexpr size_t TT_MASK = TT_SIZE - 1;
    struct TTEntry {
        uint64_t hash;    // 8 bytes (Zobrist, double-check key)
        float    score;   // 4 bytes (exact expected score)
        uint8_t  depth;   // 1 byte (remaining depth)
        uint8_t  flag;    // 1 byte (0=empty, 1=valid)
        uint16_t pad;      // 2
    };                    // 16 bytes
    std::unique_ptr<TTEntry[]> m_tt;

    // TT is never cleared (initialized once at construction), so formationSame/adoptFormation are unnecessary
    // Placeholder members kept to avoid API breakage (adopt becomes a no-op)
    MultiFormation m_lastFormation;
    bool      m_formationValid = false;

    void ensureTT() {
        if (!m_tt) {
            m_tt = std::make_unique<TTEntry[]>(TT_SIZE);
            std::memset(m_tt.get(), 0, TT_SIZE * sizeof(TTEntry));
            m_formationValid = false;
            m_lastFormation = MultiFormation();
        }
    }

    // adopt: no-op in the new version (TT never cleared, accumulates across steps)
    void adoptFormation(const MultiFormation& formation) {
        (void)formation;
        m_lastFormation = formation;
        m_formationValid = true;
    }

    __forceinline bool ttGet(uint64_t hash, int depth, float& outScore) const {
        const TTEntry& e = m_tt[hash & TT_MASK];
        if (e.flag && e.hash == hash && e.depth >= (uint8_t)depth) {
            outScore = e.score;
            return true;
        }
        return false;
    }
    // always-replace strategy (aligned with official ziap):
    //   Old depth-first strategy blocks shallow writes -> stale entries persist across games
    //   -> new games hit stale scores from old games -> AI degrades
    //   Official ziap uses always-replace: new search results naturally overwrite old entries,
    //   no stale accumulation across games; in-game reuse gives the highest hit rate
    __forceinline void ttPut(uint64_t hash, int depth, float score) {
        TTEntry& e = m_tt[hash & TT_MASK];
        e.hash = hash;
        e.score = score;
        e.depth = (uint8_t)depth;
        e.flag = 1;
    }

    // ========================================================================
    //  Iterative-deepening Expectimax
    //
    //  Aligned with official search.zig + 5x5 stability:
    //    1. MAX_DEPTH 12, TIME_BUDGET 90ms (with 80ms animation + 20ms step delay = 100ms/move)
    //    2. NODE_BUDGET = official budget 2^19 x 1.5 grid factor (5x5 is 1.56x larger than 4x4)
    //       Official benchmark: 4x4 budget=2^19 reaches 16384 84.6%, 32768 17.6%
    //       5x5: 2^19x1.5 ~ 2^19.58 ~ 786k, set to 2^20 = 1,048,576 for endgame depth
    //    3. BFS approximate depth (finer tiers for 5x5's 25 empty-cell range)
    //    4. fullChance: depth<=4 spawns both 2 and 4 (official 4x4 is double-valued tree-wide; 5x5 shallow layers too)
    // ========================================================================
    int          m_maxDepth = 12;   // iterative-deepening cap (original constant, overridable via the facade's three tiers)
    static constexpr int    MIN_DEPTH      = 3;     // iterative-deepening start
    static constexpr int    MAX_SAMPLE     = 20;    // max empty cells sampled in the chance layer (5x5 has up to 25)
    double       m_timeMs   = 90.0;  // per-move time budget in ms (original 90ms)
    uint64_t     m_budget   = 1ULL << 20;  // node budget (1M)
    static constexpr float  NEG_INF        = -1e30f;

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_deadline;
    bool      m_timedOut = false;
    uint64_t  m_nodeCount = 0;
    bool      m_nodeBudgetExceeded = false;

    __forceinline bool checkTimeout() {
        m_nodeCount++;
        // node budget first (official budget mechanism: stable depth, immune to CPU variance)
        if ((m_nodeCount & 0x1FF) == 0) {  // check every 512 nodes
            if (m_nodeCount >= m_budget) { m_nodeBudgetExceeded = true; return true; }
            if (Clock::now() >= m_deadline)   { m_timedOut = true; return true; }
        }
        return false;
    }

    // Max layer: player picks the best direction + MultiFormation filter (4 corners intact in parallel)
    float searchMax(const Board& b, int depth, uint64_t hash, const MultiFormation& formation) {
        if (depth == 0) return (float)evaluate(b);
        float cached;
        if (ttGet(hash, depth, cached)) return cached;
        if (checkTimeout()) { m_timedOut = true; return (float)evaluate(b); }

        float best = NEG_INF;
        Board nb;
        for (int d = 0; d < 4; ++d) {
            int gain;
            if (!moveBoard(b, d, nb, gain)) continue;
            // MultiFormation protection: 4-corner parallel intact filter
            if (formation.anyActive && !formation.intact(nb)) continue;
            uint64_t h = zobristHash(nb);
            float cv = searchChance(nb, depth - 1, h, formation);
            float val = (float)gain + cv;
            if (val > best) best = val;
            if (m_timedOut || m_nodeBudgetExceeded) break;
        }
        if (best <= NEG_INF) {
            best = (float)evaluate(b) - (float)LOST_PENALTY;
        }
        ttPut(hash, depth, best);
        return best;
    }

    // Chance layer: random tile spawn + MultiFormation pass-through
    //  Official ziap: evaluate all empty cells; 5x5: shallow layers (<=4) spawn both 2 and 4
    float searchChance(const Board& b, int depth, uint64_t hash, const MultiFormation& formation) {
        if (depth == 0) return (float)evaluate(b);

        float cached;
        if (ttGet(hash, depth, cached)) return cached;
        if (checkTimeout()) { m_timedOut = true; return (float)evaluate(b); }

        int empties[CELLS];
        int ne = 0;
        for (int i = 0; i < CELLS; ++i) {
            int r = i / N, c = i % N;
            if (((b.r[r] >> (c * CELL_BITS)) & CELL_MASK) == 0) empties[ne++] = i;
        }
        if (ne == 0) {
            return searchMax(b, depth, hash, formation);
        }

        sortEmptiesByPriority(empties, ne);
        int K = ne < MAX_SAMPLE ? ne : MAX_SAMPLE;
        bool fullChance = (depth <= 4);  // double-value below depth 4

        double sum = 0.0;
        for (int s = 0; s < K; ++s) {
            int idx = empties[s];
            int r = idx / N, c = idx % N;
            Board b2 = b;
            setCell(b2, r, c, 1);
            uint64_t h2 = hash ^ s_zobrist[idx][1];
            double v2 = searchMax(b2, depth - 1, h2, formation);
            if (fullChance) {
                Board b4 = b;
                setCell(b4, r, c, 2);
                uint64_t h4 = hash ^ s_zobrist[idx][2];
                double v4 = searchMax(b4, depth - 1, h4, formation);
                sum += 0.9 * v2 + 0.1 * v4;
            } else {
                sum += v2;
            }
            if (m_timedOut || m_nodeBudgetExceeded) break;
        }
        double expected = sum / (double)K;
        ttPut(hash, depth, (float)expected);
        return (float)expected;
    }

    // Simple insertion sort: by s_strategicRank ascending (corners first), then by idx
    static void sortEmptiesByPriority(int* arr, int n) {
        for (int i = 1; i < n; ++i) {
            int key = arr[i];
            int kr = s_strategicRank[key];
            int j = i - 1;
            while (j >= 0 && (s_strategicRank[arr[j]] > kr ||
                   (s_strategicRank[arr[j]] == kr && arr[j] > key))) {
                arr[j + 1] = arr[j];
                --j;
            }
            arr[j + 1] = key;
        }
    }

    // ========================================================================
    //  Encode/decode: BoardMatrix (int[5][5] face values) <-> Board (rank representation)
    // ========================================================================
    static Board encode(const BoardMatrix& m) {
        Board b = {{0, 0, 0, 0, 0}};
        for (int r = 0; r < N; ++r) {
            uint32_t row = 0;
            for (int c = 0; c < N; ++c) {
                int v = m[r][c];
                int logVal = 0;
                if (v > 0) { while (v > 1) { v >>= 1; ++logVal; } }
                row |= ((uint32_t)logVal) << (c * CELL_BITS);
            }
            b.r[r] = row;
        }
        return b;
    }

public:
    Solver5x5Fast() { initStatic(); ensureTT(); }
    ~Solver5x5Fast() = default;
    Solver5x5Fast(const Solver5x5Fast&) = delete;
    Solver5x5Fast& operator=(const Solver5x5Fast&) = delete;

    // ===== Facade-compatible control interface (three intelligence tiers) =====
    void setBudget(uint64_t budget, double timeMs) { m_budget = budget; m_timeMs = timeMs; }
    void setTimeBudgetMs(double ms) { m_timeMs = ms; }
    void setMaxDepth(int d) { m_maxDepth = std::max(1, d); }
    void setBoardSize(int) {}      // compatibility placeholder (engine is fixed 5x5)
    void setParallel(int) {}       // compatibility placeholder (deterministic single-threaded search)
    void newGame() {}              // TT is never cleared (accumulates across steps, gets smarter)
    void clearTT() {}              // same as above: no-op

    // ===== Main interface: 4-direction scores (iterative-deepening Expectimax) =====
    std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
        initStatic();
        ensureTT();
        std::vector<MoveScore> result;
        result.reserve(4);

        Board b = encode(board);

        // Parallel multi-formation detection: each corner detects independently, all active together
        //    Fixes single-formation mismatch (none) when big tiles span two corners -> big-snake failure
        MultiFormation formation = detectMultiFormation(b);

        // TT never cleared (accumulates across steps, gets smarter)
        adoptFormation(formation);

        // First compute legality and immediate gain for the 4 directions (reused at every depth)
        Board after[4];
        int  gain[4]   = {0, 0, 0, 0};
        bool moved[4]  = {false, false, false, false};
        for (int d = 0; d < 4; ++d) {
            moved[d] = moveBoard(b, d, after[d], gain[d]);
            // MultiFormation protection: root also filters directions that break any of the 4 corner formations
            if (moved[d] && formation.anyActive && !formation.intact(after[d])) {
                moved[d] = false;
            }
        }

        // BFS adaptive depth (matching official ziap's bfs.expand().depth + 1 granularity)
        //   5x5's 25 cells cannot be fully BFS'd; estimated from empty count + max rank:
        //     >=14 empty: BFS depth 2 (search depth 3) - early open board, shallow is enough
        //     10-13 empty: BFS depth 4 (search depth 5)
        //     7-9 empty: BFS depth 6 (search depth 7)
        //     4-6 empty: BFS depth 8 (search depth 9)
        //     1-3 empty: BFS depth 10 (search depth 11, near MAX)
        //     0 empty: BFS depth 11 (search depth 12, MAX_DEPTH)
        //    Larger max rank -> +1 depth (dangerous endgame needs deeper search)
        int emptyCount = 0;
        int maxRank = 0;
        for (int i = 0; i < CELLS; ++i) {
            int r = i / N, c = i % N;
            int rk = (int)((b.r[r] >> (c * CELL_BITS)) & CELL_MASK);
            if (rk == 0) emptyCount++;
            else if (rk > maxRank) maxRank = rk;
        }
        int bfsDepth;
        if      (emptyCount >= 14) bfsDepth = 2;
        else if (emptyCount >= 10) bfsDepth = 4;
        else if (emptyCount >= 7)  bfsDepth = 6;
        else if (emptyCount >= 4)  bfsDepth = 8;
        else if (emptyCount >= 1)  bfsDepth = 10;
        else                       bfsDepth = 11;
        // Endgame bonus: rank >= 2^13 (8192) -> search 1 more ply to keep the big tile alive
        if (maxRank >= 13) bfsDepth = std::min(bfsDepth + 1, 11);
        int targetDepth = std::min(bfsDepth + 1, m_maxDepth);
        if (targetDepth < MIN_DEPTH) targetDepth = MIN_DEPTH;

        m_deadline = Clock::now() + std::chrono::milliseconds((int)m_timeMs);
        m_timedOut = false;
        m_nodeCount = 0;
        m_nodeBudgetExceeded = false;

        float cur[4] = { NEG_INF, NEG_INF, NEG_INF, NEG_INF };
        bool  haveResult = false;

        // Iterative deepening: from MIN_DEPTH to targetDepth
        for (int depth = MIN_DEPTH; depth <= targetDepth; ++depth) {
            float trial[4] = { NEG_INF, NEG_INF, NEG_INF, NEG_INF };
            bool depthComplete = true;

            for (int d = 0; d < 4; ++d) {
                if (!moved[d]) { trial[d] = NEG_INF; continue; }
                uint64_t h = zobristHash(after[d]);
                float cached;
                float v;
                if (ttGet(h, depth - 1, cached)) {
                    v = cached;
                } else {
                    // top-level entry (official expectNode)
                    v = searchChance(after[d], depth - 1, h, formation);
                    if (m_timedOut || m_nodeBudgetExceeded) { depthComplete = false; break; }
                    ttPut(h, depth - 1, v);
                }
                trial[d] = (float)gain[d] + v;
            }

            if (!depthComplete) break;        // this depth was interrupted by timeout/budget; keep the previous depth's result
            for (int d = 0; d < 4; ++d) cur[d] = trial[d];
            haveResult = true;
            if (m_timedOut || m_nodeBudgetExceeded) break;  // this depth completed but budget is exhausted; stop deepening
        }

        if (!haveResult) {
            // extreme fallback: depth=0 evaluation (evaluate each direction's resulting board)
            for (int d = 0; d < 4; ++d) {
                cur[d] = moved[d] ? (float)evaluate(after[d]) : NEG_INF;
            }
            haveResult = true;
        }

        for (int d = 0; d < 4; ++d) {
            MoveScore ms;
            ms.dir = d;
            ms.score = moved[d] ? cur[d] : NEG_INF;
            ms.allowed = moved[d];
            ms.safe = moved[d];
            result.push_back(ms);
        }
        std::sort(result.begin(), result.end(),
                  [](const MoveScore& a, const MoveScore& b) { return a.score > b.score; });
        return result;
    }

    // ===== Evaluation interface (used by undo checks etc.) =====
    float evaluateBoard(const BoardMatrix& board) {
        initStatic();
        ensureTT();
        return (float)evaluate(encode(board));
    }

    // ===== Detect whether a merge path exists after undo =====
    // Try all 4 directions; true if any produces an actual merge (gain>0)
    bool hasMergeableMove(const BoardMatrix& board) {
        initStatic();
        Board b = encode(board);
        for (int d = 0; d < 4; ++d) {
            Board nb;
            int gain = 0;
            if (moveBoard(b, d, nb, gain) && gain > 0) return true;
        }
        return false;
    }

    std::string getStatsString() const {
        return "AI[5x5 u128 ziap-eval MONO+SUM+MERGES+EMPTY Formation-24pattern-protect Zobrist-TT(2^22=64MB,adopt) ID-Expectimax BFS-depth-adaptive]";
    }
};

#endif // SOLVER_5X5_FAST_H
