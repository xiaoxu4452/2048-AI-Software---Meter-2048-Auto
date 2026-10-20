#ifndef SOLVER_4X4_H
#define SOLVER_4X4_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <mutex>
#include <algorithm>
#include <bit>

// ============ C++17 compatibility macros ============
#if __cplusplus < 202002L
// C++17 replacement for std::bit_cast
template <typename To, typename From>
inline To bit_cast(const From& src) noexcept {
	static_assert(sizeof(To) == sizeof(From), "size mismatch");
	To dst;
	std::memcpy(&dst, &src, sizeof(To));
	return dst;
}

// C++17 replacement for std::countr_zero
inline int countr_zero(unsigned int x) {
	if (x == 0) return 32;
#if defined(_MSC_VER)
	unsigned long idx;
	_BitScanForward(&idx, x);
	return (int)idx;
#else
	return __builtin_ctz(x);
#endif
}

// C++17 replacement for std::popcount
inline int popcount(uint64_t x) {
#if defined(_MSC_VER)
	return (int)__popcnt64(x);
#else
	return __builtin_popcountll(x);
#endif
}

// Map std:: versions to our implementations via macros
#define std_bit_cast bit_cast
#define std_countr_zero countr_zero
#define std_popcount popcount
#else
#define std_bit_cast std::bit_cast
#define std_countr_zero std::countr_zero
#define std_popcount std::popcount
#endif
// ========================================

class Solver4x4 {
public:
	using BoardMatrix = std::vector<std::vector<int>>;
	
	struct MoveScore {
		int   dir;
		float score;
		bool  safe;
		bool  allowed;
	};
	
	static constexpr float INVALID_SCORE = -1e30f;
	
	Solver4x4() {
		initMoveTable();
		initHeuristic();
		setBudget(1u << 17);
	}
	~Solver4x4() = default;
	
	Solver4x4(const Solver4x4&) = delete;
	Solver4x4& operator=(const Solver4x4&) = delete;
	
	void setBudget(std::size_t budget) {
		std::lock_guard<std::mutex> lk(m_mutex);
		if (budget < 2) budget = 2;
		m_budget = budget;
		m_bfsBuf.assign(budget, 0);
		if (m_radixCount.size() < 65536) m_radixCount.assign(65536, 0);
		if (m_cache.empty()) m_cache.assign(CACHE_SIZE, TEntry{});
		cacheClear();
	}
	
	void newGame()      { std::lock_guard<std::mutex> lk(m_mutex); cacheClear(); }
	void clearCache()   { std::lock_guard<std::mutex> lk(m_mutex); cacheClear(); }
	
	std::vector<MoveScore> getAllMoveScores(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		
		std::vector<MoveScore> out;
		out.reserve(4);
		
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);
		
		float scores[4] = { INVALID_SCORE, INVALID_SCORE, INVALID_SCORE, INVALID_SCORE };
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		
		const int officialOrder[4] = { 2, 0, 3, 1 };
		for (int oi = 0; oi < 4; ++oi) {
			const int d = officialOrder[oi];
			if (arr[d] != data && intact(f, arr[d])) validDirs[nValid++] = d;
		}
		if (nValid == 0) {
			for (int oi = 0; oi < 4; ++oi) {
				const int d = officialOrder[oi];
				if (arr[d] != data) validDirs[nValid++] = d;
			}
		}
		
		if (nValid == 0) {
			for (int d = 0; d < 4; ++d)
				out.push_back({ d, INVALID_SCORE, false, false });
			return out;
		}
		
		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validDirs[i]];
		
		const int depth = bfsExpand(validBoards, nValid) + 1;
		
		for (int i = 0; i < nValid; ++i) {
			const int d = validDirs[i];
			scores[d] = scoreFrontier(validBoards[i], (uint8_t)depth);
		}
		
		for (int d = 0; d < 4; ++d) {
			const bool ok = scores[d] > -1e29f;
			out.push_back({ d, scores[d], ok, ok });
		}
		
		std::sort(out.begin(), out.end(),
			[](const MoveScore& a, const MoveScore& b) { return a.score > b.score; });
		return out;
	}
	
	float evaluateBoard(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		return evaluate(encode(board));
	}
	
	std::vector<float> debugScoreAtDepth(const BoardMatrix& board, int depth) {
		std::lock_guard<std::mutex> lk(m_mutex);
		cacheClear();
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		const int officialOrder[4] = { 2, 0, 3, 1 };
		for (int oi = 0; oi < 4; ++oi) {
			const int d = officialOrder[oi];
			if (arr[d] != data && intact(f, arr[d])) validDirs[nValid++] = d;
		}
		if (nValid == 0)
			for (int oi = 0; oi < 4; ++oi) {
				const int d = officialOrder[oi];
				if (arr[d] != data) validDirs[nValid++] = d;
			}
		std::vector<float> scores(4, INVALID_SCORE);
		for (int i = 0; i < nValid; ++i) {
			const int d = validDirs[i];
			scores[d] = scoreFrontier(arr[d], (uint8_t)depth);
		}
		return scores;
	}
	
	std::vector<float> debugRawScores(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		cacheClear();
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const Formation f = get(data);
		const uint64_t arr[4] = { mv.up, mv.right, mv.down, mv.left };
		int validIdx[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		for (int oi = 0; oi < 4; ++oi) {
			const int d = oi;
			if (arr[d] != data && intact(f, arr[d])) validIdx[nValid++] = d;
		}
		if (nValid == 0)
			for (int oi = 0; oi < 4; ++oi) {
				const int d = oi;
				if (arr[d] != data) validIdx[nValid++] = d;
			}
		std::vector<float> scores(4, INVALID_SCORE);
		if (nValid == 0) return scores;
		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validIdx[i]];
		const int depth = bfsExpand(validBoards, nValid) + 1;
		for (int i = 0; i < nValid; ++i) {
			const int d = validIdx[i];
			scores[d] = scoreFrontier(arr[d], (uint8_t)depth);
		}
		return scores;
	}
	
	int debugDepth(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		const Formation f = get(data);
		int validDirs[4] = { -1, -1, -1, -1 };
		int nValid = 0;
		for (int d = 0; d < 4; ++d)
			if (arr[d] != data && intact(f, arr[d])) validDirs[nValid++] = d;
		if (nValid == 0)
			for (int d = 0; d < 4; ++d)
				if (arr[d] != data) validDirs[nValid++] = d;
		if (nValid == 0) return 0;
		uint64_t validBoards[4];
		for (int i = 0; i < nValid; ++i) validBoards[i] = arr[validDirs[i]];
		return bfsExpand(validBoards, nValid) + 1;
	}
	
	std::vector<BoardMatrix> debugMoveBoards(const BoardMatrix& board) {
		std::lock_guard<std::mutex> lk(m_mutex);
		const uint64_t data = encode(board);
		const Moves mv = getMoves(data);
		const uint64_t arr[4] = { mv.right, mv.left, mv.up, mv.down };
		std::vector<BoardMatrix> out;
		for (int d = 0; d < 4; ++d) {
			BoardMatrix b(4, std::vector<int>(4, 0));
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c) {
					const int rank = (int)((arr[d] >> ((3 - r) * 16 + 4 * c)) & 0xF);
					b[r][c] = (rank == 0) ? 0 : (1 << rank);
				}
			out.push_back(b);
		}
		return out;
	}
	
private:
	static constexpr uint64_t MULT_64 = 0xf1357aea2e62a9c5ULL;
	static constexpr uint64_t LANES   = 0x0F0F0F0F0F0F0F0FULL;
	static constexpr uint64_t ONES    = 0x0101010101010101ULL;
	static constexpr uint64_t GUARD   = 0x8080808080808080ULL;
	static constexpr std::size_t MAX_ROW  = 1ULL << 16;
	static constexpr int  MIN_RANK   = 9;
	static constexpr int  CACHE_BITS = 18;
	static constexpr std::size_t CACHE_SIZE = 1ULL << CACHE_BITS;
	
	static constexpr uint64_t FORMATIONS[24] = {
		0x00000000ff00fff0ULL, 0x0fff00ff00000000ULL, 0x0000000f00ff00ffULL, 0xff00ff00f0000000ULL,
		0xfff0ff0000000000ULL, 0x00ff00ff000f0000ULL, 0x0000f000ff00ff00ULL, 0x0000000000ff0fffULL,
		0xff00ff0000000000ULL, 0x00ff00ff00000000ULL, 0x00000000ff00ff00ULL, 0x0000000000ff00ffULL,
		0xff00f00000000000ULL, 0x00ff000f00000000ULL, 0x00000000f000ff00ULL, 0x00000000000f00ffULL,
		0xff00000000000000ULL, 0x000f000f00000000ULL, 0x00000000f000f000ULL, 0x00000000000000ffULL,
		0x000000000000ff00ULL, 0x00ff000000000000ULL, 0x00000000000f000fULL, 0xf000f00000000000ULL,
	};
	
	struct Formation {
		uint64_t even = 0, odd = 0, addend = 0, layout = 0;
	};
	
	static inline uint64_t atLeast(uint64_t board, int rank) {
		const uint64_t evens = board & LANES;
		const uint64_t odds  = (board >> 4) & LANES;
		const uint64_t addend = (0x80 - (uint64_t)rank) * ONES;
		const uint64_t even_hit = (evens + addend) & GUARD;
		const uint64_t odd_hit  = (odds  + addend) & GUARD;
		const uint64_t even_cells = (even_hit >> 3) - (even_hit >> 7);
		const uint64_t odd_cells  = (odd_hit  >> 3) - (odd_hit  >> 7);
		return even_cells | (odd_cells << 4);
	}
	
	static inline uint64_t largeTiles(uint64_t board) {
		return board & atLeast(board, MIN_RANK);
	}
	
	static inline bool intact(const Formation& f, uint64_t board) {
		if ((f.even | f.odd) == 0) return true;
		const uint64_t evens = board & LANES;
		const uint64_t odds  = (board >> 4) & LANES;
		return ((evens + f.addend) & f.even) == f.even &&
		((odds  + f.addend) & f.odd)  == f.odd;
	}
	
	static inline Formation update(const Formation& f, uint64_t board) {
		return (f.layout == largeTiles(board)) ? f : get(board);
	}
	
	static inline uint64_t maskedValue(uint64_t board, uint64_t cells) {
		uint64_t masked = board & cells;
		uint64_t total = 0;
		for (int i = 0; i < 16; ++i) {
			const uint64_t tile = masked & 0xF;
			masked >>= 4;
			total += (tile != 0) ? (1ULL << tile) : 0ULL;
		}
		return total;
	}
	
	static Formation get(uint64_t board) {
		const uint64_t layout = largeTiles(board);
		const Formation empty{ 0, 0, 0, layout };
		
		uint16_t present = 0, repeated = 0;
		uint64_t data = board;
		for (int i = 0; i < 16; ++i) {
			const uint8_t tile = (uint8_t)(data & 0xF);
			data >>= 4;
			if (tile < MIN_RANK) continue;
			const uint16_t bit = (uint16_t)(1u << tile);
			repeated |= present & bit;
			present  |= bit;
		}
		if (present == 0) return empty;
		
		int cutoff = std::min((int)std_countr_zero((unsigned)present) + 1, 15);
		
		if ((repeated >> cutoff) != 0) return empty;
		
		const uint64_t big    = atLeast(board, cutoff);
		const uint64_t addend = (0x80 - (uint64_t)cutoff) * ONES;
		
		Formation best = empty;
		uint64_t bestValue = 0;
		for (uint64_t cells : FORMATIONS) {
			if ((big & cells) != cells) continue;
			const uint64_t value = maskedValue(board, cells);
			if (value <= bestValue) continue;
			bestValue = value;
			best = { (cells & ONES) << 7, ((cells >> 4) & ONES) << 7, addend, layout };
		}
		return best;
	}
	
	static inline uint64_t emptyPos(uint64_t b) {
		b |= (b >> 2) & 0x3333333333333333ULL;
		b |= (b >> 1);
		return ~b & 0x1111111111111111ULL;
	}
	
	static inline uint64_t transpose(uint64_t x) {
		uint64_t b = (x ^ (x >> 12)) & 0x0000f0f00000f0f0ULL;
		x ^= b ^ (b << 12);
		b = (x ^ (x >> 24)) & 0x00000000ff00ff00ULL;
		x ^= b ^ (b << 24);
		return x;
	}
	
	static inline uint16_t reverse16(uint16_t x) {
		return (uint16_t)((x >> 12) | ((x >> 4) & 0x00f0) | ((x << 4) & 0x0f00) | (x << 12));
	}
	
	static inline uint64_t hashBits(uint64_t data, int bits) {
		return (data * MULT_64) >> (64 - bits);
	}
	
	uint16_t m_forward[MAX_ROW];
	uint16_t m_reverse[MAX_ROW];
	
	static void moveLine(uint8_t* line) {
		int furthest = 4;
		bool merged = false;
		for (int i = 3; i >= 0; --i) {
			if (line[i] == 0) continue;
			if (!merged && furthest < 4 && line[i] == line[furthest]) {
				line[furthest] = (uint8_t)std::min(line[furthest] + 1, 15);   // u4 saturation: rank15+15 -> 15
				line[i] = 0;
				merged = true;
			} else if (furthest == i + 1) {
				furthest = i;
			} else {
				furthest -= 1;
				line[furthest] = line[i];
				line[i] = 0;
				merged = false;
			}
		}
	}
	
	void initMoveTable() {
		for (std::size_t row = 0; row < MAX_ROW; ++row) {
			uint8_t line[4] = {
				(uint8_t)(row & 0xF),
				(uint8_t)((row >> 4) & 0xF),
				(uint8_t)((row >> 8) & 0xF),
				(uint8_t)((row >> 12) & 0xF),
			};
			moveLine(line);
			const uint16_t moved = (uint16_t)(line[0] | (line[1] << 4) | (line[2] << 8) | (line[3] << 12));
			m_forward[(uint16_t)row] = moved;
			m_reverse[reverse16((uint16_t)row)] = reverse16(moved);
		}
	}
	
	struct Moves { uint64_t up, right, down, left; };
	
	__forceinline Moves getMoves(uint64_t data) const {
		const uint64_t dataT = transpose(data);
		uint64_t up = 0, right = 0, down = 0, left = 0;
		for (int idx = 0; idx < 4; ++idx) {
			const int shift = (3 - idx) * 16;
			const uint16_t row = (uint16_t)(data >> shift);
			const uint16_t col = (uint16_t)(dataT >> shift);
			up    <<= 16; right <<= 16; down <<= 16; left <<= 16;
			up    |= m_forward[col];
			down  |= m_reverse[col];
			left  |= m_forward[row];
			right |= m_reverse[row];
		}
		up = transpose(up);
		down = transpose(down);
		return { up, right, down, left };
	}
	
	float m_scoreTable[MAX_ROW];
	float m_sumPow[16];
	float m_monoPow[16];
	
	void initHeuristic() {
		m_sumPow[0] = 0; m_monoPow[0] = 0;
		for (int idx = 1; idx < 16; ++idx) {
			const float fidx = (float)idx;
			m_sumPow[idx]  = std::exp2(std::log2(fidx) * 3.5f);
			m_monoPow[idx] = std::exp2(std::log2(fidx) * 4.0f);
		}
		
		for (std::size_t row = 0; row < MAX_ROW; ++row) {
			const uint8_t line[4] = {
				(uint8_t)(row & 0xF),
				(uint8_t)((row >> 4) & 0xF),
				(uint8_t)((row >> 8) & 0xF),
				(uint8_t)((row >> 12) & 0xF),
			};
			
			float sum = 0;
			int   empty = 0, merges = 0;
			int   prev = 0, counter = 0;
			
			for (int i = 0; i < 4; ++i) {
				const int rank = line[i];
				sum += m_sumPow[rank];
				if (rank == 0) {
					empty += 1;
				} else {
					if (prev == rank) {
						counter += 1;
					} else if (counter > 0) {
						merges += 1 + counter;
						counter = 0;
					}
					prev = rank;
				}
			}
			if (counter > 0) merges += 1 + counter;
			
			float monoLeft = 0, monoRight = 0;
			for (int i = 1; i < 4; ++i) {
				const float l = m_monoPow[line[i - 1]];
				const float r = m_monoPow[line[i]];
				if (line[i - 1] > line[i]) monoLeft  += l - r;
				else                      monoRight += r - l;
			}
			
			m_scoreTable[row] = 200000.0f
			+ 270.0f * (float)empty
			+ 700.0f * (float)merges
			- 47.0f  * std::min(monoLeft, monoRight)
			- 11.0f  * sum;
		}
	}
	
	__forceinline float evaluate(uint64_t data) const {
		const uint64_t dataT = transpose(data);
		float score = 0;
		for (int idx = 0; idx < 4; ++idx) {
			const int shift = idx * 16;
			const uint16_t row = (uint16_t)(data >> shift);
			const uint16_t col = (uint16_t)(dataT >> shift);
			score += m_scoreTable[row] + m_scoreTable[col];
		}
		return score;
	}
	
	struct TEntry {
		uint64_t key;
		uint64_t data;
	};
	std::vector<TEntry> m_cache;
	
	static inline uint64_t mix(uint8_t depth, float score) {
		// Reinterpret the bits of a float as uint32_t
		uint32_t scoreBits;
		std::memcpy(&scoreBits, &score, sizeof(score));
		const uint64_t m = (uint64_t)depth | ((uint64_t)scoreBits << 6);
		return m & 0x3FFFFFFFFFULL;
	}
	
	__forceinline void cacheClear() {
		std::memset(m_cache.data(), 0, m_cache.size() * sizeof(TEntry));
	}
	
	__forceinline void cacheInsert(uint64_t board, uint8_t depth, float score) {
		const std::size_t idx = (std::size_t)hashBits(board, CACHE_BITS);
		const uint64_t m = mix(depth, score);
		m_cache[idx].key  = board ^ m;
		m_cache[idx].data = m;
	}
	
	__forceinline bool cacheQuery(uint64_t board, uint8_t depth, float& out) const {
		const std::size_t idx = (std::size_t)hashBits(board, CACHE_BITS);
		const uint64_t key  = m_cache[idx].key;
		const uint64_t data = m_cache[idx].data;
		if ((key ^ data) != board) return false;
		const uint8_t d = (uint8_t)(data & 0x3F);
		if (d < depth) return false;
		const uint32_t scoreBits = (uint32_t)((data >> 6) & 0xFFFFFFFF);
		std::memcpy(&out, &scoreBits, sizeof(out));
		return true;
	}
	
	std::vector<uint64_t> m_bfsBuf;
	std::vector<uint32_t> m_radixCount;
	std::size_t m_budget = 0;
	
	__forceinline static void radixSort(uint64_t* src, uint64_t* dst,
		std::size_t len, uint32_t* count) {
			for (int pass = 0; pass < 2; ++pass) {
				std::memset(count, 0, 65536 * sizeof(uint32_t));
				const uint32_t shift = (uint32_t)pass * 16;
				for (std::size_t i = 0; i < len; ++i)
					++count[(((uint32_t)hashBits(src[i], 32)) >> shift) & 0xFFFF];
				uint32_t acc = 0;
				for (int i = 0; i < 65536; ++i) {
					const uint32_t old = acc;
					acc += count[i];
					count[i] = old;
				}
				for (std::size_t i = 0; i < len; ++i) {
					const uint32_t radix = (((uint32_t)hashBits(src[i], 32)) >> shift) & 0xFFFF;
					dst[count[radix]] = src[i];
					++count[radix];
				}
				std::swap(src, dst);
			}
		}
	
	int bfsExpand(const uint64_t* initial, int initialLen) {
		const std::size_t half = m_budget / 2;
		if (half == 0) return 0;
		const std::size_t cap = std::min((std::size_t)initialLen, half);
		uint64_t* cur = m_bfsBuf.data();
		uint64_t* nxt = m_bfsBuf.data() + half;
		uint32_t* count = m_radixCount.data();
		
		std::memcpy(cur, initial, cap * sizeof(uint64_t));
		std::size_t curLen = cap;
		int depth = 0;
		std::size_t prevLen = 1;
		const int MAX_DEPTH = 63;
		
		while (curLen > 0 && depth < MAX_DEPTH) {
			const uint64_t est = (uint64_t)curLen * curLen / prevLen;
			if (est > (uint64_t)half * 2) break;
			
			radixSort(cur, nxt, curLen, count);
			
			std::size_t nextLen = 0;
			uint64_t last = 0;
			bool full = false;
			
			for (std::size_t i = 0; i < curLen; ++i) {
				const uint64_t nb = cur[i];
				if (last == nb) continue;
				last = nb;
				
				const Formation f = get(nb);
				uint64_t mask = emptyPos(nb);
				while (mask != 0) {
					const uint64_t tile = mask & (~mask + 1);
					mask ^= tile;
					const uint64_t sp2 = last | tile;
					const uint64_t sp4 = last | (tile << 1);
					
					const Moves m2 = getMoves(sp2);
					const Moves m4 = getMoves(sp4);
					const uint64_t a2[4] = { m2.up, m2.right, m2.down, m2.left };
					const uint64_t a4[4] = { m4.up, m4.right, m4.down, m4.left };
					
					for (int k = 0; k < 4; ++k) {
						if (a2[k] != sp2 && intact(f, a2[k])) {
							if (nextLen == half) { full = true; break; }
							nxt[nextLen++] = a2[k];
						}
						if (a4[k] != sp4 && intact(f, a4[k])) {
							if (nextLen == half) { full = true; break; }
							nxt[nextLen++] = a4[k];
						}
					}
					if (full) break;
				}
				if (full) break;
			}
			if (full) break;
			
			std::swap(cur, nxt);
			prevLen = curLen;
			curLen = nextLen;
			depth += 1;
		}
		
		return depth;
	}
	
	std::mutex m_mutex;
	
	__forceinline float expectNode(uint64_t board, uint8_t depth, const Formation& f) {
		if (depth == 0) return evaluate(board);
		
		float cached = 0;
		if (cacheQuery(board, depth, cached)) return cached;
		
		uint64_t mask = emptyPos(board);
		const float total = (float)std_popcount(mask);
		const float w2 = 0.9f / total;
		const float w4 = 0.1f / total;
		float score = 0;
		
		const Formation nf = update(f, board);
		
		while (mask != 0) {
			const uint64_t tile = mask & (~mask + 1);
			mask ^= tile;
			score += w2 * maxNode(board | tile, depth, nf);
			score += w4 * maxNode(board | (tile << 1), depth, nf);
		}
		
		cacheInsert(board, depth, score);
		return score;
	}
	
	__forceinline float maxNode(uint64_t board, uint8_t depth, const Formation& f) {
		const Moves m = getMoves(board);
		const uint64_t arr[4] = { m.up, m.right, m.down, m.left };
		float maxScore = 0;
		for (int k = 0; k < 4; ++k) {
			const uint64_t nb = arr[k];
			if (nb != board && intact(f, nb))
				maxScore = std::max(maxScore, expectNode(nb, (uint8_t)(depth - 1), f));
		}
		return maxScore;
	}
	
	__forceinline float scoreFrontier(uint64_t board, uint8_t depth) {
		return expectNode(board, depth, get(board));
	}
	
	static uint64_t encode(const BoardMatrix& board) {
		uint64_t data = 0;
		for (int r = 0; r < 4; ++r) {
			for (int c = 0; c < 4; ++c) {
				int v = board[r][c];
				int logVal = 0;
				if (v <= 0) {
					logVal = 0;
				} else if (v >= (1 << 28)) {
					logVal = 15;
				} else {
					while (v > 1) { v >>= 1; ++logVal; }
					if (logVal > 15) logVal = 15;   // 65536+ -> rank15
				}
				data |= (uint64_t)logVal << ((3 - r) * 16 + 4 * c);
			}
		}
		return data;
	}
};

// Clean up macros to avoid polluting the global namespace
#undef std_bit_cast
#undef std_countr_zero
#undef std_popcount

#endif
