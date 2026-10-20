#ifndef ZIG_SOLVER_AI_2048_H
#define ZIG_SOLVER_AI_2048_H

// ============================================================================
// ZigSolverAI2048 - unified AI engine facade
//   4x4 board: Solver4x4 - official ziap architecture, C++ port (MoveTable + Formation + BFS budget +
//              Expectimax TT 2^18; compiled into the exe, no DLL dependency)
//   3x3 board: MacroXue (macroxue/2048-ai, compiled into the exe, best on 3x3);
//              falls back to Solver3x3Fast when the board has blocks/obstacles
//   5x5:      Solver5x5Fast (ziap heuristic + 24-formation protection + Zobrist TT 2^22), compiled into the exe
//   Only one search engine by default (the classic engine was removed; setNewEngine is a no-op kept for compatibility)
//   (pure algorithm software, no endgame tables)
// ============================================================================

#include "SolverAI2048.h"
#include "Solver5x5Fast.h"
#include "Solver3x3Fast.h"
#include "MacroXueSolverAI2048.h"   // 3x3: macroxue/2048-ai (compiled into the exe)
#include "Solver4x4.h"              // 4x4: Solver4x4 (official ziap architecture, C++ port)
#include <mutex>
#include <memory>
#include <string>

class ZigSolverAI2048 {
public:
    using BoardMatrix = std::vector<std::vector<int>>;
    using BoardSize = SolverAI2048::BoardSize;
    using MoveScore = SolverAI2048::MoveScore;
    using Phase = SolverAI2048::Phase;

private:
    // Lazy C++ fallback: only constructed when the board has blocks/obstacles
    std::unique_ptr<SolverAI2048> m_fallback;
    // 5x5 pure C++ solver (thread_local instance, 64MB TT reused forever, zero shared writes)
    std::unique_ptr<Solver5x5Fast> m_solver5x5;
    // 3x3 pure C++ solver (thread_local instance, 64MB TT reused forever, zero shared writes)
    std::unique_ptr<Solver3x3Fast> m_solver3x3;
    // 3x3: MacroXue (macroxue/2048-ai, compiled into the exe; lazy load saves 72MB cache)
    std::unique_ptr<MacroXueSolverAI2048> m_macroxue;
    // 4x4 dedicated engine (official ziap/2048-ai pure C++ port; lazy load saves BFS buffers)
    std::unique_ptr<Solver4x4> m_ziap4;
    int m_size;
    int m_depth = 12;         // intelligence tier (three tiers {3,6,12}, mapped to 4x4 BFS budget / 5x5 node budget)

    SolverAI2048& fallback() {
        if (!m_fallback) m_fallback = std::make_unique<SolverAI2048>(static_cast<SolverAI2048::BoardSize>(m_size));
        return *m_fallback;
    }

    // 5x5 solver lazy load (64MB TT allocated on first use, then reused forever)
    Solver5x5Fast& solver5x5() {
        if (!m_solver5x5) m_solver5x5 = std::make_unique<Solver5x5Fast>();
        return *m_solver5x5;
    }

    // 3x3 solver lazy load (64MB TT allocated on first use, then reused forever)
    Solver3x3Fast& solver3x3() {
        if (!m_solver3x3) m_solver3x3 = std::make_unique<Solver3x3Fast>();
        return *m_solver3x3;
    }

    // 3x3 MacroXue engine lazy load (used in classic search mode; construction includes 3 engines = 72MB cache)
    MacroXueSolverAI2048& macroxue() {
        if (!m_macroxue) {
            m_macroxue = std::make_unique<MacroXueSolverAI2048>();
            m_macroxue->setBoardSize(3);
            m_macroxue->setDepth(m_depth > 8 ? 8 : m_depth);   // MacroXue tiers {3,5,8}
        }
        return *m_macroxue;
    }

    // 4x4 dedicated engine lazy load (official ziap pure C++ port; first use allocates BFS buffers + 16MB TT)
    Solver4x4& ziap4() {
        if (!m_ziap4) {
            m_ziap4 = std::make_unique<Solver4x4>();
            setDepth(m_depth);   // map current tier to 4x4 BFS budget {2^17, 2^19, 2^21}
        }
        return *m_ziap4;
    }

public:
    explicit ZigSolverAI2048(BoardSize sz = BoardSize::STANDARD_4x4)
        : m_size(static_cast<int>(sz)) {
    }

    ~ZigSolverAI2048() {}

    // ===== Main interface: getAllMoveScores =====
    // Board has blocks/obstacles? -> needs the C++ engine that supports obstacle semantics (ziap / MacroXue do not)
    static bool hasFill(const BoardMatrix& board) {
        for (const auto& row : board)
            for (int v : row) if (v < 0 || v >= (1 << 28)) return true;
        return false;
    }

    std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
        // 5x5: Solver5x5Fast (supports blocks too)
        if (m_size == 5) {
            auto r5 = solver5x5().getAllMoveScores(board);
            std::vector<MoveScore> result;
            result.reserve(4);
            for (const auto& ms : r5) {
                MoveScore out;
                out.dir = ms.dir; out.score = ms.score;
                out.allowed = ms.allowed; out.safe = ms.safe;
                result.push_back(out);
            }
            return result;  // already sorted by score descending inside the solver
        }
        if (m_size == 3) {
            // 3x3 always uses MacroXue (clearly better); with blocks MacroXue cannot represent them -> Solver3x3Fast
            if (!hasFill(board)) {
                macroxue().setBoardSize(3);   // 4x4 fallback logic may switch to size 4; switch back explicitly before 3x3 use
                auto r3 = macroxue().getAllMoveScores(board);
                std::vector<MoveScore> result;
                result.reserve(4);
                for (const auto& ms : r3) {
                    MoveScore out;
                    out.dir = ms.dir; out.score = ms.score;
                    out.allowed = ms.allowed; out.safe = ms.safe;
                    result.push_back(out);
                }
                std::sort(result.begin(), result.end(),
                    [](const MoveScore& a, const MoveScore& b) { return a.score > b.score; });
                return result;
            }
            auto r3 = solver3x3().getAllMoveScores(board);
            std::vector<MoveScore> result;
            result.reserve(4);
            for (const auto& ms : r3) {
                MoveScore out;
                out.dir = ms.dir; out.score = ms.score;
                out.allowed = ms.allowed; out.safe = ms.safe;
                result.push_back(out);
            }
            return result;  // already sorted by score descending inside the solver
        }

        // ===== 4x4: Solver4x4 (official ziap architecture, C++ port, the only search engine) =====
        if (m_size == 4) {
            // ziap cannot represent blocks/obstacles -> use the C++ fallback (rank15 no-merge supported)
            if (hasFill(board))
                return fallback().getAllMoveScores(board);
            auto r4 = ziap4().getAllMoveScores(board);
            std::vector<MoveScore> result;
            result.reserve(4);
            // Direction mapping lives in the main program dispatcher (solver4ToCppDir); pass raw engine dir here
            for (const auto& ms : r4)
                result.push_back({ ms.dir, ms.score, ms.safe, ms.allowed });
            return result;  // already sorted by score descending inside the engine
        }

        // Fallback
        return fallback().getAllMoveScores(board);
    }

    // ===== Evaluation function =====
    float evaluateBoard(const BoardMatrix& board) {
        // 3x3 / 5x5: existing in-house engines
        if (m_size == 5) return solver5x5().evaluateBoard(board);
        if (m_size == 3) {
            // 3x3 always uses MacroXue; with blocks -> Solver3x3Fast
            if (!hasFill(board)) { macroxue().setBoardSize(3); return macroxue().evaluateBoard(board); }
            return solver3x3().evaluateBoard(board);
        }
        // 4x4: with blocks/obstacles -> C++ fallback
        if (hasFill(board)) return fallback().evaluateBoard(board);
        // 4x4: Solver4x4 pure C++ heuristic evaluation
        return ziap4().evaluateBoard(board);
    }

    // ===== Proxy methods =====
    void setBoardSize(int newSize) {
        m_size = newSize;
        if (m_fallback) m_fallback->setBoardSize(newSize);
    }

    // ===== Engine control =====
    // Kept for compatibility: only one search engine by default (classic engine removed), this method is a no-op
    void setNewEngine(bool /*use*/) {}

    // Three intelligence tiers:
    //   4x4 ziap -> BFS budget {2^17, 2^19, 2^21} (aligned with the official benchmark)
    //   5x5     -> node budget {2^21, 2^23, 2^24} + time {150, 600, 1200}ms + depth {14, 18, 20}
    //   3x3 MacroXue -> search depth {3,5,8}
    void setDepth(int depth) {
        m_depth = depth;
        if (m_macroxue) m_macroxue->setDepth(depth > 8 ? 8 : depth);
        // 5x5: three tiers of node budget 2^20 / 2^21 / 2^22 + time 90 / 150 / 250ms + depth 12 / 14 / 16
        //      tier 1 = official baseline (aligned with the reference build: TIME_BUDGET_MS=90 / NODE_BUDGET=2^20 / MAX_DEPTH=12),
        //      tiers 2-3 deepen moderately on top to avoid 5x5 lag
        if (m_solver5x5) {
            if (depth <= 3) {
                m_solver5x5->setBudget(1ULL << 20, 90.0);
                m_solver5x5->setMaxDepth(12);
            } else if (depth <= 6) {
                m_solver5x5->setBudget(1ULL << 21, 150.0);
                m_solver5x5->setMaxDepth(14);
            } else {
                m_solver5x5->setBudget(1ULL << 22, 250.0);
                m_solver5x5->setMaxDepth(16);
            }
        }
        // 4x4 engine (Solver4x4): BFS budget {2^17, 2^19, 2^21}
        if (m_ziap4) {
            unsigned int budget = 2097152;
            if (depth <= 3)      budget = 131072;   // 2^17
            else if (depth <= 6) budget = 524288;   // 2^19
            else                 budget = 2097152;  // 2^21
            m_ziap4->setBudget(budget);
        }
    }
    // Clear transposition tables: 4x4 engine clears TT at the start of each game (official behavior: hot in-game cache, no cross-game pollution);
    // MacroXue / 5x5 clear per game to avoid cross-game degradation (Solver5x5Fast's clearTT is a no-op, TT never cleared)
    void clearCache() {
        if (m_macroxue) m_macroxue->clearCache();
        if (m_solver5x5) m_solver5x5->clearTT();
        if (m_ziap4) m_ziap4->clearCache();
    }
    // Reset per game (avoid degradation as games accumulate): clear MacroXue / 4x4 engine caches
    void newGame() {
        if (m_macroxue) m_macroxue->clearCache();
        if (m_solver5x5) m_solver5x5->clearTT();
        setDepth(m_depth);
        if (m_ziap4) m_ziap4->newGame();
    }

    // ===== Runtime switch of the 4x4 engine budget (intelligence tiers 2^17/2^19/2^21) =====
    void setBudget(unsigned int budget) {
        if (m_ziap4) m_ziap4->setBudget(budget);
    }

    std::string getStatsString() {
        if (m_size == 5) return solver5x5().getStatsString();
        if (m_size == 3) return macroxue().getStatsString();   // 3x3 always MacroXue (Solver3x3Fast when blocked)
        // 4x4: Solver4x4 (official ziap architecture, C++ port)
        return "AI[4x4 Solver4x4 (C++: MoveTable+Formation+BFSbudget+ExpectimaxTT2^18)]";
    }

    // ===== Static method proxies =====
    static Phase getPhase(int size, int emptyCount) { return SolverAI2048::getPhase(size, emptyCount); }
    static int countEmptyBoard(const BoardMatrix& board) { return SolverAI2048::countEmptyBoard(board); }
    static int getMaxTile(const BoardMatrix& board) { return SolverAI2048::getMaxTile(board); }
    static bool isBoardValid(const BoardMatrix& board, int sz) { return SolverAI2048::isBoardValid(board, sz); }
    static std::string getEngineInfo() {
        std::string s = "4x4 Solver4x4 (official architecture C++ port, no DLL, reaches 65536)";
        s += " + 3x3 MacroXue/Solver3x3Fast";
        s += " + 5x5 Solver5x5Fast";
        return s;
    }
};

#endif // ZIG_SOLVER_AI_2048_H
