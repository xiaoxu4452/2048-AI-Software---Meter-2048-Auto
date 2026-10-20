#ifndef SOLVER_3X3_FAST_H
#define SOLVER_3X3_FAST_H

// ============================================================================
// Solver3x3Fast - brand-new 3x3 (9 cells) 2048 AI solver
//   Core idea: "4x4 ziap algorithm architecture" + "3x3-specific calibration" + "dynamic snake protection"
//
// Why not copy 4x4 / 5x5 directly?
//   - 3x3 space is extremely tight (9 cells vs 16/25), late game has ~0-2 empty cells, huge deadlock risk
//   - A 3-cell merge window is short; keeping monotonicity is hard -> no fixed snake, use bidirectional MIN(MONO)
//   - An isolated big tile hurts far more than in 4x4 -> a SUM penalty is required, misplacement is penalized immediately
//   - Branching factor is far smaller than 4x4 -> deeper search (depth=12) and full chance-layer evaluation are affordable
//
// Core design:
//   1. Board: single uint64_t, 9 cells x 4 bits = 36 bits, rank 0..15
//      - GET_CELL / SET_CELL are zero-cost inlined
//   2. Move (full lookup): move_row_left[4096] + score_row_left[4096] (12-bit row)
//      - Full board move = 3 row lookups + bit stitching, no loops
//      - Transpose is pure masks + shifts, no loops
//   3. Evaluation (4x4 ziap architecture, 3x3-calibrated weights, stays pure Ziap formula):
//      - Precomputed score_table[4096] (each row scored independently)
//      - evaluate = sum of 3 row score_table[row] + 3 column score_table[col]
//      - Five-term heuristic:
//          LOST_PENALTY (baseline offset)
//        + EMPTY_W x empty  (empty cells are extremely precious)
//        + MERGES_W x merges (mergeable pairs)
//        - MONO_W x min(left-decreasing diff, right-decreasing diff)   <- bidirectional, corner not forced
//        - SUM_W  x sum sumPow(rank)             <- penalizes isolated/scattered tiles
//   4. Search: Expectimax + iterative deepening, MAX_DEPTH=12 (affordable on 3x3)
//   5. Transposition table: 2^22 x 16B = 64MB per instance, Zobrist hash, lock-free (thread_local)
//   6. Chance layer: 3x3 has <= 9 empty cells, MAX_SAMPLE=9 full evaluation; depth<=4 spawns both 2 and 4
//   7. Hot-path constraints: no division/modulo, no malloc (TT allocated once), recursion depth MAX=12 <= spec 12
//   8. Formation system (ported from official ziap 4x4 search.zig + Formation.zig):
//      - Root node detectFormation() detects big-tile clustering at board corners (dynamic snake)
//      - searchMax uses formation.intact(nb) to filter moves that break the cluster
//      - searchChance passes the formation through (random tiles are not filtered)
//      - 3x3 has 20 formations: 2x2 corner block (4) + corner L (4) + 3-cell corner row/col (4) + corner+1 neighbor (8)
//      - MIN_RANK=8 (256), cutoff = ctz(present)+1; no formation when a big rank repeats
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

class Solver3x3Fast {
public:
    using BoardMatrix = std::vector<std::vector<int>>;
    struct MoveScore { int dir; float score; bool safe; bool allowed; };

private:
    static constexpr int N = 3;                          // 3×3
    static constexpr int CELLS = N * N;                  // 9
    static constexpr uint64_t CELL_MASK = 0xFu;          // 4 bit
    static constexpr int CELL_BITS = 4;
    static constexpr uint64_t ROW_MASK = 0xFFFu;         // 12 bits (3 cells x 4 bits)
    static constexpr int ROW_BITS = 12;
    static constexpr int ROW_TABLE_SIZE = 1 << ROW_BITS; // 4096

    // Board layout: idx = r*3 + c, bit position = idx * 4
    //   idx 0,1,2 → row 0 (bit 0..11)
    //   idx 3,4,5 → row 1 (bit 12..23)
    //   idx 6,7,8 → row 2 (bit 24..35)

    // ========================================================================
    //  Zero-cost cell access (__forceinline, compiler turns it into bit ops)
    // ========================================================================
    static __forceinline int getCell(uint64_t b, int idx) {
        return (int)((b >> (idx * CELL_BITS)) & CELL_MASK);
    }
    static __forceinline uint64_t setCell(uint64_t b, int idx, uint64_t v) {
        uint64_t sh = (uint64_t)(idx * CELL_BITS);
        return (b & ~(CELL_MASK << sh)) | ((v & CELL_MASK) << sh);
    }
    static __forceinline uint64_t getRow(uint64_t b, int r) {
        return (b >> (r * ROW_BITS)) & ROW_MASK;
    }
    static __forceinline uint64_t setRow(uint64_t b, int r, uint64_t row) {
        uint64_t sh = (uint64_t)(r * ROW_BITS);
        return (b & ~((uint64_t)ROW_MASK << sh)) | ((row & ROW_MASK) << sh);
    }

    // ========================================================================
    //  Full-lookup moves (4096 entries, built once at program level) + full-lookup score_table
    // ========================================================================
    static inline uint32_t s_moveRowLeft[ROW_TABLE_SIZE];
    static inline int      s_scoreRowLeft[ROW_TABLE_SIZE];
    static inline float    s_scoreTable[ROW_TABLE_SIZE];   // ziap-style row scoring table

    // ===== 4x4 ziap-style 3x3-calibrated weights =====
    //   3x3 space is tight: EMPTY up a lot, MERGES slightly up, MONO slightly down (3-cell monotonicity is hard), SUM required
    static constexpr float LOST_PENALTY  = 500000.0f;
    static constexpr float EMPTY_WEIGHT  = 3000.0f;     // 4x4=270; each 3x3 cell is ~11x more precious
    static constexpr float MERGES_WEIGHT = 1200.0f;     // 4x4=700, slightly up
    static constexpr float MONO_WEIGHT   = 30.0f;       // 4x4=47; bidirectional on 3 cells is hard -> slightly down
    static constexpr float MONO_POWER    = 4.0f;        // same as 4x4
    static constexpr float SUM_WEIGHT    = 50.0f;       // 4x4=11; isolated tiles hurt 4.5x more on 3x3
    static constexpr float SUM_POWER     = 3.5f;        // same as 4x4

    // Precomputed power table (avoid repeated std::pow inside evaluate)
    struct StaticPowTables {
        float sumPow[17];   // rank 0..15 (3x3 practical limit 2^15=32768)
        float monoPow[17];
        StaticPowTables() {
            sumPow[0]  = 0.0f;
            monoPow[0] = 0.0f;
            for (int i = 1; i <= 16; ++i) {
                float fi = (float)i;
                sumPow[i]  = std::pow(2.f, std::log2(fi) * SUM_POWER);
                monoPow[i] = std::pow(2.f, std::log2(fi) * MONO_POWER);
            }
        }
    };
    static inline StaticPowTables s_pow;

    // Hand-unrolled 3-element slide-and-merge (LEFT), used only for table building
    static void computeMoveRow3(uint32_t row, uint32_t& newRow, int& scoreGain) {
        uint32_t c0 = row & CELL_MASK;
        uint32_t c1 = (row >> 4) & CELL_MASK;
        uint32_t c2 = (row >> 8) & CELL_MASK;

        uint32_t cmp[3] = {0, 0, 0};
        int w = 0;
        if (c0) cmp[w++] = c0;
        if (c1) cmp[w++] = c1;
        if (c2) cmp[w++] = c2;

        uint32_t res[3] = {0, 0, 0};
        int out = 0, i = 0;
        scoreGain = 0;
        while (i < w) {
            // Filled cell (obstacle, highest rank): never merges (treated as a huge tile)
            if (i + 1 < w && cmp[i] == cmp[i + 1] && cmp[i] < (uint32_t)CELL_MASK) {
                uint32_t merged = cmp[i] + 1;
                res[out++] = merged;
                scoreGain += (1 << (cmp[i] + 1));  // mergeTileValue: 2^(rank+1)
                i += 2;
            } else {
                res[out++] = cmp[i];
                i++;
            }
        }
        newRow = res[0] | (res[1] << 4) | (res[2] << 8);
    }

    static __forceinline uint32_t reverseRow3(uint32_t row) {
        uint32_t c0 = row & CELL_MASK;
        uint32_t c1 = (row >> 4) & CELL_MASK;
        uint32_t c2 = (row >> 8) & CELL_MASK;
        return c2 | (c1 << 4) | (c0 << 8);
    }

    // ziap style: heuristic score for a single 3-cell row (used to build score_table[4096])
    //   score = LOST_PENALTY + EMPTY*empty + MERGES*merges - MONO*min(left diff, right diff) - SUM*sum
    static float computeRowScore(uint32_t row) {
        const int r0 = (int)(row & CELL_MASK);
        const int r1 = (int)((row >> 4) & CELL_MASK);
        const int r2 = (int)((row >> 8) & CELL_MASK);

        // 1) SUM: accumulate sumPow[rank]
        float sum = s_pow.sumPow[r0] + s_pow.sumPow[r1] + s_pow.sumPow[r2];

        // 2) EMPTY
        int empty = 0;
        if (r0 == 0) empty++;
        if (r1 == 0) empty++;
        if (r2 == 0) empty++;

        // 3) MERGES: count adjacent equal pairs (3 in a row = merges += 1+2 = 3)
        int merges = 0;
        int prev = 0; int counter = 0;
        int cells[3] = {r0, r1, r2};
        for (int i = 0; i < 3; ++i) {
            const int rank = cells[i];
            if (rank == 0) continue;
            if (prev == rank) {
                counter++;
            } else if (counter > 0) {
                merges += 1 + counter;
                counter = 0;
            }
            prev = rank;
        }
        if (counter > 0) merges += 1 + counter;

        // 4) MONO: bidirectional min(left-decreasing diff, right-decreasing diff)
        //    left-decreasing diff: cells[0]>=cells[1]>=cells[2] penalty sum (any violation adds)
        //    right-decreasing diff: cells[2]>=cells[1]>=cells[0] penalty sum
        //    Take the smaller -> any row/column may be monotone in either direction; corner not forced
        float mono_left = 0.f, mono_right = 0.f;
        const float m0 = s_pow.monoPow[r0];
        const float m1 = s_pow.monoPow[r1];
        const float m2 = s_pow.monoPow[r2];
        // Adjacent 0-1
        if (r0 > r1)       mono_left  += m0 - m1;
        else if (r0 < r1)  mono_right += m1 - m0;
        // Adjacent 1-2
        if (r1 > r2)       mono_left  += m1 - m2;
        else if (r1 < r2)  mono_right += m2 - m1;

        return LOST_PENALTY
             + EMPTY_WEIGHT  * (float)empty
             + MERGES_WEIGHT * (float)merges
             - MONO_WEIGHT   * std::min(mono_left, mono_right)
             - SUM_WEIGHT    * sum;
    }

    static inline std::once_flag s_tableOnce;
    static void initTables() {
        std::call_once(s_tableOnce, []() {
            for (int i = 0; i < ROW_TABLE_SIZE; ++i) {
                uint32_t row = (uint32_t)i;
                uint32_t nr; int sg;
                computeMoveRow3(row, nr, sg);
                s_moveRowLeft[i]  = nr;
                s_scoreRowLeft[i] = sg;
                s_scoreTable[i]   = computeRowScore(row);  // row score table
            }
        });
    }

    // ========================================================================
    //  Pure mask+shift Transpose (no loops, fully unrolled)
    // ========================================================================
    static __forceinline uint64_t transpose3(uint64_t b) {
        uint64_t c0 = (b      ) & CELL_MASK;
        uint64_t c1 = (b >>  4) & CELL_MASK;
        uint64_t c2 = (b >>  8) & CELL_MASK;
        uint64_t c3 = (b >> 12) & CELL_MASK;
        uint64_t c4 = (b >> 16) & CELL_MASK;
        uint64_t c5 = (b >> 20) & CELL_MASK;
        uint64_t c6 = (b >> 24) & CELL_MASK;
        uint64_t c7 = (b >> 28) & CELL_MASK;
        uint64_t c8 = (b >> 32) & CELL_MASK;
        return (c0      )        // (0,0) → bit 0
             | (c3 <<  4)        // (1,0)→(0,1) → bit 4
             | (c6 <<  8)        // (2,0)→(0,2) → bit 8
             | (c1 << 12)        // (0,1)→(1,0) → bit 12
             | (c4 << 16)        // (1,1) → bit 16
             | (c7 << 20)        // (2,1)→(1,2) → bit 20
             | (c2 << 24)        // (0,2)→(2,0) → bit 24
             | (c5 << 28)        // (1,2)→(2,1) → bit 28
             | (c8 << 32);       // (2,2) → bit 32
    }

    // ========================================================================
    //  Full board move (3 row lookups + bit stitching, no loops)
    //  dir: 0=LEFT, 1=RIGHT, 2=UP, 3=DOWN
    // ========================================================================
    static __forceinline bool moveBoard(uint64_t in, int dir, uint64_t& out, int& gain) {
        gain = 0;
        if (dir == 0) {
            uint32_t r0 = (uint32_t)getRow(in, 0);
            uint32_t r1 = (uint32_t)getRow(in, 1);
            uint32_t r2 = (uint32_t)getRow(in, 2);
            uint32_t nr0 = s_moveRowLeft[r0];
            uint32_t nr1 = s_moveRowLeft[r1];
            uint32_t nr2 = s_moveRowLeft[r2];
            gain = s_scoreRowLeft[r0] + s_scoreRowLeft[r1] + s_scoreRowLeft[r2];
            out = ((uint64_t)nr0) | ((uint64_t)nr1 << 12) | ((uint64_t)nr2 << 24);
            return out != in;
        }
        if (dir == 1) {
            uint32_t r0 = (uint32_t)getRow(in, 0);
            uint32_t r1 = (uint32_t)getRow(in, 1);
            uint32_t r2 = (uint32_t)getRow(in, 2);
            uint32_t rev0 = reverseRow3(r0);
            uint32_t rev1 = reverseRow3(r1);
            uint32_t rev2 = reverseRow3(r2);
            uint32_t m0 = s_moveRowLeft[rev0];
            uint32_t m1 = s_moveRowLeft[rev1];
            uint32_t m2 = s_moveRowLeft[rev2];
            gain = s_scoreRowLeft[rev0] + s_scoreRowLeft[rev1] + s_scoreRowLeft[rev2];
            out = ((uint64_t)reverseRow3(m0))       |
                  ((uint64_t)reverseRow3(m1) << 12) |
                  ((uint64_t)reverseRow3(m2) << 24);
            return out != in;
        }
        if (dir == 2) {
            uint64_t t = transpose3(in);
            uint32_t r0 = (uint32_t)getRow(t, 0);
            uint32_t r1 = (uint32_t)getRow(t, 1);
            uint32_t r2 = (uint32_t)getRow(t, 2);
            uint32_t nr0 = s_moveRowLeft[r0];
            uint32_t nr1 = s_moveRowLeft[r1];
            uint32_t nr2 = s_moveRowLeft[r2];
            gain = s_scoreRowLeft[r0] + s_scoreRowLeft[r1] + s_scoreRowLeft[r2];
            uint64_t t2 = ((uint64_t)nr0) | ((uint64_t)nr1 << 12) | ((uint64_t)nr2 << 24);
            out = transpose3(t2);
            return out != in;
        }
        // DOWN
        uint64_t t = transpose3(in);
        uint32_t r0 = (uint32_t)getRow(t, 0);
        uint32_t r1 = (uint32_t)getRow(t, 1);
        uint32_t r2 = (uint32_t)getRow(t, 2);
        uint32_t rev0 = reverseRow3(r0);
        uint32_t rev1 = reverseRow3(r1);
        uint32_t rev2 = reverseRow3(r2);
        uint32_t m0 = s_moveRowLeft[rev0];
        uint32_t m1 = s_moveRowLeft[rev1];
        uint32_t m2 = s_moveRowLeft[rev2];
        gain = s_scoreRowLeft[rev0] + s_scoreRowLeft[rev1] + s_scoreRowLeft[rev2];
        uint64_t t2 = ((uint64_t)reverseRow3(m0))       |
                      ((uint64_t)reverseRow3(m1) << 12) |
                      ((uint64_t)reverseRow3(m2) << 24);
        out = transpose3(t2);
        return out != in;
    }

    // ========================================================================
    //  Formation system (ported from official ziap 4x4 Zig search.zig + Formation.zig)
    //
    //  This is the essence of 4x4 "dynamic snake protection" - the snake is not a fixed path;
    //  it automatically switches to the corner where the largest tiles cluster, and the search
    //  filters moves that would break that cluster.
    //  Official implementation (Formation.zig::get):
    //    1. MIN_RANK=9, collect tiles with rank>=9, tracking present/repeated bitmaps
    //    2. cutoff = ctz(present)+1  (lowest big-tile rank + 1, i.e. "must grow one more level")
    //    3. Two equal ranks >= cutoff (still directly mergeable) -> no formation (let evaluation merge)
    //    4. Pick the largest formation whose cells are all >= cutoff as the dynamic-snake target
    //
    //  Official search layer (search.zig):
    //    - reset(formation) is called at the root; the same formation is passed down the whole tree
    //    - maxNode: only accepts formation.intact(next_board) moves (filters formation breakers)
    //    - expectNode: spawned random tiles are not filtered, formation is only passed through
    //
    //  3x3 adaptation:
    //    - 9-cell tightness -> more compact formations (2x2 corner / corner L / corner+1 neighbor / 3-cell corner row-col)
    //    - MIN_RANK=8 (256), slightly lower than 4x4 because the 3x3 limit is 2^15
    //    - cellMask = 9 bits (uint16_t is enough)
    // ========================================================================
    struct Formation {
        uint16_t cellMask;   // 9 bits: which cells belong to the formation (bit i = cell[i])
        int      cutoff;     // minimum rank for a tile to be "safe" in the formation (>= this value)
        bool     active;

        Formation() : cellMask(0), cutoff(0), active(false) {}

        // Is the formation intact: all marked cells have rank >= cutoff
        __forceinline bool intact(uint64_t b) const {
            if (!active) return true;
            for (int i = 0; i < CELLS; ++i) {
                if (cellMask & (1u << i)) {
                    int rk = (int)((b >> (i * CELL_BITS)) & CELL_MASK);
                    if (rk < cutoff) return false;
                }
            }
            return true;
        }

        // Formation value: sum of marked-cell ranks (used to pick the best among candidates)
        __forceinline double value(uint64_t b) const {
            if (!active) return 0;
            double v = 0;
            for (int i = 0; i < CELLS; ++i) {
                if (cellMask & (1u << i)) {
                    int rk = (int)((b >> (i * CELL_BITS)) & CELL_MASK);
                    if (rk > 0) v += (double)rk;
                }
            }
            return v;
        }
    };

    // 3x3 formation patterns (cellMask 9 bits, bit i = cell[i])
    //   Board layout:  0 1 2
    //                 3 4 5
    //                 6 7 8
    // === 2x2 corner blocks (4) ===
    static constexpr uint16_t FORM_2X2_TL = 0x01B; // top-left (0,1,3,4)
    static constexpr uint16_t FORM_2X2_TR = 0x072; // top-right (1,2,4,5)
    static constexpr uint16_t FORM_2X2_BL = 0x1D8; // bottom-left (3,4,6,7)
    static constexpr uint16_t FORM_2X2_BR = 0x1D0; // bottom-right (4,5,7,8)
    // === corner L: corner + two neighbors (4) ===
    static constexpr uint16_t FORM_L_TL = 0x00B; // top-left (0,1,3)
    static constexpr uint16_t FORM_L_TR = 0x02E; // top-right (1,2,5)
    static constexpr uint16_t FORM_L_BL = 0x188; // bottom-left (3,6,7)
    static constexpr uint16_t FORM_L_BR = 0x160; // bottom-right (5,7,8)
    // === corner + 1 neighbor (along a row or column, 8) ===
    static constexpr uint16_t FORM_CNR_TL_R = 0x003; // top-left + right (0,1)
    static constexpr uint16_t FORM_CNR_TL_D = 0x009; // top-left + down (0,3)
    static constexpr uint16_t FORM_CNR_TR_L = 0x006; // top-right + left (1,2)
    static constexpr uint16_t FORM_CNR_TR_D = 0x024; // top-right + down (2,5)
    static constexpr uint16_t FORM_CNR_BL_R = 0x0C0; // bottom-left + right (6,7)
    static constexpr uint16_t FORM_CNR_BL_U = 0x048; // bottom-left + up (3,6)
    static constexpr uint16_t FORM_CNR_BR_L = 0x180; // bottom-right + left (7,8)
    static constexpr uint16_t FORM_CNR_BR_U = 0x120; // bottom-right + up (5,8)
    // === 3-cell corner row/column (4) ===
    static constexpr uint16_t FORM_ROW_TOP = 0x007; // top row (0,1,2)
    static constexpr uint16_t FORM_COL_LEFT = 0x049; // left column (0,3,6)
    static constexpr uint16_t FORM_ROW_BOT = 0x1C0; // bottom row (6,7,8)
    static constexpr uint16_t FORM_COL_RIGHT = 0x124; // right column (2,5,8)

    static constexpr uint16_t FORMATION_PATTERNS[] = {
        // Priority high to low: 2x2 corner block > corner L > 3-cell corner row/col > corner+1 neighbor
        FORM_2X2_TL, FORM_2X2_TR, FORM_2X2_BL, FORM_2X2_BR,
        FORM_L_TL, FORM_L_TR, FORM_L_BL, FORM_L_BR,
        FORM_ROW_TOP, FORM_COL_LEFT, FORM_ROW_BOT, FORM_COL_RIGHT,
        FORM_CNR_TL_R, FORM_CNR_TL_D, FORM_CNR_TR_L, FORM_CNR_TR_D,
        FORM_CNR_BL_R, FORM_CNR_BL_U, FORM_CNR_BR_L, FORM_CNR_BR_U,
    };
    static constexpr int FORMATION_COUNT = 20;

    // 3x3: rank>=8 (256) activates formation protection (4x4=9; 3x3 limit is lower -> slightly lower)
    static constexpr int FORMATION_MIN_RANK = 8;

    // Detect the current board's dynamic-snake formation (ported from 4x4 Formation.zig::get)
    Formation detectFormation(uint64_t b) const {
        // 1) Collect tiles with rank>=MIN_RANK, tracking present/repeated bitmaps
        uint16_t present = 0;
        uint16_t repeated = 0;
        for (int i = 0; i < CELLS; ++i) {
            int rk = (int)((b >> (i * CELL_BITS)) & CELL_MASK);
            if (rk >= FORMATION_MIN_RANK) {
                uint16_t bit = (uint16_t)(1u << rk);
                repeated |= present & bit;   // this rank already appeared -> mark repeated
                present  |= bit;
            }
        }
        if (present == 0) return Formation();

        // 2) cutoff = ctz(present) + 1  (lowest big-tile rank + 1)
        //    Official logic: formation tiles must "grow one more level", avoiding protection of immediately mergeable pairs
        int ctz = 0;
        uint16_t p = present;
        while ((p & 1) == 0 && ctz < 16) { p >>= 1; ctz++; }
        int cutoff = ctz + 1;
        if (cutoff > 15) cutoff = 15;

        // 3) Two equal ranks >= cutoff (still directly mergeable) -> no formation
        //    Official: `if (repeated >> cutoff != 0) return .none;`
        if ((repeated >> cutoff) != 0) return Formation();

        // 4) Pick the largest formation whose cells are all >= cutoff
        Formation best;
        double bestValue = 0;
        for (int pat = 0; pat < FORMATION_COUNT; ++pat) {
            uint16_t pattern = FORMATION_PATTERNS[pat];
            // Check all formation cells are >= cutoff
            bool allMeet = true;
            double val = 0;
            for (int i = 0; i < CELLS; ++i) {
                if (pattern & (1u << i)) {
                    int rk = (int)((b >> (i * CELL_BITS)) & CELL_MASK);
                    if (rk < cutoff) { allMeet = false; break; }
                    if (rk > 0) val += (double)rk;
                }
            }
            if (!allMeet) continue;
            if (val > bestValue) {
                bestValue = val;
                best.cellMask = pattern;
                best.cutoff = cutoff;
                best.active = true;
            }
        }
        return best;
    }

    // ========================================================================
    //  Evaluation (ziap style: 3 rows + 3 columns from score_table lookups)
    //  - 3 row lookups + 1 transpose + 3 column lookups = 7 array accesses (O(1))
    //  - No loops, no division, no std::pow calls
    //  - Structure protection is done by the Formation system at search time; evaluation stays a pure Ziap formula
    // ========================================================================
    __forceinline float evaluate(uint64_t b) const {
        // 3 row lookups
        uint32_t r0 = (uint32_t)getRow(b, 0);
        uint32_t r1 = (uint32_t)getRow(b, 1);
        uint32_t r2 = (uint32_t)getRow(b, 2);
        float score = s_scoreTable[r0] + s_scoreTable[r1] + s_scoreTable[r2];
        // 3 column lookups (transpose turns columns into rows)
        uint64_t t = transpose3(b);
        uint32_t c0 = (uint32_t)getRow(t, 0);
        uint32_t c1 = (uint32_t)getRow(t, 1);
        uint32_t c2 = (uint32_t)getRow(t, 2);
        score += s_scoreTable[c0] + s_scoreTable[c1] + s_scoreTable[c2];
        return score;
    }

    // ========================================================================
    //  Zobrist hashing (9 cells x 16 values)
    // ========================================================================
    static inline uint64_t s_zobrist[CELLS][16];
    static inline std::once_flag s_zobristOnce;

    static void initZobrist() {
        std::call_once(s_zobristOnce, []() {
            std::mt19937_64 rng(0x2048330ULL);
            for (int i = 0; i < CELLS; ++i)
                for (int v = 0; v < 16; ++v)
                    s_zobrist[i][v] = rng();
        });
    }
    static __forceinline uint64_t zobristHash(uint64_t b) {
        uint64_t h = 0;
        // Unrolled 9 times (no loop, compiler inlines)
        for (int i = 0; i < CELLS; ++i) {
            int v = (int)((b >> (i * CELL_BITS)) & CELL_MASK);
            if (v) h ^= s_zobrist[i][v];
        }
        return h;
    }

    // ========================================================================
    //  Transposition table (2^22 x 16B = 64MB per instance; thread_local single-threaded access -> lock-free)
    // ========================================================================
    static constexpr int TT_BITS = 22;
    static constexpr size_t TT_SIZE = (size_t)1 << TT_BITS;
    static constexpr size_t TT_MASK = TT_SIZE - 1;
    struct TTEntry {
        uint64_t hash;    // 8 bytes (Zobrist, double-check)
        float    score;   // 4
        uint8_t  depth;   // 1
        uint8_t  flag;    // 1 byte (0=empty, 1=valid)
        uint16_t pad;     // 2
    };                    // 16 bytes
    std::unique_ptr<TTEntry[]> m_tt;

    void ensureTT() {
        if (!m_tt) {
            m_tt = std::make_unique<TTEntry[]>(TT_SIZE);
            std::memset(m_tt.get(), 0, TT_SIZE * sizeof(TTEntry));
        }
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
    //   -> new games hit stale expected scores -> AI degrades
    __forceinline void ttPut(uint64_t hash, int depth, float score) {
        TTEntry& e = m_tt[hash & TT_MASK];
        e.hash = hash;
        e.score = score;
        e.depth = (uint8_t)depth;
        e.flag = 1;
    }

    // ========================================================================
    //  Iterative-deepening Expectimax
    //    3x3 has a small branching factor: MAX_DEPTH=12, full chance-layer evaluation, 90ms budget (~10 moves/s target)
    //   Aligned with official ziap: a NODE_BUDGET is added so CPU contention cannot destabilize depth
    //      BFS adaptive depth tiers (empty count + max rank heuristic); don't always run 1..12 wasting compute
    // ========================================================================
    static constexpr int    MAX_DEPTH      = 12;
    static constexpr int    MIN_DEPTH      = 3;
    static constexpr int    MAX_SAMPLE     = 9;     // 3x3 has at most 9 empty cells; full evaluation, no pruning
    static constexpr double TIME_BUDGET_MS = 90.0;  // 90ms (same budget as 4x4/5x5)
    static constexpr uint64_t NODE_BUDGET  = 1ULL << 21;  // ~2.09M nodes (3x3 is very light, be generous)
    static constexpr float  NEG_INF        = -1e30f;

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_deadline;
    bool      m_timedOut = false;
    bool      m_nodeBudgetExceeded = false;
    uint64_t  m_nodeCount = 0;

    __forceinline bool realCheckTimeout() {
        m_nodeCount++;
        if ((m_nodeCount & 0x3FF) != 0) return false;   // check every 1024 nodes
        if (m_nodeCount >= NODE_BUDGET) { m_nodeBudgetExceeded = true; return true; }
        if (Clock::now() >= m_deadline)  { m_timedOut = true; return true; }
        return false;
    }

    // Max layer: player picks the best direction + Formation filter
    //   Official search.zig::maxNode: only accepts formation.intact(next_board) moves
    //   Chance-layer random tiles are not filtered; formation is passed through (same as official)
    __forceinline float searchMax(uint64_t b, int depth, uint64_t hash,
                                  const Formation& formation) {
        if (depth == 0) return evaluate(b);
        float cached;
        if (ttGet(hash, depth, cached)) return cached;
        if (realCheckTimeout()) { m_timedOut = true; return evaluate(b); }

        float best = NEG_INF;
        for (int d = 0; d < 4; ++d) {
            uint64_t nb; int gain;
            if (!moveBoard(b, d, nb, gain)) continue;
            // Formation protection: filter moves that break the formation (core of dynamic-snake protection)
            if (formation.active && !formation.intact(nb)) continue;
            uint64_t h = zobristHash(nb);
            float cv = searchChance(nb, depth - 1, h, formation);
            float val = (float)gain + cv;
            if (val > best) best = val;
            if (m_timedOut || m_nodeBudgetExceeded) break;
        }
        if (best <= NEG_INF) {
            best = evaluate(b) - LOST_PENALTY;
        }
        ttPut(hash, depth, best);
        return best;
    }

    // Chance layer: evaluate all empty cells (3x3 has at most 9, affordable) + formation pass-through
    //   Depth-adaptive: depth<=4 spawns both 2 and 4 (full probability), depth>=5 spawns only 2 (fast)
    __forceinline float searchChance(uint64_t b, int depth, uint64_t hash,
                                     const Formation& formation) {
        if (depth == 0) return evaluate(b);
        float cached;
        if (ttGet(hash, depth, cached)) return cached;
        if (realCheckTimeout()) { m_timedOut = true; return evaluate(b); }

        // Collect empty cells (at most 9 on 3x3)
        int empties[CELLS];
        int ne = 0;
        for (int i = 0; i < CELLS; ++i) {
            if (((b >> (i * CELL_BITS)) & CELL_MASK) == 0) empties[ne++] = i;
        }
        if (ne == 0) {
            // Board full: check whether moves remain (deadlock is judged by the searchMax above)
            return searchMax(b, depth, hash, formation);
        }

        // 3x3 has few empty cells, evaluate all (no strategic pruning; <=9 cells, cheap to run fully)
        int K = (ne < MAX_SAMPLE) ? ne : MAX_SAMPLE;  // ne <= 9 in practice

        // Depth-adaptive: depth<=4 double value (0.9/0.1), depth>=5 single value (2 only)
        bool fullChance = (depth <= 4);

        double sum = 0.0;
        for (int s = 0; s < K; ++s) {
            int idx = empties[s];
            uint64_t b2 = setCell(b, idx, 1);
            uint64_t h2 = hash ^ s_zobrist[idx][1];
            double v2 = searchMax(b2, depth - 1, h2, formation);
            if (fullChance) {
                uint64_t b4 = setCell(b, idx, 2);
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

    // ========================================================================
    //  Encode/decode: BoardMatrix (int[3][3] face values) <-> uint64_t (rank representation)
    // ========================================================================
    static uint64_t encode(const BoardMatrix& m) {
        uint64_t b = 0;
        const int cm = (int)CELL_MASK;
        for (int r = 0; r < N; ++r) {
            for (int c = 0; c < N; ++c) {
                int v = m[r][c];
                int logVal = 0;
                if (v < 0 || v >= (1 << 28)) {
                    // Filled cell (obstacle/huge tile): encoded as the highest rank.
                    // moveRowLeft already guards with cmp[i] < CELL_MASK -> the highest rank never merges;
                    // expectimax treats it as a huge tile in evaluation and move planning.
                    logVal = CELL_MASK;
                } else if (v > 0) {
                    while (v > 1) { v >>= 1; ++logVal; }
                    if (logVal > cm) logVal = cm;
                }
                b = setCell(b, r * N + c, (uint64_t)logVal);
            }
        }
        return b;
    }

public:
    Solver3x3Fast() {
        initTables();
        initZobrist();
        ensureTT();
    }
    ~Solver3x3Fast() = default;
    Solver3x3Fast(const Solver3x3Fast&) = delete;
    Solver3x3Fast& operator=(const Solver3x3Fast&) = delete;

    // ===== Main interface: 4-direction scores (iterative-deepening Expectimax) =====
    std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
        initTables();
        initZobrist();
        ensureTT();
        std::vector<MoveScore> result;
        result.reserve(4);

        uint64_t b = encode(board);

        // Detect the current board's Formation (dynamic-snake protection, ported from official 4x4 search.zig::reset)
        Formation formation = detectFormation(b);

        // First compute legality and immediate gain for the 4 directions
        uint64_t after[4];
        int      gain[4] = {0, 0, 0, 0};
        bool     moved[4] = {false, false, false, false};
        for (int d = 0; d < 4; ++d) {
            moved[d] = moveBoard(b, d, after[d], gain[d]);
            // Formation protection: root also filters directions that break the formation (same as official)
            if (moved[d] && formation.active && !formation.intact(after[d])) {
                moved[d] = false;
            }
        }

        // BFS adaptive depth (official ziap 3x3 idea: empty count + max rank heuristic)
        //   3x3 9 cells: >=7 empty -> early, shallow is enough; 4-6 -> mid; 1-3 -> late deep search; 0 -> deepest endgame
        int emptyCount = 0;
        int maxRank = 0;
        for (int i = 0; i < CELLS; ++i) {
            int rk = (int)((b >> (i * CELL_BITS)) & CELL_MASK);
            if (rk == 0) emptyCount++;
            else if (rk > maxRank) maxRank = rk;
        }
        int bfsDepth;
        if      (emptyCount >= 7) bfsDepth = 2;
        else if (emptyCount >= 4) bfsDepth = 5;
        else if (emptyCount >= 1) bfsDepth = 9;
        else                      bfsDepth = 11;
        // Endgame bonus: rank >= 2^10 (1024) -> search 1 more ply (3x3 limit is 2^15, 1024 is already big)
        if (maxRank >= 10) bfsDepth = std::min(bfsDepth + 1, 11);
        int targetDepth = std::min(bfsDepth + 1, MAX_DEPTH);
        if (targetDepth < MIN_DEPTH) targetDepth = MIN_DEPTH;

        m_deadline = Clock::now() + std::chrono::milliseconds((int)TIME_BUDGET_MS);
        m_timedOut = false;
        m_nodeBudgetExceeded = false;
        m_nodeCount = 0;

        float cur[4] = { NEG_INF, NEG_INF, NEG_INF, NEG_INF };
        bool  haveResult = false;

        // Iterative deepening: from MIN_DEPTH to targetDepth; timeout/budget returns the last complete depth
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
                    v = searchChance(after[d], depth - 1, h, formation);
                    if (m_timedOut || m_nodeBudgetExceeded) { depthComplete = false; break; }
                    ttPut(h, depth - 1, v);
                }
                trial[d] = (float)gain[d] + v;
            }

            if (!depthComplete) break;
            for (int d = 0; d < 4; ++d) cur[d] = trial[d];
            haveResult = true;
            if (m_timedOut || m_nodeBudgetExceeded) break;
        }

        if (!haveResult) {
            // extreme fallback: depth=0 evaluation
            for (int d = 0; d < 4; ++d) {
                cur[d] = moved[d] ? evaluate(after[d]) : NEG_INF;
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
        initTables();
        initZobrist();
        ensureTT();
        return evaluate(encode(board));
    }

    bool hasMergeableMove(const BoardMatrix& board) {
        initTables();
        initZobrist();
        uint64_t b = encode(board);
        for (int d = 0; d < 4; ++d) {
            uint64_t nb;
            int gain = 0;
            if (moveBoard(b, d, nb, gain) && gain > 0) return true;
        }
        return false;
    }

    std::string getStatsString() const {
        return "AI[3x3 ziap-style row/col-score-table ID-Expectimax(depth<=12) "
               "Formation(20-patterns,MIN_RANK=8) TT(2^22=64MB)]";
    }
};

#endif // SOLVER_3X3_FAST_H
