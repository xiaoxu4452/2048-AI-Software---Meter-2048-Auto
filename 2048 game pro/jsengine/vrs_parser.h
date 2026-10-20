// ============================================================================
// vrs_parser - lightweight C++ parser for 2048 website .vrs replay files (no JS engine / no duktape)
//   Supports pku2048.cn/replay (Verse new format / PKU v2 extension / PKU v1 base64)
//   and common 2048 website replay formats:
//     1) JSON: {"size":4,"moves":[{"dir":"u","t":120},...]}
//        JSON arrays: [["u",120],["r",80],...] or [[0,120],[1,80],...]
//     2) base64 (2bit/direction packed, 2 bytes = move count, followed by the move bit stream; 2048-Solver URL compatible)
//     3) plain text: one direction per line (w/a/s/d | u/l/d/r | arrows | 0..3)
//   Returns: move sequence {dir: 0=left 1=right 2=up 3=down, t: relative ms} + board size
// ============================================================================
#ifndef VRS_PARSER_H
#define VRS_PARSER_H

#include <string>
#include <vector>

namespace vrs {

struct Move {
    int dir = 0;      // 0=left 1=right 2=up 3=down (same as the software)
    int timeMs = 0;   // relative ms from the start
};

// ===== PKU 2048 (pku2048.cn) native .vrs format =====
//   Verse new format: "{rows}x{cols}-{version}_{3-char events}"
//   PKU v2:      "pku-replay-v2.{mode}{rows}x{cols}_{3-char events}"
//   PKU v1:      "pku-replay-v1.{base64url JSON}"
//   Each event = 3 chars from a 128-char table -> 21-bit packed:
//     bits 0-1: direction (PKU: 0=up 1=down 2=left 3=right)
//     bit 2:    spawned tile value (0=2, 1=4) [pow2 classic mode]
//     bit 3:    direction high bit (diagonal directions, not in classic mode)
//     bits 4-6: spawn column x (0..7)
//     bits 7-9: spawn row y (0..7)
//     bits 10-12: time tier (0..7)
//     bits 13-20: time quantized value (0..255); tier=7 & quant=255 means no time
struct PkuSpawn {
    int value = 0;   // 2 or 4 (classic pow2 mode)
    int x = 0;       // �?
    int y = 0;       // �?
};

struct PkuEvent {
    int dir = -1;          // PKU direction: 0=up 1=down 2=left 3=right; ignored for the initial spawn event
    PkuSpawn spawn;
    int timeMs = -1;       // -1 = no time record
    bool initial = false;  // true = initial spawn event (no move, placed directly)
};

struct PkuReplay {
    int rows = 4;
    int cols = 4;
    std::string mode;          // "classic" / "undo" / "capped" / ...
    std::vector<PkuEvent> events;
    std::string format;        // "verse-new" / "pku-v2" / "pku-v1"
    int initialSpawnCount = 2; // initial spawned tiles (2 in classic mode)
};

// Parse PKU 2048 .vrs content into PkuReplay (with exact spawn positions and times)
// Returns true on success; false on failure with err filled in
bool parsePku(const std::string& content, PkuReplay& replay, std::string& err);

// Decode a 21-bit packed value directly into a PkuEvent (direction/spawn/time, for 2048verse binary format reuse)
// packed layout: bits0-1 direction, bit2 spawn value, bit3 direction high bit,
//   bits4-6 spawn column, bits7-9 spawn row, bits10-12 time tier, bits13-20 time quant
void decodePkuPacked(unsigned int packed, PkuEvent& ev);

// Parse .vrs file content -> move sequence + board size (3/4/5, default 4)
// Returns true on success; false on failure with err filled in
bool parse(const std::string& content, std::vector<Move>& moves,
           int& boardSize, std::string& err);

} // namespace vrs

#endif // VRS_PARSER_H
