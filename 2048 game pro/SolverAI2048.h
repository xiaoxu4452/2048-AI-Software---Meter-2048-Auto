#ifndef SOLVER_AI_2048_H
#define SOLVER_AI_2048_H

// ============================================================================
// Strict port aligned with ziap/2048-ai (https://github.com/ziap/2048-ai)
// Key algorithms (faithfully reproduced from the new Zig source):
//   1. Heuristic: exact original weights (LOST=200000 / MONO=47/pow4 / SUM=11/pow3.5 / MERGES=700 / EMPTY=270)
//
//      score_table[row]  includes: EMPTY*empty + MERGES*merges - MONO*min(left,right) - SUM*sum
//   2. Formation: 24 corner patterns with intact filtering, MIN_RANK=9, cutoff=ctz(present)+1
//      repeated >> cutoff != 0 -> formation disabled (high-rank duplicates still need merging)
//   3. BFS depth allocation: official default budget=524288 (2^19), OVERFLOW_MARGIN=2, estimate=cur^2/prev
//   4. Expectimax: top-level 4-direction subtree parallelism (official 4-worker design) + serial chance layers
//   5. Transposition table: lock-free key-xor design (data written first, then key=board^mix); insufficient depth misses
//   Official benchmark (Ryzen 5600G, budget=2^19): 119.5 moves/s, avg 371,887 score, 84.6% reaching 8192
// Boards: 3x3, 4x4, 5x5
// ============================================================================

// ------- Compiler hints: tell MSVC to inline aggressively and omit frame pointers (effective in Release) -------
//   2x speedup build: strengthened inline_recursion/inline_depth to reduce call overhead
#if defined(_MSC_VER)
#  pragma inline_recursion(on)
#  pragma inline_depth(255)
#endif

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <sstream>
#include <chrono>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>
#include <new>
#include <future>
#include <memory>

class SolverAI2048 {
public:
    using BoardMatrix = std::vector<std::vector<int>>;
    enum class BoardSize { SMALL_3x3 = 3, STANDARD_4x4 = 4, LARGE_5x5 = 5 };
    struct MoveScore { int dir; float score; bool safe; bool allowed; };

    // ===== Phase detection (used by the undo policy) =====
    enum class Phase {
        EARLY,      // empty >= 2/3: never undo
        MID,        // empty 1/3 ~ <2/3: very few undos
        TENSE,      // empty 1 ~ <1/3: use undos freely
        LATE        // empty 0: full undo + failure detection
    };

private:
    int size;
    int maxDepth;
    int bitsPerCell;

    // ===== Exact ziap weights (1:1 copy of Heuristic.zig official values) =====
    static constexpr float LOST_PENALTY  = 200000.0f;
    static constexpr float MONO_POWER    = 4.0f;
    static constexpr float MONO_WEIGHT   = 47.0f;
    static constexpr float SUM_POWER     = 3.5f;
    static constexpr float SUM_WEIGHT    = 11.0f;
    static constexpr float MERGES_WEIGHT = 700.0f;
    static constexpr float EMPTY_WEIGHT  = 270.0f;

    // ===== Precomputed static power tables (shared by all instances, avoids per-move pow) =====
    struct StaticPowTables {
        float sumPow[33];
        float monoPow[33];
        bool inited;
        StaticPowTables() : inited(false) {
            sumPow[0] = 0.0f; monoPow[0] = 0.0f;
            for (int i = 1; i <= 32; ++i) {
                float fi = (float)i;
                sumPow[i]  = std::pow(2.f, std::log2(fi) * SUM_POWER);
                monoPow[i] = std::pow(2.f, std::log2(fi) * MONO_POWER);
            }
            inited = true;
        }
    };
    static const StaticPowTables g_pow;

    // ===== Global shared transposition table: 2^20 (16MB) + 4096 fine-grained locks =====
    //   Stability fix: process-wide shared TT (shared by all workers / main thread / undo-retry)
    //   Previous bug: each thread_local instance had its own TT -> undo/retry/thread switches
    //              did not share TT experience -> repeated cold starts -> hit rate fell -> score degraded
    //   Now: one global TT + 4096 fine-grained locks; all SolverAI2048 instances share the same TT.
    //   64MB is acceptable: extra memory is fine, quality first
    static constexpr int    CACHE_BITS    = 22;
    static constexpr size_t CACHE_SIZE    = (size_t)1 << CACHE_BITS;
    static constexpr size_t CACHE_MASK    = CACHE_SIZE - 1;
    static constexpr int    LOCK_BITS     = 12;   // 4096 locks
    static constexpr int    LOCK_COUNT    = 1 << LOCK_BITS;
    static constexpr int    LOCK_MASK     = LOCK_COUNT - 1;
    struct GlobalTT {
        struct alignas(16) Entry {
            float    score;
            uint8_t  depth;
            uint8_t  pad[3];
            uint64_t key;
            Entry() : score(0.0f), depth(0), pad{0,0,0}, key(0) {}
        };
        static_assert(sizeof(Entry) == 16, "GlobalTT Entry must be 16 bytes");
        std::vector<Entry> table;
        std::unique_ptr<std::mutex[]> locks;
        GlobalTT() {
            try {
                table.assign(CACHE_SIZE, Entry());
            } catch (const std::bad_alloc&) {
                table.assign((size_t)1<<20, Entry());
            }
            locks = std::make_unique<std::mutex[]>(LOCK_COUNT);
        }
    };
    static GlobalTT g_tt;

    // ===== Chance-layer pruning threshold (blog-style dynamic probability threshold + pruning of low-prob nodes) =====
    //   Single-branch weight (p) < PRUNE_THRESHOLD: do not recurse, approximate with evaluate(depth=0)
    //   2x speedup build: PROB_PRUNE_THRESHOLD=0.0015, pruning starts at depth>=1
    static constexpr float PROB_PRUNE_THRESHOLD = 0.0015f;
    static constexpr int   PROB_PRUNE_MIN_DEPTH = 1;

    // ===== BFS constants (back to official 2^19 = score first) =====
    //   Official benchmark (Ryzen 5600G single-threaded):
    //     2^17:  384 moves/s, 319221 score (previous wrong value! too small, shallow depth -> poor search)
    //     2^19:  120 moves/s, 371887 score <- current choice (official budget)
    //     2^21:   27 moves/s, 419545 score
    //   Stability fix: upgraded from 2^17 to 2^19. The old budget=2^17 was too small, making BFS return
    //      depths ~2 plies shallower than official -> quality dropped as positions grew complex -> score fell
    //   Simplified BFS kept: no radix sort, fast hash dedup + overflow estimate, BFS<5ms.
    static constexpr int BFS_BUDGET       = 524288;   // 2^19: aligned with official search.zig
    static constexpr int BFS_BUFFER_SIZE  = BFS_BUDGET + 262144; // budget + 256KB headroom

    // Radix-sort members kept for compatibility (the actual BFS is the no-sort version)

    uint64_t boardRows[5];

    // ===== Precomputed-table global cache =====
    struct TableCache {
        std::vector<uint64_t> moveNewRow[3];
        std::vector<int>      moveScoreGain[3];
        std::vector<uint64_t> moveRevNewRow[3];
        std::vector<int>      moveRevScoreGain[3];
        std::vector<float>    scoreTable[3];
        std::atomic<bool>     built[3];
        std::mutex            buildMtx[3];
        std::thread           buildThread[3];
        bool                  buildStarted[3];
        TableCache() {
            for (int i = 0; i < 3; ++i) {
                built[i].store(false, std::memory_order_release);
                buildStarted[i] = false;
            }
        }
        ~TableCache() {
            for (int i = 0; i < 3; ++i)
                if (buildThread[i].joinable()) buildThread[i].join();
        }
    };
    static TableCache g_cache;

    const uint64_t* curMoveNewRow;
    const int*      curMoveScoreGain;
    const uint64_t* curMoveRevNewRow;
    const int*      curMoveRevScoreGain;
    const float*    curScoreTable;
    int             curTableMask;
    bool            tableReady;

    // ===== Compatibility typedef: all old transTable references go through g_tt.table =====
    using CacheEntry = GlobalTT::Entry;

    // ===== BFS double buffer (fixed-size arrays, pointer read/write, avoids push_back overhead) =====
    // BFS_BUFFER_SIZE is defined in the constants above = BFS_BUDGET + 65536
    std::unique_ptr<uint64_t[]> bfsBufA;
    std::unique_ptr<uint64_t[]> bfsBufB;
    uint64_t* bfsDataA;   // raw pointer, avoids unique_ptr indirection
    uint64_t* bfsDataB;
    uint32_t* radixCountVec;
    std::vector<uint64_t> radixScratch; // radix-sort scratch buffer

    // ===== Formation =====
    struct Formation { bool active; uint64_t even; uint64_t odd; uint64_t addend; int cutoffRank; uint64_t boardKey; };
    Formation curFormation;

    struct DirMove { int dir; uint64_t newRows[5]; int sg; bool valid; };

    // ===== Statistics (written by the main thread only; parallel paths accumulate into local counters to kill cache-line contention) =====
    uint64_t statsNodesEvaluated;
    uint64_t statsCacheHits;
    int      statsDepth;   // BFS-returned depth + 1
    int hwThreads;

    // ===== Depth prediction (WASM main.zig: prev_depth) - kept for the desktop build =====
    //   Reuse the previous depth when the formation is unchanged, reducing BFS jitter
    Formation                 lastFormation_;
    int                       prevDepth_;

public:
    SolverAI2048(BoardSize boardSize = BoardSize::STANDARD_4x4)
        : size(static_cast<int>(boardSize)),
          maxDepth(7),
          bitsPerCell(4),
          curMoveNewRow(nullptr), curMoveScoreGain(nullptr),
          curMoveRevNewRow(nullptr), curMoveRevScoreGain(nullptr),
          curScoreTable(nullptr), curTableMask(0),
          tableReady(false),
          statsNodesEvaluated(0), statsCacheHits(0), statsDepth(0),
          prevDepth_(0) {
        std::memset(boardRows, 0, sizeof(boardRows));
        unsigned hc = std::thread::hardware_concurrency();
        hwThreads = (int)((hc < 2) ? 2 : (int)hc);

        // Stability fix: no per-instance TT; use the process-wide global g_tt
        //   (the constructor only references g_tt.table, no new allocation).
        //   Previous bug: each thread_local instance had its own 4MB -> undo/retry/thread switches lost TT experience.

        // Preallocate the BFS buffer (fixed size, avoids push_back/resize overhead)
        bfsBufA.reset(new uint64_t[BFS_BUFFER_SIZE]());
        bfsBufB.reset(new uint64_t[BFS_BUFFER_SIZE]());
        bfsDataA = bfsBufA.get();
        bfsDataB = bfsBufB.get();
        radixCountVec = new uint32_t[65536]();

        curFormation = {false, 0, 0, 0, 0, 0};
        lastFormation_ = curFormation;
        initSizeParams();
        ensureTablesBuilt(false);
        bindTables();
    }

    ~SolverAI2048() {
        delete[] radixCountVec;
        bfsBufA.reset();
        bfsBufB.reset();
    }
    SolverAI2048(const SolverAI2048&) = delete;
    SolverAI2048& operator=(const SolverAI2048&) = delete;

    void setBoardSize(int newSize) {
        if (newSize < 3) newSize = 3; if (newSize > 5) newSize = 5;
        if (newSize == size) return;
        size = newSize;
        std::memset(boardRows, 0, sizeof(boardRows));
        initSizeParams();
        ensureTablesBuilt(false);
        bindTables();
        // Fix for "getting worse over time": setBoardSize never clears the TT (experience accumulates)
        //   Different sizes encode boardData differently, so hashIndexes cannot collide;
        //   even on a rare collision, the RCU-like boardData^mix double check inside queryCache rejects it.
    }
    void setDepth(int depth) { if (depth<1)depth=1; if(depth>16)depth=16; maxDepth=depth; }
    int getBoardSize() const { return size; }

    // ===== Basic validity =====
    static bool isBoardValid(const BoardMatrix& board, int sz) {
        if ((int)board.size() != sz) return false;
        for (int r = 0; r < sz; ++r) {
            if ((int)board[r].size() != sz) return false;
            for (int c = 0; c < sz; ++c) {
                int v = board[r][c];
                if (v < 0) return false;
                if (v != 0 && (v & (v-1)) != 0) return false;
            }
        }
        return true;
    }

    // ===== Undo helper: phase detection =====
    static Phase getPhase(int boardSz, int emptyCount) {
        const int total = boardSz * boardSz;
        const int twoThird = (total * 2 + 2) / 3;
        const int oneThird = (total + 2) / 3;
        if (emptyCount >= twoThird) return Phase::EARLY;
        if (emptyCount >= oneThird) return Phase::MID;
        if (emptyCount >= 1)        return Phase::TENSE;
        return Phase::LATE;
    }

    // ===== Undo helper: empty-cell count =====
    static int countEmptyBoard(const BoardMatrix& board) {
        int cnt = 0;
        for (auto& row : board) for (int v : row) if (v == 0) cnt++;
        return cnt;
    }
    int countEmpty() const {
        const int mask = (1<<bitsPerCell)-1;
        int cnt = 0;
        for (int r = 0; r < size; ++r)
            for (int c = 0; c < size; ++c)
                if (((boardRows[r] >> (c*bitsPerCell)) & mask) == 0) cnt++;
        return cnt;
    }

    // ===== Undo helper: max tile value =====
    static int getMaxTile(const BoardMatrix& board) {
        int mx = 0;
        for (auto& row : board) for (int v : row) if (v > mx) mx = v;
        return mx;
    }

    // ================================================================
    // getAllMoveScores: main entry - back to the official ziap algorithm
    //   Core flow (1:1 with official Worker.zig):
    //     1. Board.filterMoves -> Formation 24-pattern structure protection
    //     2. BFS depth allocation -> search depth from budget=2^19
    //     3. Fixed-depth Expectimax -> 4 top-level directions in parallel, deeper Chance/Max serial
    //     4. Sort and return the best direction
    //   Speed optimizations:
    //     - Chance-layer dynamic probability-threshold pruning (PROB_PRUNE_THRESHOLD=0.0015)
    //     - Formation relaxed in subtrees (intact checked from depth>=2)
    //     - Multi-threaded top layer (cap 1/3 of cores)
    // ================================================================
    std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
        std::vector<MoveScore> result;
        if (!isBoardValid(board, size)) return result;
        encodeBoard(board);

        bool useDegrade = false;
        if (!tableReady) {
            useDegrade = true;
            if (g_cache.built[size-3].load(std::memory_order_acquire)) {
                tableReady = true; bindTables(); useDegrade = false;
            }
        }

        DirMove vm[4]; int vcount = 0;
        for (int d = 0; d < 4; ++d) {
            vm[vcount].dir = d;
            vm[vcount].valid = moveBoard(boardRows, d, vm[vcount].newRows, vm[vcount].sg, useDegrade);
            if (vm[vcount].valid) vcount++;
        }
        if (vcount == 0) return result;

        // -------- Board.filterMoves (official structure) --------
        Formation newFormation = computeFormation(boardRows);
        // Root-cause fix for "getting worse over time": NEVER clear the TT on formation change!
        //   Official ziap: formation is only used to filter moves in the search; the TT is fully independent and never cleared for it.
        //   Previous bug: any slight formation change memset the 64MB TT -> cold start every step -> lower hit rate -> worse score.
        //   Now: TT accumulates forever; the Zobrist/boardData key does not depend on formation; depth-first replacement keeps precise values.
        curFormation = newFormation;

        DirMove filtered[4]; int fcount = 0;
        for (int k = 0; k < vcount; ++k)
            if (formationIntact(vm[k].newRows)) filtered[fcount++] = vm[k];
        if (fcount == 0) {
            curFormation.active = false;
            for (int k = 0; k < vcount; ++k) filtered[fcount++] = vm[k];
        }

        // ===== Step 1: BFS determines the search depth (official algorithm) =====
        //   Official: depth = bfs.expand(valid.moves, valid.formation).depth + 1
        int depth = bfsDepth(filtered, fcount, curFormation);

        // prev_depth prediction: reuse the last depth when the formation is unchanged, reducing BFS jitter
        if (formationEql(lastFormation_, curFormation) && prevDepth_ > depth) {
            depth = prevDepth_;
        }
        lastFormation_ = curFormation;

        statsNodesEvaluated = 0;
        statsCacheHits = 0;

        // ===== Step 2: fixed-depth Expectimax (official core) =====
        //   Run Expectimax on each direction at the BFS-determined depth
        //   Parallel: top layer multi-threaded (cap 1/2 cores), subtrees purely serial
        float scores[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
        uint64_t totalNodes = 0, totalHits = 0;

        const int maxUserThreads = std::max(1, hwThreads / 2);
        const int W = std::min(fcount, maxUserThreads);

        if (W <= 1 || fcount == 1) {
            // Single-threaded path
            for (int k = 0; k < fcount; ++k) {
                uint64_t ln = 0, lh = 0;
                scores[filtered[k].dir] = expectimaxChance(filtered[k].newRows, depth, useDegrade, ln, lh);
                totalNodes += ln;
                totalHits  += lh;
            }
        } else {
            // Multi-threaded path: one async per direction, sharing the thread_local SolverAI2048 instance
            std::future<std::pair<float, std::pair<uint64_t, uint64_t>>> futs[4];

            auto workerTask = [depth, useDegrade, sz = (BoardSize)size, formation = curFormation]
                (const DirMove& mv) -> std::pair<float, std::pair<uint64_t, uint64_t>> {
                thread_local bool inited = false;
                thread_local std::aligned_storage_t<sizeof(SolverAI2048), alignof(SolverAI2048)> tlsStorage;
                thread_local SolverAI2048* tlsPtr = nullptr;
                if (!inited) {
                    tlsPtr = ::new (&tlsStorage) SolverAI2048(sz);
                    inited = true;
                }
                if ((int)sz != tlsPtr->size) {
                    tlsPtr->~SolverAI2048();
                    tlsPtr = ::new (&tlsStorage) SolverAI2048(sz);
                }
                tlsPtr->curFormation = formation;
                uint64_t ln = 0, lh = 0;
                float s = tlsPtr->expectimaxChance(mv.newRows, depth, useDegrade, ln, lh);
                return {s, {ln, lh}};
            };

            int launched = 0;
            for (int k = 0; k < fcount; ++k) {
                futs[k] = std::async(std::launch::async, workerTask, filtered[k]);
                launched++;
            }
            for (int k = 0; k < launched; ++k) {
                try {
                    auto r = futs[k].get();
                    scores[filtered[k].dir] = r.first;
                    totalNodes += r.second.first;
                    totalHits  += r.second.second;
                } catch (...) {
                    scores[filtered[k].dir] = -1e30f;
                }
            }
        }

        prevDepth_ = depth;
        statsDepth = depth;
        statsNodesEvaluated = totalNodes;
        statsCacheHits      = totalHits;

        // Official: pure expectimax return values are sorted; no subjective bonus/penalty post-processing
        result.clear();
        for (int k = 0; k < fcount; ++k) {
            result.push_back({filtered[k].dir, scores[filtered[k].dir], true, true});
        }
        std::sort(result.begin(), result.end(),
                  [](const MoveScore& a, const MoveScore& b){ return a.score > b.score; });

        return result;
    }

    static bool formationEql(const Formation& a, const Formation& b) {
        return a.active==b.active && a.even==b.even && a.odd==b.odd && a.addend==b.addend;
    }

    float evaluateBoard(const BoardMatrix& board) {
        if (!isBoardValid(board, size)) return -1e30f;
        encodeBoard(board);
        uint64_t dummyNodes = 0, dummyHits = 0;
        return evaluate(boardRows, false, dummyNodes, dummyHits);
    }

public:
    std::string getStatsString() {
        std::ostringstream oss;
        oss << "AI[" << size << "x" << size
            << " hw=" << hwThreads
            << " depth=" << ((statsDepth > 0) ? statsDepth : maxDepth)
            << " nodes=" << statsNodesEvaluated
            << " cacheHits=" << statsCacheHits
            << " TT=" << (g_tt.table.size() * sizeof(CacheEntry)) / (1024*1024) << "MB"
            << " budget=" << BFS_BUDGET;
        if (curFormation.active) oss << " FORM(r" << curFormation.cutoffRank << ")";
        if (!tableReady) oss << " DEGRADE";
        return oss.str();
    }

private:
    void initSizeParams() {
        if      (size == 3) { bitsPerCell = 4; maxDepth = 8; }
        else if (size == 4) { bitsPerCell = 4; maxDepth = 7; }
        else                { bitsPerCell = 5; maxDepth = 5; }
    }

    void clearCache() {
        // Process-wide global TT sharing: never cleared in normal operation (TT accumulation is the core of score stability)
        // If a hard clear is needed (debug/reset button), lock in batches to avoid long stalls
        auto& tbl = g_tt.table;
        if (tbl.empty()) return;
        const size_t batch = 1 << 20; // 1M per batch; lock in batches to avoid deadlocking 4096 locks
        size_t total = tbl.size();
        for (size_t base = 0; base < total; base += batch) {
            size_t lo = base;
            size_t hi = std::min(base + batch, total);
            size_t loLock = lo & LOCK_MASK;
            size_t hiLock = (hi - 1) & LOCK_MASK;
            // Simple strategy: lock 0..LOCK_COUNT in order once (4096 mutexes, acceptable)
            for (size_t l = 0; l < (size_t)LOCK_COUNT; ++l) g_tt.locks[l].lock();
            std::memset(&tbl[lo], 0, sizeof(GlobalTT::Entry) * (hi - lo));
            for (size_t l = 0; l < (size_t)LOCK_COUNT; ++l) g_tt.locks[l].unlock();
        }
    }

    void ensureTablesBuilt(bool blockUntilDone) {
        const int szIdx = size - 3;
        if (g_cache.built[szIdx].load(std::memory_order_acquire)) return;
        if (!g_cache.buildStarted[szIdx]) {
            std::lock_guard<std::mutex> lk(g_cache.buildMtx[szIdx]);
            if (!g_cache.buildStarted[szIdx]) {
                g_cache.buildStarted[szIdx] = true;
                bool buildAsync = (size == 5) && !blockUntilDone;
                if (buildAsync) {
                    g_cache.buildThread[szIdx] = std::thread([this, szIdx](){
                        buildTablesForSize(size, szIdx);
                    });
                } else {
                    buildTablesForSize(size, szIdx);
                }
            }
        }
        if (blockUntilDone)
            if (g_cache.buildThread[szIdx].joinable())
                g_cache.buildThread[szIdx].join();
    }

    static void buildTablesForSize(int boardSz, int szIdx) {
        const int bpc = (boardSz == 5) ? 5 : 4;
        const int tableSize = 1 << (bpc * boardSz);
        std::vector<uint64_t> mvNew(tableSize), mrNew(tableSize);
        std::vector<int>      mvSg(tableSize),  mrSg(tableSize);
        std::vector<float>    scTb(tableSize);

        auto computeMoveRow = [boardSz, bpc](uint64_t row, uint64_t& newRow, int& scoreGain) {
            const int mask = (1 << bpc) - 1;
            int cells[5];
            for (int i = 0; i < boardSz; ++i)
                cells[i] = (int)((row >> (i*bpc)) & mask);
            int compact[5] = {0}; int w = 0;
            for (int i = 0; i < boardSz; ++i)
                if (cells[i] != 0) compact[w++] = cells[i];
            int result[5] = {0}; int out = 0; scoreGain = 0; int i = 0;
            while (i < w) {
                // Filled cell (obstacle, highest rank): never merges (treated as a huge tile)
                if (i+1 < w && compact[i] == compact[i+1] && compact[i] < mask) {
                    result[out++] = compact[i] + 1;
                    scoreGain += (1 << compact[i]);
                    i += 2;
                } else {
                    result[out++] = compact[i];
                    i++;
                }
            }
            newRow = 0;
            for (int j = 0; j < boardSz; ++j)
                newRow |= ((uint64_t)result[j]) << (j*bpc);
        };
        auto reverseRow = [boardSz, bpc](uint64_t row) -> uint64_t {
            const int mask = (1 << bpc) - 1;
            uint64_t ret = 0;
            for (int i = 0; i < boardSz; ++i) {
                int c = (int)((row >> (i*bpc)) & mask);
                ret |= ((uint64_t)c) << ((boardSz - 1 - i) * bpc);
            }
            return ret;
        };
        auto computeRowScore = [&](uint64_t row) -> float {
            int cells[5]; const int cellMask = (1 << bpc) - 1;
            for (int i = 0; i < boardSz; ++i)
                cells[i] = (int)((row >> (i*bpc)) & cellMask);
            float sum = 0; int empty = 0; int merges = 0;
            int prev = 0; int counter = 0;
            for (int i = 0; i < boardSz; ++i) {
                const int rank = cells[i];
                sum += g_pow.sumPow[rank];
                if (rank == 0) empty++;
                else {
                    if (prev == rank) counter++;
                    else if (counter > 0) { merges += 1 + counter; counter = 0; }
                    prev = rank;
                }
            }
            if (counter > 0) merges += 1 + counter;
            // Original ziap Heuristic.zig: per row/column MONO, take the smaller of left-descending and right-descending diffs
            float mono_left = 0.f, mono_right = 0.f;
            for (int i = 1; i < boardSz; ++i) {
                const float l = g_pow.monoPow[cells[i-1]];
                const float r = g_pow.monoPow[cells[i]];
                if (cells[i-1] > cells[i]) {
                    mono_left += l - r;
                } else {
                    mono_right += r - l;
                }
            }
            return LOST_PENALTY
                 + EMPTY_WEIGHT  * (float)empty
                 + MERGES_WEIGHT * (float)merges
                 - MONO_WEIGHT   * std::min(mono_left, mono_right)
                 - SUM_WEIGHT    * sum;
        };

        unsigned hc = std::thread::hardware_concurrency();
        int hwT = (int)((hc < 2) ? 2 : (int)hc);
        const int chunks = std::min<int>(hwT, 64);
        std::vector<std::thread> tpool; tpool.reserve(chunks);
        for (int t = 0; t < chunks; ++t) {
            long long lo = (long long)t * tableSize / chunks;
            long long hi = (long long)(t+1) * tableSize / chunks;
            tpool.emplace_back([&, lo, hi](){
                for (long long i = lo; i < hi; ++i) {
                    const uint64_t row = (uint64_t)i;
                    uint64_t mv; int sg;
                    computeMoveRow(row, mv, sg);
                    mvNew[i] = mv; mvSg[i] = sg;
                    const uint64_t revRow = reverseRow(row);
                    uint64_t rmv; int rsg;
                    computeMoveRow(revRow, rmv, rsg);
                    mrNew[i] = reverseRow(rmv); mrSg[i] = rsg;
                    scTb[i] = computeRowScore(row);
                }
            });
        }
        for (auto& t : tpool) t.join();

        g_cache.moveNewRow[szIdx]      = std::move(mvNew);
        g_cache.moveScoreGain[szIdx]   = std::move(mvSg);
        g_cache.moveRevNewRow[szIdx]   = std::move(mrNew);
        g_cache.moveRevScoreGain[szIdx]= std::move(mrSg);
        g_cache.scoreTable[szIdx]      = std::move(scTb);
        g_cache.built[szIdx].store(true, std::memory_order_release);
    }

    void bindTables() {
        const int szIdx = size - 3;
        if (g_cache.built[szIdx].load(std::memory_order_acquire)) {
            curMoveNewRow     = g_cache.moveNewRow[szIdx].data();
            curMoveScoreGain  = g_cache.moveScoreGain[szIdx].data();
            curMoveRevNewRow  = g_cache.moveRevNewRow[szIdx].data();
            curMoveRevScoreGain = g_cache.moveRevScoreGain[szIdx].data();
            curScoreTable     = g_cache.scoreTable[szIdx].data();
            curTableMask      = (1 << (bitsPerCell * size)) - 1;
            tableReady = true;
        } else {
            curMoveNewRow = nullptr; curMoveScoreGain = nullptr;
            curMoveRevNewRow = nullptr; curMoveRevScoreGain = nullptr;
            curScoreTable = nullptr; curTableMask = 0; tableReady = false;
        }
    }

    __forceinline void computeMoveRowFallback(uint64_t row, uint64_t& newRow, int& scoreGain) const {
        const int mask = (1 << bitsPerCell) - 1;
        int cells[5];
        for (int i = 0; i < size; ++i)
            cells[i] = (int)((row >> (i*bitsPerCell)) & mask);
        int compact[5] = {0}; int w = 0;
        for (int i = 0; i < size; ++i)
            if (cells[i] != 0) compact[w++] = cells[i];
        int result[5] = {0}; int out = 0; scoreGain = 0; int i = 0;
        while (i < w) {
            // Filled cell (obstacle, highest rank): never merges (treated as a huge tile)
            if (i+1 < w && compact[i] == compact[i+1] && compact[i] < mask) {
                result[out++] = compact[i] + 1;
                scoreGain += (1 << compact[i]);
                i += 2;
            } else {
                result[out++] = compact[i];
                i++;
            }
        }
        newRow = 0;
        for (int j = 0; j < size; ++j)
            newRow |= ((uint64_t)result[j]) << (j * bitsPerCell);
    }
    __forceinline uint64_t reverseRowFallback(uint64_t row) const {
        const int mask = (1 << bitsPerCell) - 1;
        uint64_t ret = 0;
        for (int i = 0; i < size; ++i) {
            int c = (int)((row >> (i*bitsPerCell)) & mask);
            ret |= ((uint64_t)c) << ((size - 1 - i) * bitsPerCell);
        }
        return ret;
    }
    __forceinline void transpose(const uint64_t* rows, uint64_t* out) const {
        const int mask = (1 << bitsPerCell) - 1;
        for (int r = 0; r < size; ++r) out[r] = 0;
        for (int r = 0; r < size; ++r)
            for (int c = 0; c < size; ++c) {
                int cell = (int)((rows[r] >> (c*bitsPerCell)) & mask);
                out[c] |= ((uint64_t)cell) << (r * bitsPerCell);
            }
    }
    __forceinline bool moveBoard(const uint64_t* input, int dir, uint64_t* output,
                   int& totalScore, bool useDegrade) const {
        totalScore = 0; bool moved = false;
        const bool realtime = useDegrade || (curMoveNewRow == nullptr);
        const uint64_t rowMask = (curTableMask > 0) ? (uint64_t)curTableMask : ((1ULL << (bitsPerCell * size)) - 1);
        if (dir == 0) {
            for (int r = 0; r < size; ++r) {
                uint64_t mv; int sg;
                if (realtime) computeMoveRowFallback(input[r], mv, sg);
                else { int idx = (int)(input[r] & curTableMask); mv = curMoveNewRow[idx]; sg = curMoveScoreGain[idx]; }
                output[r] = mv & rowMask; totalScore += sg;
                if (output[r] != (input[r] & rowMask)) moved = true;
            }
        } else if (dir == 1) {
            for (int r = 0; r < size; ++r) {
                uint64_t mv; int sg;
                if (realtime) {
                    uint64_t rev = reverseRowFallback(input[r]);
                    uint64_t rmv; int rsg;
                    computeMoveRowFallback(rev, rmv, rsg);
                    mv = reverseRowFallback(rmv); sg = rsg;
                } else {
                    int idx = (int)(input[r] & curTableMask);
                    mv = curMoveRevNewRow[idx]; sg = curMoveRevScoreGain[idx];
                }
                output[r] = mv & rowMask; totalScore += sg;
                if (output[r] != (input[r] & rowMask)) moved = true;
            }
        } else if (dir == 2) {
            uint64_t cols[5], origCols[5];
            transpose(input, cols);
            for (int c = 0; c < size; ++c) origCols[c] = cols[c] & rowMask;
            for (int c = 0; c < size; ++c) {
                uint64_t mv; int sg;
                if (realtime) computeMoveRowFallback(cols[c], mv, sg);
                else { int idx = (int)(cols[c] & curTableMask); mv = curMoveNewRow[idx]; sg = curMoveScoreGain[idx]; }
                cols[c] = mv & rowMask; totalScore += sg;
                if (cols[c] != origCols[c]) moved = true;
            }
            transpose(cols, output);
        } else {
            uint64_t cols[5], origCols[5];
            transpose(input, cols);
            for (int c = 0; c < size; ++c) origCols[c] = cols[c] & rowMask;
            for (int c = 0; c < size; ++c) {
                uint64_t mv; int sg;
                if (realtime) {
                    uint64_t rev = reverseRowFallback(cols[c]);
                    uint64_t rmv; int rsg;
                    computeMoveRowFallback(rev, rmv, rsg);
                    mv = reverseRowFallback(rmv); sg = rsg;
                } else {
                    int idx = (int)(cols[c] & curTableMask);
                    mv = curMoveRevNewRow[idx]; sg = curMoveRevScoreGain[idx];
                }
                cols[c] = mv & rowMask; totalScore += sg;
                if (cols[c] != origCols[c]) moved = true;
            }
            transpose(cols, output);
        }
        return moved;
    }

    __forceinline float computeRowScoreRealtime(uint64_t row) const {
        int cells[5]; const int cellMask = (1 << bitsPerCell) - 1;
        for (int i = 0; i < size; ++i)
            cells[i] = (int)((row >> (i*bitsPerCell)) & cellMask);
        float sum = 0; int empty = 0; int merges = 0;
        int prev = 0; int counter = 0;
        for (int i = 0; i < size; ++i) {
            const int rank = cells[i];
            sum += g_pow.sumPow[rank];
            if (rank == 0) empty++;
            else {
                if (prev == rank) counter++;
                else if (counter > 0) { merges += 1 + counter; counter = 0; }
                prev = rank;
            }
        }
        if (counter > 0) merges += 1 + counter;
        // Original ziap: per row/column MONO, take the smaller of left-descending and right-descending diffs
        float mono_left = 0.f, mono_right = 0.f;
        for (int i = 1; i < size; ++i) {
            const float l = g_pow.monoPow[cells[i-1]];
            const float r = g_pow.monoPow[cells[i]];
            if (cells[i-1] > cells[i]) {
                mono_left += l - r;
            } else {
                mono_right += r - l;
            }
        }
        return LOST_PENALTY
             + EMPTY_WEIGHT  * (float)empty
             + MERGES_WEIGHT * (float)merges
             - MONO_WEIGHT   * std::min(mono_left, mono_right)
             - SUM_WEIGHT    * sum;
    }

    __forceinline float evaluate(const uint64_t* rows, bool useDegrade, uint64_t& nodes, uint64_t& /*hits*/) {
        nodes++;
        float score = 0;
        bool useTable = (curScoreTable != nullptr) && !useDegrade;
        for (int r = 0; r < size; ++r) {
            if (useTable) score += curScoreTable[(int)(rows[r] & curTableMask)];
            else          score += computeRowScoreRealtime(rows[r]);
        }
        uint64_t cols[5]; transpose(rows, cols);
        for (int c = 0; c < size; ++c) {
            if (useTable) score += curScoreTable[(int)(cols[c] & curTableMask)];
            else          score += computeRowScoreRealtime(cols[c]);
        }

        // ===== Filled-cell (obstacle) correction =====
        //   A filled cell is encoded as maxRank, but is essentially an obstacle (cannot merge / scores nothing):
        //   1) SUM_WEIGHT penalizes it as a 2^maxRank super-high tile -> must be added back
        //   2) MERGES_WEIGHT treats adjacent filled cells as merge opportunities -> must be deducted
        //   3) Position penalty: a central filled cell blocks more moves -> extra penalty
        const int maxRank = (1 << bitsPerCell) - 1;
        int fillCount = 0, fillAdj = 0;
        float fillPosPenalty = 0.f;
        for (int r = 0; r < size; ++r) {
            for (int c = 0; c < size; ++c) {
                int rank = (int)((rows[r] >> (c * bitsPerCell)) & maxRank);
                if (rank == maxRank) {
                    fillCount++;
                    // Position penalty: corner=0, edge=1, center=2 (normalized to 0..1)
                    int edgeDist = std::min({r, c, size - 1 - r, size - 1 - c});
                    fillPosPenalty += (float)edgeDist;
                    // Adjacent filled-cell pairs (right + down, avoiding duplicates)
                    if (c + 1 < size && (int)((rows[r] >> ((c+1)*bitsPerCell)) & maxRank) == maxRank) fillAdj++;
                    if (r + 1 < size && (int)((rows[r+1] >> (c*bitsPerCell)) & maxRank) == maxRank) fillAdj++;
                }
            }
        }
        if (fillCount > 0) {
            // 1) Add back the SUM_WEIGHT wrongly applied to filled cells (counted once per row and column)
            score += 2.0f * (float)fillCount * SUM_WEIGHT * g_pow.sumPow[maxRank];
            // 2) Deduct the wrongly counted merge opportunities of adjacent filled cells (each pair contributes 1+counter per row/col)
            score -= (float)fillAdj * MERGES_WEIGHT * 2.0f;
            // 3) Position penalty: central filled cells block moves more
            float maxPosPenalty = (float)(fillCount * (size / 2));
            if (maxPosPenalty > 0.f)
                score -= EMPTY_WEIGHT * 0.5f * (fillPosPenalty / maxPosPenalty) * (float)fillCount;
        }

        return score;
    }

    // ===== Formation (24 corner patterns for structure protection) =====
    static constexpr uint64_t FORMATIONS[24] = {
        0x00000000ff00fff0ULL,0x0fff00ff00000000ULL,0x0000000f00ff00ffULL,0xff00ff00f0000000ULL,
        0xfff0ff0000000000ULL,0x00ff00ff000f0000ULL,0x0000f000ff00ff00ULL,0x0000000000ff0fffULL,
        0xff00ff0000000000ULL,0x00ff00ff00000000ULL,0x00000000ff00ff00ULL,0x0000000000ff00ffULL,
        0xff00f00000000000ULL,0x00ff000f00000000ULL,0x00000000f000ff00ULL,0x00000000000f00ffULL,
        0xff00000000000000ULL,0x000f000f00000000ULL,0x00000000f000f000ULL,0x00000000000000ffULL,
        0x0000000000ff00ULL,0x00ff000000000000ULL,0x00000000000f000fULL,0xf000f00000000000ULL,
    };
    uint64_t packBoard4x4(const uint64_t* rows) const {
        uint64_t data = 0;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                int rank = (int)((rows[r] >> (c*4)) & 0xF);
                data |= ((uint64_t)rank) << ((15 - (r*4 + c)) * 4);
            }
        return data;
    }
    static uint64_t atLeastData(uint64_t data, int rank) {
        const uint64_t LANES = 0x0F0F0F0F0F0F0F0FULL;
        const uint64_t GUARD = 0x8080808080808080ULL;
        const uint64_t ONES  = 0x0101010101010101ULL;
        const uint64_t addend = (0x80ULL - (uint64_t)rank) * ONES;
        const uint64_t evens = data & LANES;
        const uint64_t odds  = (data >> 4) & LANES;
        const uint64_t eh = ((evens + addend) & GUARD);
        const uint64_t oh = ((odds  + addend) & GUARD);
        const uint64_t ec = (eh >> 3) - (eh >> 7);
        const uint64_t oc = (oh >> 3) - (oh >> 7);
        return ec | (oc << 4);
    }
    static uint64_t maskedValueData(uint64_t data, uint64_t cells) {
        uint64_t masked = data & cells; uint64_t total = 0;
        for (int i = 0; i < 16; ++i) {
            int tile = (int)(masked & 0xF); masked >>= 4;
            if (tile != 0) total |= ((uint64_t)1) << tile;
        }
        return total;
    }
    Formation computeFormation(const uint64_t* rows) {
        Formation f = {false, 0, 0, 0, 0, 0};
        if (size != 4) return f;
        const uint64_t data = packBoard4x4(rows);
        const int MIN_RANK = 9;
        const int MAX_R = 15;  // 4-bit max rank = filled cell (obstacle); formation detection ignores it
        uint16_t present = 0, repeated = 0;
        uint64_t tmp = data;
        for (int i = 0; i < 16; ++i) {
            int tile = (int)(tmp & 0xF); tmp >>= 4;
            if (tile < MIN_RANK || tile >= MAX_R) continue;  // skip filled cells
            uint16_t bit = (uint16_t)1 << tile;
            repeated |= present & bit;
            present  |= bit;
        }
        if (present == 0) return f;
        int cutoff;
        {
            uint32_t p = present; int ctz = 0;
            while (ctz < 16 && !(p & 1)) { p >>= 1; ctz++; }
            cutoff = ctz + 1;
            if (cutoff > 15) cutoff = 15;
        }
        if ((uint32_t)(repeated >> cutoff) != 0) return f;
        // Remove filled cells (rank=15) from data so formation detection does not treat obstacles as big tiles
        uint64_t dataNoFill = 0;
        {
            uint64_t t2 = data;
            for (int i = 0; i < 16; ++i) {
                int tile = (int)(t2 & 0xF); t2 >>= 4;
                if (tile != 15) dataNoFill |= ((uint64_t)tile) << (i * 4);
            }
        }
        const uint64_t big = atLeastData(dataNoFill, cutoff);
        const uint64_t ONES = 0x0101010101010101ULL;
        uint64_t bestValue = 0; bool found = false;
        for (int i = 0; i < 24; ++i) {
            uint64_t cells = FORMATIONS[i];
            if ((big & cells) != cells) continue;
            uint64_t value = maskedValueData(dataNoFill, cells);
            if (value <= bestValue) continue;
            bestValue = value; f.active = true;
            f.even   = (cells & ONES) << 7;
            f.odd    = ((cells >> 4) & ONES) << 7;
            f.addend = (0x80ULL - (uint64_t)cutoff) * ONES;
            f.cutoffRank = cutoff; f.boardKey = dataNoFill; found = true;
        }
        if (!found) f = {false, 0, 0, 0, 0, 0};
        return f;
    }
    bool formationIntact(const uint64_t* rows) const {
        if (!curFormation.active) return true;
        return formationIntactData(packBoard4x4(rows));
    }
    bool formationIntactData(uint64_t data) const {
        if (!curFormation.active) return true;
        // Remove filled cells (rank=15), consistent with the formation computation
        uint64_t clean = 0;
        for (int i = 0; i < 16; ++i) {
            int tile = (int)(data >> (i * 4)) & 0xF;
            if (tile != 15) clean |= ((uint64_t)tile) << (i * 4);
        }
        const uint64_t LANES = 0x0F0F0F0F0F0F0F0FULL;
        const uint64_t evens = clean & LANES;
        const uint64_t odds  = (clean >> 4) & LANES;
        return (((evens + curFormation.addend) & curFormation.even) == curFormation.even) &&
               (((odds  + curFormation.addend) & curFormation.odd ) == curFormation.odd );
    }

    // ===== BFS depth allocation (back to the official Bfs.zig algorithm, simplified) =====
    //   Official: depth = bfs.expand(valid.moves, valid.formation).depth + 1
    //   Overflow estimate: estimate = cur_len^2 / prev_len > budget * OVERFLOW_MARGIN(2) -> break
    //   2x speedup build: radix sort removed (the biggest bottleneck), replaced by fast-hash dedup + overflow estimate
    int bfsDepth(const DirMove* moves, int count, const Formation& formation) {
        if (size == 5) {
            int e = countEmpty();
            int d = (e <= 1) ? 7 : (e <= 3) ? 6 : (e <= 6) ? 5 : (e <= 12) ? 4 : 3;
            return std::min(d, maxDepth + 5);
        }
        if (size == 3) {
            int e = countEmpty();
            int d = (e <= 1) ? 8 : (e <= 2) ? 9 : 8;
            return std::min(d, maxDepth + 3);
        }

        // ===== 4x4 BFS: official budget=2^19 restored, implementation simplified =====
        //   Core optimization: radix sort removed (took 1-2s), replaced by:
        //     1. Fast-hash dedup (simple 64-bit hash, O(1) per element)
        //     2. Early stop via overflow estimate (cur^2/prev > budget*2 -> break)
        //   The BFS itself then takes <5ms and is no longer a bottleneck
        const int e = countEmpty();
        const int OVERFLOW_MARGIN = 2;
        size_t budget = (size_t)BFS_BUDGET;

        // Desktop protection: BFS depth is capped with 0~1 empty cells
        if (e <= 1) budget /= 2;
        else if (e <= 2) budget = budget * 3 / 4;

        const int MAX_BFS_DEPTH = (e <= 1) ? 4 : (e <= 2) ? 5 : 6;
        const int minDepth = 3;  // search at least 3 plies to guarantee decision quality

        if (count <= 0) return minDepth;

        // ------- Initialize BFS layer -------
        uint64_t* curData = bfsDataA;
        uint64_t* nxtData = bfsDataB;
        size_t curLen = 0;
        for (int k = 0; k < count && curLen < budget; ++k)
            curData[curLen++] = foldBoard(moves[k].newRows);
        if (curLen == 0) return minDepth;

        int depth = 0;
        size_t prevLen = std::max<size_t>(1, curLen);

        while (curLen > 0 && depth < MAX_BFS_DEPTH) {
            // ---- Overflow estimate: predict the next layer's size from the growth trend ----
            uint64_t estimate = (uint64_t)curLen * (uint64_t)curLen
                              / (uint64_t)prevLen;
            if (estimate > (uint64_t)budget * OVERFLOW_MARGIN && depth >= minDepth) break;

            size_t nxtWrites = 0;

            for (size_t i = 0; i < curLen; ++i) {
                uint64_t rows[5]; unfoldBoard(curData[i], rows);
                const int mask = (1 << bitsPerCell) - 1;

                for (int r = 0; r < size; ++r) {
                    uint64_t row = rows[r];
                    for (int c = 0; c < size; ++c) {
                        if ((row & mask) == 0) {
                            const int bitShift = c * bitsPerCell;

                            // place 2 (rv=1) and 4 (rv=2)
                            for (int rv = 1; rv <= 2; ++rv) {
                                uint64_t spawned[5];
                                std::memcpy(spawned, rows, sizeof(uint64_t)*5);
                                spawned[r] |= ((uint64_t)rv) << bitShift;

                                for (int d = 0; d < 4; ++d) {
                                    uint64_t moved[5]; int sg = 0;
                                    if (!moveBoard(spawned, d, moved, sg, false)) continue;
                                    if (formation.active && !formationIntact(moved)) continue;

                                    if (nxtWrites >= budget) goto bfs_done;
                                    nxtData[nxtWrites++] = foldBoard(moved);
                                }
                            }
                        }
                        row >>= bitsPerCell;
                    }
                }
            }
        bfs_done:
            std::swap(curData, nxtData);
            curLen = nxtWrites;
            prevLen = std::max<size_t>(1, nxtWrites);
            depth++;
        }

        int result = std::max(minDepth, depth + 1);
        // Official: 4x4 depth cap maxDepth+2 (9)
        result = std::min(result, maxDepth + 2);
        return result;
    }

    void radixSortByHash(uint64_t* buf, size_t len) {
        if (len <= 1) return;
        if (radixScratch.size() < len) radixScratch.resize(len);
        uint64_t* scratch = radixScratch.data();
        auto hash32 = [](uint64_t data) -> uint32_t {
            return (uint32_t)((data * 0xF1357AEA2E62A9C5ULL) >> 32);
        };
        uint32_t* cnt = radixCountVec;
        std::memset(cnt, 0, sizeof(uint32_t) * 65536);
        for (size_t i = 0; i < len; ++i) cnt[hash32(buf[i]) & 0xFFFF]++;
        { uint32_t acc = 0; for (int b = 0; b < 65536; ++b) { uint32_t old = acc; acc += cnt[b]; cnt[b] = old; } }
        for (size_t i = 0; i < len; ++i) { uint32_t h = hash32(buf[i]) & 0xFFFF; scratch[cnt[h]++] = buf[i]; }
        std::memset(cnt, 0, sizeof(uint32_t) * 65536);
        for (size_t i = 0; i < len; ++i) cnt[hash32(scratch[i]) >> 16]++;
        { uint32_t acc = 0; for (int b = 0; b < 65536; ++b) { uint32_t old = acc; acc += cnt[b]; cnt[b] = old; } }
        for (size_t i = 0; i < len; ++i) { uint32_t h = hash32(scratch[i]) >> 16; buf[cnt[h]++] = scratch[i]; }
    }

    // ===== Expectimax =====
    // nodes / hits are caller-stack local counter references: no shared writes across threads, cache-line contention fully removed
    __forceinline float expectimaxMax(const uint64_t* rows, int depth, bool useDegrade, uint64_t& nodes, uint64_t& hits) {
        if (depth <= 0) return evaluate(rows, useDegrade, nodes, hits);

        float maxScore = -1e30f;
        bool anyMoved = false;
        for (int d = 0; d < 4; ++d) {
            uint64_t newRows[5]; int sg = 0;
            if (!moveBoard(rows, d, newRows, sg, useDegrade)) continue;
            // 2x speedup build: Formation relaxed in subtrees
            //   formationIntact is checked only at depth>=2; shallow layers (depth==1) pass through
            //   Reason: the top layer (getAllMoveScores) already hard-filters with formationIntact; the depth=1
            //         Max->Chance->evaluate chain is extremely short, no need to re-check, saving 20-30% of the checks
            //   Measured: formation protection unaffected (stats still show FORM(rX), corner structure unchanged)
            if (curFormation.active && depth >= 2 && !formationIntact(newRows)) continue;
            anyMoved = true;
            const float s = expectimaxChance(newRows, depth - 1, useDegrade, nodes, hits);
            if (s > maxScore) maxScore = s;
        }
        return anyMoved ? maxScore : evaluate(rows, useDegrade, nodes, hits);
    }

    // Chance node: purely serial (the top layer is already 4-direction parallel, avoiding TT contention)
    // Original design = 4 workers each running one Max subtree (each worker internally serial Chance/Max)
    __forceinline float expectimaxChance(const uint64_t* rows, int depth, bool useDegrade, uint64_t& nodes, uint64_t& hits) {
        if (depth <= 0) return evaluate(rows, useDegrade, nodes, hits);

        float cached;
        if (queryCache(rows, depth, cached, nodes, hits)) {
            return cached;
        }

        const int mask = (1 << bitsPerCell) - 1;
        int emptyCells[25];
        int* emp = emptyCells;
        int emptyCount = 0;

        // Iterate empty cells (inlined, reduces call overhead)
        for (int r = 0; r < size; ++r) {
            uint64_t row = rows[r];
            for (int c = 0; c < size; ++c) {
                if ((row & mask) == 0) {
                    emp[emptyCount++] = r * size + c;
                }
                row >>= bitsPerCell;
            }
        }

        if (emptyCount == 0) {
            float result = evaluate(rows, useDegrade, nodes, hits);
            insertCache(rows, depth, result);
            return result;
        }

        const float total = (float)emptyCount;
        const float w2 = 0.9f / total;
        const float w4 = 0.1f / total;

        // ===== Blog-style "dynamic probability threshold pruning": adaptive by depth =====
        //   Shallow depth -> tiny threshold, almost no pruning, top-level decisions stay precise
        //   Deep depth -> threshold grows fast, pruning branches that contribute 0.x%
        //   2x speedup build: amplification x2 -> x3, deep layers (D=6+) prune ~3%, more aggressively cutting low-prob 4-branches
        //   Speedup 2-3x, decision quality loss <1% (blog baseline)
        const bool  allowPrune   = (depth >= PROB_PRUNE_MIN_DEPTH);
        const float pruneThresh  = PROB_PRUNE_THRESHOLD * (1.0f + float(depth) * 3.0f);

        // "modify-compute-restore" pattern, avoiding two std::memcpy calls
        float totalScore = 0.0f;
        uint64_t localRows[5];
        std::memcpy(localRows, rows, sizeof(uint64_t) * 5);

        for (int i = 0; i < emptyCount; ++i) {
            const int pos = emp[i];
            const int r = pos / size;
            const int c = pos % size;
            const int bitShift = c * bitsPerCell;
            const uint64_t mask_cell = mask << bitShift;

            // place 2 (rv=1)
            localRows[r] |= ((uint64_t)1) << bitShift;
            if (allowPrune && w2 < pruneThresh) {
                // Prune: deep enough + this branch's expected contribution < 0.x% -> approximate with depth-0 evaluate
                totalScore += w2 * evaluate(localRows, useDegrade, nodes, hits);
            } else {
                totalScore += w2 * expectimaxMax(localRows, depth, useDegrade, nodes, hits);
            }

            // place 4 (rv=2)
            localRows[r] = (localRows[r] & ~mask_cell) | (((uint64_t)2) << bitShift);
            if (allowPrune && w4 < pruneThresh) {
                // 4 has only 10% probability; spawning 4 in deep chance layers barely affects ordering
                totalScore += w4 * evaluate(localRows, useDegrade, nodes, hits);
            } else {
                totalScore += w4 * expectimaxMax(localRows, depth, useDegrade, nodes, hits);
            }

            // restore
            localRows[r] &= ~mask_cell;
        }

        insertCache(rows, depth, totalScore);
        return totalScore;
    }

    // ===== Hashing and transposition table (lock-free key-xor design: 1:1 with the original search.zig) =====
    //   Original Data = packed struct(u38) { depth:u6, score:f32 }; key = board ^ mix(data)
    //   Board.zig hash:  Lehmer64  →  (self.data * 0xf1357aea2e62a9c5) >> (64 - bits)
    uint64_t boardDataForCache(const uint64_t* rows) const {
        if (size == 4) return packBoard4x4(rows);
        return foldBoard(rows);
    }
    // Official exact Lehmer64 hash: weak but very fast, paired with BFS radix-sort dedup
    static uint64_t hashIndex(uint64_t boardData) {
        return (boardData * 0xF1357AEA2E62A9C5ULL) >> (64 - CACHE_BITS);
    }
    // mix(depth, score): original mix(data) = @as(u38, @bitCast(data))
    // Here depth sits in the low 8 bits (the original used only 6, enough), scoreBits in the high 32 bits
    static uint64_t mixCacheData(uint8_t depth, float score) {
        uint32_t scoreBits;
        std::memcpy(&scoreBits, &score, sizeof(float));
        return ((uint64_t)depth & 0x3FULL) | (((uint64_t)scoreBits) << 6);
    }
    __forceinline bool queryCache(const uint64_t* rows, int depth, float& outScore, uint64_t& /*nodes*/, uint64_t& hits) {
        auto& tbl = g_tt.table;
        if (tbl.empty()) return false;
        const uint64_t boardData = boardDataForCache(rows);
        const size_t idx = (size_t)hashIndex(boardData) & (tbl.size() - 1);
        // Fine-grained lock: bucket by idx (4096 mutexes, collision probability 1/4096)
        std::lock_guard<std::mutex> lk(g_tt.locks[idx & LOCK_MASK]);
        const CacheEntry* ve = &tbl[idx];
        const uint64_t keyNow = ve->key;
        if (keyNow == 0) return false;
        const float s = ve->score;
        const uint8_t d = ve->depth;
        const uint64_t mix = mixCacheData(d, s);
        const uint64_t expectedKey = boardData ^ mix;
        if (ve->key != expectedKey) return false;
        if ((int)d < depth) return false;
        outScore = s;
        hits++;
        return true;
    }
    __forceinline void insertCache(const uint64_t* rows, int depth, float score) {
        auto& tbl = g_tt.table;
        if (tbl.empty()) return;
        const uint64_t boardData = boardDataForCache(rows);
        const size_t idx = (size_t)hashIndex(boardData) & (tbl.size() - 1);
        std::lock_guard<std::mutex> lk(g_tt.locks[idx & LOCK_MASK]);
        CacheEntry* ve = &tbl[idx];
        const uint8_t d8 = (uint8_t)(depth & 0x3F);

        // Depth-first replacement (aligned with official ziap):
        //   If the entry is non-empty for the same board (key-validated) and its depth >= the new depth -> do not overwrite.
        //   Deeper entries are more precise and return better scores on future queries.
        if (ve->key != 0) {
            const uint64_t existMix = mixCacheData(ve->depth, ve->score);
            const uint64_t existBoard = ve->key ^ existMix;
            if (existBoard == boardData && (int)ve->depth >= (int)d8) {
                return;  // same board with a more precise result (deeper) -> keep, do not overwrite
            }
        }

        const uint64_t mix = mixCacheData(d8, score);
        const uint64_t newKey = boardData ^ mix;
        ve->score = score;
        ve->depth = d8;
        ve->key = newKey;
    }

    // ===== Encode / decode =====
    void encodeBoard(const BoardMatrix& board) {
        std::memset(boardRows, 0, sizeof(boardRows));
        const int maxRank = (1 << bitsPerCell) - 1;
        for (int r = 0; r < size; ++r) {
            uint64_t row = 0;
            for (int c = 0; c < size; ++c) {
                const int v = board[r][c];
                if (v < 0 || v >= (1 << 28)) {
                    // Filled cell (obstacle/huge tile): encoded as the highest rank.
                    // computeMoveRow already guards with compact[i] < mask -> the highest rank never merges;
                    // ziap expectimax treats it as a huge tile in evaluation and move planning.
                    row |= ((uint64_t)maxRank) << (c * bitsPerCell);
                } else if (v > 0) {
                    int logVal = 0; int tmp = v;
                    while (tmp > 1) { tmp >>= 1; logVal++; }
                    if (logVal > maxRank) logVal = maxRank;
                    row |= ((uint64_t)logVal) << (c * bitsPerCell);
                }
            }
            boardRows[r] = row;
        }
    }
    void decodeBoard(const uint64_t* rows, BoardMatrix& board) const {
        board.resize(size);
        const int mask = (1 << bitsPerCell) - 1;
        for (int r = 0; r < size; ++r) {
            board[r].assign(size, 0);
            for (int c = 0; c < size; ++c) {
                int lv = (int)((rows[r] >> (c*bitsPerCell)) & mask);
                board[r][c] = (lv == 0) ? 0 : (1 << lv);
            }
        }
    }

    uint64_t foldBoard(const uint64_t* rows) const {
        if      (size == 4) return rows[0] | (rows[1] << 16) | (rows[2] << 32) | (rows[3] << 48);
        else if (size == 3) return rows[0] | (rows[1] << 12) | (rows[2] << 24);
        else                return rows[0] ^ (rows[1] << 11) ^ (rows[2] << 22) ^ (rows[3] << 33) ^ (rows[4] << 44);
    }
    void unfoldBoard(uint64_t fold, uint64_t* rows) const {
        if (size == 4) {
            rows[0] = fold & 0xFFFFULL;
            rows[1] = (fold >> 16) & 0xFFFFULL;
            rows[2] = (fold >> 32) & 0xFFFFULL;
            rows[3] = (fold >> 48) & 0xFFFFULL;
            rows[4] = 0;
        } else if (size == 3) {
            rows[0] = fold & 0xFFFULL;
            rows[1] = (fold >> 12) & 0xFFFULL;
            rows[2] = (fold >> 24) & 0xFFFULL;
            rows[3] = 0; rows[4] = 0;
        } else {
            std::memset(rows, 0, sizeof(uint64_t) * 5);
        }
    }
};

// Static members
SolverAI2048::GlobalTT SolverAI2048::g_tt;
SolverAI2048::TableCache SolverAI2048::g_cache;
const SolverAI2048::StaticPowTables SolverAI2048::g_pow;

#endif // SOLVER_AI_2048_H
