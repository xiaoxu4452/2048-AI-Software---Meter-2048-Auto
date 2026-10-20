// ============================================================================
// vrs_parser.cpp - lightweight C++ .vrs replay parser (no duktape / JS engine dependency)
//   Supports pku2048.cn/replay and common 2048 website replay formats:
//     1) JSON:  {"size":4,"moves":[{"dir":"u","t":120},...]}
//              {"moves":["u","r",...]} or {"steps":[[0,120],...]}
//              JSON arrays: [[0,120],[1,80],...] or [0,1,2,3,...] or ["u","r",...]
//     2) base64: 2 little-endian bytes = move count, then 2 bits per direction (2048-Solver / PKU compatible)
//     3) plain text: one direction per line (w/a/s/d | u/l/d/r | 0..3 | Chinese | arrows), compact strings supported
//   Directions: 0=left 1=right 2=up 3=down (same as the software)
// ============================================================================
#include "vrs_parser.h"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

namespace vrs {

// ============================ Lightweight JSON parser ============================
namespace json {

struct Value;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object_ } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    Object obj;
};

class Parser {
public:
    explicit Parser(const std::string& s) : s(s) {}
    bool parse(Value& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return true;   // allow trailing content (fault tolerant)
    }
private:
    const std::string s;   // holds a string copy (avoids dangling references to temporaries)
    int pos = 0;
    void skipWs() { while (pos < (int)s.size() && isspace((unsigned char)s[pos])) pos++; }
    bool parseValue(Value& v) {
        skipWs();
        if (pos >= (int)s.size()) return false;
        char c = s[pos];
        if (c == '{') return parseObject(v);
        if (c == '[') return parseArray(v);
        if (c == '"') return parseString(v);
        if (c == 't' || c == 'f') return parseBool(v);
        if (c == 'n') { if (s.compare(pos, 4, "null") == 0) { pos += 4; v.type = Value::Null; return true; } return false; }
        return parseNumber(v);
    }
    bool parseObject(Value& v) {
        v.type = Value::Object_; pos++;   // '{'
        skipWs();
        if (pos < (int)s.size() && s[pos] == '}') { pos++; return true; }
        while (true) {
            skipWs();
            std::string key;
            if (!parseStringRaw(key)) return false;
            skipWs();
            if (pos >= (int)s.size() || s[pos] != ':') return false;
            pos++;
            Value val;
            if (!parseValue(val)) return false;
            v.obj.emplace_back(key, std::move(val));
            skipWs();
            if (pos >= (int)s.size()) return false;
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == '}') { pos++; return true; }
            return false;
        }
    }
    bool parseArray(Value& v) {
        v.type = Value::Array; pos++;     // '['
        skipWs();
        if (pos < (int)s.size() && s[pos] == ']') { pos++; return true; }
        while (true) {
            skipWs();
            Value val;
            if (!parseValue(val)) return false;
            v.arr.push_back(std::move(val));
            skipWs();
            if (pos >= (int)s.size()) return false;
            if (s[pos] == ',') { pos++; continue; }
            if (s[pos] == ']') { pos++; return true; }
            return false;
        }
    }
    bool parseStringRaw(std::string& out) {
        skipWs();
        if (pos >= (int)s.size() || s[pos] != '"') return false;
        pos++;
        out.clear();
        while (pos < (int)s.size()) {
            char c = s[pos];
            if (c == '"') { pos++; return true; }
            if (c == '\\') {
                pos++;
                if (pos >= (int)s.size()) return false;
                char e = s[pos];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (pos + 4 >= (int)s.size()) return false;
                        unsigned int code = 0;
                        for (int k = 0; k < 4; ++k) {
                            int d = hexVal(s[pos + 1 + k]);
                            if (d < 0) return false;
                            code = code * 16 + (unsigned int)d;
                        }
                        pos += 4;
                        if (code < 0x80) out += (char)code;
                        else if (code < 0x800) {
                            out += (char)(0xC0 | (code >> 6));
                            out += (char)(0x80 | (code & 0x3F));
                        } else {
                            out += (char)(0xE0 | (code >> 12));
                            out += (char)(0x80 | ((code >> 6) & 0x3F));
                            out += (char)(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: return false;
                }
                pos++;
            } else { out += c; pos++; }
        }
        return false;
    }
    bool parseString(Value& v) { v.type = Value::String; return parseStringRaw(v.str); }
    bool parseBool(Value& v) {
        if (s.compare(pos, 4, "true") == 0) { pos += 4; v.type = Value::Bool; v.b = true; return true; }
        if (s.compare(pos, 5, "false") == 0) { pos += 5; v.type = Value::Bool; v.b = false; return true; }
        return false;
    }
    bool parseNumber(Value& v) {
        int start = pos;
        if (pos < (int)s.size() && (s[pos] == '-' || s[pos] == '+')) pos++;
        bool digit = false;
        while (pos < (int)s.size() && isdigit((unsigned char)s[pos])) { pos++; digit = true; }
        if (pos < (int)s.size() && s[pos] == '.') {
            pos++;
            while (pos < (int)s.size() && isdigit((unsigned char)s[pos])) { pos++; digit = true; }
        }
        if (pos < (int)s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < (int)s.size() && (s[pos] == '-' || s[pos] == '+')) pos++;
            while (pos < (int)s.size() && isdigit((unsigned char)s[pos])) pos++;
        }
        if (!digit) return false;
        v.type = Value::Number;
        v.num = atof(s.substr(start, pos - start).c_str());
        return true;
    }
    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
};

} // namespace json

// ============================ Direction mapping ============================
// Returns 0=left 1=right 2=up 3=down; -1 if unrecognized
static int mapDir(const json::Value& v)
{
    if (v.type == json::Value::Number) return ((int)v.num % 4 + 4) % 4;
    if (v.type == json::Value::String) {
        std::string c = v.str;
        // strip whitespace, lowercase
        std::string t;
        for (char ch : c) if (!isspace((unsigned char)ch)) t += (char)tolower((unsigned char)ch);
        if (t == "l" || t == "a" || t == "left") return 0;
        if (t == "r" || t == "d" || t == "right") return 1;
        if (t == "u" || t == "w" || t == "up") return 2;
        if (t == "d" || t == "s" || t == "down") return 3;
        if (t == "上" || t == "up" || t == "\xe4\xb8\x8a") return 2;   // up
        if (t == "下" || t == "\xe4\xb8\x8b") return 3;                // down
        if (t == "左" || t == "\xe5\xb7\xa6") return 0;                // left
        if (t == "右" || t == "\xe5\x8f\xb3") return 1;                // right
        if (t == "\xe2\x86\x91") return 2;   // ↑
        if (t == "\xe2\x86\x93") return 3;   // ↓
        if (t == "\xe2\x86\x90") return 0;   // ←
        if (t == "\xe2\x86\x92") return 1;   // →
        if (!t.empty() && isdigit((unsigned char)t[0])) {
            char* end = nullptr;
            long n = strtol(t.c_str(), &end, 10);
            if (end && *end == 0) return (int)((n % 4 + 4) % 4);
        }
        return -1;
    }
    return -1;
}

// Append one directional move
static void pushMove(std::vector<Move>& moves, int dir, double tms)
{
    if (dir < 0 || dir > 3) return;
    Move m;
    m.dir = dir;
    m.timeMs = (tms > 0 && tms < 1e9) ? (int)tms : 0;
    moves.push_back(m);
}

// Extract a move from a JSON array element (multiple representations supported)
static void extractJsonMove(const json::Value& m, std::vector<Move>& moves)
{
    if (m.type == json::Value::Number) { pushMove(moves, (int)m.num, 0); return; }
    if (m.type == json::Value::String) { pushMove(moves, mapDir(m), 0); return; }
    if (m.type == json::Value::Array) {
        // [dir, t] or ["u", t]
        if (!m.arr.empty()) {
            int d = mapDir(m.arr[0]);
            double t = 0;
            if (m.arr.size() > 1 && m.arr[1].type == json::Value::Number) t = m.arr[1].num;
            pushMove(moves, d, t);
        }
        return;
    }
    if (m.type == json::Value::Object_) {
        int d = -1;
        double t = 0;
        for (const auto& kv : m.obj) {
            if (kv.first == "dir" || kv.first == "d" || kv.first == "direction")
                d = mapDir(kv.second);
            else if (kv.first == "t" || kv.first == "ms" || kv.first == "time" || kv.first == "dt")
                if (kv.second.type == json::Value::Number) t = kv.second.num;
        }
        pushMove(moves, d, t);
    }
}

// Attempt JSON parsing
static bool tryParseJson(const std::string& content, std::vector<Move>& moves, int& boardSize)
{
    std::string s = content;
    // strip BOM
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s = s.substr(3);

    // locate the first JSON start character (tolerates surrounding junk text)
    size_t brace = s.find('{');
    size_t bracket = s.find('[');
    size_t start;
    if (brace == std::string::npos && bracket == std::string::npos) return false;
    if (brace == std::string::npos) start = bracket;
    else if (bracket == std::string::npos) start = brace;
    else start = std::min(brace, bracket);

    json::Value root;
    json::Parser parser(s.substr(start));
    if (!parser.parse(root)) return false;

    if (root.type == json::Value::Array) {
        for (const auto& m : root.arr) extractJsonMove(m, moves);
        return !moves.empty();
    }
    if (root.type == json::Value::Object_) {
        const json::Value* movesVal = nullptr;
        for (const auto& kv : root.obj) {
            if (kv.first == "size") {
                if (kv.second.type == json::Value::Number) {
                    int sz = (int)kv.second.num;
                    if (sz == 3 || sz == 5) boardSize = sz;
                }
            } else if (kv.first == "moves" || kv.first == "steps" || kv.first == "data" || kv.first == "ops" || kv.first == "movesList") {
                movesVal = &kv.second;
            }
        }
        if (!movesVal) return false;
        // moves may be an array, a string (JSON or direction string)
        if (movesVal->type == json::Value::Array) {
            for (const auto& m : movesVal->arr) extractJsonMove(m, moves);
            return !moves.empty();
        }
        if (movesVal->type == json::Value::String) {
            // try parsing the string as a JSON array
            const std::string& ms = movesVal->str;
            if (!ms.empty() && (ms[0] == '[' || ms[0] == '{')) {
                json::Value sub;
                json::Parser sp(ms);
                if (sp.parse(sub)) {
                    if (sub.type == json::Value::Array) {
                        for (const auto& m : sub.arr) extractJsonMove(m, moves);
                        return !moves.empty();
                    }
                    if (sub.type == json::Value::Object_) {
                        for (const auto& kv : sub.obj)
                            if (kv.first == "moves" || kv.first == "steps" || kv.first == "data") {
                                if (kv.second.type == json::Value::Array)
                                    for (const auto& m : kv.second.arr) extractJsonMove(m, moves);
                            }
                        return !moves.empty();
                    }
                }
            }
            // treat as a direction string (compact text)
            for (char ch : ms) {
                if (isspace((unsigned char)ch) || ch == ',' || ch == ';') continue;
                std::string one(1, ch);
                json::Value vv; vv.type = json::Value::String; vv.str = one;
                pushMove(moves, mapDir(vv), 0);
            }
            return !moves.empty();
        }
    }
    return false;
}

// ============================ base64 ============================
static bool base64Decode(const std::string& in, std::vector<unsigned char>& out)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val[256];
    for (int i = 0; i < 256; ++i) val[i] = -1;
    for (int i = 0; i < 64; ++i) val[(unsigned char)tbl[i]] = i;
    int buf = 0, bits = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || isspace(c)) continue;
        int v = val[c];
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buf >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// 2048-Solver / PKU style: 2 little-endian bytes = move count, then 2 bits per direction
static bool tryParseBase64(const std::string& content, std::vector<Move>& moves)
{
    std::string b64;
    for (char c : content) if (!isspace((unsigned char)c)) b64 += c;
    if (b64.size() < 2) return false;
    // only try when the whole chunk is base64 characters
    for (char c : b64) {
        if (!isalnum((unsigned char)c) && c != '+' && c != '/' && c != '=') return false;
    }
    std::vector<unsigned char> bin;
    if (!base64Decode(b64, bin)) return false;
    if (bin.size() < 2) return false;
    int n = bin[0] | (bin[1] << 8);
    if (n <= 0 || n > 100000) return false;
    for (int i = 0; i < n; ++i) {
        int byteI = 2 + ((i * 2) >> 3);
        if (byteI >= (int)bin.size()) break;
        int shift = (i * 2) & 7;
        int d = (bin[byteI] >> shift) & 3;
        pushMove(moves, d, 0);
    }
    return !moves.empty();
}

// ============================ Plain text ============================
static bool tryParseText(const std::string& content, std::vector<Move>& moves)
{
    // split by lines / commas / semicolons
    std::string seg;
    for (size_t i = 0; i <= content.size(); ++i) {
        char c = (i < content.size()) ? content[i] : '\n';
        if (c == '\r' || c == '\n' || c == ',' || c == ';' || i == content.size()) {
            if (!seg.empty()) {
                // the whole chunk may be a single numeric direction
                json::Value vv; vv.type = json::Value::String; vv.str = seg;
                if (mapDir(vv) >= 0) {
                    pushMove(moves, mapDir(vv), 0);
                } else {
                    // every character in the chunk is one direction (compact string, e.g. "wwssaadd")
                    for (char ch : seg) {
                        std::string one(1, ch);
                        json::Value v2; v2.type = json::Value::String; v2.str = one;
                        pushMove(moves, mapDir(v2), 0);
                    }
                }
                seg.clear();
            }
        } else {
            seg += c;
        }
    }
    return !moves.empty();
}

// ============================ PKU 2048 native .vrs parsing ============================
// Fixed 128-char table (identical to pku2048.cn /game/replay.js)
static const char* PKU_CHARS =
    "0123456789"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "\xC0\xC1\xC2\xC3\xC4\xC5\xC6"       // ÀÁÂÃÄÅÆ
    "\xC7\xC8\xC9\xCA\xCB\xCC\xCD\xCE\xCF" // ÇÈÉÊËÌÍÎÏ
    "\xD0\xD1\xD2\xD3\xD4\xD5\xD6\xD7\xD8\xD9\xDA\xDB\xDC\xDD\xDE\xDF" // ÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞß
    "\xE0\xE1\xE2\xE3\xE4\xE5\xE6\xE7\xE8\xE9\xEA\xEB\xEC\xED\xEE\xEF" // àáâãäåæçèéêëìíîï
    "\xF0\xF1\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9\xFA\xFB\xFC\xFD\xFE\xFF" // ðñòóôõö÷øùúûüýþÿ
    "\xA4\xBE";                            // ¤¾

// PKU table index lookup: accepts UTF-8 chars (1 or 2 bytes) or Latin-1 single bytes, decoded to Latin-1 code points for lookup
static int pkuCharIndex(const std::string& ch)
{
    if (ch.empty()) return -1;
    unsigned char c0 = (unsigned char)ch[0];
    int codepoint;
    if (c0 < 0x80) {
        codepoint = c0;
    } else if ((c0 & 0xE0) == 0xC0 && ch.size() >= 2) {
        unsigned char c1 = (unsigned char)ch[1];
        if ((c1 & 0xC0) != 0x80) return -1;
        codepoint = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
    } else if (c0 >= 0x80 && c0 <= 0xFF) {
        // Latin-1 single byte (the file may be ISO-8859-1 encoded)
        codepoint = c0;
    } else {
        return -1;
    }
    if (codepoint >= '0' && codepoint <= '9') return codepoint - '0';
    if (codepoint >= 'a' && codepoint <= 'z') return 10 + (codepoint - 'a');
    if (codepoint >= 'A' && codepoint <= 'Z') return 36 + (codepoint - 'A');
    if (codepoint >= 0xC0 && codepoint <= 0xC6) return 62 + (codepoint - 0xC0);
    if (codepoint >= 0xC7 && codepoint <= 0xCF) return 69 + (codepoint - 0xC7);
    if (codepoint >= 0xD0 && codepoint <= 0xDF) return 78 + (codepoint - 0xD0);
    if (codepoint >= 0xE0 && codepoint <= 0xEF) return 94 + (codepoint - 0xE0);
    if (codepoint >= 0xF0 && codepoint <= 0xFF) return 110 + (codepoint - 0xF0);
    if (codepoint == 0xA4) return 126;
    if (codepoint == 0xBE) return 127;
    return -1;
}

// Time tier table (same as pku2048.cn)
//   value(tier, quant) = BASE[tier] + AMT[tier] * quant  (quant = 0..255)
//   BASE[tier] = MAX[tier] - 255 * AMT[tier] (derived from the MAX table so quant=255 maps exactly to MAX)
static const int PKU_TIME_AMT[8]  = { 1, 3, 12, 48, 192, 768, 3072, 12288 };
static const int PKU_TIME_MAX[8]  = { 255, 1021, 4084, 16336, 65344, 265966, 1050094, 4186606 };
static const int PKU_TIME_BASE[8] = { 0, 256, 1024, 4096, 16384, 70126, 266734, 1053166 };

// Decode a 21-bit packed value directly into a PkuEvent (direction/spawn/time)
// packed layout: bits0-1 direction, bit2 spawn value, bit3 direction high bit,
//   bits4-6 spawn column, bits7-9 spawn row, bits10-12 time tier, bits13-20 time quant
void decodePkuPacked(unsigned int packed, PkuEvent& ev)
{
    // direction (classic mode: low 2 bits, bit3=0)
    ev.dir = packed & 3;

    // spawn tile: bit2 -> 0=2, 1=4
    ev.spawn.value = ((packed >> 2) & 1) ? 4 : 2;
    ev.spawn.x = (packed >> 4) & 7;
    ev.spawn.y = (packed >> 7) & 7;

    // time
    int tier = (packed >> 10) & 7;
    int quant = (packed >> 13) & 255;
    if (tier == 7 && quant == 255) {
        ev.timeMs = -1;
    } else {
        ev.timeMs = PKU_TIME_BASE[tier] + PKU_TIME_AMT[tier] * quant;
    }
}

// Decode a 3-char event -> 21-bit packed -> PkuEvent (classic pow2 mode)
// Each ch is a UTF-8 char (1 or 2 bytes) or a Latin-1 single byte
static bool pkuDecodeEvent(const std::string& ch0, const std::string& ch1, const std::string& ch2, PkuEvent& ev)
{
    int i0 = pkuCharIndex(ch0);
    int i1 = pkuCharIndex(ch1);
    int i2 = pkuCharIndex(ch2);
    if (i0 < 0 || i1 < 0 || i2 < 0) return false;
    unsigned int packed = (i0 << 14) | (i1 << 7) | i2;
    decodePkuPacked(packed, ev);
    return true;
}

// Split a string by characters: supports UTF-8 (Latin-1 extended chars are 2 bytes) and Latin-1 (single byte)
// Returns the raw byte string of each character (1 or 2 bytes)
static std::vector<std::string> pkuSplitChars(const std::string& s)
{
    std::vector<std::string> chars;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            chars.push_back(std::string(1, (char)c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            unsigned char c1 = (unsigned char)s[i + 1];
            if ((c1 & 0xC0) == 0x80) {
                // valid 2-byte UTF-8
                chars.push_back(s.substr(i, 2));
                i += 2;
            } else {
                // non-UTF-8 continuation byte -> treat as a Latin-1 single byte
                chars.push_back(std::string(1, (char)c));
                ++i;
            }
        } else {
            // Latin-1 single byte (0x80-0xFF but not a UTF-8 lead byte)
            chars.push_back(std::string(1, (char)c));
            ++i;
        }
    }
    return chars;
}

// Decode the event body (3 chars per group)
static bool pkuDecodeEvents(const std::string& body, int initialCount,
                             std::vector<PkuEvent>& events, std::string& err)
{
    std::vector<std::string> chars = pkuSplitChars(body);
    if (chars.size() % 3 != 0) {
        err = "replay event body length is not a multiple of 3 (file may be corrupt)";
        return false;
    }
    events.clear();
    events.reserve(chars.size() / 3);
    for (size_t i = 0; i < chars.size(); i += 3) {
        PkuEvent ev;
        if (!pkuDecodeEvent(chars[i], chars[i + 1], chars[i + 2], ev)) {
            err = "replay event contains unrecognized characters";
            return false;
        }
        ev.initial = ((int)(i / 3) < initialCount);
        events.push_back(ev);
    }
    if ((int)events.size() < initialCount) {
        err = "too few replay events (below the initial spawn count)";
        return false;
    }
    return true;
}

// base64url decode (for PKU v1)
static bool pkuBase64UrlDecode(const std::string& in, std::string& out)
{
    static const int tbl[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    int buf = 0, bits = 0;
    out.clear();
    for (char ch : in) {
        if (ch == '=' || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
        // base64url: - → +, _ → /
        unsigned char c = (unsigned char)ch;
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        int v = tbl[c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// Extract events from PKU v1 JSON (simplified: find move/spawn inside the events array)
// PKU v1 JSON structure: {config:{rows,cols,mode,...}, events:[{move,spawn:{value,x,y},ms},...]}
static bool pkuParseV1Json(const std::string& json, PkuReplay& replay, std::string& err)
{
    // extract rows/cols
    auto findInt = [&](const char* key, int& val) -> bool {
        std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return false;
        p = json.find(':', p + k.size());
        if (p == std::string::npos) return false;
        ++p;
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
        if (p >= json.size() || !isdigit((unsigned char)json[p])) return false;
        val = 0;
        while (p < json.size() && isdigit((unsigned char)json[p])) { val = val * 10 + (json[p] - '0'); ++p; }
        return true;
    };
    int rows = 4, cols = 4;
    findInt("rows", rows);
    findInt("cols", cols);
    replay.rows = rows;
    replay.cols = cols;
    replay.mode = "classic";
    replay.format = "pku-v1";
    replay.initialSpawnCount = 2;

    // extract the events array (simplified parsing: find each {move,spawn,ms})
    size_t evStart = json.find("\"events\"");
    if (evStart == std::string::npos) { err = "PKU v1 JSON has no events field"; return false; }
    size_t arrStart = json.find('[', evStart);
    if (arrStart == std::string::npos) { err = "PKU v1 events is not an array"; return false; }
    size_t pos = arrStart + 1;
    int idx = 0;
    while (pos < json.size()) {
        // find the next {
        size_t objStart = json.find('{', pos);
        if (objStart == std::string::npos) break;
        // find the matching }
        int depth = 0;
        size_t objEnd = objStart;
        for (; objEnd < json.size(); ++objEnd) {
            if (json[objEnd] == '{') ++depth;
            else if (json[objEnd] == '}') { --depth; if (depth == 0) break; }
        }
        if (objEnd >= json.size()) break;
        std::string obj = json.substr(objStart, objEnd - objStart + 1);

        PkuEvent ev;
        ev.initial = (idx < replay.initialSpawnCount);

        // move: "up"/"down"/"left"/"right"
        size_t mp = obj.find("\"move\"");
        if (mp != std::string::npos) {
            size_t q1 = obj.find('"', mp + 6);
            size_t q2 = obj.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string m = obj.substr(q1 + 1, q2 - q1 - 1);
                if (m == "up") ev.dir = 0;
                else if (m == "down") ev.dir = 1;
                else if (m == "left") ev.dir = 2;
                else if (m == "right") ev.dir = 3;
                else ev.dir = 0;
            }
        }

        // spawn: {value, x, y}
        size_t sp = obj.find("\"spawn\"");
        if (sp != std::string::npos) {
            size_t so = obj.find('{', sp);
            size_t sc = obj.find('}', so);
            if (so != std::string::npos && sc != std::string::npos) {
                std::string sobj = obj.substr(so, sc - so + 1);
                int v = 0, x = 0, y = 0;
                auto extractInt = [&](const char* key, int& val) {
                    std::string k = std::string("\"") + key + "\"";
                    size_t p = sobj.find(k);
                    if (p == std::string::npos) return;
                    p = sobj.find(':', p + k.size());
                    if (p == std::string::npos) return;
                    ++p;
                    while (p < sobj.size() && (sobj[p] == ' ' || sobj[p] == '\t')) ++p;
                    if (p < sobj.size() && (isdigit((unsigned char)sobj[p]) || sobj[p] == '-')) {
                        bool neg = (sobj[p] == '-');
                        if (neg) ++p;
                        val = 0;
                        while (p < sobj.size() && isdigit((unsigned char)sobj[p])) { val = val * 10 + (sobj[p] - '0'); ++p; }
                        if (neg) val = -val;
                    }
                };
                extractInt("value", v);
                extractInt("x", x);
                extractInt("y", y);
                ev.spawn.value = (v == 4) ? 4 : 2;
                ev.spawn.x = x;
                ev.spawn.y = y;
            }
        }

        // ms
        size_t msp = obj.find("\"ms\"");
        if (msp != std::string::npos) {
            size_t cp = obj.find(':', msp);
            if (cp != std::string::npos) {
                ++cp;
                while (cp < obj.size() && (obj[cp] == ' ' || obj[cp] == '\t')) ++cp;
                if (cp < obj.size() && isdigit((unsigned char)obj[cp])) {
                    ev.timeMs = 0;
                    while (cp < obj.size() && isdigit((unsigned char)obj[cp])) { ev.timeMs = ev.timeMs * 10 + (obj[cp] - '0'); ++cp; }
                } else {
                    ev.timeMs = -1;
                }
            }
        } else {
            ev.timeMs = -1;
        }

        replay.events.push_back(ev);
        ++idx;
        pos = objEnd + 1;
    }
    if (replay.events.empty()) { err = "PKU v1 has no valid events"; return false; }
    return true;
}

bool parsePku(const std::string& content, PkuReplay& replay, std::string& err)
{
    err.clear();
    std::string code = content;
    // strip BOM and surrounding whitespace
    if (code.size() >= 3 && (unsigned char)code[0] == 0xEF && (unsigned char)code[1] == 0xBB && (unsigned char)code[2] == 0xBF)
        code = code.substr(3);
    while (!code.empty() && (code.front() == ' ' || code.front() == '\t' || code.front() == '\r' || code.front() == '\n')) code.erase(code.begin());
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t' || code.back() == '\r' || code.back() == '\n')) code.pop_back();

    if (code.empty()) { err = "file is empty"; return false; }

    // PKU v1: pku-replay-v1.{base64url}
    if (code.compare(0, 13, "pku-replay-v1") == 0) {
        size_t dot = code.find('.', 13);
        if (dot == std::string::npos) { err = "PKU v1 format error (missing base64 data)"; return false; }
        std::string b64 = code.substr(dot + 1);
        std::string json;
        if (!pkuBase64UrlDecode(b64, json)) { err = "PKU v1 base64 decode failed"; return false; }
        return pkuParseV1Json(json, replay, err);
    }

    // PKU v2: pku-replay-v2.{mode}{rows}x{cols}_{events}
    if (code.compare(0, 13, "pku-replay-v2") == 0) {
        // regex: ^pku-replay-v2\.(?:(u|f|e|t|h|y|i|c\d+|s(?:ring|windmill|turtle|tee)|w[slx]\d{8}|g(?:layered|l3|628|63l|tee))\.)?(\d+)x(\d+)_(.+)$
        size_t dot = code.find('.', 13);
        if (dot == std::string::npos) { err = "PKU v2 format error"; return false; }
        std::string rest = code.substr(dot + 1);
        // find the {rows}x{cols}_ pattern
        // mode may be empty (classic) or contain alphanumerics
        size_t us = rest.find('_');
        if (us == std::string::npos) { err = "PKU v2 missing event separator"; return false; }
        std::string header = rest.substr(0, us);
        std::string body = rest.substr(us + 1);
        // extract NxN from the end of the header
        size_t xpos = header.rfind('x');
        if (xpos == std::string::npos) { err = "PKU v2 missing board size"; return false; }
        // cols = digits after x
        int cols = 0;
        size_t cp = xpos + 1;
        while (cp < header.size() && isdigit((unsigned char)header[cp])) { cols = cols * 10 + (header[cp] - '0'); ++cp; }
        // rows = digits before x (may have a mode prefix)
        int rows = 0;
        size_t rp = xpos;
        while (rp > 0 && isdigit((unsigned char)header[rp - 1])) { --rp; }
        for (size_t i = rp; i < xpos; ++i) rows = rows * 10 + (header[i] - '0');
        if (rows < 1 || cols < 1) { err = "PKU v2 invalid board size"; return false; }

        // mode prefix (part before rows in the header, trailing '.' removed)
        std::string modeCode = (rp > 0) ? header.substr(0, rp - 1) : "";
        std::string mode = "classic";
        if (modeCode == "u") mode = "undo";
        else if (modeCode == "f") mode = "fibonacci";
        else if (modeCode == "e") mode = "eight";
        else if (modeCode == "t") mode = "threes";
        else if (modeCode == "h") mode = "threes-hard";
        else if (modeCode == "y") mode = "threes-easy";
        else if (modeCode == "i") mode = "isotopic";
        else if (!modeCode.empty() && modeCode[0] == 'c') mode = "capped";
        else if (!modeCode.empty() && modeCode[0] == 's') mode = "shape";
        else if (!modeCode.empty() && modeCode[0] == 'w') mode = "weekly";
        else if (!modeCode.empty() && modeCode[0] == 'g') mode = "endgame";

        replay.rows = rows;
        replay.cols = cols;
        replay.mode = mode;
        replay.format = "pku-v2";
        replay.initialSpawnCount = 2;

        if (!pkuDecodeEvents(body, replay.initialSpawnCount, replay.events, err)) return false;
        return true;
    }

    // Verse new format: {rows}x{cols}-{version}_{events}
    // match: ^(\d+)x(\d+)-([^_]*)_(.+)$
    {
        size_t xpos = code.find('x');
        if (xpos != std::string::npos && xpos > 0 && isdigit((unsigned char)code[0])) {
            int rows = 0;
            for (size_t i = 0; i < xpos; ++i) {
                if (!isdigit((unsigned char)code[i])) { rows = -1; break; }
                rows = rows * 10 + (code[i] - '0');
            }
            if (rows > 0) {
                size_t dash = code.find('-', xpos + 1);
                if (dash != std::string::npos) {
                    int cols = 0;
                    size_t cp = xpos + 1;
                    while (cp < dash && isdigit((unsigned char)code[cp])) { cols = cols * 10 + (code[cp] - '0'); ++cp; }
                    if (cols > 0 && cp == dash) {
                        size_t us = code.find('_', dash + 1);
                        if (us != std::string::npos) {
                            std::string body = code.substr(us + 1);
                            replay.rows = rows;
                            replay.cols = cols;
                            replay.mode = "classic";
                            replay.format = "verse-new";
                            replay.initialSpawnCount = 2;
                            if (!pkuDecodeEvents(body, replay.initialSpawnCount, replay.events, err)) return false;
                            return true;
                        }
                    }
                }
            }
        }
    }

    err = "not a PKU 2048 .vrs format (no pku-replay-v1/v2 or NxN- prefix)";
    return false;
}

// ============================ Main entry ============================
bool parse(const std::string& content, std::vector<Move>& moves,
           int& boardSize, std::string& err)
{
    moves.clear();
    boardSize = 4;
    err.clear();
    if (content.empty()) { err = "file is empty"; return false; }

    // try PKU native format first (exact spawn positions/values; rebuilt by the upper layer via replayFromPkuEvents)
    PkuReplay pku;
    std::string pkuErr;
    if (parsePku(content, pku, pkuErr) && !pku.events.empty()) {
        // extract only the direction sequence (fallback for the old path; exact rebuild goes through parsePku directly)
        boardSize = (pku.rows == pku.cols) ? pku.rows : 4;
        static const int pkuToCpp[4] = { 2, 3, 0, 1 }; // PKU up/down/left/right -> C++ left/right/up/down
        for (size_t i = pku.initialSpawnCount; i < pku.events.size(); ++i) {
            Move m;
            m.dir = pkuToCpp[pku.events[i].dir & 3];
            m.timeMs = (pku.events[i].timeMs > 0) ? pku.events[i].timeMs : 0;
            moves.push_back(m);
        }
        if (!moves.empty()) return true;
    }

    // Try order: JSON -> base64 -> plain text
    if (tryParseJson(content, moves, boardSize)) return true;
    if (tryParseBase64(content, moves)) { boardSize = 4; return true; }
    if (tryParseText(content, moves)) return true;

    err = "unrecognized .vrs replay format (please confirm it is a pku2048.cn or 2048-Solver replay file)";
    return false;
}

} // namespace vrs
