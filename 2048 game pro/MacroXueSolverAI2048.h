#ifndef MACROXUE_SOLVER_AI_2048_H
#define MACROXUE_SOLVER_AI_2048_H

// ============================================================================
// MacroXueSolverAI2048 - ported from the open-source project macroxue/2048-ai (GPL-3.0)
//   Source: https://github.com/macroxue/2048-ai
// Ported scope: search core (Expectimax + score_map heuristic + probability pruning + Cache)
//           + BlockPlan endgame planner (plan.h port, no big tables, preferred when 4x4 big tiles cluster)
//             (Tuple10/Tuple11 big lookup tables are NOT included: they need 5~27GB RAM, not feasible on desktop)
//   Generalized: template N in {3,4,5}, three intelligence tiers = search depth {3,5,8} (2022 parameters)
//
//
//   Implementation notes:
//   - Internally the board is stored as rank (log2(value)), int b[N][N], 0=empty
//   - Moves: N<=4 row lookup (N*5 bits) for speed; N=5 hand-written 5-element slide-and-merge
//   - Evaluation: macroxue score_map heuristic (same formula for rows/columns); N<=4 lookup, N=5 hand-written O(N)
//   - Search: iterative deepening + probability pruning (min_prob = 1/2^(depth+4)) + 128-bit key Cache
//   - Timeout: background threads can set a time budget; returns the current best on timeout (prevents 5x5 depth-8 lag)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "MacroXuePlan.h"   // BlockPlan endgame planner (port of the official plan.h, 4x4)

// Search-timeout interrupt signal (thrown when a fixed-depth search exceeds the budget; caught by getMoveScores)
struct MxTimeout {};

// ==================== Generic Cache (port of macroxue cache.h, 64-bit Zobrist key) ====================
template <int N>
class MxCache {
public:
    struct Entry {
        uint64_t key;
        float prob;
        int  depth;
        double score;
    };
    static constexpr int kBits = 20;               // 1M entries (~24MB per engine, high hit rate)
    static constexpr int kSize = 1 << kBits;       // 1048576

    MxCache() { std::memset(m_entries, 0, sizeof(m_entries)); }

    void Clear() { std::unique_lock<std::shared_mutex> lk(m_mtx); std::memset(m_entries, 0, sizeof(m_entries)); }

    static uint64_t hashKey(uint64_t key) {
        return key + (key >> 22) + (key >> 44);
    }

    bool Lookup(uint64_t key, float prob, int depth, double* score) const {
        std::shared_lock<std::shared_mutex> lk(m_mtx);
        const Entry& e = m_entries[hashKey(key) & (kSize - 1)];
        if (e.key == key && e.prob >= prob && e.depth >= depth) {
            *score = e.score;
            return true;
        }
        return false;
    }

    void Update(uint64_t key, float prob, int depth, double score) {
        std::unique_lock<std::shared_mutex> lk(m_mtx);
        Entry& e = m_entries[hashKey(key) & (kSize - 1)];
        e.key = key;
        e.prob = prob;
        e.depth = depth;
        e.score = score;
    }

private:
    mutable std::shared_mutex m_mtx;
    Entry m_entries[kSize];
};

// ==================== Template engine (N in {3,4,5}) ====================
template <int N>
class MacroXueEngine {
public:
    using BoardMatrix = std::vector<std::vector<int>>;

    static constexpr int kRowBits = N * 5;                     // N cells x 5 bits per row
    static constexpr int kRowTableSize = (N <= 4) ? (1 << kRowBits) : 0;
    static constexpr int kTableSize = (N <= 4) ? (1 << kRowBits) : 1;  // 3×3:2^15 4×4:2^20 5×5:1

    struct MoveResult { int dir; float score; bool allowed; bool safe; };

    MacroXueEngine() {
        buildTables();
        m_cache = new MxCache<N>();
        m_maxDepth = 8;
        applyMinProb(8);
        m_timeBudgetMs = 0;
    }
    ~MacroXueEngine() { delete m_cache; }

    // ===== Configuration =====
    void setDepth(int depth) {
        if (depth < 1) depth = 1;
        if (depth > 24) depth = 24;
        m_maxDepth = depth;
        applyMinProb(depth);
    }
    int  getDepth() const { return m_maxDepth; }

private:
    // Probability-pruning threshold: fully aligned with official macroxue (min_prob = 1/2^(depth+4))
    //   Earlier extra-aggressive pruning (depth+2/+3) for speed hurt strength - user reported lower scores than ziap
    //   Official coefficients restored; the time budget (timeout interrupt) now controls per-move time, keeping search thorough
    void applyMinProb(int depth) {
        m_minProb = 1.0 / (1 << (depth + 4));
    }

public:
    // Time budget (ms per move), 0 = unlimited; interrupts fixed-depth search on timeout
    void setTimeBudgetMs(int ms) { m_timeBudgetMs = ms; }

    // Clear the transposition table (aligned with official macroxue: clear at each game start to avoid cross-game degradation)
    void clearCache() { m_cache->Clear(); }

    // ===== Main interface =====
    std::vector<MoveResult> getMoveScores(const BoardMatrix& board) {
        std::vector<MoveResult> out;
        out.reserve(4);
        Node n;
        n.load(board);
        n.min_prob = m_minProb;
        n.cache = m_cache;
        // Correct pruning threshold (macroxue Search logic): pass_score varies with the position, pruning low-score subtrees
        double maxTileScore = (double)n.MaxRank() * (double)(1LL << n.MaxRank());
        double ev0 = n.Evaluate();
        n.pass_score = (ev0 < 0) ? -std::numeric_limits<double>::max() : maxTileScore * 2;
        n.game_over_score = -std::max((double)(1 << 17), maxTileScore * 2);
        if (!n.anyMove()) {
            for (int d = 0; d < 4; ++d) out.push_back({ d, (float)n.game_over_score, false, false });
            return out;
        }

        // Fixed-depth search + fine-grained timeout interrupt (following the original engine's BFS depth-allocation idea):
        //   Average positions finish the full maxDepth search within budget; hard positions stop when the budget ends, stable per-move time
        m_clockStart = std::chrono::steady_clock::now();
        n.clockRef = m_clockStart;
        n.budgetMs = m_timeBudgetMs;
        n.stopFlag = &m_stop;
        n.nodeCounter = &m_nodes;
        m_stop.store(false, std::memory_order_relaxed);
        m_nodes.store(0, std::memory_order_relaxed);
        double scores[4] = { 0, 0, 0, 0 };
        bool ok[4] = { false, false, false, false };
        if constexpr (N == 5) {
            n.fam = detectMultiFormation(n.b);   // top-level detection of 4-corner (+4-edge) formation protection
            // 5x5 iterative deepening + BFS adaptive depth (ported from Solver5x5Fast):
            //   target depth estimated from empty count + max rank, deepening from MIN layer by layer;
            //   budget interrupt keeps the last complete layer; min_prob tracks the target depth dynamically, tightening budget to the main line
            int emptyCount = 0, maxRank = 0;
            for (int i = 0; i < N * N; ++i) {
                int r = n.b[i % N][i / N];
                if (r == 0) ++emptyCount;
                else if (r > maxRank) maxRank = r;
            }
            int bfsDepth;
            if      (emptyCount >= 14) bfsDepth = 2;
            else if (emptyCount >= 10) bfsDepth = 4;
            else if (emptyCount >= 7)  bfsDepth = 6;
            else if (emptyCount >= 4)  bfsDepth = 8;
            else if (emptyCount >= 1)  bfsDepth = 10;
            else                       bfsDepth = 11;
            if (maxRank >= 13) bfsDepth = std::min(bfsDepth + 1, 11);   // endgame big tiles: search one ply deeper
            const int MIN_DEPTH = 3;
            int targetDepth = std::min(bfsDepth + 1, m_maxDepth);
            if (targetDepth < MIN_DEPTH) targetDepth = MIN_DEPTH;

            m_minProb = 1.0 / (1 << (targetDepth + 4));   // probability-pruning threshold follows the target depth dynamically
            n.min_prob = m_minProb;

            double cur[4] = { 0, 0, 0, 0 };
            bool curOk[4] = { false, false, false, false };
            bool haveResult = false;
            for (int depth = MIN_DEPTH; depth <= targetDepth; ++depth) {
                double trial[4] = { 0, 0, 0, 0 };
                bool trialOk[4] = { false, false, false, false };
                bool depthComplete = true;
                for (int i = 0; i < 4; ++i) {
                    Node m = n;
                    if (!m.Move(i)) { trial[i] = n.game_over_score; continue; }
                    if (m.fam.anyActive && !MacroXueEngine<N>::multiIntact(m.fam, m.b)) {
                        trial[i] = n.game_over_score;   // formation protection: top level also filters formation-breaking directions
                        continue;
                    }
                    trial[i] = m.TryAllTiles(depth - 1, 1.0f);
                    trialOk[i] = true;
                    if (m_stop.load(std::memory_order_relaxed)) { depthComplete = false; break; }
                }
                if (!depthComplete) break;                          // timeout/budget -> keep the previous layer
                for (int i = 0; i < 4; ++i) { cur[i] = trial[i]; curOk[i] = trialOk[i]; }
                haveResult = true;
                if (m_stop.load(std::memory_order_relaxed)) break;  // layer complete but budget exhausted
            }
            if (!haveResult) {                                      // fallback: at least complete shallow layer 1
                for (int i = 0; i < 4; ++i) {
                    Node m = n;
                    if (m.Move(i)) { cur[i] = m.TryAllTiles(0, 1.0f); curOk[i] = true; }
                    else cur[i] = n.game_over_score;
                }
            }
            for (int i = 0; i < 4; ++i) { scores[i] = cur[i]; ok[i] = curOk[i]; }
        } else {
            if constexpr (N == 4) n.fam = detectMultiFormation(n.b);   // 4x4 classic: ziap formation discipline
            auto runOne = [&](int i) {
                Node m = n;
                if (m.Move(i)) {
                    scores[i] = m.TryAllTiles(m_maxDepth - 1, 1.0f);
                    ok[i] = true;   // use partial results when the time budget (m_stop) is hit
                } else {
                    scores[i] = n.game_over_score;
                }
            };
            const int hw = (int)std::thread::hardware_concurrency();
            // Parallel only for "heavy search": 3x3 is very fast / shallow layers are fast -> serial to avoid thread overhead
            if (hw >= 2 && N != 3 && m_maxDepth >= 5) {
                std::atomic<int> nextDir{ 0 };
                const int nt = std::min(4, hw);
                std::vector<std::thread> ths;
                ths.reserve(nt);
                auto worker = [&]() {
                    while (true) {
                        int i = nextDir.fetch_add(1);
                        if (i >= 4) break;
                        runOne(i);
                    }
                };
                for (int t = 0; t < nt; ++t) ths.emplace_back(worker);
                for (auto& th : ths) th.join();
            } else {
                for (int i = 0; i < 4; ++i) runOne(i);
            }
        }
        // BlockPlan endgame planner (official no-table mode: plan suggestion wins over search when prob>0)
        //   4x4 only: when big tiles already cluster (applicable), exhaustively plan exact merge paths with Expectimax, boosting endgame strength
        if constexpr (N == 4) {
            int b4[4][4];
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) b4[x][y] = n.b[x][y];
            auto sug = m_plan4.SuggestMove(b4);
            if (sug.prob > 0) {
                static const int planToEngine[4] = {2, 0, 1, 3};  // plan(up,left,right,down) -> engine(left,right,up,down)
                int edir = planToEngine[sug.move];
                double bestV = -1e30;
                for (int i = 0; i < 4; ++i) if (ok[i] && scores[i] > bestV) bestV = scores[i];
                if (!ok[edir]) { scores[edir] = 0; ok[edir] = true; }
                scores[edir] = bestV + 1e9;   // force the plan direction to the highest score -> AI picks it
            }
        }
        for (int i = 0; i < 4; ++i)
            out.push_back({ i, (float)scores[i], ok[i], ok[i] });
        std::sort(out.begin(), out.end(),
            [](const MoveResult& a, const MoveResult& b) {
                if (a.allowed != b.allowed) return a.allowed;
                return a.score > b.score;
            });
        return out;
    }

    float evaluate(const BoardMatrix& board) {
        Node n;
        n.load(board);
        return (float)n.Evaluate();
    }

    std::string stats() const {
        return "MacroXue/2048-ai N=" + std::to_string(N) + "x" + std::to_string(N) +
               " depth=" + std::to_string(m_maxDepth) + " cache=2^" + std::to_string(MxCache<N>::kBits);
    }

private:
    // ===== 5x5 formation protection (ported from Solver5x5Fast: protect formed big-snake/corner formations from being broken) =====
    struct Formation {
        uint32_t cellMask = 0;   // bit i = cell[i/N][i%N]
        int      cutoff = 0;     // minimum rank for a formation tile (>= this value)
        bool     active = false;
        Formation() : cellMask(0), cutoff(0), active(false) {}
    };
    struct MultiFormation {
        Formation corners[4];    // 0=TL 1=TR 2=BL 3=BR (corner formations)
        Formation edges[4];      // 0=top 1=bottom 2=left 3=right (edge-line formations)
        bool anyActive = false;
    };
    // 2x2 corner block (4)
    static constexpr uint32_t FORM_2X2_TL = (1u<<0)|(1u<<1)|(1u<<5)|(1u<<6);
    static constexpr uint32_t FORM_2X2_TR = (1u<<3)|(1u<<4)|(1u<<8)|(1u<<9);
    static constexpr uint32_t FORM_2X2_BL = (1u<<15)|(1u<<16)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_2X2_BR = (1u<<18)|(1u<<19)|(1u<<23)|(1u<<24);
    // 2x2 corner block + 1 extension (8)
    static constexpr uint32_t FORM_2X2_TL_R = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<5)|(1u<<6);
    static constexpr uint32_t FORM_2X2_TL_D = (1u<<0)|(1u<<1)|(1u<<5)|(1u<<6)|(1u<<10);
    static constexpr uint32_t FORM_2X2_TR_L = (1u<<2)|(1u<<3)|(1u<<4)|(1u<<8)|(1u<<9);
    static constexpr uint32_t FORM_2X2_TR_D = (1u<<3)|(1u<<4)|(1u<<8)|(1u<<9)|(1u<<13);
    static constexpr uint32_t FORM_2X2_BL_R = (1u<<15)|(1u<<16)|(1u<<17)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_2X2_BL_U = (1u<<10)|(1u<<15)|(1u<<16)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_2X2_BR_L = (1u<<17)|(1u<<18)|(1u<<19)|(1u<<23)|(1u<<24);
    static constexpr uint32_t FORM_2X2_BR_U = (1u<<13)|(1u<<18)|(1u<<19)|(1u<<23)|(1u<<24);
    // Corner L (4)
    static constexpr uint32_t FORM_L_TL = (1u<<0)|(1u<<1)|(1u<<5);
    static constexpr uint32_t FORM_L_TR = (1u<<3)|(1u<<4)|(1u<<9);
    static constexpr uint32_t FORM_L_BL = (1u<<15)|(1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_L_BR = (1u<<19)|(1u<<23)|(1u<<24);
    // Corner + 1 neighbor (8)
    static constexpr uint32_t FORM_C_TL_R = (1u<<0)|(1u<<1);
    static constexpr uint32_t FORM_C_TL_D = (1u<<0)|(1u<<5);
    static constexpr uint32_t FORM_C_TR_L = (1u<<3)|(1u<<4);
    static constexpr uint32_t FORM_C_TR_D = (1u<<4)|(1u<<9);
    static constexpr uint32_t FORM_C_BL_R = (1u<<20)|(1u<<21);
    static constexpr uint32_t FORM_C_BL_U = (1u<<15)|(1u<<20);
    static constexpr uint32_t FORM_C_BR_L = (1u<<23)|(1u<<24);
    static constexpr uint32_t FORM_C_BR_U = (1u<<19)|(1u<<24);
    static constexpr uint32_t FORMATION_PATTERNS[] = {
        // 3x3 corner block (4 patterns, 9 cells) -- 5x5 extension: larger corner formations
        (1u<<0)|(1u<<1)|(1u<<2)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<10)|(1u<<11)|(1u<<12),  // TL 3x3
        (1u<<2)|(1u<<3)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<12)|(1u<<13)|(1u<<14),  // TR 3x3
        (1u<<10)|(1u<<11)|(1u<<12)|(1u<<15)|(1u<<16)|(1u<<17)|(1u<<20)|(1u<<21)|(1u<<22),  // BL 3x3
        (1u<<12)|(1u<<13)|(1u<<14)|(1u<<17)|(1u<<18)|(1u<<19)|(1u<<22)|(1u<<23)|(1u<<24),  // BR 3x3
        // 2x2 corner block + 1 extension (8)
        FORM_2X2_TL_R, FORM_2X2_TL_D, FORM_2X2_TR_L, FORM_2X2_TR_D,
        FORM_2X2_BL_R, FORM_2X2_BL_U, FORM_2X2_BR_L, FORM_2X2_BR_U,
        // 2x2 corner block (4)
        FORM_2X2_TL, FORM_2X2_TR, FORM_2X2_BL, FORM_2X2_BR,
        // Corner L (4)
        FORM_L_TL, FORM_L_TR, FORM_L_BL, FORM_L_BR,
        // Corner + 1 neighbor (8)
        FORM_C_TL_R, FORM_C_TL_D, FORM_C_TR_L, FORM_C_TR_D,
        FORM_C_BL_R, FORM_C_BL_U, FORM_C_BR_L, FORM_C_BR_U,
        // 2x3 corner rectangle (8 patterns, 6 cells) -- 5x5 extension
        (1u<<0)|(1u<<1)|(1u<<2)|(1u<<5)|(1u<<6)|(1u<<7),   // TL 2x3 horizontal
        (1u<<2)|(1u<<3)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<9),   // TR 2x3 horizontal
        (1u<<15)|(1u<<16)|(1u<<17)|(1u<<20)|(1u<<21)|(1u<<22), // BL 2x3 horizontal
        (1u<<17)|(1u<<18)|(1u<<19)|(1u<<22)|(1u<<23)|(1u<<24), // BR 2x3 horizontal
        (1u<<0)|(1u<<1)|(1u<<5)|(1u<<6)|(1u<<10)|(1u<<11),  // TL 2x3 vertical
        (1u<<3)|(1u<<4)|(1u<<8)|(1u<<9)|(1u<<13)|(1u<<14),  // TR 2x3 vertical
        (1u<<10)|(1u<<11)|(1u<<15)|(1u<<16)|(1u<<20)|(1u<<21), // BL 2x3 vertical
        (1u<<13)|(1u<<14)|(1u<<18)|(1u<<19)|(1u<<23)|(1u<<24), // BR 2x3 vertical
        // 2x2 corner block + bidirectional extension (4 patterns, 6 cells) -- 5x5 extension
        (1u<<0)|(1u<<1)|(1u<<2)|(1u<<5)|(1u<<6)|(1u<<10),  // TL: 2x2 + right1 + down1
        (1u<<2)|(1u<<3)|(1u<<4)|(1u<<8)|(1u<<9)|(1u<<13),  // TR: 2x2 + left1 + down1
        (1u<<10)|(1u<<15)|(1u<<16)|(1u<<17)|(1u<<20)|(1u<<21), // BL: 2x2 + right1 + up1
        (1u<<13)|(1u<<17)|(1u<<18)|(1u<<19)|(1u<<23)|(1u<<24), // BR: 2x2 + left1 + up1
        // Edge mid-line (4 patterns, 3 cells) -- 5x5 extension: protects an edge-middle line
        (1u<<1)|(1u<<2)|(1u<<3),      // TOP   middle
        (1u<<16)|(1u<<17)|(1u<<18),   // BOTTOM middle
        (1u<<5)|(1u<<10)|(1u<<15),    // LEFT  middle
        (1u<<9)|(1u<<14)|(1u<<19),    // RIGHT middle
    };
    static constexpr int FORMATION_COUNT = 44;
    static constexpr int FORMATION_MIN_RANK = 9;   // 512 (same value as official ziap 4x4/5x5)
    // 5x5 corner pattern subset (10 per corner: 3x3 block + 2x3h + 2x3v + 2x2+bidirectional + 2x2+extension x2 + 2x2 + L + C x2)
    static constexpr int CORNER_PATTERNS[4][10] = {
        {0, 28, 32, 36, 4, 5, 12, 16, 20, 21},
        {1, 29, 33, 37, 6, 7, 13, 17, 22, 23},
        {2, 30, 34, 38, 8, 9, 14, 18, 24, 25},
        {3, 31, 35, 39, 10, 11, 15, 19, 26, 27}
    };
    // 4x4 corner pattern subset (6 per corner: 2x2+extension x2 + 2x2 + L + C x2) -- fully aligned with official ziap FORMATIONS
    //   (official 24 patterns = 2x2+extension(8) + 2x2(4) + L(4) + C(8); only cells inside the 4x4 corner 2x2 area are used)
    static constexpr int CORNER_PATTERNS_4[4][6] = {
        {4, 5, 12, 16, 20, 21},   // TL: +R, +D, 2×2, L, C_R, C_D
        {6, 7, 13, 17, 22, 23},   // TR: +L, +D, 2×2, L, C_L, C_D
        {8, 9, 14, 18, 24, 25},   // BL: +R, +U, 2×2, L, C_R, C_U
        {10, 11, 15, 19, 26, 27}, // BR: +L, +U, 2×2, L, C_L, C_U
    };
    // Detect a corner formation (b: rank grid, b[x column][y row]; cell i = b[i%N][i/N])
    static Formation detectFormation5(const int b[N][N], int cornerIdx) {
        uint32_t present = 0, repeated = 0;
        for (int i = 0; i < N * N; ++i) {
            int rk = b[i % N][i / N];
            if (rk >= FORMATION_MIN_RANK) {
                uint32_t bit = 1u << rk;
                repeated |= present & bit;
                present |= bit;
            }
        }
        if (present == 0) return Formation();
        int lowestRank = FORMATION_MIN_RANK;
        for (int i = FORMATION_MIN_RANK; i < 32; ++i) if (present & (1u << i)) { lowestRank = i; break; }
        int cutoff = std::min(lowestRank + 1, 31);
        uint32_t cutoffMask = 0;
        for (int i = cutoff; i < 32; ++i) if (present & (1u << i)) cutoffMask |= (1u << i);
        if (repeated & cutoffMask) return Formation();
        uint32_t big = 0;
        for (int i = 0; i < N * N; ++i) { int rk = b[i % N][i / N]; if (rk >= cutoff) big |= (1u << i); }
        Formation best; double bestValue = 0;
        // N=4 uses the official ziap 24-pattern subset; N=5 uses the extended 44-pattern subset
        if constexpr (N == 4) {
            const int* patterns4 = CORNER_PATTERNS_4[cornerIdx];
            for (int p = 0; p < 6; ++p) {
                uint32_t pattern = FORMATION_PATTERNS[patterns4[p]];
                if ((big & pattern) != pattern) continue;
                uint32_t seen = 0; double val = 0;
                for (int i = 0; i < N * N; ++i) {
                    if (pattern & (1u << i)) {
                        int rk = b[i % N][i / N];
                        if (rk > 0) { uint32_t bit = 1u << rk; if (!(seen & bit)) { val += 1.0; seen |= bit; } }
                    }
                }
                if (val > bestValue) { bestValue = val; best.cellMask = pattern; best.cutoff = cutoff; best.active = true; }
            }
        } else {
            const int* patterns = CORNER_PATTERNS[cornerIdx];
            for (int p = 0; p < 10; ++p) {
                uint32_t pattern = FORMATION_PATTERNS[patterns[p]];
                if ((big & pattern) != pattern) continue;
                uint32_t seen = 0; double val = 0;
                for (int i = 0; i < N * N; ++i) {
                    if (pattern & (1u << i)) {
                        int rk = b[i % N][i / N];
                        if (rk > 0) { uint32_t bit = 1u << rk; if (!(seen & bit)) { val += 1.0; seen |= bit; } }
                    }
                }
                if (val > bestValue) { bestValue = val; best.cellMask = pattern; best.cutoff = cutoff; best.active = true; }
            }
        }
        return best;
    }
    static bool formationIntact(const Formation& f, const int b[N][N]) {
        if (!f.active) return true;
        for (int i = 0; i < N * N; ++i)
            if (f.cellMask & (1u << i))
                if (b[i % N][i / N] < f.cutoff) return false;
        return true;
    }
    static bool multiIntact(const MultiFormation& f, const int b[N][N]) {
        if (!f.anyActive) return true;
        for (int i = 0; i < 4; ++i) {
            if (f.corners[i].active && !formationIntact(f.corners[i], b)) return false;
            if (f.edges[i].active && !formationIntact(f.edges[i], b)) return false;
        }
        return true;
    }
    // Edge-line formation detection (0=top,1=bottom,2=left,3=right): 3 edge-middle cells in a line
    static Formation detectFormationEdge5(const int b[N][N], int edgeIdx) {
        static const uint32_t EDGE_PATTERNS[4] = {
            (1u<<1)|(1u<<2)|(1u<<3),      // top
            (1u<<16)|(1u<<17)|(1u<<18),   // bottom
            (1u<<5)|(1u<<10)|(1u<<15),    // left
            (1u<<9)|(1u<<14)|(1u<<19),    // right
        };
        uint32_t present = 0, repeated = 0;
        for (int i = 0; i < N * N; ++i) {
            int rk = b[i % N][i / N];
            if (rk >= FORMATION_MIN_RANK) {
                uint32_t bit = 1u << rk;
                repeated |= present & bit;
                present |= bit;
            }
        }
        if (present == 0) return Formation();
        int lowestRank = FORMATION_MIN_RANK;
        for (int i = FORMATION_MIN_RANK; i < 32; ++i) if (present & (1u << i)) { lowestRank = i; break; }
        int cutoff = std::min(lowestRank + 1, 31);
        uint32_t cutoffMask = 0;
        for (int i = cutoff; i < 32; ++i) if (present & (1u << i)) cutoffMask |= (1u << i);
        if (repeated & cutoffMask) return Formation();
        uint32_t big = 0;
        for (int i = 0; i < N * N; ++i) { int rk = b[i % N][i / N]; if (rk >= cutoff) big |= (1u << i); }
        uint32_t pattern = EDGE_PATTERNS[edgeIdx];
        if ((big & pattern) != pattern) return Formation();
        Formation f; f.cellMask = pattern; f.cutoff = cutoff; f.active = true;
        return f;
    }
    // Parallel multi-formation detection (4 corners + 4 edges) + anti-drift fallback:
    //   when formation=none and the largest tile (rank>=10) is already in a corner, hard-lock that single corner cell (cutoff=max rank)
    //   prevents big-tile drift (covers the key unprotected states: single big tile alone in a corner / two equal big ranks awaiting merge)
    // Parallel multi-formation detection (4 corners [+ 4 edges, 5x5 only]) + anti-drift fallback:
    //   when formation=none and the largest tile (rank>=10) is already in a corner, hard-lock that single corner cell (cutoff=max rank)
    //   prevents big-tile drift (covers the key unprotected states: single big tile alone in a corner / two equal big ranks awaiting merge)
    static MultiFormation detectMultiFormation(const int b[N][N]) {
        MultiFormation mf; mf.anyActive = false;
        for (int i = 0; i < 4; ++i) {
            mf.corners[i] = detectFormation5(b, i);
            if (mf.corners[i].active) mf.anyActive = true;
            if constexpr (N == 5) {
                mf.edges[i] = detectFormationEdge5(b, i);
                if (mf.edges[i].active) mf.anyActive = true;
            }
        }
        if (mf.anyActive) return mf;
        // ---- Anti-drift fallback: hard-lock a single corner cell ----
        // Corner cell bits (bit i = b[col=i%N][row=i/N]): 4x4={0,3,12,15}, 5x5={0,4,20,24}
        static const int CORNER_CELL_4[4]  = { 0, 3, 12, 15 };
        static const int CORNER_CELL_5[4]  = { 0, 4, 20, 24 };
        const int* cornerCell = (N == 4) ? CORNER_CELL_4 : CORNER_CELL_5;
        int maxRank = 0, maxIdx = -1;
        for (int i = 0; i < N * N; ++i) {
            int rk = b[i % N][i / N];
            if (rk > maxRank) { maxRank = rk; maxIdx = i; }
        }
        if (maxRank >= 10 && maxIdx >= 0) {
            for (int ci = 0; ci < 4; ++ci) {
                if (maxIdx == cornerCell[ci]) {
                    Formation f; f.cellMask = (1u << maxIdx); f.cutoff = maxRank; f.active = true;
                    mf.corners[ci] = f; mf.anyActive = true;
                    break;
                }
            }
        }
        return mf;
    }

    // ===== Board node =====
    struct Node {
        int b[N][N];           // rank storage
        double pass_score = 0;
        double game_over_score = -(1LL << 22);
        bool skip_cache = false;
        double min_prob = 0.0;                 // probability-pruning threshold (injected by the engine before search)
        MxCache<N>* cache = nullptr;           // transposition table (injected by the engine before search)
        MultiFormation fam;                    // 5x5 formation protection (detected at top level, copied down the tree)
        // Timeout control (shared node counter + stop flag; avoids per-Node copies invalidating the counter)
        int budgetMs = 0;
        std::chrono::steady_clock::time_point clockRef{};
        std::atomic<bool>* stopFlag = nullptr;
        std::atomic<long long>* nodeCounter = nullptr;

        Node() { std::memset(b, 0, sizeof(b)); }

        static int TileScore(int r) { return r << r; }
        static int Tile(int r) { return r ? (1 << r) : 0; }

        void load(const BoardMatrix& board) {
            std::memset(b, 0, sizeof(b));
            for (int y = 0; y < N && y < (int)board.size(); ++y) {
                const auto& row = board[y];
                for (int x = 0; x < N && x < (int)row.size(); ++x) {
                    int v = row[x], r = 0;
                    if (v > 0) { while (v > 1) { v >>= 1; ++r; } }
                    b[x][y] = r;
                }
            }
        }

        // ---- Row encoding (row[0] is the lowest bits) ----
        int MaxRank() const {
            int m = 0;
            for (int i = 0; i < N * N; ++i) {
                int r = b[i % N][i / N];
                if (r > m) m = r;
            }
            return m;
        }

        static uint32_t rowCode(const int row[N]) {
            uint32_t c = 0;
            for (int x = 0; x < N; ++x) c |= (uint32_t)(row[x] & 0x1F) << (5 * x);
            return c;
        }
        static void decodeRow(uint32_t c, int row[N]) {
            for (int x = 0; x < N; ++x) row[x] = (int)((c >> (5 * x)) & 0x1F);
        }

        // ---- Left slide-and-merge one row (standard 2048: adjacent equal values merge once) ----
        static void mergeRowLeft(const int in[N], int out[N]) {
            int cmp[N] = {0};
            int w = 0;
            for (int x = 0; x < N; ++x) if (in[x]) cmp[w++] = in[x];
            int o = 0, i = 0;
            while (i < w) {
                if (i + 1 < w && cmp[i] == cmp[i + 1]) { out[o++] = cmp[i] + 1; i += 2; }
                else out[o++] = cmp[i++];
            }
            while (o < N) out[o++] = 0;
        }

        // ---- Row left shift (lookup for N<=4 / hand-written for N=5) ----
        static uint32_t mergeRowLeftCode(uint32_t c) {
            if constexpr (N <= 4) return sm_moveMap[c];
            else {
                int row[N]; decodeRow(c, row);
                int out[N]; mergeRowLeft(row, out);
                return rowCode(out);
            }
        }

        // ---- Move (direction: 0=LEFT 1=RIGHT 2=UP 3=DOWN) ----
        bool MoveLeft() {
            bool moved = false;
            for (int y = 0; y < N; ++y) {
                int row[N]; for (int x = 0; x < N; ++x) row[x] = b[x][y];
                uint32_t v = rowCode(row);
                uint32_t c = mergeRowLeftCode(v);
                if (c == v) continue;
                moved = true;
                decodeRow(c, row);
                for (int x = 0; x < N; ++x) b[x][y] = row[x];
            }
            return moved;
        }
        bool MoveRight() {
            bool moved = false;
            for (int y = 0; y < N; ++y) {
                int row[N]; for (int x = 0; x < N; ++x) row[x] = b[x][y];
                std::reverse(row, row + N);
                uint32_t v = rowCode(row);
                uint32_t c = mergeRowLeftCode(v);
                if (c == v) continue;
                moved = true;
                decodeRow(c, row);
                std::reverse(row, row + N);
                for (int x = 0; x < N; ++x) b[x][y] = row[x];
            }
            return moved;
        }
        void transpose() {
            int t[N][N];
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) t[x][y] = b[y][x];
            std::memcpy(b, t, sizeof(b));
        }
        bool MoveUp()   { transpose(); bool m = MoveLeft();  transpose(); return m; }
        bool MoveDown() { transpose(); bool m = MoveRight(); transpose(); return m; }
        bool Move(int dir) {
            switch (dir) {
                case 0: return MoveLeft();
                case 1: return MoveRight();
                case 2: return MoveUp();
                default: return MoveDown();
            }
        }
        bool anyMove() const {
            Node m = *this;
            for (int d = 0; d < 4; ++d) if (m.Move(d)) return true;
            return false;
        }

        // ---- Evaluation (size-specific) ----
        // 4x4: macroxue score_map heuristic (aligned with the open source)
        // 3x3: tight space -> high EMPTY weight + SUM penalizes isolated big tiles + bidirectional MONO + MERGES
        // 5x5: ziap formula + snake gradient (big tiles cluster in corners, anti-drift)
        // 4x4 row eval: ziap official (LOST=200000, EMPTY=270, MERGES=700, MONO=47^4, SUM=11^3.5)
        static double rowScore(const int row[N]) {
            const double LOST = 200000.0, MONO_W = 47.0, SUM_W = 11.0,
                         MERGES_W = 700.0, EMPTY_W = 270.0;
            double sum = 0; int empty = 0, merges = 0;
            int prev = 0, counter = 0;
            for (int x = 0; x < N; ++x) {
                int rank = row[x];
                sum += s_sumPow[rank];
                if (rank == 0) { empty++; continue; }
                if (prev == rank) counter++;
                else if (counter > 0) { merges += 1 + counter; counter = 0; }
                prev = rank;
            }
            if (counter > 0) merges += 1 + counter;
            double monoL = 0, monoR = 0;
            for (int x = 1; x < N; ++x) {
                double l = s_monoPow[row[x-1]], r = s_monoPow[row[x]];
                if (row[x-1] > row[x]) monoL += l - r;
                else monoR += r - l;
            }
            return LOST + EMPTY_W * empty + MERGES_W * merges
                 - MONO_W * std::min(monoL, monoR) - SUM_W * sum;
        }
        // 3x3 evaluation: MacroXue score_map idea (consistent with the 4x4 rowScore)
        //   TileScore(r)=r<<r -> big tiles score high; monotonic increase rewarded; adjacent equal tiles rewarded again (merge potential)
        //   Measured: pure score_map averages 4553; adding an empty-weight (+120/cell) is over-conservative -> average drops to 3326
        //   -> no forced empty weight; let the search handle survival naturally
        static double lineEval33(const int l[3]) {
            double score = TileScore(l[0]);
            for (int x = 0; x < 2; ++x) {
                double a = TileScore(l[x]), bb = TileScore(l[x + 1]);
                score += (a >= bb) ? (a + bb) : (a - bb) * 12.0;
                score += (a == bb) ? a : 0;
            }
            return score;
        }
        static double lineEval55(const int l[5]) {
            const double LOST = 200000.0, MONO_W = 47.0, SUM_W = 11.0,
                         MERGES_W = 800.0, EMPTY_W = 270.0;   // MERGES 700->800 (user request: reasonably raise the merge weight)
            double sum = 0; int empty = 0;
            for (int x = 0; x < 5; ++x) { sum += s_sumPow[l[x]]; if (l[x] == 0) ++empty; }
            double monoL = 0, monoR = 0;
            for (int x = 0; x < 4; ++x) {
                if (l[x] > l[x + 1]) monoL += s_monoPow[l[x]] - s_monoPow[l[x + 1]];
                else if (l[x] < l[x + 1]) monoR += s_monoPow[l[x + 1]] - s_monoPow[l[x]];
            }
            int merges = 0, prev5 = 0, counter5 = 0;
            for (int x = 0; x < 5; ++x) {
                if (l[x] == 0) continue;
                if (prev5 == l[x]) counter5++;
                else if (counter5 > 0) { merges += 1 + counter5; counter5 = 0; }
                prev5 = l[x];
            }
            if (counter5 > 0) merges += 1 + counter5;
            return LOST + EMPTY_W * empty + MERGES_W * merges
                 - MONO_W * std::min(monoL, monoR) - SUM_W * sum;
        }
        // 5x5 snake gradient: rank^2 x snake weight, max over the 4 corner paths -> big tiles land in corners without drifting
        //   Active only when a clear big tile exists (maxRank>=9=512); small positions skip the O(100) cost
        double snakeGradient55() const {
            int mr = 0;
            for (int i = 0; i < 25; ++i) { int r = b[i % 5][i / 5]; if (r > mr) mr = r; }
            if (mr < 9) return 0;
            double best = -1e300;
            for (int p = 0; p < 4; ++p) {
                double s = 0;
                for (int y = 0; y < 5; ++y)
                    for (int x = 0; x < 5; ++x)
                        if (b[x][y]) s += (double)(b[x][y] * b[x][y]) * s_snake[p][y][x];
                if (s > best) best = s;
            }
            return best;
        }
        // Formation-template gradient: over 16 templates (L3 / T-shape 442 / mixed L-T / snake x 4 directions)
        //   take the max of rank x weight sums -> encourages big tiles to evolve into the most favorable formation (more parameters/density)
        double formationGradient() const {
            double best = -1e300;
            for (int t = 0; t < 16; ++t) {
                double s = 0;
                const double* w = s_formW[t][0];
                for (int y = 0; y < N; ++y)
                    for (int x = 0; x < N; ++x)
                        if (b[x][y]) s += (double)b[x][y] * w[y * N + x];
                if (s > best) best = s;
            }
            return best;
        }
        double Evaluate() const {
            if constexpr (N == 3) {
                double score = 0;
                for (int y = 0; y < 3; ++y) { int l[3]; for (int x = 0; x < 3; ++x) l[x] = b[x][y]; score += s_eval33[rowCode(l)]; }
                for (int x = 0; x < 3; ++x) { int l[3]; for (int y = 0; y < 3; ++y) l[y] = b[x][y]; score += s_eval33[rowCode(l)]; }
                return score;
            } else if constexpr (N == 5) {
                double score = 0;
                for (int y = 0; y < 5; ++y) { int l[5]; for (int x = 0; x < 5; ++x) l[x] = b[x][y]; score += lineEval55(l); }
                for (int x = 0; x < 5; ++x) { int l[5]; for (int y = 0; y < 5; ++y) l[y] = b[x][y]; score += lineEval55(l); }
                score += 6.0 * snakeGradient55();   // SNAKE_WEIGHT=6.0 (5x5 extension: snake pull, relieves big-tile detachment; raised from 4.0)
                return score;
            } else {
                // 4x4: pure ziap score_map (row+col), NO extra position/snake/formation terms
                double score = 0;
                for (int y = 0; y < 4; ++y) { int l[4]; for (int x = 0; x < 4; ++x) l[x] = b[x][y]; score += sm_scoreMap[rowCode(l)]; }
                for (int x = 0; x < 4; ++x) { int l[4]; for (int y = 0; y < 4; ++y) l[y] = b[x][y]; score += sm_scoreMap[rowCode(l)]; }
                return score;
            }
        }

        // ---- 64-bit Zobrist cache key (lookup XOR, faster than full encoding) ----
        uint64_t compactKey() const {
            uint64_t h = 0;
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                    h ^= sm_zob[x][y][b[x][y] & 0x1F];
            return h;
        }

        // ---- Search: Chance layer (enumerate empty cells placing 2/4) ----
        // Cache hit first: saves Evaluate/empty-scan on hot starts (similar positions), noticeably faster
        double TryAllTiles(int depth, float prob) {
            // Timeout stop-flag check (shared across threads; one direction timing out immediately notifies others to exit fast)
            if (stopFlag && stopFlag->load(std::memory_order_relaxed))
                return Evaluate();
            // Shared node counter: check the clock every 64 nodes (accumulated across recursion/threads; sets the stop flag on timeout)
            if (nodeCounter) {
                long long cnt = nodeCounter->fetch_add(1, std::memory_order_relaxed);
                if ((cnt & 63) == 0 && budgetMs > 0 &&
                    std::chrono::steady_clock::now() > clockRef + std::chrono::milliseconds(budgetMs)) {
                    if (stopFlag) stopFlag->store(true, std::memory_order_relaxed);
                    return Evaluate();
                }
            }
            uint64_t key = compactKey();
            if (!skip_cache && cache) {
                double s;
                if (cache->Lookup(key, prob, depth, &s)) return s;
            }
            double score = Evaluate();
            if (score < pass_score || depth < 0 || prob < min_prob) return score;
            // One pass collects empty cells (avoids a second traversal, hot path)
            int empty = 0;
            int ex[N * N], ey[N * N];
            for (int i = 0; i < N * N; ++i) {
                int x = i % N, y = i / N;
                if (b[x][y] == 0) { ex[empty] = x; ey[empty] = y; ++empty; }
            }
            if (empty == 0) return score;
            float tile2_prob = prob / empty * 0.9f;
            float tile4_prob = prob / empty * 0.1f;
            double total = 0;
            if constexpr (N == 5) {
                // 5x5 strategic-priority sampling: empty cells sorted corner-first, only the first min(empty,MAX_SAMPLE) expanded
                // (aligned with Solver5x5Fast: concentrate budget on high-value positions, replacing the old random Floyd sampling)
                constexpr int MAX_SAMPLE = 20;
                int idx[25];
                for (int k = 0; k < empty; ++k) idx[k] = k;
                for (int i = 1; i < empty; ++i) {
                    int v = idx[i];
                    int si = s_strategic[ey[v] * N + ex[v]];
                    int j = i - 1;
                    while (j >= 0 && s_strategic[ey[idx[j]] * N + ex[idx[j]]] > si) {
                        idx[j + 1] = idx[j]; --j;
                    }
                    idx[j + 1] = v;
                }
                int K = empty < MAX_SAMPLE ? empty : MAX_SAMPLE;
                for (int k = 0; k < K; ++k) {
                    int x = ex[idx[k]], y = ey[idx[k]];
                    b[x][y] = 1;
                    total += 0.9 * TryAllMoves(depth, tile2_prob);
                    b[x][y] = 2;
                    total += 0.1 * TryAllMoves(depth, tile4_prob);
                    b[x][y] = 0;
                }
                double avg = total / K;
                if (cache) cache->Update(key, prob, depth, avg);
                return avg;
            }
            for (int k = 0; k < empty; ++k) {
                int x = ex[k], y = ey[k];
                b[x][y] = 1;
                total += 0.9 * TryAllMoves(depth, tile2_prob);
                b[x][y] = 2;
                total += 0.1 * TryAllMoves(depth, tile4_prob);
                b[x][y] = 0;
            }
            double avg = total / empty;
            if (cache) cache->Update(key, prob, depth, avg);
            return avg;
        }

        // ---- Search: Max layer (pick the best direction) ----
        double TryAllMoves(int depth, float prob) {
            double best = game_over_score;
            for (int i = 0; i < 4; ++i) {
                Node m = *this;
                if (!m.Move(i)) continue;
                if constexpr (N == 4 || N == 5) {
                    // Formation protection: Max layer skips directions that break formed corner/edge formations (4x4 aligned with ziap, 5x5 in-house extension)
                    if (m.fam.anyActive && !MacroXueEngine<N>::multiIntact(m.fam, m.b)) continue;
                }
                double s = m.TryAllTiles(depth - 1, prob);
                if (best == game_over_score || best < s) best = s;
            }
            return best;
        }
    };

    // ===== Table building (N<=4 row lookups + full-size Zobrist/pow/snake tables) =====
    static void buildTables() {
        static std::once_flag once;
        std::call_once(once, []() {
            // Power-weight table (for 3x3/5x5 evaluation)
            for (int r = 0; r < 33; ++r) {
                s_sumPow[r] = std::pow((double)r, 3.5);
                s_monoPow[r] = std::pow((double)r, 4.0);
            }
            // 5x5 snake weights: 4 corner boustrophedon paths, weights 31->7 (big tiles land in corners)
            if constexpr (N == 5) {
                auto fillSnake = [](double g[5][5], int startR, int startC) {
                    int order[25]; int idx = 0;
                    bool ltr = (startC == 0);
                    for (int r = 0; r < 5; ++r) {
                        int rr = (startR == 4) ? (4 - r) : r;
                        if (ltr) { for (int c = 0; c < 5; ++c) order[idx++] = rr * 5 + c; }
                        else     { for (int c = 4; c >= 0; --c) order[idx++] = rr * 5 + c; }
                        ltr = !ltr;
                    }
                    for (int i = 0; i < 25; ++i)
                        g[order[i] / 5][order[i] % 5] = 31.0 - i;   // 31 → 7
                };
                fillSnake(s_snake[0], 0, 0);   // top-left
                fillSnake(s_snake[1], 0, 4);   // top-right
                fillSnake(s_snake[2], 4, 0);   // bottom-left
                fillSnake(s_snake[3], 4, 4);   // bottom-right
            }
            // Formation templates: 4 types x 4 directions = 16 NxN weight tables (rank x weight sums; evaluation takes the max)
            //   Type 0 = L-shape (L3: corner + top/left edge descending)
            //   Type 1 = T-shape (442: full top row + middle column descending)
            //   Type 2 = mixed L/T (L edge + middle column/row bulge)
            //   Type 3 = snake (boustrophedon row snake, same as s_snake but a separate table)
            //   Rotation: 0=TL/up  1=TR (horizontal flip)  2=BR (180 deg)  3=BL (vertical flip)
            {
                auto orient = [&](int x, int y, int rot, int& ox, int& oy) {
                    switch (rot) {
                        case 0: ox = x;            oy = y;            break;
                        case 1: ox = N - 1 - x;    oy = y;            break;
                        case 2: ox = N - 1 - x;    oy = N - 1 - y;    break;
                        default:ox = x;            oy = N - 1 - y;    break;
                    }
                };
                for (int type = 0; type < 4; ++type)
                    for (int rot = 0; rot < 4; ++rot) {
                        double* g = s_formW[type * 4 + rot][0];
                        double base[N][N];
                        for (int y = 0; y < N; ++y)
                            for (int x = 0; x < N; ++x) {
                                double w = 0;
                                if (type == 0) {          // L-shape (L3): top edge + left edge
                                    if (x == 0 || y == 0) w = 16.0 - (double)(x + y);
                                } else if (type == 1) {   // T-shape (442): full top row + middle column
                                    if (y == 0) w = 16.0 - (double)x;
                                    else if (x == N / 2) w = 16.0 - (double)y;
                                } else if (type == 2) {   // mixed L/T: L edge + middle column/row
                                    if (x == 0 || y == 0) w = 16.0 - (double)(x + y);
                                    else if (x == N / 2)  w = 9.0 - (double)y * 0.8;
                                    else if (y == N / 2)  w = 9.0 - (double)x * 0.8;
                                } else {                  // snake (boustrophedon, corner 16 -> inner 1)
                                    bool ltr = true;
                                    int seq = 0;
                                    for (int r = 0; r < N; ++r) {
                                        if (ltr) { for (int c = 0; c < N; ++c) { if (r == y && c == x) w = 16.0 - (double)seq; ++seq; } }
                                        else     { for (int c = N - 1; c >= 0; --c) { if (r == y && c == x) w = 16.0 - (double)seq; ++seq; } }
                                        ltr = !ltr;
                                    }
                                    if (w < 1) w = 1;
                                }
                                base[y][x] = w;
                            }
                        for (int y = 0; y < N; ++y)
                            for (int x = 0; x < N; ++x) {
                                int ox, oy; orient(x, y, rot, ox, oy);
                                g[oy * N + ox] = base[y][x];
                            }
                    }
            }
            // Empty-cell strategic sampling priority: corner=0 edge=1 center=2 (ascending = corner first; used for 5x5 sampling sort)
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    int i = r * N + c;
                    bool corner = (r == 0 || r == N - 1) && (c == 0 || c == N - 1);
                    bool edge   = (r == 0 || r == N - 1 || c == 0 || c == N - 1);
                    s_strategic[i] = ((uint8_t)(corner ? 0 : (edge ? 1 : 2)));
                }
            // Deterministic Zobrist table (splitmix64, per-size seeds, thread-safe)
            uint64_t seed = 0x9E3779B97F4A7C15ull ^ (uint64_t)N;
            auto splitmix = [&seed]() {
                uint64_t z = (seed += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            };
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                    for (int v = 0; v < 32; ++v)
                        sm_zob[x][y][v] = splitmix();
            // 3x3 row-evaluation lookup table (2^15)
            if constexpr (N == 3) {
                for (uint32_t i = 0; i < (1u << 15); ++i) {
                    int l[3]; Node::decodeRow(i, l);
                    s_eval33[i] = Node::lineEval33(l);
                }
            }
            if constexpr (N <= 4) {
                for (uint32_t i = 0; i < (uint32_t)kRowTableSize; ++i) {
                    int row[N]; Node::decodeRow(i, row);
                    int out[N]; Node::mergeRowLeft(row, out);
                    sm_moveMap[i] = Node::rowCode(out);
                    sm_scoreMap[i] = Node::rowScore(row);
                }
            }
        });
    }

    // ===== Members =====
    int m_maxDepth;
    double m_minProb;
    int m_timeBudgetMs;
    std::chrono::steady_clock::time_point m_clockStart;
    MxCache<N>* m_cache;
    std::atomic<bool> m_stop{ false };   // timeout stop signal (shared, avoids exception-unwind overhead)
    std::atomic<long long> m_nodes{ 0 }; // shared node counter (accumulated across recursion/threads, triggers timeout checks)

    long long elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_clockStart).count();
    }

    // BlockPlan endgame planner (official plan.h port, no big tables; preferred over search when 4x4 big tiles cluster)
    MacroXuePlan m_plan4;

    // Static tables (N<=4 use them; N=5 degrades to 1 element, no allocation)
    static uint32_t sm_moveMap[kTableSize];
    static double   sm_scoreMap[kTableSize];
    static uint64_t sm_zob[N][N][32];   // Zobrist random table (cache key)
    static double s_sumPow[33];         // precomputed pow(rank, 3.5) (3x3/5x5 evaluation)
    static double s_monoPow[33];        // precomputed pow(rank, 4.0) (3x3/5x5 evaluation)
    static double s_snake[4][5][5];     // 5x5 snake weights (4 corner paths, anti-drift)
    static double s_formW[16][N][N];    // formation-template weights: 4 types (L/T/mixed/snake) x 4 directions, rank x weight evaluation
                                        //  (more parameters/density: encourages big tiles to evolve into L3/T-shape442/mixed-L-T/snake formations)
    static double s_eval33[(N == 3) ? (1 << 15) : 1];  // 3x3 row-evaluation lookup table (2^15)
    static uint8_t s_strategic[N * N];  // empty-cell strategic sampling priority: corner=0 edge=1 center=2 (5x5 sampling sort)
};

// Static member definitions
template <int N>
uint32_t MacroXueEngine<N>::sm_moveMap[MacroXueEngine<N>::kTableSize] = {};
template <int N>
double MacroXueEngine<N>::sm_scoreMap[MacroXueEngine<N>::kTableSize] = {};
template <int N>
uint64_t MacroXueEngine<N>::sm_zob[N][N][32] = {};
template <int N>
double MacroXueEngine<N>::s_sumPow[33] = {};
template <int N>
double MacroXueEngine<N>::s_monoPow[33] = {};
template <int N>
double MacroXueEngine<N>::s_snake[4][5][5] = {};
template <int N>
double MacroXueEngine<N>::s_formW[16][N][N] = {};
template <int N>
double MacroXueEngine<N>::s_eval33[(N == 3) ? (1 << 15) : 1] = {};
template <int N>
uint8_t MacroXueEngine<N>::s_strategic[N * N] = {};

// ==================== Facade class (dispatches by size) ====================
class MacroXueSolverAI2048 {
public:
    using BoardMatrix = std::vector<std::vector<int>>;
    struct MoveScore { int dir; float score; bool safe; bool allowed; };

    MacroXueSolverAI2048() {
        m_e3.setDepth(8); m_e4.setDepth(8); m_e5.setDepth(8);
        // Time budget: 100ms (user prioritizes strength; timeout acceptable; easy positions finish full depth 8 in budget, hard positions interrupt)
        m_e3.setTimeBudgetMs(100);
        m_e4.setTimeBudgetMs(100);
        m_e5.setTimeBudgetMs(100);
    }

    void setBoardSize(int sz) { m_size = sz; }

    // Clear the transposition table (called at each new game; prevents cross-game pollution/degradation)
    void clearCache() { m_e3.clearCache(); m_e4.clearCache(); m_e5.clearCache(); }

    // Three intelligence tiers = search depth {3,5,8}
    void setDepth(int depth) {
        m_e3.setDepth(depth);
        m_e4.setDepth(depth);
        m_e5.setDepth(depth);
    }
    int getDepth() const { return (m_size == 3) ? m_e3.getDepth() : (m_size == 4) ? m_e4.getDepth() : m_e5.getDepth(); }

    // Time budget (ms per move, 0 = unlimited); passed to engines of all sizes
    void setTimeBudgetMs(int ms) {
        m_e3.setTimeBudgetMs(ms);
        m_e4.setTimeBudgetMs(ms);
        m_e5.setTimeBudgetMs(ms);
    }

    std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
        std::vector<MoveScore> out;
        out.reserve(4);
        if (m_size == 3) {
            for (auto& m : m_e3.getMoveScores(board)) out.push_back({ m.dir, m.score, m.safe, m.allowed });
        } else if (m_size == 4) {
            for (auto& m : m_e4.getMoveScores(board)) out.push_back({ m.dir, m.score, m.safe, m.allowed });
        } else {
            for (auto& m : m_e5.getMoveScores(board)) out.push_back({ m.dir, m.score, m.safe, m.allowed });
        }
        return out;
    }

    float evaluateBoard(const BoardMatrix& board) {
        if (m_size == 3) return m_e3.evaluate(board);
        if (m_size == 4) return m_e4.evaluate(board);
        return m_e5.evaluate(board);
    }

    std::string getStatsString() const {
        if (m_size == 3) return m_e3.stats();
        if (m_size == 4) return m_e4.stats();
        return m_e5.stats();
    }

private:
    int m_size = 4;
    MacroXueEngine<3> m_e3;
    MacroXueEngine<4> m_e4;
    MacroXueEngine<5> m_e5;
};

#endif // MACROXUE_SOLVER_AI_2048_H
