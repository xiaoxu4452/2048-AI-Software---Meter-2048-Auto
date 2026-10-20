#define NOMINMAX
#pragma warning(push)
#pragma warning(disable: 26495 6386 6262 6297)
#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <SFML/System.hpp>
#pragma warning(pop)
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <random>
#include <filesystem>
#include <windows.h>
#include <commdlg.h>   // GetOpenFileNameA (import history file picker)
#include <shellapi.h>  // ShellExecuteA (open the About technical document)
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <mutex>
#include <atomic>
#include "ZigSolverAI2048.h"
#include "jsengine/vrs_parser.h"   // lightweight C++ .vrs replay parser (JSON/base64/text)

// AI engines: 4x4 search = Solver4x4.h (self-contained C++ header, fully aligned with ziap/2048-ai official,
//   compiled into the exe, no DLL dependency); classic search mode = ziap (same core, no MacroXue) - pure algorithm, no endgame tables;
//   3x3/5x5 in-house engines compiled into the exe (Solver3x3Fast / Solver5x5Fast / MacroXueSolverAI2048)
//   Since v3.2.18 cbind.dll is obsolete; 4x4 no longer loads any DLL (the old cbind.dll fallback logic was removed)
// The raw SolverAI2048 class is still available for static helpers (getPhase, countEmptyBoard, etc.)

// ========== Run directory and log utilities ==========
// All data files (saves/history/logs/icons) resolve to the exe directory,
// so launching from a shortcut never writes saves into the system directory.
static const std::string& getExeDir()
{
    static std::string dir = []() {
        wchar_t wpath[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(nullptr, wpath, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return std::string(".");
        int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len : 1, '\0');
        if (len > 0) WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &s[0], len, nullptr, nullptr);
        if (!s.empty() && s.back() == '\0') s.pop_back();
        size_t pos = s.find_last_of("\\/");
        return (pos != std::string::npos) ? s.substr(0, pos) : std::string(".");
    }();
    return dir;
}

// HSV <-> RGB helpers
static sf::Color hsvToRgb(float h, float s, float v)
{
    // h in [0,360)
    float c = v * s;
    float hh = h / 60.f;
    float x = c * (1 - fabs(fmod(hh, 2.f) - 1));
    float r=0,g=0,b=0;
    if (0 <= hh && hh < 1) { r=c; g=x; b=0; }
    else if (1 <= hh && hh < 2) { r=x; g=c; b=0; }
    else if (2 <= hh && hh < 3) { r=0; g=c; b=x; }
    else if (3 <= hh && hh < 4) { r=0; g=x; b=c; }
    else if (4 <= hh && hh < 5) { r=x; g=0; b=c; }
    else { r=c; g=0; b=x; }
    float m = v - c;
    sf::Uint8 R = (sf::Uint8)std::round((r + m) * 255.f);
    sf::Uint8 G = (sf::Uint8)std::round((g + m) * 255.f);
    sf::Uint8 B = (sf::Uint8)std::round((b + m) * 255.f);
    return sf::Color(R,G,B);
}

static void rgbToHsv(const sf::Color& c, float& outH, float& outS, float& outV)
{
    float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
    float mx = std::max(r, std::max(g,b));
    float mn = std::min(r, std::min(g,b));
    float d = mx - mn;
    outV = mx;
    outS = (mx == 0) ? 0.f : d / mx;
    float h = 0.f;
    if (d == 0) h = 0.f;
    else if (mx == r) h = 60.f * fmod(((g - b) / d), 6.f);
    else if (mx == g) h = 60.f * (((b - r) / d) + 2.f);
    else h = 60.f * (((r - g) / d) + 4.f);
    if (h < 0) h += 360.f;
    outH = h;
}

static std::string pathInExeDir(const char* name)
{
    return getExeDir() + "\\" + name;
}

// Scan the "game fonts" folder next to the exe and return the full path of a font file
// Supports .ttf / .otf / .ttc; returns an empty string if none is found
// Prefer the original full font SourceHanSansCN-Medium.otf so a subset residual is not picked first
static std::string findGameFont()
{
    std::string dir = pathInExeDir("game fonts");
    WIN32_FIND_DATAA fd;
    std::string search = dir + "\\*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return std::string();
    std::string firstFont;  // fallback: the first font found
    std::string prefFont;   // preferred: the original full font
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string fn = fd.cFileName;
        std::string lower = fn;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find(".ttf") != std::string::npos ||
            lower.find(".otf") != std::string::npos ||
            lower.find(".ttc") != std::string::npos)
        {
            if (firstFont.empty())
                firstFont = dir + "\\" + fn;
            // prefer the original font (no "subset" in the name)
            if (lower.find("subset") == std::string::npos)
                prefFont = dir + "\\" + fn;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    if (!prefFont.empty()) return prefFont;
    return firstFont;
}

// ========== Log management: archive per game + auto cleanup ==========
namespace fs = std::filesystem;
static const int MAX_LOG_FILES = 100;

class Logger {
    std::ofstream m_log;
public:
    Logger() = default;
    ~Logger() { if (m_log.is_open()) m_log.close(); }
    
    void openGameLog(const std::string& logDir, int gameId) {
        if (m_log.is_open()) m_log.close();
        try { fs::create_directories(logDir); } catch (...) {}
        std::string filename = logDir + "\\game_" +
            timeStampForFile() + "_" + std::to_string(gameId) + ".log";
        m_log.open(filename, std::ios::app);
    }
    
    void write(const std::string& msg)
    {
        if (!m_log.is_open()) return;
        time_t now = std::time(nullptr);
        struct tm t;
        localtime_s(&t, &now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
        m_log << "[" << ts << "] " << msg << "\n";
        m_log.flush();
    }
    
    bool isOpen() const { return m_log.is_open(); }

private:
    static std::string timeStampForFile() {
        time_t now = std::time(nullptr);
        struct tm t;
        localtime_s(&t, &now);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
        return std::string(buf);
    }
};

// Get the log directory path
static std::string getLogDir()
{
    static std::string dir = getExeDir() + "\\logs";
    return dir;
}

// Clean the log directory: keep the newest MAX_LOG_FILES .log files
static void cleanupLogFiles()
{
    std::string logDir = getLogDir();
    try {
        if (!fs::exists(logDir)) return;
        std::vector<fs::directory_entry> entries;
        for (auto& entry : fs::directory_iterator(logDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".log")
                entries.push_back(entry);
        }
        if ((int)entries.size() <= MAX_LOG_FILES) return;
        // sort by modification time
        std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                return a.last_write_time() > b.last_write_time();
            });
        for (int i = MAX_LOG_FILES; i < (int)entries.size(); i++) {
            try { fs::remove(entries[i].path()); } catch (...) {}
        }
    } catch (...) {}
}

// Global logger instance + game ID counter
static Logger g_logger;
static int g_gameIdCounter = 0;

// Start a new game log: called from Game2048::init()
static void startNewGameLog(const std::string& boardSize)
{
    g_gameIdCounter++;
    cleanupLogFiles();
    g_logger.openGameLog(getLogDir(), g_gameIdCounter);
    g_logger.write("=== New Game #");
    g_logger.write(std::to_string(g_gameIdCounter) + " | Board: " + boardSize + " | Exe: " + getExeDir());
}

// Unified log interface: every logMsg call writes to the currently active log file
static void logMsg(const std::string& msg)
{
    if (!g_logger.isOpen()) return;
    // Lite build keeps logs short: only errors and key lifecycle events (ERROR / finished) are written
    if (msg.rfind("ERROR", 0) == 0 || msg.find("finished") != std::string::npos)
        g_logger.write(msg);
}

// ========== Centralized configuration constants ==========
const int DEFAULT_BOARD_SIZE = 4;
const int MAX_BOARD_SIZE = 5;
const int MIN_BOARD_SIZE = 3;
const int NUM_SCHEMES = 6;            // number of original presets
const int CUSTOM_SCHEME_IDX = 6;      // index of the custom scheme in the array
const int ADDED_SCHEMES = 26;         // number of added color schemes
const int TOTAL_SCHEMES = NUM_SCHEMES + 1 + ADDED_SCHEMES;   // 33 = 6 original + 1 custom + 26 added
const int NUM_PRESET_SCHEMES = NUM_SCHEMES + ADDED_SCHEMES;  // 32 (selectable presets, excluding the custom one)
const int MAX_POWER = 26;
const int MAX_CUSTOM_PRESETS = 20;    // max custom color preset slots
// Save paths resolve to the exe directory (lambda cache keeps pointers valid)
static const char* const SAVE_FILE = []() -> const char* {
    static std::string p = pathInExeDir("2048save.dat");
    return p.c_str();
}();
// Timer mode uses a separate save per board size, fully isolated from the normal slots:
// auto-written after saving / on direct exit / on mid-game size switch; restored on the next session
// (timing restarts on the first move). Switching back to a size restores that size's timed game.
static const char* tmBoardFileFor(int size)
{
    static std::string p3 = pathInExeDir("tm_board_3.dat");
    static std::string p4 = pathInExeDir("tm_board_4.dat");
    static std::string p5 = pathInExeDir("tm_board_5.dat");
    switch (size) { case 3: return p3.c_str(); case 5: return p5.c_str(); default: return p4.c_str(); }
}
// Per-board-size slot saves (each size keeps its own progress)
//   2048slot_3.dat / 2048slot_4.dat / 2048slot_5.dat
//   The main save 2048save.dat still records "where you left off", read at startup
//   Switching size: save current -> load target slot (progress never lost)
//   Restart: only clears the current size's slot, other sizes are untouched
static const char* const SLOT_FILE_3 = []() -> const char* {
    static std::string p = pathInExeDir("2048slot_3.dat");
    return p.c_str();
}();
static const char* const SLOT_FILE_4 = []() -> const char* {
    static std::string p = pathInExeDir("2048slot_4.dat");
    return p.c_str();
}();
static const char* const SLOT_FILE_5 = []() -> const char* {
    static std::string p = pathInExeDir("2048slot_5.dat");
    return p.c_str();
}();
static const char* slotFileFor(int size)
{
    switch (size) {
        case 3: return SLOT_FILE_3;
        case 4: return SLOT_FILE_4;
        case 5: return SLOT_FILE_5;
        default: return SAVE_FILE;
    }
}
static const char* const CUSTOM_PRESETS_FILE = []() -> const char* {
    static std::string p = pathInExeDir("custom_presets.dat");
    return p.c_str();
}();
// Language config file: written by the installer from the user's language choice, read at startup
//   [Language]
//   Lang=zh | en
// Priority: lang.ini > g_language field in 2048save.dat
//   - First launch (no 2048save.dat) -> use lang.ini
//   - In-game switch -> both 2048save.dat and lang.ini are updated -> new value on next launch
//   - Reinstall with another language -> ISS overwrites lang.ini -> new choice applies
static const char* const LANG_FILE = []() -> const char* {
    static std::string p = pathInExeDir("lang.ini");
    return p.c_str();
}();
// AI settings file: remembers the user's last AI rate and intelligence tier, restored on next launch
//   [AISettings]
//   Rate=30        (7..50 moves/s, default 30 on first install)
//   Level=2        (0..2, maps to 2^15/2^17/2^19; higher = smarter, default tier 3)
static const char* const AI_SETTINGS_FILE = []() -> const char* {
    static std::string p = pathInExeDir("ai_settings.ini");
    return p.c_str();
}();

// Challenge project file: remembers the last challenge project, restored on next launch
//   [Challenge]
//   Project=0       (0..5, the 6 challenge projects; no file on first install -> default 0 = 4x4 2048)
static const char* const CHALLENGE_SETTINGS_FILE = []() -> const char* {
    static std::string p = pathInExeDir("challenge.ini");
    return p.c_str();
}();

// Custom theme file: persists SCHEMES[CUSTOM_SCHEME_IDX] (the theme the user applied)
//   The save (2048save.dat) stores only the theme index, not the content; after restart the custom slot
//   reverts to defaults, losing the applied theme and breaking list highlighting.
//   This file stores the full custom-slot palette and restores it at startup.
static const char* const CUSTOM_THEME_FILE = []() -> const char* {
    static std::string p = pathInExeDir("custom_theme.dat");
    return p.c_str();
}();
static const char* const schemeNames[TOTAL_SCHEMES] = {
    "Cool", "Classic", "Neon", "Forest", "Sunset", "Ocean", "Custom",
    "Graphite", "Dark", "Midnight", "Night", "Sky", "Deep Blue", "Violet", "Purple", "Pink",
    "Mono Light", "Mono Dark",
    "Mist", "Dawn", "Mint", "Coral", "Evergreen", "Obsidian", "Sand",
    "Mist Blue", "Berry", "Cream", "Champagne", "Celadon", "Amethyst", "Terracotta", "Night Sakura"
};
static const char* const schemeNamesZh[TOTAL_SCHEMES] = {
    "清凉", "经典", "霓虹", "森林", "日落", "海洋", "自定义",
    "石墨", "深黑", "墨蓝", "夜阑", "天蓝", "深蓝", "紫罗兰", "紫色", "粉花",
    "黑白浅", "黑白深",
    "雾白", "晨曦", "薄荷", "珊瑚", "墨绿", "曜石", "金砂",
    "雾蓝", "莓果", "奶油", "香槟", "青瓷", "紫晶", "赤陶", "夜樱"
};
const int TILE_SIZE = 100;
const int TILE_GAP = 12;
const int BOARD_PADDING = 24;
const int TOP_BAR_HEIGHT = 80;
const int DRAG_THRESHOLD = 30;

const int WINDOW_WIDTH = DEFAULT_BOARD_SIZE * TILE_SIZE + (DEFAULT_BOARD_SIZE + 1) * TILE_GAP + BOARD_PADDING * 2;
const int WINDOW_HEIGHT = WINDOW_WIDTH + TOP_BAR_HEIGHT;

// Default window size on first open (no save): 495x615
const int FIRST_RUN_WINDOW_W = 495;
const int FIRST_RUN_WINDOW_H = 615;

// ========== Font enumeration data structures ==========
struct FontEntry {
    std::string name;
    std::string path;
};

static std::vector<FontEntry> g_systemFonts;
static const char* const DEFAULT_FONT_PATH = []() -> const char* {
    static std::string p = findGameFont();
    if (p.empty()) p = "C:/Windows/Fonts/msyh.ttc";
    return p.c_str();
}();
static std::string g_currentFontPath = DEFAULT_FONT_PATH;
static std::string g_previousFontPath = DEFAULT_FONT_PATH;
static unsigned int g_savedWindowW = FIRST_RUN_WINDOW_W;
static unsigned int g_savedWindowH = FIRST_RUN_WINDOW_H;
static bool g_saveDirty = false;
static sf::Clock g_saveTimer;
static const float SAVE_DEBOUNCE = 0.4f; // save 0.4s after the last action

// ========== Language and localization ==========
// English and Chinese are supported; switching to Chinese auto-selects Microsoft YaHei (msyh)
enum Language { LANG_EN = 0, LANG_ZH = 1 };
static int g_language = LANG_ZH;
// Whether the save carried a language field (main uses this to decide whether to fall back to lang.ini)
static bool s_saveHadLanguageField = false;
// lang.ini cache (does not touch g_language directly; the save wins)
static int s_langIniLoaded = LANG_EN;
static bool s_langIniFileExists = false;

// UTF-8 -> sf::String: SFML 2.6.1's sf::String(const std::string&) mishandles UTF-8 Chinese,
// so UTF-8 must first go through MultiByteToWideChar to UTF-16 before building sf::String
static sf::String toSFString(const char* utf8)
{
    if (utf8 == nullptr || *utf8 == '\0') return sf::String();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 1) return sf::String(utf8);
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], wlen);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return sf::String(ws);
}

// System ANSI (GBK) path/filename -> sf::String
// GetOpenFileNameA returns ANSI; reading it as UTF-8 would garble Chinese filenames
static sf::String sfStringFromAnsi(const std::string& s)
{
    if (s.empty()) return sf::String();
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (wlen <= 1) return toSFString(s.c_str());
    std::wstring ws(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &ws[0], wlen);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return sf::String(ws);
}

// Localized text: takes English / Chinese UTF-8 strings, returns sf::String per current language
static sf::String tr(const char* en, const char* zh)
{
    return (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en);
}

// ========== Font fallback system ==========
// With an English font selected, ASCII uses that font and Chinese falls back to the default game font
static sf::Font g_fallbackFont;      // fallback font loaded from game fonts
static bool g_fallbackLoaded = false; // whether the fallback font is loaded
static bool g_usingFallback = false;  // whether the main font lacks Chinese (needs chunked rendering)

// Is the character ASCII (including common punctuation)
static bool isAsciiChar(sf::Uint32 ch)
{
    return ch < 128;
}

// Draw text in chunks with the fallback font
// align: 0=left, 1=center, 2=right
// Returns the total text width
static float drawTextFB(sf::RenderWindow& window, const sf::Font& primaryFont,
                        const sf::String& str, float x, float y,
                        unsigned int size, const sf::Color& color,
                        uint32_t style = 0, int align = 0)
{
    if (str.isEmpty()) return 0.f;

    // Pure ASCII and no forced fallback: use the main font directly
    bool hasNonAscii = false;
    for (std::size_t i = 0; i < str.getSize(); ++i) { if (!isAsciiChar(str[i])) { hasNonAscii = true; break; } }
    if (!g_usingFallback && !hasNonAscii)
    {
        sf::Text t(str, primaryFont, size);
        t.setFillColor(color);
        t.setStyle(style);
        sf::FloatRect b = t.getLocalBounds();
        if (align == 1)
            t.setOrigin(b.left + b.width / 2, b.top + b.height / 2);
        else if (align == 2)
            t.setOrigin(b.left + b.width, b.top + b.height / 2);
        else
            t.setOrigin(b.left, b.top + b.height / 2);
        t.setPosition(x, y);
        window.draw(t);
        return b.width;
    }

    // Chunk: split the string into ASCII and non-ASCII segments
    struct Seg { sf::String text; bool ascii; };
    std::vector<Seg> segs;
    bool lastAscii = isAsciiChar(str[0]);
    sf::String cur;
    cur += str[0];
    for (std::size_t i = 1; i < str.getSize(); ++i)
    {
        bool a = isAsciiChar(str[i]);
        if (a == lastAscii)
            cur += str[i];
        else
        {
            segs.push_back({ cur, lastAscii });
            cur.clear();
            cur += str[i];
            lastAscii = a;
        }
    }
    segs.push_back({ cur, lastAscii });

    // Compute total width
    float totalW = 0.f;
    for (auto& s : segs)
    {
        sf::Text t(s.text, s.ascii ? primaryFont : g_fallbackFont, size);
        t.setStyle(style);
        totalW += t.getLocalBounds().width;
    }

    // Start X
    float startX = x;
    if (align == 1) startX = x - totalW / 2.f;
    else if (align == 2) startX = x - totalW;

    // Draw segment by segment
    float curX = startX;
    for (auto& s : segs)
    {
        const sf::Font& f = s.ascii ? primaryFont : g_fallbackFont;
        sf::Text t(s.text, f, size);
        t.setFillColor(color);
        t.setStyle(style);
        sf::FloatRect b = t.getLocalBounds();
        t.setOrigin(b.left, b.top + b.height / 2);
        t.setPosition(curX, y);
        window.draw(t);
        curX += b.width;
    }
    return totalW;
}

// Measure text width (same ASCII/fallback chunking as drawTextFB, no drawing)
static float measureTextFB(const sf::Font& primaryFont, const sf::String& str, unsigned int size)
{
    if (str.isEmpty()) return 0.f;
    bool hasNonAscii = false;
    for (std::size_t i = 0; i < str.getSize(); ++i) { if (!isAsciiChar(str[i])) { hasNonAscii = true; break; } }
    if (!g_usingFallback && !hasNonAscii)
    {
        sf::Text t(str, primaryFont, size);
        return t.getLocalBounds().width;
    }
    float w = 0.f;
    struct Seg { sf::String text; bool ascii; };
    std::vector<Seg> segs;
    bool lastAscii = isAsciiChar(str[0]);
    sf::String cur; cur += str[0];
    for (std::size_t i = 1; i < str.getSize(); ++i)
    {
        bool a = isAsciiChar(str[i]);
        if (a == lastAscii) cur += str[i];
        else { segs.push_back({ cur, lastAscii }); cur.clear(); cur += str[i]; lastAscii = a; }
    }
    segs.push_back({ cur, lastAscii });
    for (auto& s : segs)
    {
        sf::Text t(s.text, s.ascii ? primaryFont : g_fallbackFont, size);
        w += t.getLocalBounds().width;
    }
    return w;
}

// Is the font a symbol font (WingDings / Symbol etc., no Chinese support)
static bool isSymbolFont(const std::string& fontPath)
{
    std::string lower = fontPath;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    static const char* symbolKeys[] = {
        "wingding", "symbol", "segm符号", "segoe符号", "mt extra",
        "geometric", "mathematical", "monotype", "icon", "dingbats"
    };
    for (const char* k : symbolKeys)
    {
        if (lower.find(k) != std::string::npos) return true;
    }
    return false;
}

// Does the font file support Chinese (decided by filename keywords, excluding symbol fonts)
// Cache to avoid repeatedly probing fonts on disk
static std::unordered_map<std::string, bool> g_fontChineseSupportCache;

// More robust check: try loading the font and query glyph for a Chinese character.
static bool fontSupportsChinese(const std::string& fontPath)
{
    auto it = g_fontChineseSupportCache.find(fontPath);
    if (it != g_fontChineseSupportCache.end()) return it->second;

    if (isSymbolFont(fontPath))
    {
        g_fontChineseSupportCache[fontPath] = false;
        return false;
    }

    // Try to load font and test glyph availability for a common Chinese character
    sf::Font f;
    bool support = false;
    if (f.loadFromFile(fontPath))
    {
        // Use the CJK char (U+4E2D) as a test character at a reasonable size
        sf::Uint32 ch = 0x4E2D;
        unsigned int testSize = 32;
        sf::Glyph g = f.getGlyph(ch, testSize, false);
        // If glyph has non-zero advance or a non-empty texture rect, consider it supported
        if (g.advance != 0.f || (g.textureRect.width > 0 && g.textureRect.height > 0))
            support = true;
    }

    // Fallback heuristic by filename if loading failed or glyph absent
    if (!support)
    {
        std::string lower = fontPath;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.find("game fonts") != std::string::npos) support = true;
        static const char* cnKeys[] = {
            "msyh", "yahei", "simhei", "simsun", "nsimsun",
            "simkai", "simfang", "stxinwei", "stzhongs", "stsong",
            "stkaiti", "stfangso", "deng", "fzshuti", "fzyaoti",
            "genjyuugothic", "genjyuu", "sourcehan", "noto", "sarasa",
            "simhei", "simsun", "msyh", "msyhbd"
        };
        for (const char* k : cnKeys)
        {
            if (lower.find(k) != std::string::npos) { support = true; break; }
        }
    }

    g_fontChineseSupportCache[fontPath] = support;
    return support;
}

// Find an available Chinese system font (prefer simhei.ttf)
static std::string findChineseFont()
{
    // prefer fonts in the game fonts directory
    std::string gameFont = findGameFont();
    if (!gameFont.empty())
    {
        std::ifstream f(gameFont, std::ios::binary);
        if (f.good()) return gameFont;
    }
    const char* candidates[] = {
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/simkai.ttf"
    };
    for (const char* p : candidates)
    {
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return std::string(p);
    }
    return gameFont;
}

// On language switch, sync the font: no auto font change, only set the fallback flag
// When g_usingFallback=true, drawTextFB renders Chinese with the fallback font
static void applyLanguageFont()
{
    g_usingFallback = (g_language == LANG_ZH && !fontSupportsChinese(g_currentFontPath));
}

// Mark the save as dirty (debounced)
static void markSaveDirty()
{
    g_saveDirty = true;
    g_saveTimer.restart();
}

void enumerateSystemFonts()
{
    if (!g_systemFonts.empty()) return;

    // scan the game fonts directory first
    std::string gameFontDir = pathInExeDir("game fonts");
    {
        WIN32_FIND_DATAA fd;
        std::string search = gameFontDir + "\\*.ttf";
        HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do {
                std::string fileName = fd.cFileName;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    std::string name = fileName;
                    size_t dot = name.rfind('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);
                    g_systemFonts.push_back({ name, gameFontDir + "\\" + fileName });
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    // scan the system fonts directory
    std::string fontDir = "C:\\Windows\\Fonts\\";

    WIN32_FIND_DATAA fd;
    std::string search = fontDir + "*.ttf";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            std::string fileName = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string name = fileName;
                size_t dot = name.rfind('.');
                if (dot != std::string::npos) name = name.substr(0, dot);
                g_systemFonts.push_back({ name, fontDir + fileName });
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    search = fontDir + "*.ttc";
    hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            std::string fileName = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string name = fileName;
                size_t dot = name.rfind('.');
                if (dot != std::string::npos) name = name.substr(0, dot);
                g_systemFonts.push_back({ name, fontDir + fileName });
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    std::sort(g_systemFonts.begin(), g_systemFonts.end(),
        [](const FontEntry& a, const FontEntry& b) { return a.name < b.name; });
}

// ========== Color scheme data structures ==========
struct SchemeData {
    sf::Color tileColors[MAX_POWER + 1];
    sf::Color emptyColor;
    sf::Color windowBg;
    sf::Color boardBg;
    sf::Color scoreBoxColor;
    sf::Color textLight;
    sf::Color textDark;
    int lightTextFromValue;
    const char* name;
};

static SchemeData SCHEMES[TOTAL_SCHEMES] = {
    // 0: Cool
    {
        {
            sf::Color(0,0,0),
            sf::Color(210, 235, 255), sf::Color(180, 220, 255), sf::Color(140, 200, 255),
            sf::Color(100, 180, 255), sf::Color(60, 150, 255),  sf::Color(30, 120, 240),
            sf::Color(20, 90, 220),   sf::Color(15, 70, 200),   sf::Color(10, 50, 180),
            sf::Color(40, 40, 160),   sf::Color(70, 30, 140),   sf::Color(90, 20, 130),
            sf::Color(110, 10, 120),  sf::Color(120, 0, 110),   sf::Color(100, 0, 100),
            sf::Color(80, 0, 90),     sf::Color(75, 0, 130),
            sf::Color(60, 0, 110),    sf::Color(50, 0, 100),    sf::Color(40, 0, 90),
            sf::Color(30, 0, 80),     sf::Color(25, 0, 70),     sf::Color(20, 0, 60),
            sf::Color(15, 0, 50),     sf::Color(10, 0, 40),     sf::Color(5, 0, 30),
        },
        sf::Color(205, 193, 180, 100),
        sf::Color(250, 248, 239), sf::Color(187, 173, 160), sf::Color(187, 173, 160),
        sf::Color(249, 246, 242), sf::Color(119, 110, 101), 8, "Cool"
    },
    // 1: Classic
    {
        {
            sf::Color(0,0,0),
            sf::Color(238, 228, 218), sf::Color(237, 224, 200), sf::Color(242, 177, 121),
            sf::Color(245, 149, 99),  sf::Color(246, 124, 95),  sf::Color(246, 94, 59),
            sf::Color(237, 207, 114), sf::Color(237, 204, 97),  sf::Color(237, 200, 80),
            sf::Color(237, 197, 63),  sf::Color(237, 194, 46),  sf::Color(225, 175, 75), sf::Color(210, 130, 85),  sf::Color(215, 85, 50),
            sf::Color(190, 50, 42),  sf::Color(195, 65, 90),   sf::Color(180, 55, 80),
            sf::Color(168, 48, 72),  sf::Color(155, 42, 65),   sf::Color(142, 36, 58),
            sf::Color(130, 32, 52),  sf::Color(118, 28, 46),   sf::Color(106, 24, 40),
            sf::Color(94, 20, 34),   sf::Color(82, 16, 28),    sf::Color(70, 12, 22),
        },
        sf::Color(205, 193, 180),
        sf::Color(250, 248, 239), sf::Color(187, 173, 160), sf::Color(187, 173, 160),
        sf::Color(249, 246, 242), sf::Color(119, 110, 101), 8, "Classic"
    },
    // 2: Neon
    {
        {
            sf::Color(0,0,0),
            sf::Color(50, 50, 80),    sf::Color(60, 40, 100),   sf::Color(80, 30, 120),
            sf::Color(100, 20, 140),  sf::Color(120, 10, 160),  sf::Color(140, 0, 180),
            sf::Color(0, 100, 100),   sf::Color(0, 120, 120),   sf::Color(0, 140, 140),
            sf::Color(0, 160, 160),   sf::Color(0, 180, 180),   sf::Color(0, 200, 200),
            sf::Color(0, 220, 220),   sf::Color(50, 255, 255),  sf::Color(100, 255, 255),
            sf::Color(150, 255, 255), sf::Color(180, 240, 250),
            sf::Color(160, 200, 240),sf::Color(130, 160, 220),sf::Color(100, 120, 195),
            sf::Color(75, 85, 170),   sf::Color(60, 60, 145),   sf::Color(48, 45, 125),
            sf::Color(38, 35, 108),   sf::Color(30, 32, 92),    sf::Color(20, 28, 76),
        },
        sf::Color(30, 30, 50),
        sf::Color(20, 20, 35), sf::Color(40, 40, 60), sf::Color(40, 40, 60),
        sf::Color(249, 246, 242), sf::Color(200, 200, 200), 2, "Neon"
    },
    // 3: Forest
    {
        {
            sf::Color(0,0,0),
            sf::Color(200, 230, 201), sf::Color(165, 214, 167), sf::Color(129, 199, 132),
            sf::Color(102, 187, 106), sf::Color(76, 175, 80),   sf::Color(56, 142, 60),
            sf::Color(46, 125, 50),   sf::Color(27, 94, 32),    sf::Color(20, 80, 35),
            sf::Color(15, 65, 30),    sf::Color(10, 55, 25),    sf::Color(8, 45, 20),
            sf::Color(6, 38, 18),     sf::Color(8, 48, 38),     sf::Color(10, 58, 55),
            sf::Color(12, 68, 70),    sf::Color(14, 78, 82),
            sf::Color(15, 85, 92),    sf::Color(30, 100, 85),   sf::Color(55, 115, 78),
            sf::Color(85, 130, 70),   sf::Color(110, 140, 65),  sf::Color(115, 130, 60),
            sf::Color(105, 110, 55),  sf::Color(90, 90, 48),    sf::Color(70, 70, 40),
        },
        sf::Color(200, 220, 200),
        sf::Color(245, 248, 243), sf::Color(180, 200, 170), sf::Color(180, 200, 170),
        sf::Color(249, 246, 242), sf::Color(60, 80, 50), 8, "Forest"
    },
    // 4: Sunset
    {
        {
            sf::Color(0,0,0),
            sf::Color(255, 235, 175), sf::Color(255, 213, 145), sf::Color(255, 193, 120),
            sf::Color(255, 171, 105), sf::Color(255, 143, 90),  sf::Color(255, 112, 67),
            sf::Color(255, 87, 52),   sf::Color(230, 74, 25),   sf::Color(210, 50, 20),
            sf::Color(180, 40, 30),   sf::Color(150, 30, 40),   sf::Color(130, 25, 60),
            sf::Color(110, 20, 80),   sf::Color(115, 25, 85),   sf::Color(125, 35, 95),
            sf::Color(140, 50, 110),  sf::Color(160, 70, 130),  sf::Color(180, 90, 150),
            sf::Color(175, 90, 155), sf::Color(170, 92, 162), sf::Color(163, 94, 170),
            sf::Color(155, 96, 178),
            sf::Color(145, 98, 186), sf::Color(133, 100, 195), sf::Color(120, 102, 203),
            sf::Color(106, 104, 210),
        },
        sf::Color(250, 230, 210),
        sf::Color(255, 248, 240), sf::Color(230, 200, 180), sf::Color(230, 200, 180),
        sf::Color(249, 246, 242), sf::Color(120, 80, 60), 8, "Sunset"
    },
    // 5: Ocean
    {
        {
            sf::Color(0,0,0),
            sf::Color(187, 222, 251), sf::Color(144, 199, 255), sf::Color(100, 181, 246),
            sf::Color(64, 164, 223),  sf::Color(33, 150, 243),  sf::Color(30, 136, 229),
            sf::Color(25, 118, 210),  sf::Color(21, 101, 192),  sf::Color(3, 169, 244),
            sf::Color(0, 188, 212),   sf::Color(0, 172, 193),   sf::Color(0, 150, 167),
            sf::Color(0, 137, 123),   sf::Color(0, 105, 92),    sf::Color(0, 80, 70),
            sf::Color(0, 140, 155),  sf::Color(0, 120, 165),  sf::Color(0, 100, 180),
            sf::Color(0, 75, 200),   sf::Color(0, 40, 210),   sf::Color(0, 15, 195),
            sf::Color(8, 8, 170),    sf::Color(12, 5, 145),   sf::Color(20, 5, 130),
            sf::Color(40, 5, 140),   sf::Color(60, 5, 155),
        },
        sf::Color(200, 225, 230),
        sf::Color(240, 248, 252), sf::Color(170, 200, 210), sf::Color(170, 200, 210),
        sf::Color(249, 246, 242), sf::Color(60, 80, 90), 8, "Ocean"
    },
    // 6: Custom (defaults to Classic palette; user editable)
    {
        {
            sf::Color(0,0,0),
            sf::Color(238, 228, 218), sf::Color(237, 224, 200), sf::Color(242, 177, 121),
            sf::Color(245, 149, 99),  sf::Color(246, 124, 95),  sf::Color(246, 94, 59),
            sf::Color(237, 207, 114), sf::Color(237, 204, 97),  sf::Color(237, 200, 80),
            sf::Color(237, 197, 63),  sf::Color(237, 194, 46),  sf::Color(225, 175, 75), sf::Color(210, 130, 85),  sf::Color(215, 85, 50),
            sf::Color(190, 50, 42),  sf::Color(195, 65, 90),   sf::Color(180, 55, 80),
            sf::Color(168, 48, 72),  sf::Color(155, 42, 65),   sf::Color(142, 36, 58),
            sf::Color(130, 32, 52),  sf::Color(118, 28, 46),   sf::Color(106, 24, 40),
            sf::Color(94, 20, 34),   sf::Color(82, 16, 28),    sf::Color(70, 12, 22),
        },
        sf::Color(205, 193, 180),
        sf::Color(250, 248, 239), sf::Color(187, 173, 160), sf::Color(187, 173, 160),
        sf::Color(249, 246, 242), sf::Color(119, 110, 101), 8, "Custom"
    },
    // 7: Graphite (gray family -> premium graphite steel gray, clearly distinct from mono themes)
    {
        {
            sf::Color(0,0,0),
            sf::Color(233, 236, 240), sf::Color(220, 225, 231), sf::Color(207, 213, 222),
            sf::Color(194, 201, 212), sf::Color(181, 189, 202), sf::Color(168, 176, 191),
            sf::Color(154, 163, 179), sf::Color(141, 150, 168), sf::Color(128, 137, 156),
            sf::Color(116, 125, 145), sf::Color(105, 113, 133), sf::Color(94, 102, 122),
            sf::Color(84, 92, 111),   sf::Color(75, 82, 100),   sf::Color(66, 73, 90),
            sf::Color(58, 64, 80),    sf::Color(51, 56, 71),    sf::Color(45, 49, 63),
            sf::Color(39, 43, 55),    sf::Color(34, 38, 48),    sf::Color(30, 33, 42),
            sf::Color(26, 29, 37),    sf::Color(23, 25, 32),    sf::Color(20, 22, 28),
            sf::Color(17, 19, 25),    sf::Color(15, 16, 22),
        },
        sf::Color(199, 205, 212),
        sf::Color(244, 246, 249), sf::Color(186, 193, 201), sf::Color(186, 193, 201),
        sf::Color(255, 255, 255), sf::Color(54, 62, 74), 512, "Graphite"
    },
    // 8: Dark (dark family 1, deep black)
    {
        {
            sf::Color(0,0,0),
            sf::Color(90, 30, 30), sf::Color(120, 34, 30), sf::Color(150, 40, 32),
            sf::Color(180, 46, 34), sf::Color(210, 52, 36), sf::Color(230, 64, 40),
            sf::Color(240, 84, 44), sf::Color(245, 110, 50), sf::Color(250, 140, 60),
            sf::Color(255, 170, 70), sf::Color(255, 195, 80), sf::Color(255, 214, 96),
            sf::Color(250, 226, 112), sf::Color(240, 235, 130), sf::Color(225, 240, 150),
            sf::Color(200, 245, 170), sf::Color(170, 250, 190),
            sf::Color(140, 240, 205), sf::Color(120, 230, 220), sf::Color(110, 215, 235),
            sf::Color(105, 195, 245), sf::Color(100, 170, 250), sf::Color(95, 140, 240),
            sf::Color(90, 110, 225), sf::Color(85, 85, 205), sf::Color(80, 65, 185),
        },
        sf::Color(40, 30, 30),
        sf::Color(25, 20, 20), sf::Color(70, 50, 46), sf::Color(70, 50, 46),
        sf::Color(255, 250, 245), sf::Color(230, 220, 210), 8, "Dark"
    },
    // 9: Midnight (dark family 2, ink blue)
    {
        {
            sf::Color(0,0,0),
            sf::Color(35, 50, 80), sf::Color(40, 60, 95), sf::Color(45, 70, 110),
            sf::Color(50, 82, 128), sf::Color(56, 95, 146), sf::Color(62, 108, 164),
            sf::Color(70, 122, 182), sf::Color(80, 136, 198), sf::Color(92, 150, 214),
            sf::Color(106, 164, 228), sf::Color(122, 178, 240), sf::Color(140, 192, 248),
            sf::Color(158, 204, 252), sf::Color(176, 214, 255), sf::Color(194, 224, 255),
            sf::Color(210, 233, 255), sf::Color(224, 240, 255),
            sf::Color(236, 246, 255), sf::Color(246, 250, 255), sf::Color(255, 252, 250),
            sf::Color(255, 244, 235), sf::Color(255, 232, 214), sf::Color(255, 218, 190),
            sf::Color(255, 202, 166), sf::Color(255, 186, 144), sf::Color(255, 170, 124),
        },
        sf::Color(20, 32, 52),
        sf::Color(14, 24, 40), sf::Color(44, 62, 92), sf::Color(44, 62, 92),
        sf::Color(16, 26, 46), sf::Color(255, 255, 255), 256, "Midnight"
    },
    // 10: Night (dark family 3, night)
    {
        {
            sf::Color(0,0,0),
            sf::Color(40, 30, 60), sf::Color(52, 34, 74), sf::Color(64, 38, 90),
            sf::Color(78, 42, 108), sf::Color(94, 46, 128), sf::Color(112, 50, 148),
            sf::Color(130, 58, 166), sf::Color(150, 70, 182), sf::Color(170, 86, 196),
            sf::Color(190, 104, 208), sf::Color(208, 124, 218), sf::Color(222, 146, 226),
            sf::Color(232, 170, 232), sf::Color(240, 194, 236), sf::Color(246, 216, 240),
            sf::Color(250, 232, 246), sf::Color(255, 244, 252),
            sf::Color(244, 240, 255), sf::Color(228, 232, 255), sf::Color(208, 220, 255),
            sf::Color(186, 206, 255), sf::Color(164, 190, 255), sf::Color(142, 172, 255),
            sf::Color(122, 152, 255), sf::Color(104, 132, 255), sf::Color(90, 114, 255),
        },
        sf::Color(30, 24, 48),
        sf::Color(18, 14, 34), sf::Color(58, 46, 92), sf::Color(58, 46, 92),
        sf::Color(24, 18, 40), sf::Color(255, 255, 255), 512, "Night"
    },
    // 11: Sky (sky blue, extra blue family 1)
    {
        {
            sf::Color(0,0,0),
            sf::Color(224, 245, 255), sf::Color(198, 236, 255), sf::Color(170, 226, 255),
            sf::Color(142, 215, 255), sf::Color(114, 204, 252), sf::Color(92, 192, 248),
            sf::Color(74, 178, 244), sf::Color(60, 162, 240), sf::Color(50, 146, 236),
            sf::Color(44, 130, 232), sf::Color(42, 114, 228), sf::Color(44, 98, 222),
            sf::Color(50, 84, 216), sf::Color(60, 72, 208), sf::Color(74, 62, 200),
            sf::Color(92, 56, 192), sf::Color(112, 54, 184),
            sf::Color(134, 56, 176), sf::Color(156, 62, 168), sf::Color(176, 72, 162),
            sf::Color(194, 84, 158), sf::Color(176, 80, 150), sf::Color(156, 68, 138),
            sf::Color(136, 58, 126), sf::Color(116, 48, 114), sf::Color(96, 40, 102),
        },
        sf::Color(215, 235, 245),
        sf::Color(250, 252, 255), sf::Color(178, 206, 222), sf::Color(178, 206, 222),
        sf::Color(255, 255, 255), sf::Color(40, 55, 75), 256, "Sky"
    },
    // 12: Deep Blue (deep blue, extra blue family 2)
    {
        {
            sf::Color(0,0,0),
            sf::Color(150, 200, 255), sf::Color(120, 178, 255), sf::Color(96, 156, 250),
            sf::Color(78, 136, 244), sf::Color(64, 116, 238), sf::Color(54, 98, 232),
            sf::Color(48, 82, 226), sf::Color(46, 68, 220), sf::Color(48, 56, 214),
            sf::Color(54, 48, 208), sf::Color(64, 44, 202), sf::Color(78, 44, 196),
            sf::Color(94, 48, 190), sf::Color(112, 56, 184), sf::Color(130, 68, 178),
            sf::Color(148, 82, 172), sf::Color(164, 98, 168),
            sf::Color(130, 78, 130), sf::Color(108, 64, 112), sf::Color(90, 54, 96),
            sf::Color(74, 46, 82), sf::Color(62, 40, 70), sf::Color(52, 34, 60),
            sf::Color(44, 30, 52), sf::Color(38, 26, 46), sf::Color(32, 22, 40),
        },
        sf::Color(40, 60, 110),
        sf::Color(26, 40, 78), sf::Color(70, 92, 148), sf::Color(70, 92, 148),
        sf::Color(255, 255, 255), sf::Color(30, 44, 80), 256, "Deep Blue"
    },
    // 13: Violet (violet, purple family 1)
    {
        {
            sf::Color(0,0,0),
            sf::Color(235, 228, 255), sf::Color(218, 205, 252), sf::Color(200, 182, 248),
            sf::Color(182, 159, 244), sf::Color(164, 138, 240), sf::Color(146, 118, 236),
            sf::Color(128, 100, 232), sf::Color(112, 84, 228), sf::Color(98, 70, 224),
            sf::Color(86, 60, 220), sf::Color(78, 54, 216), sf::Color(74, 52, 212),
            sf::Color(76, 56, 208), sf::Color(84, 64, 204), sf::Color(96, 78, 200),
            sf::Color(112, 96, 196), sf::Color(130, 116, 192),
            sf::Color(148, 138, 188), sf::Color(166, 160, 186), sf::Color(182, 180, 186),
            sf::Color(168, 172, 158), sf::Color(148, 150, 136), sf::Color(128, 128, 116),
            sf::Color(108, 108, 98), sf::Color(90, 90, 82), sf::Color(74, 74, 68),
        },
        sf::Color(215, 208, 235),
        sf::Color(252, 250, 255), sf::Color(182, 172, 206), sf::Color(182, 172, 206),
        sf::Color(255, 255, 255), sf::Color(60, 50, 95), 256, "Violet"
    },
    // 14: Purple (purple, purple family 2)
    {
        {
            sf::Color(0,0,0),
            sf::Color(120, 90, 200), sf::Color(110, 74, 196), sf::Color(100, 60, 192),
            sf::Color(92, 48, 188), sf::Color(86, 40, 184), sf::Color(82, 36, 180),
            sf::Color(80, 36, 176), sf::Color(82, 42, 172), sf::Color(88, 52, 168),
            sf::Color(98, 66, 164), sf::Color(112, 84, 160), sf::Color(128, 104, 158),
            sf::Color(144, 126, 158), sf::Color(160, 148, 160), sf::Color(176, 170, 164),
            sf::Color(192, 192, 170), sf::Color(170, 150, 200), sf::Color(140, 118, 180),
            sf::Color(112, 90, 160), sf::Color(88, 68, 140), sf::Color(66, 50, 120),
            sf::Color(48, 36, 100), sf::Color(40, 30, 86), sf::Color(34, 26, 74),
            sf::Color(28, 22, 64),
        },
        sf::Color(70, 55, 110),
        sf::Color(48, 36, 80), sf::Color(104, 82, 148), sf::Color(104, 82, 148),
        sf::Color(255, 255, 255), sf::Color(250, 245, 255), 2, "Purple"
    },
    // 15: Pink (pink blossom, pink family)
    {
        {
            sf::Color(0,0,0),
            sf::Color(255, 224, 238), sf::Color(255, 206, 230), sf::Color(255, 188, 222),
            sf::Color(255, 168, 214), sf::Color(255, 146, 206), sf::Color(255, 124, 198),
            sf::Color(255, 104, 190), sf::Color(255, 86, 182), sf::Color(255, 72, 174),
            sf::Color(252, 64, 168), sf::Color(248, 60, 162), sf::Color(244, 62, 158),
            sf::Color(240, 70, 156), sf::Color(236, 84, 156), sf::Color(232, 102, 158),
            sf::Color(228, 122, 162), sf::Color(226, 142, 168),
            sf::Color(200, 140, 158), sf::Color(176, 116, 138), sf::Color(152, 94, 118),
            sf::Color(130, 76, 100), sf::Color(110, 60, 84), sf::Color(92, 48, 70),
        },
        sf::Color(255, 214, 232),
        sf::Color(255, 250, 253), sf::Color(232, 178, 200), sf::Color(232, 178, 200),
        sf::Color(255, 255, 255), sf::Color(150, 60, 104), 128, "Pink"
    },
    // 16: Mono Light (mono light, black-white gradient family 1)
    {
        {
            sf::Color(0,0,0),
            sf::Color(250, 250, 250), sf::Color(244, 244, 244), sf::Color(238, 238, 238),
            sf::Color(230, 230, 230), sf::Color(220, 220, 220), sf::Color(208, 208, 208),
            sf::Color(194, 194, 194), sf::Color(178, 178, 178), sf::Color(160, 160, 160),
            sf::Color(140, 140, 140), sf::Color(120, 120, 120), sf::Color(100, 100, 100),
            sf::Color(84, 84, 84),    sf::Color(70, 70, 70),    sf::Color(58, 58, 58),
            sf::Color(48, 48, 48),    sf::Color(40, 40, 40),
            sf::Color(34, 34, 34),    sf::Color(30, 30, 30),    sf::Color(27, 27, 27),
            sf::Color(25, 25, 25),    sf::Color(23, 23, 23),    sf::Color(21, 21, 21),
            sf::Color(19, 19, 19),    sf::Color(17, 17, 17),    sf::Color(15, 15, 15),
        },
        sf::Color(230, 230, 230),
        sf::Color(255, 255, 255), sf::Color(210, 210, 210), sf::Color(210, 210, 210),
        sf::Color(255, 255, 255), sf::Color(30, 30, 30), 256, "Mono Light"
    },
    // 17: Mono Dark (mono dark, black-white gradient family 2)
    {
        {
            sf::Color(0,0,0),
            sf::Color(30, 30, 30), sf::Color(34, 34, 34), sf::Color(38, 38, 38),
            sf::Color(42, 42, 42), sf::Color(47, 47, 47), sf::Color(52, 52, 52),
            sf::Color(58, 58, 58), sf::Color(65, 65, 65), sf::Color(73, 73, 73),
            sf::Color(82, 82, 82), sf::Color(92, 92, 92), sf::Color(103, 103, 103),
            sf::Color(116, 116, 116), sf::Color(130, 130, 130), sf::Color(146, 146, 146),
            sf::Color(163, 163, 163), sf::Color(182, 182, 182),
            sf::Color(200, 200, 200), sf::Color(216, 216, 216), sf::Color(230, 230, 230),
            sf::Color(240, 240, 240), sf::Color(247, 247, 247), sf::Color(252, 252, 252),
            sf::Color(255, 255, 255), sf::Color(255, 255, 255), sf::Color(255, 255, 255),
        },
        sf::Color(18, 18, 18),
        sf::Color(12, 12, 12), sf::Color(48, 48, 48), sf::Color(48, 48, 48),
        sf::Color(45, 45, 45), sf::Color(255, 255, 255), 8192, "Mono Dark"
    },
    // 18: Mist (mist white, warm white -> warm gray)
    {
        { sf::Color(0,0,0) },
        sf::Color(226, 223, 217), sf::Color(253, 252, 250), sf::Color(208, 204, 197), sf::Color(208, 204, 197),
        sf::Color(255, 255, 255), sf::Color(82, 74, 64), 128, "Mist"
    },
    // 19: Dawn (dawn, cream -> peach -> warm brown)
    {
        { sf::Color(0,0,0) },
        sf::Color(240, 226, 214), sf::Color(255, 250, 245), sf::Color(224, 204, 190), sf::Color(224, 204, 190),
        sf::Color(255, 255, 255), sf::Color(110, 70, 48), 128, "Dawn"
    },
    // 20: Mint (mint, mint green -> deep teal)
    {
        { sf::Color(0,0,0) },
        sf::Color(214, 232, 220), sf::Color(248, 252, 248), sf::Color(196, 216, 204), sf::Color(196, 216, 204),
        sf::Color(255, 255, 255), sf::Color(40, 70, 55), 256, "Mint"
    },
    // 21: Coral (coral, coral orange -> deep coral red)
    {
        { sf::Color(0,0,0) },
        sf::Color(238, 218, 206), sf::Color(255, 250, 246), sf::Color(224, 196, 184), sf::Color(224, 196, 184),
        sf::Color(255, 255, 255), sf::Color(110, 50, 40), 128, "Coral"
    },
    // 22: Evergreen (evergreen, light pine -> deep green)
    {
        { sf::Color(0,0,0) },
        sf::Color(200, 216, 202), sf::Color(244, 248, 244), sf::Color(180, 200, 186), sf::Color(180, 200, 186),
        sf::Color(255, 255, 255), sf::Color(40, 62, 48), 128, "Evergreen"
    },
    // 23: Obsidian (obsidian, deep charcoal -> black-teal, dark family)
    {
        { sf::Color(0,0,0) },
        sf::Color(28, 34, 42), sf::Color(16, 20, 26), sf::Color(46, 54, 64), sf::Color(46, 54, 64),
        sf::Color(255, 255, 255), sf::Color(255, 255, 255), 2, "Obsidian"
    },
    // 24: Sand (golden sand, sand gold -> bronze)
    {
        { sf::Color(0,0,0) },
        sf::Color(234, 224, 202), sf::Color(255, 251, 244), sf::Color(218, 204, 178), sf::Color(218, 204, 178),
        sf::Color(255, 255, 255), sf::Color(90, 66, 40), 128, "Sand"
    },
    // 25: Mist Blue (mist blue, mist blue -> steel blue)
    {
        { sf::Color(0,0,0) },
        sf::Color(214, 226, 236), sf::Color(250, 252, 254), sf::Color(198, 210, 222), sf::Color(198, 210, 222),
        sf::Color(255, 255, 255), sf::Color(50, 66, 84), 128, "Mist Blue"
    },
    // 26: Berry (berry, raspberry -> deep plum)
    {
        { sf::Color(0,0,0) },
        sf::Color(234, 216, 222), sf::Color(254, 250, 251), sf::Color(218, 194, 202), sf::Color(218, 194, 202),
        sf::Color(255, 255, 255), sf::Color(100, 40, 70), 256, "Berry"
    },
    // 27: Cream (cream, cream -> caramel)
    {
        { sf::Color(0,0,0) },
        sf::Color(236, 228, 214), sf::Color(255, 252, 247), sf::Color(220, 208, 190), sf::Color(220, 208, 190),
        sf::Color(255, 255, 255), sf::Color(96, 66, 44), 128, "Cream"
    },
    // 28: Champagne (champagne, champagne gold -> deep gold)
    {
        { sf::Color(0,0,0) },
        sf::Color(240, 232, 210), sf::Color(255, 252, 245), sf::Color(226, 214, 188), sf::Color(226, 214, 188),
        sf::Color(255, 255, 255), sf::Color(96, 74, 40), 128, "Champagne"
    },
    // 29: Celadon (celadon, celadon green -> teal blue)
    {
        { sf::Color(0,0,0) },
        sf::Color(222, 232, 226), sf::Color(250, 253, 250), sf::Color(206, 218, 210), sf::Color(206, 218, 210),
        sf::Color(255, 255, 255), sf::Color(46, 72, 66), 128, "Celadon"
    },
    // 30: Amethyst (amethyst, pale amethyst -> deep purple)
    {
        { sf::Color(0,0,0) },
        sf::Color(228, 222, 240), sf::Color(253, 251, 255), sf::Color(212, 204, 226), sf::Color(212, 204, 226),
        sf::Color(255, 255, 255), sf::Color(72, 54, 110), 128, "Amethyst"
    },
    // 31: Terracotta (terracotta, terracotta -> brick red)
    {
        { sf::Color(0,0,0) },
        sf::Color(236, 220, 206), sf::Color(255, 250, 246), sf::Color(222, 200, 184), sf::Color(222, 200, 184),
        sf::Color(255, 255, 255), sf::Color(100, 50, 38), 256, "Terracotta"
    },
    // 32: Night Sakura (night sakura, deep sakura pink, dark family)
    {
        { sf::Color(0,0,0) },
        sf::Color(60, 40, 58), sf::Color(30, 20, 34), sf::Color(66, 48, 66), sf::Color(66, 48, 66),
        sf::Color(255, 255, 255), sf::Color(255, 255, 255), 2, "Night Sakura"
    }
};

// ========== New color schemes: 26-stage gradient generation ==========
static sf::Color lerpCol(const sf::Color& a, const sf::Color& b, float t)
{
    return sf::Color((sf::Uint8)(a.r + (b.r - a.r) * t),
                     (sf::Uint8)(a.g + (b.g - a.g) * t),
                     (sf::Uint8)(a.b + (b.b - a.b) * t));
}
// Light segment (2..128) 3-stage gradient + dark segment (256..2^26) 3-stage gradient; threshold 128 keeps digit contrast
static void fillGradLightDark(SchemeData& s,
    const sf::Color& l0, const sf::Color& l1, const sf::Color& l2,
    const sf::Color& d0, const sf::Color& d1, const sf::Color& d2)
{
    for (int p = 1; p <= 4; ++p)  s.tileColors[p] = lerpCol(l0, l1, (float)(p - 1) / 3.f);
    for (int p = 5; p <= 7; ++p)  s.tileColors[p] = lerpCol(l1, l2, (float)(p - 5) / 2.f);
    for (int p = 8; p <= 16; ++p) s.tileColors[p] = lerpCol(d0, d1, (float)(p - 8) / 8.f);
    for (int p = 17; p <= 26; ++p)s.tileColors[p] = lerpCol(d1, d2, (float)(p - 17) / 9.f);
}
// Dark family: full range fades a -> b (white text, threshold 2)
static void fillGradAllDark(SchemeData& s, const sf::Color& a, const sf::Color& b)
{
    for (int p = 1; p <= MAX_POWER; ++p)
        s.tileColors[p] = lerpCol(a, b, (float)(p - 1) / (float)(MAX_POWER - 1));
}

static void initAddedSchemes()
{
    // 18: Mist (mist white, warm white -> warm gray)
    fillGradLightDark(SCHEMES[18], sf::Color(250,249,247), sf::Color(236,233,227), sf::Color(214,208,196),
                      sf::Color(168,158,144), sf::Color(120,110,98), sf::Color(64,56,48));
    // 19: Dawn (dawn, cream -> peach -> warm brown)
    fillGradLightDark(SCHEMES[19], sf::Color(255,248,238), sf::Color(250,230,205), sf::Color(244,210,170),
                      sf::Color(214,150,110), sf::Color(168,96,66), sf::Color(100,52,34));
    // 20: Mint (mint, mint green -> deep teal)
    fillGradLightDark(SCHEMES[20], sf::Color(238,250,240), sf::Color(214,240,218), sf::Color(186,226,196),
                      sf::Color(118,186,140), sf::Color(60,138,88), sf::Color(24,78,48));
    // 21: Coral (coral, coral orange -> deep coral red)
    fillGradLightDark(SCHEMES[21], sf::Color(255,242,234), sf::Color(252,222,206), sf::Color(248,198,176),
                      sf::Color(236,130,110), sf::Color(196,74,58), sf::Color(128,38,28));
    // 22: Evergreen (evergreen, light pine -> deep green)
    fillGradLightDark(SCHEMES[22], sf::Color(240,247,240), sf::Color(220,236,220), sf::Color(198,222,198),
                      sf::Color(130,168,132), sf::Color(64,110,70), sf::Color(24,58,32));
    // 23: Obsidian (obsidian, black-teal, dark family)
    fillGradAllDark(SCHEMES[23], sf::Color(70,84,100), sf::Color(10,14,22));
    // 24: Sand (golden sand, sand gold -> bronze)
    fillGradLightDark(SCHEMES[24], sf::Color(252,246,232), sf::Color(244,232,206), sf::Color(234,216,180),
                      sf::Color(188,156,110), sf::Color(136,104,62), sf::Color(76,54,28));
    // 25: Mist Blue (mist blue, mist blue -> steel blue)
    fillGradLightDark(SCHEMES[25], sf::Color(240,246,251), sf::Color(222,234,244), sf::Color(202,220,236),
                      sf::Color(128,158,188), sf::Color(70,100,132), sf::Color(28,46,70));
    // 26: Berry (berry, raspberry -> deep plum)
    fillGradLightDark(SCHEMES[26], sf::Color(252,240,244), sf::Color(246,222,232), sf::Color(238,204,220),
                      sf::Color(196,110,150), sf::Color(148,60,102), sf::Color(88,28,58));
    // 27: Cream (cream, cream -> caramel)
    fillGradLightDark(SCHEMES[27], sf::Color(255,250,242), sf::Color(250,240,226), sf::Color(244,228,208),
                      sf::Color(196,160,122), sf::Color(148,110,76), sf::Color(90,60,40));
    // 28: Champagne (champagne, champagne gold -> deep gold)
    fillGradLightDark(SCHEMES[28], sf::Color(255,250,240), sf::Color(250,240,222), sf::Color(244,228,202),
                      sf::Color(206,178,128), sf::Color(160,126,80), sf::Color(100,72,42));
    // 29: Celadon (celadon, celadon green -> teal blue)
    fillGradLightDark(SCHEMES[29], sf::Color(240,248,244), sf::Color(222,240,232), sf::Color(204,230,220),
                      sf::Color(124,176,166), sf::Color(58,120,112), sf::Color(24,66,62));
    // 30: Amethyst (amethyst, pale amethyst -> deep purple)
    fillGradLightDark(SCHEMES[30], sf::Color(246,242,252), sf::Color(234,226,248), sf::Color(220,208,242),
                      sf::Color(156,130,204), sf::Color(100,72,156), sf::Color(50,32,96));
    // 31: Terracotta (terracotta, terracotta -> brick red)
    fillGradLightDark(SCHEMES[31], sf::Color(255,244,238), sf::Color(250,226,212), sf::Color(244,206,184),
                      sf::Color(220,140,100), sf::Color(176,90,60), sf::Color(110,50,32));
    // 32: Night Sakura (night sakura, deep plum pink, dark family)
    fillGradAllDark(SCHEMES[32], sf::Color(90,50,70), sf::Color(22,12,20));
}

// Compare two schemes for exact equality (highlights the "currently applied" theme in the list)
static bool schemeEquals(const SchemeData& a, const SchemeData& b)
{
    if (a.emptyColor != b.emptyColor || a.windowBg != b.windowBg || a.boardBg != b.boardBg ||
        a.scoreBoxColor != b.scoreBoxColor || a.textLight != b.textLight || a.textDark != b.textDark ||
        a.lightTextFromValue != b.lightTextFromValue)
        return false;
    for (int k = 0; k <= MAX_POWER; ++k)
        if (a.tileColors[k] != b.tileColors[k]) return false;
    return true;
}

// ========== Custom color preset save ==========
struct CustomPreset {
    bool used;                            // whether this slot is occupied
    sf::Color tileColors[MAX_POWER + 1];
    sf::Color emptyColor;
    sf::Color windowBg;
    sf::Color boardBg;
    sf::Color scoreBoxColor;
    sf::Color textLight;
    sf::Color textDark;
    int lightTextFromValue;
};

// ========== Custom color dialog state ==========
enum ColorEditTarget {
    CEDIT_NONE = -1,
    CEDIT_WINDOW_BG = 0,
    CEDIT_BOARD_BG,
    CEDIT_EMPTY_COLOR,
    CEDIT_SCOREBOX_COLOR,
    CEDIT_TEXT_LIGHT,
    CEDIT_TEXT_DARK,
    CEDIT_SYSTEM_TEXT,        // system text: follows the dark text color (cs.textDark)
    CEDIT_TILE_COLOR_START = 100   // + power (1..MAX_POWER)
};

struct CustomColorDlg {
    bool open;
    float dlgX, dlgY, dlgW, dlgH;
    // color list area (1..MAX_POWER + global colors)
    float listAreaX, listAreaY, listAreaW, listAreaH;
    float itemH;
    int scrollPos;
    int hoverIdx;

    // top theme selection area (new in color dialog v2)
    int presetScroll;    // preset list scroll
    int customScroll;    // custom list scroll
    // currently selected item in the theme/custom lists (preview only, not applied); -1 = none
    int selPresetIdx;
    int selCustomIdx;
    bool needScrollToCurrent;  // auto-scroll to the current theme on the first draw after opening
    // theme/custom list item hit rects (filled while drawing)
    float presetItemX, presetItemY, presetItemW, presetItemH;
    float customItemX, customItemY, customItemW, customItemH;
    float presetListH, customListH;  // full list height (for wheel events)
    // custom list item delete-button hit rects [20]
    float customDelX[MAX_CUSTOM_PRESETS], customDelY[MAX_CUSTOM_PRESETS],
          customDelW[MAX_CUSTOM_PRESETS], customDelH[MAX_CUSTOM_PRESETS];
    // top theme selection area layout (for click hit-testing)
    float topAreaX, topAreaY, topAreaW, topAreaH;

    // current edit target
    int editTarget;   // ColorEditTarget
    // RGB sliders (fields kept for compatibility, no longer drawn)
    float rBarX, rBarY, rBarW, rBarH;
    float gBarX, gBarY, gBarW, gBarH;
    float bBarX, bBarY, bBarW, bBarH;
    float rThumbX, gThumbX, bThumbX;  // slider thumb absolute X
    bool draggingR, draggingG, draggingB;

    // RGB number box (Spinner input) click area (for hit-testing)
    float rValX, rValY, rValW, rValH;
    float gValX, gValY, gValW, gValH;
    float bValX, bValY, bValW, bValH;

    // Spinner left/right triangle arrow click areas [0=R, 1=G, 2=B]
    float spinLeftX[3], spinLeftY[3], spinLeftW[3], spinLeftH[3];
    float spinRightX[3], spinRightY[3], spinRightW[3], spinRightH[3];
    // Spinner step (10 while Shift held, otherwise 1)
    int spinStep;

    // RGB edit state (-1=none, 0=R, 1=G, 2=B)
    int rgbEditing;
    std::string rgbInputStr;   // input stored as std::string
    int rgbLastValid;   // last valid value (fallback when out of range)
    sf::Clock rgbCursorClock;  // cursor blink timer

    // preview board / color picker
    float previewX, previewY, previewSize;
    float pickerH;  // v6.1: picker height (width=previewSize, height=2/3 x width)
    int previewGrid; // 5 for 5x5 preview
    // preview board tile click hotspots
    float previewTileStartX, previewTileStartY;
    float previewTileGap, previewTileSize;
    int previewTileHoverCol, previewTileHoverRow;

    // picker hover / drag state
    bool hoverPickerActive;
    sf::Color hoverPreviewColor;
    float hoverPickerX, hoverPickerY;  // hover position (stored directly, avoids back-calculation)
    bool draggingPicker;

    // action buttons
    float saveBtnX, saveBtnY, saveBtnW, saveBtnH;
    float closeBtnX, closeBtnY, closeBtnW, closeBtnH;
    float applyBtnX, applyBtnY, applyBtnW, applyBtnH;

    // import/export theme buttons right of the middle title (filled while drawing, read on click)
    float importBtnX, importBtnY, importBtnW, importBtnH;
    float exportBtnX, exportBtnY, exportBtnW, exportBtnH;

    // save-slot (6) rect cache (filled while drawing, read on click)
    float slotX[MAX_CUSTOM_PRESETS], slotY[MAX_CUSTOM_PRESETS],
          slotW[MAX_CUSTOM_PRESETS], slotH[MAX_CUSTOM_PRESETS];
    float slotDelX[MAX_CUSTOM_PRESETS], slotDelY[MAX_CUSTOM_PRESETS],
          slotDelW[MAX_CUSTOM_PRESETS], slotDelH[MAX_CUSTOM_PRESETS];

    // save list
    CustomPreset presets[MAX_CUSTOM_PRESETS];

    // save-full hint
    bool alertPresetFull;

    // brightness/depth slider (0=blackest, 255=original)
    float brightBarX, brightBarY, brightBarW, brightBarH;
    float brightThumbX;
    bool draggingBright;
    int brightness;

    // working buffer: edits only touch this; Apply commits it to SCHEMES[CUSTOM_SCHEME_IDX]
    SchemeData workingScheme = {};
};
static CustomColorDlg g_ccDlg = {};

// ========== Animation data structures ==========
struct TileAnim {
    float fromCol = 0.f, fromRow = 0.f;
    float toCol = 0.f, toRow = 0.f;
    float progress = 0.f;    // 0-1, slide progress
    float scale = 1.f;        // current scale
    bool  spawning = false;     // appearing animation
    bool  merging = false;      // merge pulse animation
    bool  dead = false;         // tile was consumed in merge
    int   value = 0;
    float mergePhase = 0.f;   // 0-1 for merge pulse
};

// ========== Core game class ==========
class Game2048;

// Fill (obstacle) tiles: cannot merge and occupy space (used for special-formation training)
// Represented as a unique "super-large value" (>2^26): each cell = FILL_BASE + counter*16, all distinct -> never merge;
// above 2^26 they exceed any normal tile -> no merge, they participate in move/AI math;
// their digits are hidden (skipped while drawing). 0=empty; >0 and <FILL_BASE = normal tile value.
static const int TILE_FILL = -1;          // legacy marker (old saves/internal marks)
static const int FILL_BASE = (1 << 27) + 1;   // > 2^26, beyond any normal tile
static int g_fillValueCounter = 0;         // per-fill increment counter (+1 per placement)
static inline bool isFillValue(int v) { return v < 0 || v >= FILL_BASE; }
static inline int fillValueFor(int r, int c) { return FILL_BASE + (r * 8 + c) * 16; }
static inline int nextFillValue() { return FILL_BASE + (++g_fillValueCounter) * 16; }

static void saveHistory(const Game2048& game);

class Game2048
{
public:
    Game2048()
    {
        std::srand((unsigned)std::time(nullptr));
        m_boardSize = DEFAULT_BOARD_SIZE;
        m_colorScheme = 0;
        m_animating = false;
        m_animDuration = ANIM_DURATION_DEFAULT;  // default manual 150ms classic animation
        m_newTileRow = -1;
        m_newTileCol = -1;
        init();
    }

    void init()
    {
        // before starting a new game, save the current board to history
        // only when the current state is not already saved (prevents duplicate saves on slot switches)
        if (!m_historySaved && (m_score > 0 || m_board[0][0] != 0))
            saveHistory(*this);
        m_historySaved = false;  // new game not yet saved to history
        std::memset(m_board, 0, sizeof(m_board));
        m_score = 0;
        m_canUndo = false;
        std::memset(m_prevBoard, 0, sizeof(m_prevBoard));
        m_prevScore = 0;
        m_aiAssisted = false;   // new game resets the AI-assisted flag
        m_animations.clear();
        m_animating = false;
        m_newTileRow = -1;
        m_newTileCol = -1;
        m_viewingHistory = false;
        // during init: do not record the initial tiles as a move; reset the recorder after addRandomTile
        m_recInit = true;
        addRandomTile();
        addRandomTile();
        m_recInit = false;
        recReset();   // record the initial board (with the 2 starting tiles) as replay frame 0
        std::string bs = std::to_string(m_boardSize) + "x" + std::to_string(m_boardSize);
        startNewGameLog(bs);
    }

    // Finish the current animation instantly (interrupt for fast chains)
    void finishAnimation()
    {
        m_animating = false;
        m_animations.clear();
        m_newTileRow = -1;
        m_newTileCol = -1;
    }

    bool move(int dx, int dy)
    {
        // allow a new move immediately during animation: finish the current animation instantly
        if (m_animating)
            finishAnimation();

        m_animations.clear();
        bool moved = false;
        int temp[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
        std::memcpy(temp, m_board, sizeof(temp));
        int scoreBefore = m_score; // score before the move, so undo restores it correctly

        if (dx == -1)
        {
            for (int i = 0; i < m_boardSize; ++i)
                moved |= moveLineLeftTracked(temp[i], i, 0);
        }
        else if (dx == 1)
        {
            for (int i = 0; i < m_boardSize; ++i)
            {
                int line[MAX_BOARD_SIZE];
                for (int j = 0; j < m_boardSize; ++j)
                    line[j] = temp[i][m_boardSize - 1 - j];
                moved |= moveLineLeftTracked(line, i, 1);
                for (int j = 0; j < m_boardSize; ++j)
                    temp[i][m_boardSize - 1 - j] = line[j];
            }
        }


        else if (dy == -1)
        {
            for (int j = 0; j < m_boardSize; ++j)
            {
                int line[MAX_BOARD_SIZE];
                for (int i = 0; i < m_boardSize; ++i)
                    line[i] = temp[i][j];
                moved |= moveLineLeftTracked(line, j, 2);
                for (int i = 0; i < m_boardSize; ++i)
                    temp[i][j] = line[i];
            }
        }
        else if (dy == 1)
        {
            for (int j = 0; j < m_boardSize; ++j)
            {
                int line[MAX_BOARD_SIZE];
                for (int i = 0; i < m_boardSize; ++i)
                    line[i] = temp[m_boardSize - 1 - i][j];
                moved |= moveLineLeftTracked(line, j, 3);
                for (int i = 0; i < m_boardSize; ++i)
                    temp[m_boardSize - 1 - i][j] = line[i];
            }
        }

        if (moved)
        {
            saveState();
            m_prevScore = scoreBefore; // fix: undo restores the pre-move score (saveState holds the already-added m_score)
            std::memcpy(m_board, temp, sizeof(m_board));
            m_animating = true;
            m_animClock.restart();
            m_historySaved = false;  // state changed; needs re-saving to history
            if (m_recording && !m_recInit)
            {
                int dir = (dx == -1) ? 0 : (dx == 1) ? 1 : (dy == -1) ? 2 : 3;
                m_recDirs.push_back(dir);
            }
        }
        return moved;
    }

    // ===== History replay animation support =====
    // Switch this instance to a given history frame (frozen display, for replay-track drag/jump)
    void setBoardFrame(int bs, int cs, int sc, const std::vector<int>& frame)
    {
        m_boardSize = bs;
        m_colorScheme = cs;
        m_score = sc;
        m_canUndo = false;
        m_animations.clear();
        m_animating = false;
        m_newTileRow = -1;
        m_newTileCol = -1;
        m_recording = false;   // replay instances do not record
        std::memset(m_board, 0, sizeof(m_board));
        for (int i = 0; i < bs; ++i)
            for (int j = 0; j < bs; ++j)
                if (i * bs + j < (int)frame.size())
                    m_board[i][j] = frame[i * bs + j];
    }
    // Replay: execute one move and align the spawned tile to the next frame (slide/merge/spawn animation)
    void applyPlaybackMove(int dir, const std::vector<int>& nextFrame)
    {
        int dx = (dir == 0) ? -1 : (dir == 1) ? 1 : 0;
        int dy = (dir == 2) ? -1 : (dir == 3) ? 1 : 0;
        move(dx, dy);
        // find the new tile of the next frame (the only difference after this move)
        int nr = -1, nc = -1, nv = 0;
        for (int i = 0; i < m_boardSize; ++i)
            for (int j = 0; j < m_boardSize; ++j)
            {
                int next = (i * m_boardSize + j < (int)nextFrame.size()) ? nextFrame[i * m_boardSize + j] : 0;
                if (next != m_board[i][j]) { nr = i; nc = j; nv = next; }
            }
        if (nr >= 0)
        {
            m_board[nr][nc] = nv;
            m_newTileRow = nr;
            m_newTileCol = nc;
        }
    }

    // Move line left and track animations
    // Only creates animations for tiles that actually move
    bool moveLineLeftTracked(int line[], int fixed, int orientation)
    {
        int oldLine[MAX_BOARD_SIZE];
        std::memcpy(oldLine, line, sizeof(int) * m_boardSize);

        // Fill (obstacle, -1) tiles are "walls": they never move, never merge, and others cannot pass.
        // Split the row into segments at obstacles; each segment compacts/merges left independently; obstacles stay put.
        int merged[MAX_BOARD_SIZE] = { 0 };
        bool moved = false;

        int segStart = 0;
        for (int j = 0; j <= m_boardSize; ++j)
        {
            if (j == m_boardSize || isFillValue(oldLine[j]))
            {
                // ---- handle segment [segStart, j) ----
                int srcPos[MAX_BOARD_SIZE];
                int compacted[MAX_BOARD_SIZE];
                int idx = 0;
                for (int k = segStart; k < j; ++k)
                {
                    if (oldLine[k] != 0)
                    {
                        srcPos[idx] = k;
                        compacted[idx] = oldLine[k];
                        idx++;
                    }
                }
                int mIdx = segStart;
                for (int k = 0; k < idx; ++k)
                {
                    if (k + 1 < idx && compacted[k] == compacted[k + 1])
                    {
                        int mergedVal = compacted[k] * 2;
                        merged[mIdx] = mergedVal;
                        m_score += mergedVal;

                        float fromC, fromR, toC, toR;
                        getCellCoords(fixed, srcPos[k], orientation, fromC, fromR);
                        getCellCoords(fixed, mIdx, orientation, toC, toR);
                        if (srcPos[k] != mIdx)
                        {
                            addSlideAnim(fromC, fromR, toC, toR, compacted[k], true);
                            moved = true;
                        }
                        getCellCoords(fixed, srcPos[k + 1], orientation, fromC, fromR);
                        if (srcPos[k + 1] != mIdx)
                        {
                            addSlideAnim(fromC, fromR, toC, toR, compacted[k + 1], true);
                            moved = true;
                        }
                        addMergeAnim(toC, toR, mergedVal);

                        k++;
                        mIdx++;
                    }
                    else
                    {
                        merged[mIdx] = compacted[k];
                        if (srcPos[k] != mIdx)
                        {
                            float fromC, fromR, toC, toR;
                            getCellCoords(fixed, srcPos[k], orientation, fromC, fromR);
                            getCellCoords(fixed, mIdx, orientation, toC, toR);
                            addSlideAnim(fromC, fromR, toC, toR, compacted[k], false);
                            moved = true;
                        }
                        mIdx++;
                    }
                }
                // obstacles stay fixed (keep their unique super-large value so they are not treated as normal tiles)
                if (j < m_boardSize) merged[j] = oldLine[j];
                segStart = j + 1;
            }
        }

        for (int j = 0; j < m_boardSize; ++j)
            line[j] = merged[j];
        return moved;
    }

    void addSlideAnim(float fromC, float fromR, float toC, float toR, int value, bool dead)
    {
        TileAnim a;
        a.fromCol = fromC; a.fromRow = fromR;
        a.toCol = toC; a.toRow = toR;
        a.progress = 0; a.scale = 1.f;
        a.spawning = false; a.merging = false; a.dead = dead;
        a.mergePhase = 0; a.value = value;
        m_animations.push_back(a);
    }

    void addMergeAnim(float col, float row, int value)
    {
        TileAnim a;
        a.fromCol = col; a.fromRow = row;
        a.toCol = col; a.toRow = row;
        a.progress = 0; a.scale = 1.f;
        a.spawning = false; a.merging = true; a.dead = false;
        a.mergePhase = 0; a.value = value;
        m_animations.push_back(a);
    }

    // Helper: convert (fixed, pos, orientation) to board coordinates (col, row)
    void getCellCoords(int fixed, int pos, int orientation, float& col, float& row)
    {
        switch (orientation)
        {
        case 0: // row, moving left: fixed=row, pos=col
            col = (float)pos; row = (float)fixed; break;
        case 1: // row, moving right: fixed=row, pos=col (reversed)
            col = (float)(m_boardSize - 1 - pos); row = (float)fixed; break;
        case 2: // col, moving up: fixed=col, pos=row
            col = (float)fixed; row = (float)pos; break;
        case 3: // col, moving down: fixed=col, pos=row (reversed)
            col = (float)fixed; row = (float)(m_boardSize - 1 - pos); break;
        }
    }

    bool canMove() const
    {
        for (int i = 0; i < m_boardSize; ++i)
        {
            for (int j = 0; j < m_boardSize; ++j)
            {
                if (m_board[i][j] == 0) return true;
                if (!isFillValue(m_board[i][j]))   // fill (obstacle) tiles cannot move or merge
                {
                    if (i < m_boardSize - 1 && m_board[i][j] == m_board[i + 1][j]) return true;
                    if (j < m_boardSize - 1 && m_board[i][j] == m_board[i][j + 1]) return true;
                }
            }
        }
        return false;
    }

    void addRandomTile()
    {
        int empty = getEmptyCount();
        if (empty == 0) return;
        int r = std::rand() % empty;
        int idx = 0;
        for (int i = 0; i < m_boardSize; ++i)
        {
            for (int j = 0; j < m_boardSize; ++j)
            {
                if (m_board[i][j] == 0)
                {
                    if (idx == r)
                    {
                        m_board[i][j] = (std::rand() % 10 == 0) ? 4 : 2;
                        m_newTileRow = i;
                        m_newTileCol = j;
                        recordStep();   // record one step after every successful move + spawn (except during init)
                        return;
                    }
                    idx++;
                }
            }
        }
    }

    int getTile(int i, int j) const { return m_board[i][j]; }
    const int* getTileRow(int i) const { return m_board[i]; }
    const int* getBoard() const { return m_board[0]; }
    int getScore() const { return m_score; }
    int getBoardSize() const { return m_boardSize; }
    int getColorScheme() const { return m_colorScheme; }
    bool canUndo() const { return m_canUndo; }
    // previous position (before the last move), for practice-mode per-step loss analysis
    bool hasPrevState() const { return m_canUndo; }
    const int* getPrevBoardRow(int i) const { return m_prevBoard[i]; }
    int getPrevScore() const { return m_prevScore; }
    bool isAnimating() const { return m_animating; }
    int  getNewTileRow() const { return m_newTileRow; }
    int  getNewTileCol() const { return m_newTileCol; }
    const std::vector<TileAnim>& getAnimations() const { return m_animations; }
    bool isAiAssisted() const { return m_aiAssisted; }
    void markAiAssisted() { m_aiAssisted = true; }
    void clearAiAssisted() { m_aiAssisted = false; }

    // Challenge-mode reset: clears the board but does NOT write history (challenge start/switch)
    void resetNoHistory()
    {
        m_historySaved = true;   // prevent init() from saving the old game to history
        init();
        m_historySaved = false;
    }
    void setBoardSizeNoHistory(int size)
    {
        if (size < MIN_BOARD_SIZE) size = MIN_BOARD_SIZE;
        if (size > MAX_BOARD_SIZE) size = MAX_BOARD_SIZE;
        m_historySaved = true;
        m_boardSize = size;
        init();
        m_historySaved = false;
    }

    // current board max tile (challenge success check)
    int getMaxTile() const
    {
        int mx = 0;
        for (int i = 0; i < m_boardSize; ++i)
            for (int j = 0; j < m_boardSize; ++j)
                if (m_board[i][j] > mx && !isFillValue(m_board[i][j])) mx = m_board[i][j];
        return mx;
    }

    // history view mode: temporarily show a historical board
    bool m_viewingHistory = false;
    int  m_viewBoardSize = 0;
    int  m_viewColorScheme = 0;
    int  m_viewScore = 0;
    int  m_viewBoard[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    bool m_historySaved = false;  // whether the current state is already in history (prevents duplicate slot saves)

    // ===== Replay recording: board/score/elapsed per step =====
    std::vector<std::vector<int>> m_recFrames;   // board after each step (flattened n*n); [0]=initial
    std::vector<int> m_recScores;                // score after each step; [0]=0
    std::vector<int> m_recTimesMs;               // cumulative ms after each step; [0]=0
    std::vector<int> m_recDirs;                  // move direction per step (0L/1R/2U/3D); size=moveCount
    std::vector<int> m_recAnimMs;                // per-step animation ms; size=moveCount (manual 120 / AI by rate)
    float m_recMsAcc = 0.f;                      // cumulative running ms
    bool  m_recording = false;
    bool  m_recInit = false;                     // init-phase flag (initial tiles are not a move)
    bool  m_recStarted = false;                  // whether the first move happened (timing starts from it)

    void recReset()
    {
        m_recFrames.clear(); m_recScores.clear(); m_recTimesMs.clear(); m_recDirs.clear(); m_recAnimMs.clear();
        m_recMsAcc = 0.f; m_recording = true; m_recInit = false; m_recStarted = false;
        std::vector<int> init(m_boardSize * m_boardSize, 0);
        for (int i = 0; i < m_boardSize; ++i)
            for (int j = 0; j < m_boardSize; ++j)
                init[i * m_boardSize + j] = m_board[i][j];
        m_recFrames.push_back(init);
        m_recScores.push_back(0);
        m_recTimesMs.push_back(0);
    }
    void recTick(float dtMs) { if (m_recording && m_recStarted) m_recMsAcc += dtMs; }
    void recordStep()
    {
        if (!m_recording || m_recInit) return;
        if (!m_recStarted)
        {
            m_recStarted = true;
            m_recMsAcc = 0.f;   // start timing from the user's first move
        }
        std::vector<int> f(m_boardSize * m_boardSize, 0);
        for (int i = 0; i < m_boardSize; ++i)
            for (int j = 0; j < m_boardSize; ++j)
                f[i * m_boardSize + j] = m_board[i][j];
        m_recFrames.push_back(f);
        m_recScores.push_back(m_score);
        m_recTimesMs.push_back((int)m_recMsAcc);
        m_recAnimMs.push_back((int)(m_animDuration * 1000.f));   // record this step's actual animation ms
    }
    int  recMoveCount() const { return m_recording ? (int)m_recScores.size() - 1 : 0; }
    int  recTotalMs()   const { return m_recording && !m_recTimesMs.empty() ? m_recTimesMs.back() : 0; }
    int  recFrameVal(int f, int i) const { return (f >= 0 && f < (int)m_recFrames.size() && i < (int)m_recFrames[f].size()) ? m_recFrames[f][i] : 0; }
    int  recScore(int f) const { return (f >= 0 && f < (int)m_recScores.size()) ? m_recScores[f] : 0; }
    int  recTime(int f) const { return (f >= 0 && f < (int)m_recTimesMs.size()) ? m_recTimesMs[f] : 0; }
    int  recDir(int k) const { return (k >= 0 && k < (int)m_recDirs.size()) ? m_recDirs[k] : 0; }
    int  recAnimMs(int k) const { return (k >= 0 && k < (int)m_recAnimMs.size()) ? m_recAnimMs[k] : 0; }

    void enterHistoryView(int bs, int cs, int sc, const int board[MAX_BOARD_SIZE][MAX_BOARD_SIZE])
    {
        m_viewingHistory = true;
        m_viewBoardSize = bs;
        m_viewColorScheme = cs;
        m_viewScore = sc;
        std::memcpy(m_viewBoard, board, sizeof(m_viewBoard));
        finishAnimation();
    }
    // History replay: switch the view board to a given frame
    void setViewFrame(int bs, int cs, int sc, const std::vector<int>& frame)
    {
        m_viewingHistory = true;
        m_viewBoardSize = bs;
        m_viewColorScheme = cs;
        m_viewScore = sc;
        std::memset(m_viewBoard, 0, sizeof(m_viewBoard));
        for (int i = 0; i < bs; ++i)
            for (int j = 0; j < bs; ++j)
                m_viewBoard[i][j] = (int)(i * bs + j < (int)frame.size() ? frame[i * bs + j] : 0);
        finishAnimation();
    }
    void exitHistoryView() { m_viewingHistory = false; }
    bool isViewingHistory() const { return m_viewingHistory; }
    int  getViewBoardSize() const { return m_viewBoardSize; }
    int  getViewScore() const { return m_viewScore; }
    int  getViewTile(int i, int j) const { return m_viewBoard[i][j]; }

    void updateAnimations(float dt)
    {
        recTick(dt * 1000.f);   // advance the recording clock (accumulates even outside animation, for replay timestamps)
        if (!m_animating) return;

        float elapsed = m_animClock.getElapsedTime().asSeconds();
        float t = elapsed / m_animDuration;  // runtime-variable: manual 150ms / AI 50ms

        if (t >= 1.0f)
        {
            // Animation complete
            m_animating = false;
            m_animations.clear();
            m_newTileRow = -1;
            m_newTileCol = -1;
        }
        else
        {
            for (auto& anim : m_animations)
            {
                if (anim.progress < 1.0f)
                {
                    anim.progress = t;
                }
                if (anim.merging)
                {
                    anim.mergePhase = t;
                    // Scale pop: grow then shrink
                    if (t < 0.5f)
                        anim.scale = 1.0f + t * 0.3f; // grow to 1.15
                    else
                        anim.scale = 1.15f - (t - 0.5f) * 0.3f; // shrink back
                }
                else if (anim.spawning)
                {
                    // Spawn scale: 0 -> 1
                    anim.scale = t;
                }
            }
        }
    }

    void setBoardSize(int size)
    {
        if (size < MIN_BOARD_SIZE) size = MIN_BOARD_SIZE;
        if (size > MAX_BOARD_SIZE) size = MAX_BOARD_SIZE;
        // switching board size != ending the game: the current game is only saved to its slot, not history
        // mark it saved so init()'s saveHistory does not record the unfinished game
        m_historySaved = true;
        m_boardSize = size;
        init();
    }

    void setColorScheme(int scheme)
    {
        if (scheme >= 0 && scheme < TOTAL_SCHEMES)
        {
            m_colorScheme = scheme;
            logMsg("Color scheme -> " + std::to_string(scheme) + " (" + schemeNames[scheme] + ")");
        }
    }

    // Test: fill the board with big numbers to verify color schemes
    void setupTestBoard()
    {
        m_boardSize = 5;
        std::memset(m_board, 0, sizeof(m_board));
        m_animations.clear();
        m_animating = false;
        m_newTileRow = -1;
        m_newTileCol = -1;
        m_canUndo = false;
        m_score = 0;
        // place increasing big numbers per row: 2^10 to 2^24
        int powers[] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 };
        int idx = 0;
        for (int i = 0; i < m_boardSize && idx < 17; ++i)
        {
            for (int j = 0; j < m_boardSize && idx < 17; ++j)
            {
                m_board[i][j] = (1 << powers[idx]);
                m_score += m_board[i][j];
                idx++;
            }
        }
    }

    sf::Color getTileColor(int value) const
    {
        const SchemeData& s = SCHEMES[m_colorScheme];
        if (value == 0) return s.emptyColor;
        if (isFillValue(value)) return sf::Color(70, 70, 78);   // fill (obstacle): dark gray
        int power = 0;
        int v = value;
        while (v > 1) { v >>= 1; power++; }
        if (power >= 1 && power <= MAX_POWER)
            return s.tileColors[power];
        return s.emptyColor;
    }

    sf::Color getTextColor(int value) const
    {
        const SchemeData& s = SCHEMES[m_colorScheme];
        return value >= s.lightTextFromValue ? s.textLight : s.textDark;
    }

    const SchemeData& getScheme() const { return SCHEMES[m_colorScheme]; }

    // ========== Persistence: save / load ==========
    bool saveToFile(const char* filename) const
    {
        std::ofstream ofs(filename, std::ios::binary);
        if (!ofs) return false;
        ofs.write((const char*)&m_boardSize, sizeof(m_boardSize));
        ofs.write((const char*)&m_colorScheme, sizeof(m_colorScheme));
        ofs.write((const char*)&m_score, sizeof(m_score));
        ofs.write((const char*)&m_canUndo, sizeof(m_canUndo));
        ofs.write((const char*)&m_prevScore, sizeof(m_prevScore));
        ofs.write((const char*)m_board, sizeof(m_board));
        if (m_canUndo)
            ofs.write((const char*)m_prevBoard, sizeof(m_prevBoard));
        char fontPath[MAX_PATH] = {};
        strncpy_s(fontPath, g_currentFontPath.c_str(), MAX_PATH - 1);
        ofs.write(fontPath, MAX_PATH);
        ofs.write((const char*)&g_savedWindowW, sizeof(g_savedWindowW));
        ofs.write((const char*)&g_savedWindowH, sizeof(g_savedWindowH));
        ofs.write((const char*)&g_language, sizeof(g_language));
        logMsg("Save written: score=" + std::to_string(m_score) +
               " board=" + std::to_string(m_boardSize) + "x" + std::to_string(m_boardSize) +
               " scheme=" + std::to_string(m_colorScheme));
        return true;
    }

    // loadLang: whether to read and apply the global language setting g_language
    //   - true:  main save (SAVE_FILE) at startup; restores the user's last language
    //   - false: per-size slots (slotFileFor) when switching boards; language is global, not slot-scoped
    bool loadFromFile(const char* filename, bool loadLang = false)
    {
        std::ifstream ifs(filename, std::ios::binary);
        if (!ifs) { logMsg(std::string("Save load failed (file not found): ") + filename); return false; }
        ifs.read((char*)&m_boardSize, sizeof(m_boardSize));
        ifs.read((char*)&m_colorScheme, sizeof(m_colorScheme));
        ifs.read((char*)&m_score, sizeof(m_score));
        ifs.read((char*)&m_canUndo, sizeof(m_canUndo));
        ifs.read((char*)&m_prevScore, sizeof(m_prevScore));
        ifs.read((char*)m_board, sizeof(m_board));
        if (m_canUndo)
            ifs.read((char*)m_prevBoard, sizeof(m_prevBoard));
        // validate data
        if (m_boardSize < MIN_BOARD_SIZE || m_boardSize > MAX_BOARD_SIZE)
            m_boardSize = DEFAULT_BOARD_SIZE;
        if (m_colorScheme < 0 || m_colorScheme >= TOTAL_SCHEMES)
            m_colorScheme = 0;
        // read font path (legacy saves; needs a large enough file)
        ifs.seekg(0, std::ios::end);
        std::streamsize fileSize = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        std::streamsize minSize = (std::streamsize)(sizeof(m_boardSize) + sizeof(m_colorScheme) +
            sizeof(m_score) + sizeof(m_canUndo) + sizeof(m_prevScore) + sizeof(m_board));
        if (m_canUndo) minSize += sizeof(m_prevBoard);
        if (fileSize >= minSize + MAX_PATH)
        {
            char fontPath[MAX_PATH] = {};
            ifs.seekg(minSize, std::ios::beg);
            ifs.read(fontPath, MAX_PATH);
            if (ifs.gcount() > 0 && fontPath[0] != '\0')
                g_currentFontPath = fontPath;
        }
        // read window size (legacy saves)
        std::streamsize winSizeOffset = minSize + MAX_PATH;
        if (fileSize >= winSizeOffset + (std::streamsize)(sizeof(g_savedWindowW) + sizeof(g_savedWindowH)))
        {
            ifs.seekg(winSizeOffset, std::ios::beg);
            ifs.read((char*)&g_savedWindowW, sizeof(g_savedWindowW));
            ifs.read((char*)&g_savedWindowH, sizeof(g_savedWindowH));
            const unsigned int MIN_W = 320, MIN_H = 380;
            if (g_savedWindowW < MIN_W || g_savedWindowW > 4096) g_savedWindowW = FIRST_RUN_WINDOW_W;
            if (g_savedWindowH < MIN_H || g_savedWindowH > 4096) g_savedWindowH = FIRST_RUN_WINDOW_H;
        }
        // read language (legacy saves) - applied only from the main save at startup; slots do not override
        std::streamsize langOffset = winSizeOffset + sizeof(g_savedWindowW) + sizeof(g_savedWindowH);
        s_saveHadLanguageField = (fileSize >= langOffset + (std::streamsize)sizeof(g_language));
        if (loadLang && s_saveHadLanguageField)
        {
            ifs.seekg(langOffset, std::ios::beg);
            ifs.read((char*)&g_language, sizeof(g_language));
            if (g_language != LANG_EN && g_language != LANG_ZH) g_language = LANG_EN;
        }
        logMsg("Save loaded: score=" + std::to_string(m_score) +
               " board=" + std::to_string(m_boardSize) + "x" + std::to_string(m_boardSize) +
               " scheme=" + std::to_string(m_colorScheme) +
               " window=" + std::to_string(g_savedWindowW) + "x" + std::to_string(g_savedWindowH));
        m_historySaved = true;  // state loaded from a slot is already in history; no duplicate on next switch
        return true;
    }

    void undo()
    {
        if (!m_canUndo) return;
        std::memcpy(m_board, m_prevBoard, sizeof(m_board));
        m_score = m_prevScore;
        m_canUndo = false;
        m_animations.clear();
        m_animating = false;
        m_newTileRow = -1;
        m_newTileCol = -1;
        m_historySaved = false;  // undo changed the state; needs re-saving to history
        logMsg("Undo -> score=" + std::to_string(m_score));
    }

    // Restore directly to a given state (used by AI bounded undo)
    void restoreState(const int srcBoard[MAX_BOARD_SIZE][MAX_BOARD_SIZE], int srcScore)
    {
        std::memcpy(m_board, srcBoard, sizeof(m_board));
        m_score = srcScore;
        m_canUndo = false;
        m_animations.clear();
        m_animating = false;
        m_newTileRow = -1;
        m_newTileCol = -1;
        m_historySaved = false;  // state restored; needs re-saving to history
    }

private:
    int  m_board[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
    int  m_score;
    int  m_prevBoard[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
    int  m_prevScore;
    bool m_canUndo;
    int  m_boardSize;
    int  m_colorScheme;
    bool m_aiAssisted = false;  // whether this game used AI assistance (history tag)

    // animation system
    std::vector<TileAnim> m_animations;
    bool m_animating;
    sf::Clock m_animClock;
    static const float ANIM_DURATION_DEFAULT;  // manual mode default: 150ms classic
    static const float ANIM_DURATION_AI;       // AI high-frequency slide: 50ms
    float m_animDuration;                      // current effective value, switchable at runtime
    int  m_newTileRow, m_newTileCol; // for spawn animation

public:
    // manual mode: restore 150ms normal sliding feel
    // AI mode: 50ms supports 6-10 moves/s
    void setAIModeAnimations(bool aiMode) {
        m_animDuration = aiMode ? ANIM_DURATION_AI : ANIM_DURATION_DEFAULT;
    }
    // set the animation duration dynamically by AI rate (higher rate = shorter animation, never a step bottleneck)
    void setAIAnimDuration(float d) { m_animDuration = d; }
    float getAnimDuration() const { return m_animDuration; }

    // practice mode: set the board directly (build-position save / cancel restore), no history written
    void setBoardDirect(const std::vector<std::vector<int>>& board, int score)
    {
        finishAnimation();
        std::memset(m_board, 0, sizeof(m_board));
        int bs = (int)board.size();
        if (bs < 3 || bs > MAX_BOARD_SIZE) bs = m_boardSize;
        for (int i = 0; i < bs && i < MAX_BOARD_SIZE; ++i)
            for (int j = 0; j < bs && j < (int)board[i].size() && j < MAX_BOARD_SIZE; ++j)
                m_board[i][j] = board[i][j];
        m_score = score;
        m_canUndo = false;
        m_historySaved = true;   // a directly-set position counts as recorded, avoiding accidental history on switch
    }

    void saveState()
    {
        std::memcpy(m_prevBoard, m_board, sizeof(m_board));
        m_prevScore = m_score;
        m_canUndo = true;
    }

    int getEmptyCount() const
    {
        int cnt = 0;
        for (int i = 0; i < m_boardSize; ++i)
            for (int j = 0; j < m_boardSize; ++j)
                if (m_board[i][j] == 0) cnt++;
        return cnt;
    }
};

const float Game2048::ANIM_DURATION_DEFAULT = 0.120f;  // manual: 120ms classic slide; with finishAnimation() in move(), faster chaining
const float Game2048::ANIM_DURATION_AI      = 0.060f;  // AI: 60ms (target 10 moves/s)
// Game2048's constructor sets m_animDuration to DEFAULT (manual); AI start switches to AI, AI end restores

// Check and run the delayed save
static void flushSaveIfNeeded(Game2048& game)
{
    if (g_saveDirty && g_saveTimer.getElapsedTime().asSeconds() >= SAVE_DEBOUNCE)
    {
        game.saveToFile(SAVE_FILE);
        g_saveDirty = false;
    }
}

// ========== AI operation ==========
// Based on ziap/2048-ai Expectimax + Formation + undo optimization
enum AIMode { AI_INACTIVE = 0, AI_PAUSED, AI_RUNNING };
static int g_aiMode = AI_INACTIVE;
static int g_aiBoardSize = 4;
static sf::Clock g_aiStepClock;
// ===== Pipelined parallelism + Iterative Deepening 60ms/move: target 10 moves/s =====
// - TIME_BUDGET = 60ms (Iterative Deepening): strictly returns within 60ms, depth adapts to the position
// - STEP_DELAY = 5ms minimum cadence (clock resets only on successful consume; next frame consumes when ready)
// - s_aiConsumingPending: future not ready -> poll once per rendered frame (~16ms) until ready/timeout
// Actual step cadence is controlled by g_aiStepDelaySec (rate slider 7..35 moves/s)
static bool s_aiConsumingPending = false;  // true: poll the future every frame, skip STEP_DELAY
// 5x5 AI warning dialog state
static bool g_show5x5AiWarning = false;

// thread_local SolverAI2048 instances: constructed on first use per thread; TT(32MB)/tables allocated once and reused
// One for the main thread and one for the worker thread -> independent, zero shared writes, naturally thread-safe
inline ZigSolverAI2048& solverForThisThread(int boardSize) {
    thread_local ZigSolverAI2048 tl_solver(SolverAI2048::BoardSize::STANDARD_4x4);
    thread_local int tl_boardSize = -1;
    if (tl_boardSize != boardSize) {
        tl_solver.setBoardSize(boardSize);
        tl_solver.newGame();   // board size change = new game: fully reset the engine against cross-game degradation
        tl_boardSize = boardSize;
    }
    return tl_solver;
}

static ZigSolverAI2048 g_aiEngine(SolverAI2048::BoardSize::STANDARD_4x4);  // kept for compatibility
static bool g_showAIEnd = false;
static sf::FloatRect g_aiBtnPause, g_aiBtnStart, g_aiBtnCancel;

// ===== AI session stats (move count / elapsed) =====
static int      g_aiMoveCount = 0;      // AI moves made in this game
static float    g_aiElapsedMs = 0.f;    // cumulative AI ms in this game
static bool     g_aiTiming = false;     // whether timing (stops while paused)

// ===== AI settings dialog (rate slider + intelligence tier buttons) =====
static bool g_aiSettingsOpen = false;
// Rate slider: desktop CPUs are faster -> 7..50 moves/s (default 30 on first install)
static const int   AI_RATE_MIN = 7, AI_RATE_MAX = 50;
static int   g_aiRate = 30;                  // current rate (persisted to ai_settings.ini)
static float g_aiStepDelaySec = 0.077f;      // animation seconds for the current rate (30 ~ 77ms)
static bool  g_aiRateDrag = false;           // dragging the rate slider
static sf::FloatRect g_aiRateTrack;          // rate track area
static float g_aiRateKnobMinCX = 0.f, g_aiRateKnobMaxCX = 0.f;
// Intelligence tiers: 3 small rect buttons -> placeholder depths {3,6,12}, mapped in setDepth to ziap BFS budgets {2^17,2^19,2^22}
static const int   AI_LEVEL_MIN = 0, AI_LEVEL_MAX = 2;
static int   g_aiLevel = 2;                  // default highest tier (ziap budget 2^19)
static sf::FloatRect g_aiLevelBtn[3];        // hit rects of the 3 tier buttons
static const int g_aiLevelDepth[3] = { 3, 6, 12 };  // 3 tier placeholders (mapped to ziap BFS budgets)
static volatile int g_aiDepth = 12;          // currently active tier (read by the worker thread)
static volatile int g_aiCacheReset = 1;      // new-game flag: set on AI session start / board reset; worker clears its cache
// stats-row visibility toggles (the two bottom function keys of the AI settings dialog)
//   on by default on first open (count/time row); user can turn off; setting is persisted
static bool g_aiShowMoves = true;           // show the "moves" rect
static bool g_aiShowTime  = true;           // show the "time" rect
static sf::FloatRect g_aiShowMovesBtn, g_aiShowTimeBtn;   // hit rects of the two function keys
// AI uses undo (on by default): when off, AI dead ends end immediately without undo
static bool g_aiUseUndo = true;
static sf::FloatRect g_aiUseUndoBtn;        // "AI uses undo" key hit rect
static int g_aiEngineMode = 0;

// ===== Practice mode =====
static bool g_practiceActive = false;          // practice mode toggle
static unsigned int g_practiceOrigW = 0;       // window width before entering practice
static unsigned int g_practiceOrigH = 0;       // window height before entering practice
static sf::FloatRect g_practiceBtn;            // menu "Practice" key hit rect
static sf::FloatRect g_practiceBtnImport, g_practiceBtnBuild, g_practiceBtnClose;   // import-history / build-position / close
static sf::FloatRect g_practiceBtnFlipV, g_practiceBtnFlipH, g_practiceBtnRotCCW, g_practiceBtnRotCW;   // board transform keys
static sf::FloatRect g_practicePanel;          // right analysis panel area
// Prediction results (computed on a worker thread + mutex-protected, avoids lag while moving)
struct PracticePredData {
    bool ready = false;
    std::vector<std::vector<int>> board[4];    // 4-step predicted boards
    int  dirs[4] = { -1, -1, -1, -1 };        // direction chosen per step
    float probs[4] = { 0.f, 0.f, 0.f, 0.f };   // up/down/left/right probabilities (current position)
    float prevProbs[4] = { 0.f, 0.f, 0.f, 0.f };   // previous-position probabilities ("previous step" mode)
    int curOp = -1;                            // current operation direction
    int bestOp = -1;                           // best operation of the previous step
    int nextBestOp = -1;                       // best operation of the next step
    int prevBestOp = -1;                       // previous-position best direction
    float lossPct = -1.f;                      // per-step expected-deviation percentage (always valid; -1 = not computed)
    int   lossGrade = 0;                       // per-step grade: 0=great 1=good 2=fair 3=fair(worst)
};
static std::mutex g_practicePredMutex;
static PracticePredData g_practicePred;        // latest finished result (lock-protected)
static bool g_practicePredBusy = false;        // worker computing
static std::future<void> g_practicePredFuture;
// Pending prediction request: while busy, keep only the "latest frame" request (board+prev+move dir); run when idle
// Ensures prev/next step and history playback keep best-op and per-step deviation fresh (never dropped while busy)
struct PendingPredRequest {
    bool valid = false;
    std::vector<std::vector<int>> cur, prev;
    int lastMoveDir = -1;
    bool randomSpawn = false;
};
static PendingPredRequest g_pendingPredReq;
// ===== Pearson chi-square goodness-of-fit calibrator (per session, predicted prob -> actual result) =====
//   chi^2 = sum (O-E)^2/E measures how well the predicted probability distribution matches observed frequencies.
//   Buckets accumulate by probability: expected = predicted prob, observed = hit(1)/miss(0).
//   misfit01() normalizes misfit to [0,1) (0=perfect); calibScale()=observed/expected (<1 = over-optimistic prediction).
struct ChiCalibrator {
    static const int NB = 10;                 // probability buckets 0-100%
    double expCnt[NB] = { 0 };                // expected counts (accumulated predicted probabilities)
    double obsCnt[NB] = { 0 };                // observed counts (actual hits)
    int n = 0;
    void add(double p, bool hit) {
        int b = (int)std::min(NB - 1, std::max(0, (int)(p * NB)));
        expCnt[b] += p;
        obsCnt[b] += hit ? 1.0 : 0.0;
        ++n;
    }
    double chiSq() const {                    // Pearson chi-square statistic
        double chi = 0;
        for (int b = 0; b < NB; ++b)
            if (expCnt[b] > 0.5) {
                double d = obsCnt[b] - expCnt[b];
                chi += d * d / expCnt[b];
            }
        return chi;
    }
    double misfit01() const {                 // normalized misfit [0,1)
        if (n < 8) return 0.0;
        double chi = chiSq();
        return chi / (chi + NB);
    }
    double calibScale() const {               // observed/expected (<1 = over-optimistic)
        if (n < 8) return 1.0;
        double e = 0, o = 0;
        for (int b = 0; b < NB; ++b) { e += expCnt[b]; o += obsCnt[b]; }
        return (e > 0.5) ? o / e : 1.0;
    }
    void reset() {
        for (int i = 0; i < NB; ++i) { expCnt[i] = obsCnt[i] = 0; }
        n = 0;
    }
};
// Direction-prediction probability calibration (per session, chi-square fit)
static ChiCalibrator g_dirCalib;
static std::mutex g_dirCalibMutex;
// Milestone prediction confidence calibration (per session, chi-square fit)
static ChiCalibrator g_mileCalib;
static std::mutex g_mileCalibMutex;
static long long g_mileTrackVal = -1;   // milestone value being tracked (latest target)
static float   g_mileTrackP = 0.f;      // predicted probability of that milestone
static bool    g_mileTrackOn = false;

// Read cache (copied from g_practicePred every frame for drawing)
static std::vector<std::vector<int>> g_practicePredBoard[4];
static int  g_practicePredDirs[4] = { -1, -1, -1, -1 };
static float g_practiceProbs[4] = { 0.f, 0.f, 0.f, 0.f };
static float g_practiceProbsPrev[4] = { 0.f, 0.f, 0.f, 0.f };
static int  g_practiceCurOp = -1;
static int  g_practiceBestOp = -1;
static int  g_practiceNextBestOp = -1;
static int  g_practicePrevBestOp = -1;
static float g_practiceLossPct = -1.f;         // per-step deviation % (for drawing)
static int   g_practiceLossGrade = 0;          // per-step grade (for drawing)
// Milestone likelihood (score-milestone achievement rate, Monte-Carlo on a worker thread)
static std::mutex g_practiceMilestoneMutex;
static bool  g_practiceMilestoneBusy = false;
static std::future<void> g_practiceMilestoneFuture;
static float g_practiceMilestonePct[2] = { -1.f, -1.f };      // two milestone rates (%)
static long long g_practiceMilestoneVals[2] = { 0, 0 };        // two milestone values (for drawing)
static int   g_practiceMilestoneCount = 2;                     // actually shown milestones (1-2, capped by board science limit)
static bool  g_milestonePending = false;       // latest pending request (queued while busy, run when idle)
// Throttle: minimum 180ms between milestone computations (avoids bursts during replay drag / fast moves)
static DWORD g_milestoneLastStartMs = 0;
static const DWORD MILESTONE_MIN_INTERVAL_MS = 180;
static int   g_milestonePendingSz = 0;
static std::vector<std::vector<int>> g_milestonePendingBoard;
static bool  g_practicePredHidden = false;     // hide 4-step prediction (mini boards cleared; next-best/refresh keys greyed)
static sf::FloatRect g_practiceHidePredBtn, g_practiceRefreshPredBtn;   // hide/refresh 4-step prediction keys
static int  g_practiceLastMoveDir = -1;        // user's latest move direction (0=L 1=R 2=U 3=D)
// Whether the last move was made by AI: AI plays the engine's best direction -> deviation is always 0, and the direction
// prediction thread needs no solver/full search (much lower memory & CPU while AI is running)
static std::atomic<bool> g_lastMoveByAI(false);
// Build-position dialog
static bool g_practiceBuildOpen = false;       // build-position dialog open
static int  g_practiceBuildSelPow = 1;         // currently selected power (0=clear, 1=2, 2=4, ...) [fill tiles removed]
static std::vector<std::vector<int>> g_practiceBuildBoard;      // board being built
static sf::FloatRect g_practiceBuildSave, g_practiceBuildCancel;   // save/cancel
static sf::FloatRect g_practiceBuildRect;      // dialog area
static float g_practiceBuildItemH = 0.f;       // value-list item height
static int   g_practiceBuildScroll = 0;        // value-list scroll offset
static float g_practiceBuildPaletteX = 0.f, g_practiceBuildPaletteY = 0.f;
static float g_practiceBuildPaletteW = 0.f, g_practiceBuildPaletteH = 0.f;
static float g_practiceBuildBoardX = 0.f, g_practiceBuildBoardY = 0.f, g_practiceBuildCell = 0.f;
// Import history (practice review)
static bool g_practiceImport = false;          // whether in imported-history review
static std::vector<std::vector<int>> g_practicePrevBoard;        // board before import (restored on cancel)
static int  g_practicePrevScore = 0;           // score before import
static std::vector<int> g_practiceQuality;     // per-step quality: 0=perfect 1=fair 2=poor 3=bad
static bool g_practiceQualityReady = false;    // whether quality analysis finished
static std::mutex g_practiceQualityMutex;      // quality data lock
static std::future<void> g_practiceQualityFuture;   // background quality analysis

// Per-step quality colors: 0=truly best(green) 1=next best(yellow-green) 2=second worst(yellow) 3=worst(orange-red) (defined later; declared for replay drawing)
static sf::Color practiceQualityColor(int qv);

// Rate (moves/s) -> animation ms linear map: 7->143ms, 50->20ms
static float aiRateToDelayMs(int rate)
{
    float t = (float)(rate - AI_RATE_MIN) / (float)(AI_RATE_MAX - AI_RATE_MIN);
    return 143.f + (20.f - 143.f) * t;
}

// ========== Practice helpers: pure board slide/merge (no spawn) ==========
// Pure slide/merge (no animation, no random tile). Fill (obstacle,-1) tiles are "walls": they never move/merge, others cannot pass
// (same semantics as game-layer moveLineLeftTracked; used by prediction/milestone simulation)
static void slideLinePure(std::vector<int>& line)
{
    const int n = (int)line.size();
    std::vector<int> out(n, 0);
    int segStart = 0;
    for (int j = 0; j <= n; ++j)
    {
        if (j == n || isFillValue(line[j]))
        {
            // compact/merge segment [segStart, j) left independently
            std::vector<int> tmp;
            for (int k = segStart; k < j; ++k)
                if (line[k] > 0) tmp.push_back(line[k]);
            int idx = segStart;
            for (size_t i = 0; i < tmp.size(); ++i)
            {
                if (i + 1 < tmp.size() && tmp[i] == tmp[i + 1]) { out[idx++] = tmp[i] * 2; ++i; }
                else out[idx++] = tmp[i];
            }
            if (j < n) out[j] = line[j];   // obstacles stay in place (keep their unique super-large value)
            segStart = j + 1;
        }
    }
    line = std::move(out);
}

// Pure move with scoring (returns merge score; invalid move returns 0 and leaves the board unchanged) - defined below; forward-declared for milestone sims
static int moveBoardScored(std::vector<std::vector<int>>& b, int dir);

// Slide/merge once on a board matrix (no animation, no random tile); returns whether it moved
static bool moveBoardPure(std::vector<std::vector<int>>& b, int dir)
{
    const int n = (int)b.size();
    auto line = [&](std::vector<int>& l) { slideLinePure(l); };
    std::vector<std::vector<int>> temp = b;
    for (int i = 0; i < n; ++i)
    {
        if (dir == 0)      { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[i][j]; line(l); for (int j = 0; j < n; ++j) temp[i][j] = l[j]; }
        else if (dir == 1) { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[i][n - 1 - j]; line(l); for (int j = 0; j < n; ++j) temp[i][n - 1 - j] = l[j]; }
        else if (dir == 2) { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[j][i]; line(l); for (int j = 0; j < n; ++j) temp[j][i] = l[j]; }
        else if (dir == 3) { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[n - 1 - j][i]; line(l); for (int j = 0; j < n; ++j) temp[n - 1 - j][i] = l[j]; }
    }
    bool moved = (temp != b);
    if (moved) b = std::move(temp);
    return moved;
}

// Forward declaration: direct heuristic evaluation with fill tiles (defined after practiceMoveScoresEx)
static bool boardHasFill(const std::vector<std::vector<int>>& b);
static float evalFillBoard(const std::vector<std::vector<int>>& b, int sz);
static std::vector<SolverAI2048::MoveScore> fillBoardMoveScores(const std::vector<std::vector<int>>& b, int sz);
// Forward declaration: lightweight 2-ply greedy direction pick (defined in the milestone area, for prediction lookahead)
static int milestonePickDir(const std::vector<std::vector<int>>& b, int sz);

// 4x4 Solver4x4 engine dir matches the main program order: 0=L 1=R 2=U 3=D (verified against getMoves fields)
// Direction mapping lives in the main program (dispatcher); identity mapping keeps the structure for later tweaks
static const int SOLVER4_TO_CPP[4] = { 0, 1, 2, 3 };
static inline int solver4ToCppDir(int d) { return (d >= 0 && d < 4) ? SOLVER4_TO_CPP[d] : d; }

// Get move scores by AI engine mode (in sync with the AI operation engine)
static std::vector<SolverAI2048::MoveScore> practiceMoveScores(
    ZigSolverAI2048& engine, const std::vector<std::vector<int>>& b, int sz, int engineMode)
{
    (void)sz;
    (void)engineMode;
    engine.setNewEngine(engineMode == 1);   // classic search mode: 4x4 ziap + MacroXue complement
    auto scores = engine.getAllMoveScores(b);
    if (sz == 4)   // 4x4 Solver4x4: engine dir -> main-program order (mapped on the main side)
        for (auto& m : scores) m.dir = solver4ToCppDir(m.dir);
    // With fill tiles: re-validate move legality with moveBoardPure (solver rank encoding may misjudge edge cases)
    if (boardHasFill(b))
    {
        std::vector<SolverAI2048::MoveScore> valid;
        valid.reserve(scores.size());
        for (auto& m : scores)
        {
            if (m.dir < 0 || m.dir >= 4) continue;
            auto test = b;
            if (moveBoardPure(test, m.dir)) valid.push_back(m);
        }
        scores = std::move(valid);
    }
    return scores;
}

// Move scores (search heuristic; engine already chose ziap / ziap+MacroXue / in-house per mode)
struct PracticeMoveResult {
    std::vector<SolverAI2048::MoveScore> ms;
    bool lookup = false;   // always false (pure algorithm, no table path)
};
static PracticeMoveResult practiceMoveScoresEx(ZigSolverAI2048& engine,
                                               const std::vector<std::vector<int>>& b, int sz, int engineMode)
{
    PracticeMoveResult r;
    r.ms = practiceMoveScores(engine, b, sz, engineMode);
    r.lookup = false;
    return r;
}

// ===== Fast direction scoring (for direction/4-step prediction; pure function, zero search, zero memory) =====
//   Multi-step greedy lookahead (LOOKAHEAD=3, 4 steps total): after each direction, optimistically simulate 3 more steps
//   (each step: 2-ply greedy best direction + most favorable spawn), then evaluate the final position.
//   Benefit: not "short-sighted" on forming a big tile right now; it sees the whole layout 3-4 steps ahead -
//   if a direction makes a big tile short-term but breaks the follow-up formation, its score drops; the snake/T-shape
//   long-term layout the user prefers scores higher. Each step is still only ~0.05ms, so direction prediction stays sub-ms.
//   Returns MoveScores for the 4 directions (invalid = -1e18), sorted descending.

// Optimistic lookahead: simulate depth steps from position b (each step: greedy best dir + best spawn), return final eval
static float greedyLookaheadScore(std::vector<std::vector<int>> b, int sz, int depth)
{
    for (int k = 0; k < depth; ++k)
    {
        int bestDir = milestonePickDir(b, sz);   // 2-ply greedy direction
        if (bestDir < 0) break;
        if (!moveBoardPure(b, bestDir)) break;
        // optimistic spawn: put 2/4 into the empty cell maximizing the heuristic
        float bs = -1e18f;
        std::vector<std::vector<int>> bb;
        bool any = false;
        for (int i = 0; i < sz; ++i) for (int j = 0; j < sz; ++j)
        {
            if (b[i][j] != 0) continue;
            any = true;
            for (int val : {2, 4})
            {
                b[i][j] = val;
                float s = evalFillBoard(b, sz);
                if (s > bs) { bs = s; bb = b; }
                b[i][j] = 0;
            }
        }
        if (!any) break;
        b = bb;
    }
    return evalFillBoard(b, sz);
}

static std::vector<SolverAI2048::MoveScore> greedyDirScores(const std::vector<std::vector<int>>& b, int sz)
{
    const int LOOKAHEAD = 3;   // look 3 more steps after each direction (4 total)
    std::vector<SolverAI2048::MoveScore> out;
    out.reserve(4);
    for (int d = 0; d < 4; ++d)
    {
        SolverAI2048::MoveScore ms;
        ms.dir = d;
        auto b1 = b;
        if (!moveBoardPure(b1, d))
        {
            ms.score = -1e18f;
            ms.allowed = false;
            ms.safe = false;
        }
        else
        {
            ms.score = greedyLookaheadScore(b1, sz, LOOKAHEAD);
            ms.allowed = true;
            ms.safe = true;
        }
        out.push_back(ms);
    }
    std::sort(out.begin(), out.end(),
        [](const SolverAI2048::MoveScore& a, const SolverAI2048::MoveScore& b) { return a.score > b.score; });
    return out;
}

// ===== Board with fill (obstacle) tiles: direct heuristic evaluation (no rank-encoding solver) =====
//   Rank-encoding solvers (4/5 bits) assume every non-empty cell can merge, so they mishandle obstacles.
//   Here each direction is simulated with moveBoardPure (fills slide correctly, never merge), then the resulting
//   position is scored heuristically: empty count + max tile + monotonicity + mergeable pairs + smoothness.
static bool boardHasFill(const std::vector<std::vector<int>>& b)
{
    for (const auto& row : b)
        for (int v : row)
            if (isFillValue(v)) return true;
    return false;
}

static float evalFillBoard(const std::vector<std::vector<int>>& b, int sz)
{
    int empty = 0, maxTile = 0, merges = 0;
    float mono = 0.f, smooth = 0.f;
    for (int i = 0; i < sz; ++i) {
        for (int j = 0; j < sz; ++j) {
            int v = b[i][j];
            if (v == 0) { empty++; continue; }
            if (isFillValue(v)) continue;
            if (v > maxTile) maxTile = v;
            if (j + 1 < sz && b[i][j+1] == v) merges++;
            if (i + 1 < sz && b[i+1][j] == v) merges++;
            if (j + 1 < sz && !isFillValue(b[i][j+1]) && b[i][j+1] > 0) {
                float d = (float)std::abs((int)std::log2((float)v) - (int)std::log2((float)b[i][j+1]));
                smooth -= d;
            }
            if (i + 1 < sz && !isFillValue(b[i+1][j]) && b[i+1][j] > 0) {
                float d = (float)std::abs((int)std::log2((float)v) - (int)std::log2((float)b[i+1][j]));
                smooth -= d;
            }
        }
    }
    for (int i = 0; i < sz; ++i) {
        int incR = 0, decR = 0, incC = 0, decC = 0;
        for (int j = 1; j < sz; ++j) {
            int a = b[i][j-1], c = b[i][j];
            if (!isFillValue(a) && !isFillValue(c) && a > 0 && c > 0) {
                if (a > c) decR++; else if (a < c) incR++;
            }
            a = b[j-1][i]; c = b[j][i];
            if (!isFillValue(a) && !isFillValue(c) && a > 0 && c > 0) {
                if (a > c) decC++; else if (a < c) incC++;
            }
        }
        mono -= (float)(std::min(incR, decR) + std::min(incC, decC));
    }
    float maxLog = (maxTile > 0) ? (float)std::log2((float)maxTile) : 0.f;
    return empty * 270.f + maxLog * 200.f + merges * 700.f + mono * 50.f + smooth * 10.f;
}

static std::vector<SolverAI2048::MoveScore> fillBoardMoveScores(const std::vector<std::vector<int>>& b, int sz)
{
    std::vector<SolverAI2048::MoveScore> result;
    result.reserve(4);
    for (int d = 0; d < 4; ++d) {
        SolverAI2048::MoveScore ms;
        ms.dir = d;
        auto bd = b;
        if (moveBoardPure(bd, d)) {
            ms.score = evalFillBoard(bd, sz);
            ms.allowed = true;
            ms.safe = true;
        } else {
            ms.score = -1e18f;
            ms.allowed = false;
            ms.safe = false;
        }
        result.push_back(ms);
    }
    std::sort(result.begin(), result.end(),
        [](const SolverAI2048::MoveScore& a, const SolverAI2048::MoveScore& b) { return a.score > b.score; });
    return result;
}

// Direction evaluation: search score -> probability (0-1). Uses only the previous/next step's single-move search score for fast display;
//   softmax (temperature = AI engine's "close call" threshold |best|*3%) maps score gaps to probabilities,
//   keeping direction prediction on the same basis as the AI:
//     - similar scores -> similar probabilities (engine treats as a tie -> prediction too)
//     - a clearly better direction -> clearly higher probability
//   Unmovable directions are forced to 0.000%
static void practiceDirProbs(const std::vector<SolverAI2048::MoveScore>& ms, bool lookup,
                             const std::vector<std::vector<int>>& board, int sz,
                             float out[4], int& bestDir)
{
    (void)lookup; (void)board; (void)sz;
    float s[4] = { -1e30f, -1e30f, -1e30f, -1e30f };   // invalid directions keep a tiny value (getAllMoveScores only returns valid ones)
    bestDir = -1; float bestScore = -1e9f;
    for (auto& m : ms)
    {
        if (m.dir >= 0 && m.dir < 4) s[m.dir] = m.score;
        if (m.score > bestScore) { bestScore = m.score; bestDir = m.dir; }
    }
    // softmax weights: temperature matches the engine's "tie" threshold (|best|*3%), aligning prediction with the AI;
    //   then calibrated by the direction-prediction chi-square fit: if the history shows fewer "actually took best" than predicted
    //   (overconfidence) -> temperature up (flatter); otherwise sharper. The distribution tracks real hit rates (Pearson chi-square).
    float T = std::max(1.0f, std::fabs(bestScore) * 0.03f);
    {
        std::lock_guard<std::mutex> lk(g_dirCalibMutex);
        const double c = g_dirCalib.calibScale();
        if (c < 0.99)      T /= (float)std::max(0.2, c);   // overconfident (hits < predicted) -> raise temperature, flatter
        else if (c > 1.01) T /= (float)std::min(3.0, c);   // conservative (hits > predicted) -> lower temperature, sharper
    }
    float w[4] = { 0.f, 0.f, 0.f, 0.f };
    float sum = 0.f;
    int nv = 0;
    for (int d = 0; d < 4; ++d)
        if (s[d] > -1e8f)
        {
            ++nv;
            w[d] = std::exp((s[d] - bestScore) / T);
            sum += w[d];
        }
    if (nv == 0 || sum <= 0.f)
    {
        for (int d = 0; d < 4; ++d) out[d] = 0.f;
        return;
    }
    for (int d = 0; d < 4; ++d)
    {
        if (s[d] <= -1e8f) { out[d] = 0.f; continue; }     // unmovable -> 0.000%
        out[d] = w[d] / sum;                                // softmax: probability matching the AI score gap
    }
}

// Spawn 2(90%)/4(10%) into a random empty cell ("refresh 4-step prediction": simulates real random tiles)
static void practiceSpawnRandomTile(std::vector<std::vector<int>>& b, int sz)
{
    std::vector<int> empty;
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            if (b[i][j] == 0) empty.push_back(i * sz + j);
    if (empty.empty()) return;
    const int idx = empty[(int)(std::rand() % (int)empty.size())];
    const int val = (std::rand() % 10 == 0) ? 4 : 2;
    b[idx / sz][idx % sz] = val;
}

// Per-step deviation -> display percentage (error function / normal CDF scientific fit, smooth S-curve)
//   Normalized erf: shown = 99% x (erf(k*(dev-0.5)) - erf(-0.5k)) / (erf(0.5k) - erf(-0.5k))
//   Larger k flattens the tail: past dev=0.75 the slope shrinks markedly, making 99% harder
//   dev=0 -> 0%; dev=0.5 -> 49.5%; dev=0.75 -> ~95%; dev=0.97 -> ~98.96% (<=99.000%); dev=1 -> 99% (never 100%)
static float deviationToPct(float dev)
{
    const float k = 5.0f;
    float v = std::max(0.f, std::min(1.f, dev));
    float e0 = std::erf(-0.5f * k);
    float e1 = std::erf(0.5f * k);
    float e = std::erf(k * (v - 0.5f));
    return 99.f * (e - e0) / (e1 - e0);
}

// Compute practice prediction: probabilities + 4-step best predicted boards (worker thread; writes a lock-protected snapshot)
//   Speed: greedyDirScores (2-ply greedy + evalFillBoard, pure zero-search) replaces 6 full expectimax runs
//   with 6 sub-ms greedy runs; direction refresh drops from seconds to milliseconds.
static void practicePredWorker(int sz, std::vector<std::vector<int>> cur,
                               const std::vector<std::vector<int>>& prevBoard, int lastMoveDir,
                               bool randomSpawn)
{
    PracticePredData r;
    const int engineMode = g_aiEngineMode;   // capture engine mode (in sync with the AI operation engine)

    // current position: direction evaluation (probability + next best direction)
    //   4x4/5x5 calibrated to the new ziap search: use the engine score directly (same basis as AI operations),
    //   replacing the old greedy lookahead (more accurate). 3x3 still uses lightweight greedy (unchanged).
    std::vector<SolverAI2048::MoveScore> curRes;
    if (sz == 4 || sz == 5)
    {
        ZigSolverAI2048& engine = solverForThisThread(sz);
        curRes = practiceMoveScores(engine, cur, sz, engineMode);
    }
    else
    {
        curRes = greedyDirScores(cur, sz);
    }
    int bestDir = -1;
    practiceDirProbs(curRes, false, cur, sz, r.probs, bestDir);
    r.bestOp = -1;           // prev best = AI best direction at the position of the user's last op (previous frame); see prev block below
    r.nextBestOp = bestDir;  // next best = current position's best direction
    r.curOp  = g_practiceLastMoveDir;   // current op direction (worker thread only reads this global; no concurrent writes)

    // previous position: direction evaluation ("previous step" mode) + per-step expected deviation (mathematically formalized)
    r.prevBestOp = -1;
    r.lossPct = -1.f;
    r.lossGrade = 0;
    if (!prevBoard.empty() && (int)prevBoard.size() == sz)
    {
        // If the last step was AI-made: AI took the engine best -> deviation is always 0,
        //   and the "previous step" probabilities use fast greedy -> no solver/full search needed
        //   (much lower memory & CPU while AI is auto-playing).
        //   Otherwise (user move / history replay): full engine evaluation of the previous position,
        //   so the deviation matches the AI basis (scientific, stable).
        const bool aiMoved = g_lastMoveByAI.load();
        std::vector<SolverAI2048::MoveScore> prevRes;
        if (aiMoved) prevRes = greedyDirScores(prevBoard, sz);
        else {
            ZigSolverAI2048& engine = solverForThisThread(sz);
            prevRes = practiceMoveScores(engine, prevBoard, sz, engineMode);
        }
        int pBest = -1;
        practiceDirProbs(prevRes, false, prevBoard, sz, r.prevProbs, pBest);
        r.prevBestOp = pBest;
        r.bestOp = pBest;    // prev best = AI best direction at the position of the user's last op
        // per-step expected deviation (formalized): the user's relative position within the "best~worst" probability band
        //   dev = (p_best − p_d) / (p_best − p_min)，clamp [0,1]
        //   - AI took the engine best -> p_d = p_best -> dev = 0 (consistent basis, no more 98%+)
        //   - all directions close (narrow band; the engine is unsure too) -> dev ~ 0 (any pick is reasonable)
        //   - a clearly low-probability direction -> high deviation
        if (lastMoveDir >= 0 && lastMoveDir < 4)
        {
            if (aiMoved)
            {
                r.lossPct = 0.f;      // AI took the engine best -> perfect
                r.lossGrade = 0;
            }
            else
            {
                float pBestP = 0.f, pMinP = 1e9f;
                int nv = 0;
                for (int d = 0; d < 4; ++d)
                    if (r.prevProbs[d] > 1e-4f)
                    {
                        pBestP = std::max(pBestP, r.prevProbs[d]);
                        pMinP  = std::min(pMinP,  r.prevProbs[d]);
                        ++nv;
                    }
                const float pd = r.prevProbs[lastMoveDir];
                if (nv >= 2 && pd > 1e-4f)
                {
                    float dev;
                    const float span = pBestP - pMinP;
                    if (span < 0.10f) dev = 0.f;   // flat distribution: directions close, any pick is reasonable
                    else dev = std::min(1.f, std::max(0.f, (pBestP - pd) / span));
                    r.lossPct = deviationToPct(dev);   // erf model: soft cap 99.000%, never 100%
                    // grade by display %: 0-15% great / 15-50% good / 50-99% fair
                    if (r.lossPct <= 15.f)      r.lossGrade = 0;   // great -> green
                    else if (r.lossPct <= 50.f) r.lossGrade = 1;   // good -> yellow-green
                    else                        r.lossGrade = 2;   // fair -> yellow
                }
            }
            // accumulate direction-prediction calibration samples (Pearson chi-square; probability only): predicted best prob vs actually took best
            {
                std::lock_guard<std::mutex> lk(g_dirCalibMutex);
                if (r.prevBestOp >= 0 && r.prevProbs[r.prevBestOp] > 0.001f)
                    g_dirCalib.add(r.prevProbs[r.prevBestOp], lastMoveDir == r.prevBestOp);
            }
        }
    }

    // No previous-step data (start/first frame/no move) or invalid scores: keep -1 -> display --.--%
    // (2048 often has a single unique best direction, where this "expected deviation" weighting would inflate to 99%; not scientific, so no forced fill)

    // 4-step prediction: each step takes the greedy best direction, pure slide (no random spawn)
    //   With "refresh 4-step prediction", randomSpawn=true: steps 2/3/4 simulate random 2/4 spawns; layouts may differ per run
    auto b = cur;
    for (int step = 0; step < 4; ++step)
    {
        auto sc = greedyDirScores(b, sz);
        int bd = -1; float bsc = -1e9f;
        for (auto& m : sc) if (m.score > bsc) { bsc = m.score; bd = m.dir; }
        r.dirs[step] = bd;
        if (bd < 0) { r.board[step] = b; break; }
        if (!moveBoardPure(b, bd)) { r.board[step] = b; break; }
        r.board[step] = b;
        // refresh mode: after recording the current step, simulate random 2/4 spawns (step 1 spawns nothing, so steps 2-4 may differ)
        if (randomSpawn && step < 3)
            practiceSpawnRandomTile(b, sz);
    }
    r.ready = true;
    {
        std::lock_guard<std::mutex> lk(g_practicePredMutex);
        g_practicePred = std::move(r);
    }
}

// Start the prediction worker thread (internal)
static void launchPracticePrediction(const std::vector<std::vector<int>>& board,
                                     const std::vector<std::vector<int>>& prevBoard,
                                     int lastMoveDir, bool randomSpawn)
{
    const int sz = (int)board.size();
    if (sz < 3) return;
    g_practicePredBusy = true;
    g_practicePredFuture = std::async(std::launch::async,
        [sz, cur = board, prevBoard, lastMoveDir, randomSpawn]() mutable {
            practicePredWorker(sz, std::move(cur), prevBoard, lastMoveDir, randomSpawn);
            g_practicePredBusy = false;
        });
}

// Request recomputation of practice prediction (async worker, non-blocking) - with an explicit board matrix
// prevBoard/lastMoveDir optional: when given ("pre-move position + move dir"), also compute the per-step deviation (-1 = no data)
// randomSpawn=true: the 4-step prediction simulates random 2/4 spawns ("refresh 4-step prediction")
// While busy, the "latest request" is queued (never dropped) and runs when idle - prev/next step & history playback stay fresh
static void requestPracticePredictionFor(const std::vector<std::vector<int>>& board,
                                         const std::vector<std::vector<int>>& prevBoard = {},
                                         int lastMoveDir = -1,
                                         bool randomSpawn = false)
{
    g_pendingPredReq = { true, board, prevBoard, lastMoveDir, randomSpawn };
    if (g_practicePredBusy) return;
    launchPracticePrediction(board, prevBoard, lastMoveDir, randomSpawn);
}

// Request recomputation of practice prediction (async) - takes the board from the game instance
static void requestPracticePrediction(const Game2048& game, bool randomSpawn = false)
{
    const int sz = game.getBoardSize();
    std::vector<std::vector<int>> cur(sz, std::vector<int>(sz));
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            cur[i][j] = game.getTile(i, j);
    // capture the previous position (before the last move) and move direction for stability-loss analysis
    std::vector<std::vector<int>> prev;
    int lastDir = g_practiceLastMoveDir;
    if (game.hasPrevState() && lastDir >= 0 && lastDir < 4)
    {
        prev.assign(sz, std::vector<int>(sz, 0));
        for (int i = 0; i < sz; ++i)
            for (int j = 0; j < sz; ++j)
                prev[i][j] = game.getPrevBoardRow(i)[j];
    }
    requestPracticePredictionFor(cur, prev, lastDir, randomSpawn);
}

// Per-frame poll: when the worker finishes, copy the snapshot into the read cache (for drawing); start the latest queued frame when idle
static void pollPracticePrediction()
{
    if (!g_practicePredBusy && g_practicePredFuture.valid())
    {
        if (g_practicePredFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            g_practicePredFuture.get();   // reap and rethrow exceptions
    }
    if (!g_practicePredBusy && g_pendingPredReq.valid)
    {
        PendingPredRequest req = std::move(g_pendingPredReq);
        g_pendingPredReq.valid = false;
        launchPracticePrediction(req.cur, req.prev, req.lastMoveDir, req.randomSpawn);
    }
    std::lock_guard<std::mutex> lk(g_practicePredMutex);
    for (int i = 0; i < 4; ++i) { g_practicePredBoard[i] = g_practicePred.board[i]; g_practicePredDirs[i] = g_practicePred.dirs[i]; }
    for (int d = 0; d < 4; ++d) g_practiceProbs[d] = g_practicePred.probs[d];
    for (int d = 0; d < 4; ++d) g_practiceProbsPrev[d] = g_practicePred.prevProbs[d];
    g_practiceCurOp = g_practicePred.curOp;
    g_practiceBestOp = g_practicePred.bestOp;
    g_practiceNextBestOp = g_practicePred.nextBestOp;
    g_practicePrevBestOp = g_practicePred.prevBestOp;
    g_practiceLossPct = g_practicePred.lossPct;
    g_practiceLossGrade = g_practicePred.lossGrade;
}

// ===== Milestone likelihood (score-milestone achievement rate, Monte-Carlo simulation) =====
// Science: play many simulated games with AI-heuristic strategies (covering merge/snake/T-shape big-tile-friendly formations),
// count how often each milestone (512/1024/2048...) is reached from this position -> probability
static long long practiceMaxTile(const std::vector<std::vector<int>>& b)
{
    long long mx = 0;
    for (const auto& row : b)
        for (int v : row)
            if ((long long)v > mx && !isFillValue(v)) mx = v;
    return mx;
}

// Detect two value tiles mergeable in one move (same row/col, only empties between)
// For the "merge-ready bonus": two adjacent 256s -> 512 at ~99%
static bool hasMergeablePair(const std::vector<std::vector<int>>& b, int value)
{
    const int n = (int)b.size();
    for (int i = 0; i < n; ++i)
    {
        int first = -1;
        for (int j = 0; j < n; ++j)
        {
            if (b[i][j] == 0) continue;
            if (b[i][j] != value) { first = -1; continue; }
            if (first >= 0) return true;
            first = j;
        }
    }
    for (int j = 0; j < n; ++j)
    {
        int first = -1;
        for (int i = 0; i < n; ++i)
        {
            if (b[i][j] == 0) continue;
            if (b[i][j] != value) { first = -1; continue; }
            if (first >= 0) return true;
            first = i;
        }
    }
    return false;
}

// Spawn 2(90%)/4(10%) into a random empty cell (independent RNG per simulation; returns false when full)
static bool practiceSpawnRng(std::vector<std::vector<int>>& b, int sz, std::mt19937& rng)
{
    std::vector<int> empty;
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            if (b[i][j] == 0) empty.push_back(i * sz + j);
    if (empty.empty()) return false;
    std::uniform_int_distribution<int> pos(0, (int)empty.size() - 1);
    const int idx = empty[pos(rng)];
    b[idx / sz][idx % sz] = (rng() % 10 == 0) ? 4 : 2;
    return true;
}

// Count occurrences of a value on the board (merge-ready / linear-curve use)
static int countTiles(const std::vector<std::vector<int>>& b, int value)
{
    int n = 0;
    for (const auto& row : b)
        for (int v : row) if (v == value) ++n;
    return n;
}

// ===== Milestone-specific: lightweight direction pick (no solver instance; saves 64-136MB/thread of TT memory) =====
//   Strategy: 2-ply greedy + evalFillBoard heuristic (same evaluation as obstacle boards: empty+max+mono+merges+smooth)
//   Speed: ~0.01ms per move (vs 10-500ms for getAllMoveScores); 100-1000x less CPU
//   Quality: Monte-Carlo only needs "trend estimates"; greedy pushes merges correctly (formation bias averages out over many games)
static int milestonePickDir(const std::vector<std::vector<int>>& b, int sz)
{
    int bestDir = -1; float bestS = -1e18f;
    // ply-1: after each direction, do a ply-2 greedy (one more lookahead), avoiding "immediate merge breaking the formation"
    for (int d = 0; d < 4; ++d)
    {
        auto b1 = b;
        if (!moveBoardPure(b1, d)) continue;
        // ply-2: take the best evaluation of the 4 sub-directions
        float subBest = -1e18f;
        for (int d2 = 0; d2 < 4; ++d2)
        {
            auto b2 = b1;
            if (moveBoardPure(b2, d2))
                subBest = std::max(subBest, evalFillBoard(b2, sz));
        }
        // all second-step moves invalid -> fall back to evaluating the first-step position
        float s = (subBest > -1e17f) ? subBest : evalFillBoard(b1, sz);
        if (s > bestS) { bestS = s; bestDir = d; }
    }
    return bestDir;
}

// ===== Milestone-specific: deterministic merge-step BFS (independent of Monte-Carlo; guarantees "long view") =====
//   Idea: from the current position, run a bounded BFS with the milestonePickDir-like heuristic (IDDFS, max 13 steps)
//         to find the minimum steps to merge target (optimistic search: tries several spawn cells + 2/4, takes the best).
//   Independent of Monte-Carlo luck: formations that can merge a big tile within 13 steps return <=13 directly,
//   lifting the probability to 90%+, fixing "milestone long-term predictions are mediocre".
//   Returns steps (1..maxDepth) when found, maxDepth+1 otherwise
static int milestoneBfsMinSteps(std::vector<std::vector<int>> start, int sz,
                                 long long target, int maxDepth)
{
    if (practiceMaxTile(start) >= target) return 0;
    const int targetHalf = (int)(target >> 1);
    const int targetQuat = (int)(target >> 2);
    // early resource pruning: merging target needs 2 target/2 or equivalent
    // quickly reject impossible positions to avoid wasted BFS
    int halfRes = 0, quatRes = 0;
    for (const auto& r : start) for (int v : r) {
        if (v >= targetHalf) halfRes += 1 << (31 - __lzcnt((unsigned)(v / targetHalf)));
        else if (v >= targetQuat) quatRes++;
    }
    // available resources below 2x target/2 AND not enough spawn cells -> impossible
    // (loose pruning: continue only with >=1 target/2 or >=2 target/4)
    if (halfRes < 1 && quatRes < 2) return maxDepth + 1;

    // IDDFS: deepen step by step, return as soon as found (minimum steps)
    // state hash = compact board encoding, avoids revisiting same-depth positions
    // node budget: prevents optimistic expansion explosion on sparse 4x4/5x5 boards from slowing refresh (over budget = unreachable)
    auto encode = [&](const std::vector<std::vector<int>>& x) -> std::string {
        std::string k((size_t)sz * sz, '\0');
        for (int i = 0; i < sz; ++i) for (int j = 0; j < sz; ++j) {
            int v = x[i][j];
            k[(size_t)i * sz + j] = (char)(v > 127 ? 127 : (v < 0 ? -1 : v));
        }
        return k;
    };

    // per depth limit D: iterate 1..maxDepth
    struct Frame { std::vector<std::vector<int>> b; int depth; };
    std::vector<Frame> stack;
    stack.reserve(256);
    int nodes = 0;
    const int NODE_BUDGET = 30000;   // total visited-node budget (accumulated across depths)
    for (int D = 1; D <= maxDepth; ++D)
    {
        std::unordered_set<std::string> visited;
        visited.reserve(4096);
        stack.clear();
        stack.push_back({ start, 0 });
        visited.insert(encode(start));
        while (!stack.empty())
        {
            if (++nodes > NODE_BUDGET) return maxDepth + 1;   // budget exhausted: treat as unreachable
            Frame f = std::move(stack.back()); stack.pop_back();
            // move in 4 directions first, then enumerate spawns on each result (optimistic: best heuristic 2/4+cell)
            for (int d = 0; d < 4; ++d)
            {
                auto nb = f.b;
                if (!moveBoardPure(nb, d)) continue;
                const long long mt = practiceMaxTile(nb);
                if (mt >= target) return f.depth + 1;
                if (f.depth + 1 >= D) continue;   // this layer's depth limit reached
                // optimistic spawn: enumerate all empty cells x {2,4}, estimate "closeness to target" per candidate
                // heuristic = evalFillBoard + 2000*(count of targetHalf) + 1000*(count of targetQuat)
                float bestAfter = -1e18f;
                std::vector<std::vector<int>> bestBoard;
                bool anyEmpty = false;
                for (int i = 0; i < sz; ++i) for (int j = 0; j < sz; ++j) {
                    if (nb[i][j] != 0) continue;
                    anyEmpty = true;
                    for (int val : {2, 4})
                    {
                        nb[i][j] = val;
                        int hc = 0, qc = 0; long long mxv = 0;
                        for (const auto& rr : nb) for (int vv : rr) {
                            if (vv >= targetHalf) hc++;
                            else if (vv >= targetQuat) qc++;
                            if (vv > mxv) mxv = vv;
                        }
                        float prog = evalFillBoard(nb, sz)
                            + (float)hc * 2000.f + (float)qc * 800.f
                            + (mxv >= target ? 1e9f : (mxv >= targetHalf ? 5000.f : 0.f));
                        if (prog > bestAfter) {
                            bestAfter = prog;
                            bestBoard = nb;
                        }
                    }
                    nb[i][j] = 0;
                }
                if (!anyEmpty) continue;
                const std::string key = encode(bestBoard);
                if (!visited.insert(key).second) continue;
                stack.push_back({ std::move(bestBoard), f.depth + 1 });
            }
        }
    }
    return maxDepth + 1;
}

// Long-range optimistic evaluation gate: the current board must have a "substantial basis" for merging target
// (>=1 tile of target/4 or >=1 of target/8). With very few tiles (e.g. a fresh game of only 2/4),
// merge paths depend heavily on random spawn positions and optimistic simulation would overestimate (512 inflated to 87%+), so far-sight is disabled.
static bool milestoneHasMergeBase(const std::vector<std::vector<int>>& b, long long target)
{
    const long long qv = target >> 2;   // target/4
    const long long ev = target >> 3;   // target/8
    for (const auto& r : b)
        for (int v : r)
            if (v >= qv || v >= ev) return true;
    return false;
}

// ===== Long-range optimistic merge simulation (milestone "long view" extension) =====
//   Complements milestoneBfsMinSteps (strong guarantee of a path within 13): BFS sees only 13 steps,
//   this function runs a "best case" greedy merge simulation looking 30-70 steps ahead:
//   each step picks the direction most advancing target (prefer merging target/2, then target/4),
//   then optimistically spawns the new tile into the most favorable empty cell (2 or 4) until target is reached.
//   Returns the optimistic merge steps (<=maxDepth), or maxDepth+1 when unreachable.
//   Note: called only when milestoneHasMergeBase passes (a substantial merge basis exists),
//   avoiding sparse/fresh positions being inflated by optimistic simulation.
static int milestoneFarSimSteps(const std::vector<std::vector<int>>& start, int sz,
                                long long target, int maxDepth)
{
    if (practiceMaxTile(start) >= target) return 0;
    if (!milestoneHasMergeBase(start, target)) return maxDepth + 1;   // no merge basis -> unreachable
    const int targetHalf = (int)(target >> 1);
    const int targetQuat = (int)(target >> 2);
    auto b = start;
    for (int step = 1; step <= maxDepth; ++step)
    {
        // 1) pick the direction maximizing the "advance target" heuristic (half-blocks -> quarter-blocks -> empty/mono)
        int bestDir = -1; float bestS = -1e18f;
        std::vector<std::vector<int>> bestB;
        for (int d = 0; d < 4; ++d)
        {
            auto b1 = b;
            if (!moveBoardPure(b1, d)) continue;
            if (practiceMaxTile(b1) >= target) return step;   // the move itself achieves it
            float s = evalFillBoard(b1, sz)
                + (float)countTiles(b1, targetHalf) * 900.f
                + (float)countTiles(b1, targetQuat) * 300.f;
            if (s > bestS) { bestS = s; bestDir = d; bestB = b1; }
        }
        if (bestDir < 0) return maxDepth + 1;   // no valid move
        b = bestB;
        // 2) optimistic spawn: put 2/4 into the empty cell maximizing the "advance target" heuristic
        float bs2 = -1e18f;
        std::vector<std::vector<int>> bb2;
        bool any = false;
        for (int i = 0; i < sz; ++i) for (int j = 0; j < sz; ++j)
        {
            if (b[i][j] != 0) continue;
            any = true;
            for (int val : {2, 4})
            {
                b[i][j] = val;
                if (practiceMaxTile(b) >= target) return step;   // achieved right after the spawn
                float s2 = evalFillBoard(b, sz)
                    + (float)countTiles(b, targetHalf) * 900.f
                    + (float)countTiles(b, targetQuat) * 300.f;
                if (s2 > bs2) { bs2 = s2; bb2 = b; }
                b[i][j] = 0;
            }
        }
        if (!any) return maxDepth + 1;
        b = bb2;
    }
    return maxDepth + 1;
}

// Adaptive Monte-Carlo parameters (user math model v4):
//   - fill (occupancy) <20%~30%: simple position -> fewer steps, more games (<=16), quickly try different random spawns
//   - fill (occupancy) >40%: may hide a snake/T big formation -> fewer games (2~4), more steps
//     (3x3<=12 / 4x4<=24 / 5x5<=40), and use ziap search for moves (useZiap=true)
//   - fewer tiles -> more games, fewer steps; more tiles -> fewer games, more steps (inverse relation)
struct MilestoneSimParams { int sims; int maxSteps; int stallLimit; bool useZiap; };
static MilestoneSimParams milestoneSimParams(const std::vector<std::vector<int>>& b)
{
    const int sz = (int)b.size();
    const int total = sz * sz;
    int fillCnt = 0, tileCnt = 0;
    for (const auto& row : b)
        for (int v : row)
        {
            if (isFillValue(v)) ++fillCnt;   // obstacle
            else if (v > 0) ++tileCnt;       // normal tile
        }
    const float occPct = std::min(1.f, std::max(0.f, (float)(fillCnt + tileCnt) / (float)total));
    const int emptyCnt = total - (fillCnt + tileCnt);   // remaining empty cells

    // step cap (>40% fill): 3x3 12 / 4x4 24 / 5x5 40
    const int stepMax = (sz == 3) ? 12 : (sz == 4 ? 24 : 40);
    // inverse: 0% occupancy -> 16 games / few steps; 100% -> 2 games / stepMax steps
    int sims  = (int)std::lround(16.f - 14.f * occPct);
    int steps = (int)std::lround(3.f + (float)(stepMax - 3) * occPct);
    bool useZiap = false;

    if (occPct < 0.20f)
    {
        // fill <20%: more games (<=16), fewer steps
        sims  = std::max(sims, 12);
        steps = std::min(steps, 6);
    }
    else if (occPct > 0.40f)
    {
        // fill >40%: fewer games (2~4), more steps (stepMax), ziap search detects snake/T formations
        sims  = std::min(sims, 4);
        sims  = std::max(sims, 2);
        steps = stepMax;
        useZiap = true;
    }
    // between 0.20 and 0.40, transition linearly via the inverse relation

    if (sims < 2) sims = 2;
    if (sims > 16) sims = 16;
    if (steps < 3) steps = 3;
    if (steps > stepMax) steps = stepMax;

    // near-full board (only 0-2 empties left, 4x4/5x5 especially): extremely delicate -> raise games (3~5) and steps,
    // more games probe different spawns, longer steps check whether merging can continue in the cracks
    if (emptyCnt <= 2 && (sz == 4 || sz == 5))
    {
        sims  = std::min(5, std::max(3, sims));   // 3~5 games
        steps = stepMax;                           // more steps
        useZiap = true;                            // key endgames use ziap search for accuracy
    }

    const int stall = std::max(4, steps / 2);
    return { sims, steps, stall, useZiap };
}

static void practiceMilestoneWorker(int sz, std::vector<std::vector<int>> cur, const long long ms[2], int cnt)
{
    // ===== Phase 1: deterministic merge-step detection (independent of Monte-Carlo; guarantees "long view") =====
    //   - milestoneBfsMinSteps: IDDFS (<=13 steps, strong guarantee of a merge path)
    //   - milestoneFarSimSteps: optimistic merge simulation (detection depth = user-model step cap 12/24/40)
    const int farMax = (sz == 3) ? 12 : (sz == 4 ? 24 : 40);
    int bfsMinSteps[2] = { 999, 999 };
    int farMinSteps[2] = { 999, 999 };
    for (int i = 0; i < cnt; ++i)
    {
        if (ms[i] <= practiceMaxTile(cur)) { bfsMinSteps[i] = 0; farMinSteps[i] = 0; continue; }
        if (ms[i] < 4) continue;
        bfsMinSteps[i] = milestoneBfsMinSteps(cur, sz, ms[i], 13);
        farMinSteps[i] = milestoneFarSimSteps(cur, sz, ms[i], farMax);
    }

    // ===== Phase 2: adaptive Monte-Carlo simulation (user math model v4) =====
    MilestoneSimParams P = milestoneSimParams(cur);
    const int SIMS = P.sims;
    const int MAX_STEPS = P.maxSteps;
    const int STALL_LIMIT = P.stallLimit;
    const bool USE_ZIAP = P.useZiap;   // fill>40%: ziap search for moves (detects snake/T formations)

    // threads: >40% fill uses ziap search (memory+CPU heavy) -> single serial thread; otherwise 2 greedy threads
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const int maxThreads = USE_ZIAP ? 1 : 2;
    const int NTHREADS = (int)std::min<unsigned int>(hw, (unsigned)std::min(SIMS, maxThreads));
    std::vector<std::vector<int>> threadHits(NTHREADS, std::vector<int>(cnt, 0));
    std::vector<std::vector<int>> threadMinSteps(NTHREADS, std::vector<int>(cnt, 999));
    std::vector<std::future<void>> futs;
    int chunkBase = 0;
    for (int th = 0; th < NTHREADS; ++th)
    {
        int chunkSims = SIMS / NTHREADS + (th < SIMS % NTHREADS ? 1 : 0);
        if (chunkSims <= 0) continue;
        const int myBase = chunkBase;
        chunkBase += chunkSims;
        futs.push_back(std::async(std::launch::async,
            [sz, cur, ms, cnt, chunkSims, myBase, MAX_STEPS, STALL_LIMIT, USE_ZIAP, &threadHits, &threadMinSteps, th]() {
            // milestone thread at low priority: does not preempt direction-prediction/main-thread CPU
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            int local[2] = { 0, 0 };
            int localMin[2] = { 999, 999 };
            (void)th;
            // fill>40% (USE_ZIAP): ziap search picks directions (recognizes snake/T formations)
            ZigSolverAI2048* engine = nullptr;
            if (USE_ZIAP) engine = &solverForThisThread(sz);
            for (int s = 0; s < chunkSims; ++s)
            {
                std::mt19937 rng((uint32_t)((myBase + s) * 2654435761u + 0x9E3779B9u));
                auto b = cur;
                long long maxTile = practiceMaxTile(b);
                bool hit[2] = { false, false };
                for (int i = 0; i < cnt; ++i) if (ms[i] <= maxTile) hit[i] = true;
                int steps = 0;
                int stall = 0;
                long long stallMax = maxTile;
                while (steps < MAX_STEPS)
                {
                    bool all = true;
                    for (int i = 0; i < cnt; ++i) if (ms[i] > maxTile) { all = false; break; }
                    if (all) break;
                    int bestDir = -1;
                    if (engine)
                    {
                        // ziap search (user model: >40% fill uses ziap)
                        auto sc = engine->getAllMoveScores(b);
                        if (sz == 4) for (auto& m : sc) m.dir = solver4ToCppDir(m.dir);
                        if (!sc.empty())
                        {
                            float bsc = -1e30f;
                            for (auto& m : sc) if (m.score > bsc) { bsc = m.score; bestDir = m.dir; }
                        }
                    }
                    else
                    {
                        // lightweight 2-ply greedy + evalFillBoard (saves memory and CPU)
                        bestDir = milestonePickDir(b, sz);
                    }
                    if (bestDir < 0) break;
                    moveBoardScored(b, bestDir);
                    maxTile = practiceMaxTile(b);
                    ++steps;
                    for (int i = 0; i < cnt; ++i)
                        if (!hit[i] && ms[i] <= maxTile)
                        {
                            hit[i] = true;
                            if (steps < localMin[i]) localMin[i] = steps;
                        }
                    if (maxTile > stallMax) { stallMax = maxTile; stall = 0; }
                    else if (++stall > STALL_LIMIT) break;
                    if (!practiceSpawnRng(b, sz, rng)) break;
                }
                for (int i = 0; i < cnt; ++i) if (hit[i]) ++local[i];
            }
            for (int i = 0; i < cnt; ++i)
            {
                threadHits[th][i] = local[i];
                threadMinSteps[th][i] = localMin[i];
            }
        }));
    }
    for (auto& f : futs) f.get();
    int hits[2] = { 0, 0 };
    int minStepsMc[2] = { 999, 999 };   // earliest merge steps from Monte-Carlo only
    for (int th = 0; th < NTHREADS; ++th)
        for (int i = 0; i < cnt; ++i)
        {
            hits[i] += threadHits[th][i];
            if (threadMinSteps[th][i] < minStepsMc[i]) minStepsMc[i] = threadMinSteps[th][i];
        }

    // per milestone, independently:
    //   1) Bayesian smoothing: Monte-Carlo hit rate + 1/(SIMS+2)
    //   2) direct-merge detection: adjacent m/2 pair already present -> 100%
    //   3) linear curve: a single >=m/2 tile present -> 40%~98%
    //   4) long view: min(BFS steps, MC steps) <=13 -> 90%+ (luck-independent)
    const long long maxTile0 = practiceMaxTile(cur);
    float r[2];
    for (int i = 0; i < cnt; ++i)
    {
        const long long m = ms[i];
        r[i] = ((float)hits[i] + 1.f) / (float)(SIMS + 2);
        if (m <= maxTile0) { r[i] = 1.0f; continue; }
        if (m < 4) continue;
        const int half = (int)(m >> 1);
        const int halfCnt = countTiles(cur, half);
        if (hasMergeablePair(cur, half))
            r[i] = 1.0f;
        else if (halfCnt >= 1)
        {
            // resource-proximity curve (fixes "almost there yet stuck at 40%"): only one more target/2 merge is needed;
            // more target/2, target/4, target/8 tiles -> probability rises smoothly instead of fixed 0.40
            float res = (float)halfCnt
                      + (float)countTiles(cur, (int)(m >> 2)) * 0.5f
                      + (float)countTiles(cur, (int)(m >> 3)) * 0.25f;
            float prog;
            if (res >= 2.f)      prog = 0.90f;
            else if (res >= 1.f) prog = 0.55f + (res - 1.f) * 0.35f;   // 1→0.55, 2→0.90
            else                 prog = 0.35f + res * 0.20f;
            if (hasMergeablePair(cur, (int)(m >> 2))) prog += 0.05f;
            if (prog > 0.99f) prog = 0.99f;
            r[i] = std::max(r[i], prog);
        }
        // valid-merge detection -> probability boost (user model: detected merges raise to 100%, linear approximation):
        //   - BFS found a <=13-step deterministic path -> 100% (strong guarantee: the formation can merge)
        //   - farSim found a <=stepMax optimistic path -> 90%~99% linear (closer = higher)
        //   - Monte-Carlo actually merged (small minStepsMc) -> linear boost by steps
        float baseProb = -1.f;
        if (bfsMinSteps[i] <= 13)
        {
            baseProb = 1.0f;   // valid merge (deterministic path) -> 100%
        }
        else if (farMinSteps[i] <= farMax)
        {
            baseProb = 0.90f + (float)(farMax - farMinSteps[i]) / (float)farMax * 0.09f;
            if (baseProb > 0.99f) baseProb = 0.99f;   // linear rise to ~99%
        }
        else if (minStepsMc[i] <= 40)
        {
            baseProb = 0.55f + (40.0f - (float)minStepsMc[i]) / 40.0f * 0.35f;   // 55%~90%
        }
        if (baseProb > 0.f) r[i] = std::max(r[i], baseProb);
    }

    {
        std::lock_guard<std::mutex> lk(g_practiceMilestoneMutex);
        // milestone confidence calibration (Pearson chi-square): history shows systematic overestimation -> lower this prediction
        {
            std::lock_guard<std::mutex> lk2(g_mileCalibMutex);
            const double c = g_mileCalib.calibScale();
            if (c > 0.0 && c < 0.99)
                for (int i = 0; i < cnt; ++i)
                    r[i] = std::max(0.05f, r[i] * (float)c);
        }
        for (int i = 0; i < cnt; ++i)
            g_practiceMilestonePct[i] = r[i] * 100.f;
        g_practiceMilestoneCount = cnt;
        // update milestone calibration tracking: the old tracked target was replaced by a new prediction (not achieved) -> miss sample; track the new one
        {
            std::lock_guard<std::mutex> lk2(g_mileCalibMutex);
            if (g_mileTrackOn && g_mileTrackVal > 0 && g_mileTrackVal != ms[0])
                g_mileCalib.add(g_mileTrackP, false);   // window moved on = old target not achieved
            if (cnt > 0 && ms[0] > 0)
            {
                g_mileTrackVal = ms[0];
                g_mileTrackP = r[0];
                g_mileTrackOn = true;
            }
            else g_mileTrackOn = false;
        }
    }
}

// Milestone confidence calibration: when the board changes, check whether the tracked target was actually achieved (hit sample)
static void updateMilestoneCalibration(const std::vector<std::vector<int>>& board)
{
    std::lock_guard<std::mutex> lk(g_mileCalibMutex);
    if (!g_mileTrackOn) return;
    if (practiceMaxTile(board) >= g_mileTrackVal)
    {
        g_mileCalib.add(g_mileTrackP, true);   // achieved -> hit sample
        g_mileTrackOn = false;
    }
}

// Start the milestone prediction thread (internal)
static void launchMilestonePrediction(const std::vector<std::vector<int>>& board, int sz)
{
    long long maxTile = practiceMaxTile(board);
    // sliding window: 3x3 baseline 128/256; 4x4/5x5 baseline 512/1024
    // once the smallest milestone is achieved the window moves (128 disappears, 256->512 appended, and so on)
    long long base0 = (sz == 3) ? 128LL : 512LL;
    long long w0 = std::max(base0, maxTile * 2);
    // science cap: 3x3 max 2^10=1024; 4x4 max 2^16=65536; 5x5 max 2^25
    // milestones beyond the cap are not shown (e.g. 4x4 after 2^15 leaves only 2^16)
    const int maxPow = (sz == 3) ? 10 : (sz == 4 ? 16 : 25);
    const long long boardMax = 1LL << maxPow;
    long long ms[2] = { 0, 0 };
    int cnt = 0;
    long long cand = w0;
    for (int i = 0; i < 2; ++i, cand *= 2)
        if (cand <= boardMax) ms[cnt++] = cand;
    if (cnt == 0) { ms[0] = boardMax; cnt = 1; }   // cap reached: show the cap itself (100%)
    {
        std::lock_guard<std::mutex> lk(g_practiceMilestoneMutex);
        g_practiceMilestoneVals[0] = ms[0]; g_practiceMilestoneVals[1] = ms[1];
        g_practiceMilestoneCount = cnt;
    }
    g_practiceMilestoneBusy = true;
    g_milestoneLastStartMs = GetTickCount();   // record start time (for throttling)
    g_practiceMilestoneFuture = std::async(std::launch::async,
        [sz, cur = board, ms, cnt]() mutable {
            // milestones run at low priority in their own thread group: never preempt direction-prediction/main thread (avoids display lag)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            practiceMilestoneWorker(sz, std::move(cur), ms, cnt);
            g_practiceMilestoneBusy = false;
        });
}

// Request milestone likelihood (async; queues the latest frame while busy, runs when idle)
//   Throttle: if <180ms since the last start, only update the queued request (avoids recompute storms on replay drag/fast moves)
static void requestMilestonePrediction(const std::vector<std::vector<int>>& board, int sz)
{
    if (sz < 3) return;
    updateMilestoneCalibration(board);   // board changed: check whether the tracked milestone was achieved (confidence calibration)
    g_milestonePending = true;
    g_milestonePendingSz = sz;
    g_milestonePendingBoard = board;
    if (g_practiceMilestoneBusy) return;
    const DWORD now = GetTickCount();
    if (now - g_milestoneLastStartMs < MILESTONE_MIN_INTERVAL_MS) return;   // throttled: start later in poll
    launchMilestonePrediction(board, sz);
}

// Per-frame poll: reap finished milestone threads; when idle and a request is queued, start the latest frame (also checks throttle)
static void pollMilestonePrediction()
{
    if (!g_practiceMilestoneBusy && g_practiceMilestoneFuture.valid())
    {
        if (g_practiceMilestoneFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            g_practiceMilestoneFuture.get();
    }
    if (!g_practiceMilestoneBusy && g_milestonePending)
    {
        const DWORD now = GetTickCount();
        if (now - g_milestoneLastStartMs < MILESTONE_MIN_INTERVAL_MS) return;   // still throttled: try next frame
        g_milestonePending = false;
        launchMilestonePrediction(g_milestonePendingBoard, g_milestonePendingSz);
    }
}

// ===== Practice imported-history review: per-step quality analysis (worker thread) =====
// Quality: 0=perfect 1=fair 2=poor 3=bad
static void practiceQualityWorker(int sz, std::vector<std::vector<int>> frames, std::vector<int> dirs);

// Start quality analysis for a given history entry (passes a copy, avoiding main-thread mutation races)
static void startPracticeQualityAnalysis(int entryIdx);

// Forward declaration: saveAiSettings is defined after aiApplyRate, but aiApplyRate needs it
static void saveAiSettings();

// Forward declaration: layout helpers are defined in the layout section below (practice best-fit sizing needs them)
static float computeBoardPx(unsigned int winW, unsigned int winH, bool aiActive);
static float computeGapBtnRatio();

// Enter practice mode: remember the original window size; width auto-fits (board + analysis panel exactly fill)
// If the window already fills the screen (maximized), no rescale; the layout adapts
static void enterPracticeMode(sf::RenderWindow& window, const Game2048& game)
{
    g_practiceOrigW = window.getSize().x;
    g_practiceOrigH = window.getSize().y;
    g_practiceActive = true;
    g_practiceImport = false;
    g_practiceBuildOpen = false;
    g_practiceLastMoveDir = -1;
    g_lastMoveByAI = false;

    RECT rc, wa;
    GetWindowRect((HWND)window.getSystemHandle(), &rc);
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    const int sw = wa.right - wa.left, sh = wa.bottom - wa.top;
    const bool fillsScreen = (rc.right - rc.left >= sw - 4 && rc.bottom - rc.top >= sh - 4);
    if (!fillsScreen)
    {
        // best-fit width: height constraints decide the board size; width makes board+panel (1:2.15)+margins exactly fit
        // (LAYOUT_PAD_RATIO=0.016f, PRACTICE_WHOLE_R=2.15f are defined in the layout section below)
        bool aiActive = (g_aiMode != AI_INACTIVE);
        float bp = computeBoardPx(100000u, g_practiceOrigH, aiActive);   // height-constrained board size
        float newW = bp * (2.15f + 2.f * 0.016f);
        unsigned int newWU = (unsigned int)std::min(newW, (float)sw);
        if (newWU != g_practiceOrigW && newWU >= 320)
            window.setSize(sf::Vector2u(newWU, g_practiceOrigH));
    }
    requestPracticePrediction(game);
    logMsg("Practice mode ON");
}

// Exit practice mode: restore the original window size and clean practice state
static void exitPracticeMode(sf::RenderWindow& window)
{
    g_practiceActive = false;
    g_practiceImport = false;
    g_practiceBuildOpen = false;
    g_practiceLastMoveDir = -1;
    g_lastMoveByAI = false;
    if (g_practiceOrigW > 0 && g_practiceOrigH > 0)
        window.setSize(sf::Vector2u(g_practiceOrigW, g_practiceOrigH));
    logMsg("Practice mode OFF");
}

static void aiApplyRate(int rate)
{
    if (rate < AI_RATE_MIN) rate = AI_RATE_MIN;
    if (rate > AI_RATE_MAX) rate = AI_RATE_MAX;
    g_aiRate = rate;
    g_aiStepDelaySec = aiRateToDelayMs(g_aiRate) / 1000.f;
    saveAiSettings();   // save on change; restored on next launch
}
// Start a new AI session: clear count/time stats
static void aiResetSessionStats()
{
    g_aiMoveCount = 0;
    g_aiElapsedMs = 0.f;
    g_aiTiming = false;
}

// ========== AI settings persistence (ai_settings.ini) ==========
// Purpose: remember the user's last AI rate and intelligence tier; restored on next launch
// Written: immediately when the user adjusts rate/tier in the AI settings dialog (no need to wait for exit)
// First install (no file) -> defaults Rate=30, Level=2 (tier 3)
static void saveAiSettings()
{
    std::ofstream ofs(AI_SETTINGS_FILE, std::ios::trunc);
    if (!ofs) {
        logMsg(std::string("ERROR: saveAiSettings FAILED - cannot write: ") + AI_SETTINGS_FILE);
        return;
    }
    ofs << "[AISettings]\r\n";
    ofs << "Rate=" << g_aiRate << "\r\n";
    ofs << "Level=" << g_aiLevel << "\r\n";
    ofs << "ShowMoves=" << (g_aiShowMoves ? 1 : 0) << "\r\n";
    ofs << "ShowTime=" << (g_aiShowTime ? 1 : 0) << "\r\n";
    ofs << "UseUndo=" << (g_aiUseUndo ? 1 : 0) << "\r\n";
    ofs << "Engine=" << g_aiEngineMode << "\r\n";
    ofs << "PredHidden=" << (g_practicePredHidden ? 1 : 0) << "\r\n";
    ofs.close();
    logMsg(std::string("ai_settings saved: Rate=") + std::to_string(g_aiRate) +
           " Level=" + std::to_string(g_aiLevel) +
           " ShowMoves=" + std::to_string(g_aiShowMoves ? 1 : 0) +
           " ShowTime=" + std::to_string(g_aiShowTime ? 1 : 0) +
           " UseUndo=" + std::to_string(g_aiUseUndo ? 1 : 0) +
           " Engine=" + std::to_string(g_aiEngineMode) +
           " PredHidden=" + std::to_string(g_practicePredHidden ? 1 : 0));
}

static void loadAiSettings()
{
    std::ifstream ifs(AI_SETTINGS_FILE);
    if (!ifs) {
        // no config (first open): everything on by default (show count/time, AI uses undo, show 4-step prediction), anim synced to rate
        aiApplyRate(g_aiRate);
        g_aiLevel = AI_LEVEL_MAX;             // tier 3 (default highest)
        g_aiDepth = g_aiLevelDepth[g_aiLevel];
        g_aiShowMoves = true;                 // on by default on first open
        g_aiShowTime  = true;
        g_practicePredHidden = false;         // 4-step prediction shown by default
        logMsg(std::string("ai_settings.ini not found - defaults (Rate=") +
               std::to_string(g_aiRate) + ", Level=" + std::to_string(g_aiLevel) + ")");
        return;
    }
    int rate = g_aiRate;
    int level = g_aiLevel;
    bool showMoves = g_aiShowMoves;
    bool showTime  = g_aiShowTime;
    bool useUndo   = g_aiUseUndo;
    int  engine    = g_aiEngineMode;
    bool predHidden = g_practicePredHidden;
    std::string line;
    bool inSection = false;
    while (std::getline(ifs, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        std::string t = line.substr(start, end - start + 1);
        if (t.empty()) continue;
        if (t[0] == '[') { inSection = (t == "[AISettings]"); continue; }
        if (!inSection) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq);
        std::string val = t.substr(eq + 1);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        for (auto& c : val) c = (char)tolower((unsigned char)c);
        if (key == "rate") {
            try { rate = std::stoi(val); } catch (...) {}
        } else if (key == "level") {
            try { level = std::stoi(val); } catch (...) {}
        } else if (key == "showmoves") {
            try { showMoves = (std::stoi(val) != 0); } catch (...) {}
        } else if (key == "showtime") {
            try { showTime = (std::stoi(val) != 0); } catch (...) {}
        } else if (key == "useundo") {
            try { useUndo = (std::stoi(val) != 0); } catch (...) {}
        } else if (key == "engine") {
            try { engine = std::stoi(val); } catch (...) {}
        } else if (key == "predhidden") {
            try { predHidden = (std::stoi(val) != 0); } catch (...) {}
        }
    }
    // clamp into the valid range
    if (rate < AI_RATE_MIN) rate = AI_RATE_MIN;
    if (rate > AI_RATE_MAX) rate = AI_RATE_MAX;
    if (level < AI_LEVEL_MIN) level = AI_LEVEL_MIN;
    if (level > AI_LEVEL_MAX) level = AI_LEVEL_MAX;
    g_aiRate = rate;
    g_aiLevel = level;
    g_aiDepth = g_aiLevelDepth[level];
    g_aiShowMoves = showMoves;
    g_aiShowTime  = showTime;
    g_aiUseUndo   = useUndo;
    if (engine != 0 && engine != 1) engine = 0;
    g_aiEngineMode = engine;
    g_practicePredHidden = predHidden;
    aiApplyRate(rate);   // sync the animation duration
    logMsg(std::string("ai_settings loaded: Rate=") + std::to_string(rate) +
           " Level=" + std::to_string(level) +
           " ShowMoves=" + std::to_string(showMoves ? 1 : 0) +
           " ShowTime=" + std::to_string(showTime ? 1 : 0) +
           " PredHidden=" + std::to_string(predHidden ? 1 : 0));
}

// --- Precomputed result (passed across threads via std::future; no global shared writes) ---
struct PrefetchResult {
    bool ok = false;
    std::vector<std::vector<int>> startBoard;  // matched cell-by-cell on consume (discarded if undo shifted the board)
    std::vector<SolverAI2048::MoveScore> ranked;
    float evalBefore = 0.f;
    int emptyStart = 0;
    int maxTileStart = 0;
    int aiBoardSize = 4;
    std::string statsString;  // worker packs stats itself (fixes main thread always reading 0 from the thread_local instance)
};
static std::future<PrefetchResult> g_prefetchFuture;

// --- Consecutive-undo counter: prevents infinite "undo+move" loops after a dead end ---
// User logic: dead end -> undo once -> recompute -> move. This loop must not exceed 12 times
// Counts completed "undo+move" cycles (prevents AI repeatedly undoing and re-moving)
static int g_aiUndoConsecutive = 0;
// Marks whether the previous step executed an undo; the next successful move completes a cycle count
static bool g_aiUndoPending = false;
// Direction skipped on undo retry = the step that caused the dead end (prevents "undo->same move->same dead end" spinning)
static int g_aiSkipDir = -1;
// Records the AI's last actual direction (to locate the step to skip on undo)
static int g_aiLastMoveDir = -1;

static inline int aiDirToDx(int d) { return d == 0 ? -1 : (d == 1 ? 1 : 0); }
static inline int aiDirToDy(int d) { return d == 2 ? -1 : (d == 3 ? 1 : 0); }

static std::vector<std::vector<int>> buildAiBoard(const Game2048& game)
{
    int n = game.getBoardSize();
    std::vector<std::vector<int>> b(n, std::vector<int>(n, 0));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            b[i][j] = game.getTile(i, j);
    return b;
}

// ------- Start background precompute (persistent worker thread, reuses the engine, removes per-step rebuild cost) -------
// Note (v3.2.15): switched to a persistent worker thread for AI search; its thread_local solver is created once
//   and reused forever (the 32MB TT + thread pool built once), no more per-step std::async new thread + new engine
//   (the old implementation rebuilt the engine every step, costing hundreds of ms late-game - a root cause of "laggy moves").
//   Results travel through promise/future; the consume logic in aiStep is unchanged; custom-promise future destructors do not block.
static std::thread g_aiWorker;
static std::mutex g_aiWorkerMutex;
static std::condition_variable g_aiWorkerCv;
static bool g_aiWorkerStop = false;
static bool g_aiTaskPending = false;
static std::vector<std::vector<int>> g_aiTaskBoard;
static int g_aiTaskSize = 4;
static int g_aiTaskMode = 0;
static std::promise<PrefetchResult> g_aiTaskPromise;

static void aiWorkerLoop()
{
    for (;;) {
        std::vector<std::vector<int>> board;
        int size = 4, mode = 0;
        std::promise<PrefetchResult> prom;
        {
            std::unique_lock<std::mutex> lk(g_aiWorkerMutex);
            g_aiWorkerCv.wait(lk, [&] { return g_aiWorkerStop || g_aiTaskPending; });
            if (g_aiWorkerStop) return;
            board = std::move(g_aiTaskBoard);
            size = g_aiTaskSize;
            mode = g_aiTaskMode;
            prom = std::move(g_aiTaskPromise);
            g_aiTaskPending = false;
        }
        PrefetchResult r;
        try {
            // this worker's thread_local solver is reused forever (created once, never rebuilt)
            ZigSolverAI2048& engine = solverForThisThread(size);
            engine.setNewEngine(mode == 1);
            engine.setDepth(g_aiDepth);
            // new-game flag: fully reset the engine on AI session start / board reset (clear TT + restore defaults)
            // guards against AI degradation as games accumulate (start_search also resets TT each step; this is the multi-game hard guarantee)
            if (g_aiCacheReset) {
                engine.newGame();
                g_aiCacheReset = 0;
            }
            r.startBoard   = board;
            r.aiBoardSize  = size;
            r.emptyStart   = SolverAI2048::countEmptyBoard(board);
            r.maxTileStart = SolverAI2048::getMaxTile(board);
            r.ranked = engine.getAllMoveScores(board);
            r.evalBefore   = engine.evaluateBoard(board);
            r.statsString  = engine.getStatsString();
            r.ok = !r.ranked.empty();
        } catch (...) { r.ok = false; }
        {
            std::lock_guard<std::mutex> lk(g_aiWorkerMutex);
            try { prom.set_value(std::move(r)); } catch (...) {}
        }
    }
}

// Stop the persistent worker (called on exit; AI session end does NOT stop it, keeping the engine reusable)
static void aiWorkerStop()
{
    {
        std::lock_guard<std::mutex> lk(g_aiWorkerMutex);
        g_aiWorkerStop = true;
    }
    g_aiWorkerCv.notify_one();
    if (g_aiWorker.joinable()) g_aiWorker.join();
}

static void aiLaunchPrefetch(const std::vector<std::vector<int>>& board, int boardSize, bool force = false)
{
    // previous future: reap if ready; if not ready and not force, skip kicking a new one; force overwrites
    // (promise futures do not block on destruction; an old task finishing later with set_value is safe)
    if (g_prefetchFuture.valid()) {
        auto status = g_prefetchFuture.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            try { (void)g_prefetchFuture.get(); } catch (...) {}
        } else if (!force) {
            return;   // old task still running; do not kick a new one
        }
    }
    // make sure the persistent worker started (created once)
    if (!g_aiWorker.joinable())
        g_aiWorker = std::thread(aiWorkerLoop);

    // post the task (single-slot overwrite): the worker takes and processes it, overwriting an old unprocessed one
    std::promise<PrefetchResult> prom;
    auto fut = prom.get_future();
    {
        std::lock_guard<std::mutex> lk(g_aiWorkerMutex);
        g_aiTaskBoard  = board;
        g_aiTaskSize   = boardSize;
        g_aiTaskMode   = g_aiEngineMode;   // capture engine mode (in sync with the AI operation engine)
        g_aiTaskPromise = std::move(prom);
        g_aiTaskPending = true;
    }
    g_aiWorkerCv.notify_one();
    g_prefetchFuture = std::move(fut);
}

// ------- End AI session -------
// AI operation ends (dead end / unsolvable): keep the AI control UI (3 keys), show an "AI operation fully ended" notice.
// After the user closes the notice the AI UI remains (final board visible; "Play" retries, "Cancel" truly exits).
// Only the user pressing "Cancel" sets AI_INACTIVE (UI disappears).
static void aiEndSession()
{
    // reap future: consume if ready; otherwise reset directly (promise futures do not block on destruction;
    // a later set_value with no consumer is safe). The worker stays alive to reuse the engine.
    if (g_prefetchFuture.valid()) {
        auto status = g_prefetchFuture.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            try { (void)g_prefetchFuture.get(); } catch (...) {}
        }
        g_prefetchFuture = std::future<PrefetchResult>();
    }
    // session end: clear "running/ready" state but keep the AI control UI (3 keys)
    g_aiMode = AI_PAUSED;            // paused -> UI stays (Play shows "play", can retry)
    g_aiSettingsOpen = false;   // session end: close the AI settings dialog
    s_aiConsumingPending = false;  // leave the "poll pending per frame" state
    g_aiTiming = false;            // stop AI timing
    g_showAIEnd = true;
}

// Resume AI from paused/ended (shared by the Play button and "Start" after the AI-end dialog):
// continue from the current position (AI's own undo policy handles dead ends), do not clear the board or reset the AI UI.
// Sets RUNNING, resumes timing, resets undo counters, immediately kicks background precompute; AI UI stays visible.
static void aiResumeRunning(Game2048& game)
{
    g_aiMode = AI_RUNNING;
    g_aiTiming = true;           // resume timing (continues from the current accumulated value)
    // animation duration follows the current rate (animation fills most of the step interval; looks natural)
    game.setAIAnimDuration(g_aiStepDelaySec);
    game.markAiAssisted();       // mark this game as AI-assisted (history shows a blue tag)
    if (g_aiBoardSize != game.getBoardSize())
    {
        g_aiBoardSize = game.getBoardSize();
        g_aiEngine.setBoardSize(g_aiBoardSize);
    }
    // reset undo-related counters and pending flags when starting an AI session
    g_aiUndoConsecutive = 0;
    g_aiUndoPending = false;
    g_aiSkipDir = -1;  g_aiLastMoveDir = -1;
    // kick background precompute immediately on start (thread_local solver, zero sharing)
    {
        auto initBoard = buildAiBoard(game);
        aiLaunchPrefetch(initBoard, g_aiBoardSize);
    }
    g_aiStepClock.restart();
}

// ========== Challenge 2048 mode ==========
// Challenges: 6 (4x4: 2048/4096/8192; 5x5: 4096/8192/16384)
struct ChallengeProject { int boardSize; int target; };
static const ChallengeProject g_chProjects[6] = {
    { 4, 2048 }, { 4, 4096 }, { 4, 8192 },
    { 5, 4096 }, { 5, 8192 }, { 5, 16384 }
};
struct ChallengeState {
    bool active = false;        // whether in challenge mode
    int  project = 0;           // current project 0..5
    bool started = false;       // timing started (after the first valid move)
    bool finished = false;      // finished (success/fail) -> show the result dialog
    bool won = false;           // whether the challenge was won
    double elapsedMs = 0.0;     // elapsed ms
    sf::Clock clock;
    int  moveCount = 0;         // moves made
    bool settingsOpen = false;  // challenge project settings dialog
    // switch-project confirm dialog
    int  confirmSwitchProject = -1;  // >=0 = waiting for confirmation to switch to this project
    // challenge settings dialog hit rects
    sf::FloatRect settingsRect;
    sf::FloatRect projBtn[6];   // 6 project buttons
    sf::FloatRect btnClose;     // close button
    int hoverProj = -1;
    // confirm dialog buttons
    sf::FloatRect confirmYes, confirmNo;
} g_ch;

// ========== Timer Mode ==========
// New menu key below color-scheme/challenge-2048; opens to menu->options, undo->close (same as challenge 2048)
// Options: show moves / show main time / show IPS; milestone slider bar (right of board, window widens automatically)
struct TimerModeState {
    bool active = false;        // whether in timer mode
    bool started = false;       // timing started (after the first valid move)
    bool finished = false;
    bool resultDismissed = false; // whether the game-over result dialog was closed (timer mode stays; clicking the score box starts a new game)
    double elapsedMs = 0.0;     // main time (ms)
    sf::Clock clock;
    int  moveCount = 0;         // operation count
    bool settingsOpen = false;  // options panel
    // option toggles (persisted in tm.ini)
    bool showCount = true;      // show count
    bool showTime = true;       // show main time
    bool showIps = true;        // show IPS
    bool showMilestones = true; // show milestone slider bar
    // milestones: first time 2^k was merged (ms); 0=not achieved. index = log2(tile value)
    int  milestoneMs[32] = { 0 };
    int  maxPow = 1;            // highest achieved power (milestone 32 needs >=5)
    // IPS stats (operations per second)
    int   ipsWindowMoves = 0;
    double ipsWindowStart = 0.0;
    float ips = 0.f;
    // hit rects
    sf::FloatRect settingsRect;
    sf::FloatRect optCount, optTime, optIps, optMile;  // options panel toggles
    sf::FloatRect btnClose;                            // options panel close
    sf::FloatRect mileRect;                            // milestone slider bar
    sf::FloatRect mileIpsRect;                         // IPS small rect hit area
    int  mileHover = -1;                               // slider hover item
    bool mileDrag = false;                             // slider dragging
    float mileScroll = 0.f;                            // slider scroll offset
    float mileItemH = 0.f;                             // slider item height (set while drawing)
    int   mileVis = 0;                                 // visible slider items (set while drawing)
    float mileListTopOff = 0.f;                        // slider list top offset (title row; set while drawing)
    // window auto-widen (when milestones are shown)
    unsigned int origW = 0, origH = 0;
    // ===== "Save?" confirm dialog on cancel/close (persistent timer-mode restore) =====
    bool confirmClose = false;    // show the "save?" confirm dialog (timing stops meanwhile)
    sf::FloatRect confirmYesBtn, confirmNoBtn;   // yes (save) / no (discard) buttons
    int  boardSize = 0;           // timer-mode board size (for persistent restore)
    bool restoreRequested = false;// loadTimerSettings saw Active=1 -> needs restore
    // per-board-size independent accumulated time/count (saved/restored on size switch, never reset across sizes)
    double sizeElapsed[3] = { 0.0, 0.0, 0.0 };   // 3×3 / 4×4 / 5×5
    int    sizeMove[3] = { 0, 0, 0 };
} g_tm;

// Timer-mode helper: reset session state (board untouched)
static void tmResetState()
{
    g_tm.started = false;
    g_tm.finished = false;
    g_tm.resultDismissed = false;
    g_tm.elapsedMs = 0.0;
    g_tm.moveCount = 0;
    g_tm.clock.restart();
    std::memset(g_tm.milestoneMs, 0, sizeof(g_tm.milestoneMs));
    g_tm.maxPow = 1;
    g_tm.mileScroll = 0.f;
    g_tm.mileHover = -1;
    g_tm.ipsWindowMoves = 0;
    g_tm.ipsWindowStart = 0.0;
    g_tm.ips = 0.f;
}

// Timer mode: milestone time format MM:SS.mmm (minutes are NOT mod 60; supports up to 10000 minutes)
static std::string tmFormatTime(double ms)
{
    int total = (int)(ms + 0.5);
    int mm = total / 60000;
    int ss = (total / 1000) % 60;
    int mmm = total % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", mm, ss, mmm);
    return buf;
}

// Timer-mode milestone cap (2^k tile): 3x3->2^10(1024), 4x4->2^17(131072), 5x5->2^26
// (defined at drawTimerMilestones; forward-declared here for early callers)
static int tmMaxMilestonePow(int sz);

// Record the first time a 2^k tile was merged (recorded on first appearance).
// elapsedMs is the current accumulated-time source (timer mode = main time; AI mode = AI operation time).
// Auto-scroll: when all 10 visible milestones are achieved and an 11th appears, scroll up one slot
// so the new bigger tile lands at the list bottom (instead of jumping to the top).
static void tmRecordMilestones(const Game2048& game, double elapsedMs)
{
    if (g_tm.finished) return;
    const int sz = game.getBoardSize();
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
        {
            int v = game.getTile(i, j);
            if (v <= 1) continue;
            int k = 0, t = v;
            while (t > 1) { t >>= 1; ++k; }
            if (k >= 5 && k <= tmMaxMilestonePow(sz) && g_tm.milestoneMs[k] == 0)
            {
                g_tm.milestoneMs[k] = (int)elapsedMs;
                // let the new milestone appear at the bottom of the visible area (auto-scroll up one slot)
                int idx = k - 5;                     // list index (k=5->0)
                float totalItems = (float)(tmMaxMilestonePow(sz) - 4); // k=5..kMax
                float vis = g_tm.mileVis > 0 ? (float)g_tm.mileVis : 10.f;
                float maxScroll = std::max(0.f, totalItems - vis);
                float targetScroll = (float)(idx - vis + 1);   // place the new item at the bottom
                g_tm.mileScroll = std::min(std::max(0.f, targetScroll), maxScroll);
            }
            if (k > g_tm.maxPow && k <= tmMaxMilestonePow(sz)) g_tm.maxPow = k;
        }
}

// Timer mode: scan the board and record milestones (main time)
static void tmCheckMilestones(const Game2048& game)
{
    if (!g_tm.active) return;
    tmRecordMilestones(game, g_tm.elapsedMs);
}

// AI mode: scan the board and record milestones (AI operation time)
static void aiRecordMilestones(const Game2048& game)
{
    if (g_aiMode == AI_INACTIVE) return;
    tmRecordMilestones(game, g_aiElapsedMs);
}

// Reset milestone state (new session: clear records, back to list top)
static void resetMilestoneState()
{
    std::memset(g_tm.milestoneMs, 0, sizeof(g_tm.milestoneMs));
    g_tm.maxPow = 1;
    g_tm.mileScroll = 0.f;
    g_tm.mileHover = -1;
}

// Whether the milestone panel should show (timer mode or AI session, and not replaying)
// (defined after computeBoardPx in the layout section; forward-declared for early use)
static bool milestoneVisible();

// Sync window width: with milestones visible (timer/AI mode), widen the window just enough to fit
// "board + gap + milestone panel (+ min side margins)"; restored to the previous (possibly narrower) width when the session ends.
// (defined after computeBoardPx in the layout section; called every frame in the main loop; forward-declared for tmStart)
static void tmApplyWindowWidth(sf::RenderWindow& window);
// Forward declaration: timer-mode helpers and board switching (defined in the timer/switch section below)
static void saveTimerSettings();
static void switchBoardSize(Game2048& game, int newSize);
static void tmWriteFullSave(Game2048& game);
static void tmDeleteSave(int size);

// Start timer mode: clear the board, start timing (no history written)
static void tmStart(sf::RenderWindow& window, Game2048& game)
{
    g_tm.active = true;
    g_tm.confirmClose = false;
    g_tm.boardSize = game.getBoardSize();
    tmResetState();
    g_tm.settingsOpen = false;
    game.resetNoHistory();
    game.clearAiAssisted();   // a new timed game is user-played; do not inherit the previous AI-session tag
    g_aiCacheReset = 1;   // new game: fully reset the AI engine in background against cross-game degradation
    // with milestones shown, auto-widen the window so board+slider become one unit (same idea as fixed prediction)
    g_tm.origW = window.getSize().x;
    g_tm.origH = window.getSize().y;
    tmApplyWindowWidth(window);
    logMsg("Timer mode started");
}

// Close timer mode (mid-way close does not record history): restore window width; persist Active=0 (no accidental restore);
// and save a "normal initial board" to the main save so the next launch shows a normal board (not the timed one)
static void tmClose(sf::RenderWindow& window, Game2048& game)
{
    if (!g_tm.active) return;
    g_tm.active = false;
    g_tm.settingsOpen = false;
    g_tm.confirmClose = false;
    saveTimerSettings();   // record Active=0
    tmResetState();
    if (g_tm.origW > 0 && g_tm.origH > 0)
        window.setSize(sf::Vector2u(g_tm.origW, g_tm.origH));
    game.resetNoHistory();
    game.saveToFile(SAVE_FILE);   // persist the normal initial board (restart no longer shows the timed board)
    logMsg("Timer mode closed");
}

// Request closing timer mode: with any board progress (played or restored) -> "save?" confirm dialog (timing stops); otherwise close directly
static void tmRequestClose(sf::RenderWindow& window, Game2048& game)
{
    if (!g_tm.active) return;
    if (!g_tm.finished && (g_tm.moveCount > 0 || game.getScore() > 0))
    {
        g_tm.confirmClose = true;   // stop timing (accumulation requires !confirmClose) + dialog
        g_tm.settingsOpen = false;
    }
    else
    {
        tmClose(window, game);
    }
}

// Timer-mode "save?" confirm decision
//   isYes=true  -> full independent save (board + time/count/milestones) -> exit (next "timer mode" click restores)
//   isYes=false -> delete that size's save -> exit (next time starts fresh)
//   Both exit timer mode and restore a normal board (Active=0; startup does not auto-enter timer mode).
//   Note: only "direct close of the window (X) while in timer mode" writes Active=1 (auto-restore timer mode at startup).
static void tmConfirmCloseDecide(sf::RenderWindow& window, Game2048& game, bool isYes)
{
    if (!g_tm.confirmClose) return;
    g_tm.confirmClose = false;
    if (isYes)
    {
        tmWriteFullSave(game);   // full independent save (board + time/count/milestones)
        logMsg("Timer mode saved (full)");
    }
    else
    {
        tmDeleteSave(game.getBoardSize());   // no save: delete that size's old save
    }
    tmClose(window, game);   // Active=0, restore a normal board (next launch shows a normal board)
}

// ========== Timer-mode option persistence (tm.ini) ==========
// Besides display toggles, persists the "timer-mode session state" (Active/timing/count/board size),
// so reopening after closing the software restores timer mode and continues timing instantly.
static void saveTimerSettings()
{
    std::ofstream ofs(pathInExeDir("tm.ini"), std::ios::trunc);
    if (!ofs) return;
    ofs << "[Timer]\r\n";
    ofs << "ShowCount=" << (g_tm.showCount ? 1 : 0) << "\r\n";
    ofs << "ShowTime=" << (g_tm.showTime ? 1 : 0) << "\r\n";
    ofs << "ShowIPS=" << (g_tm.showIps ? 1 : 0) << "\r\n";
    ofs << "ShowMilestones=" << (g_tm.showMilestones ? 1 : 0) << "\r\n";
    // session state persistence (restore after restart)
    ofs << "Active=" << (g_tm.active ? 1 : 0) << "\r\n";
    ofs << "Started=" << (g_tm.started ? 1 : 0) << "\r\n";
    ofs << "ElapsedMs=" << (long long)g_tm.elapsedMs << "\r\n";
    ofs << "MoveCount=" << g_tm.moveCount << "\r\n";
    ofs << "BoardSize=" << g_tm.boardSize << "\r\n";
    ofs.close();
    logMsg("timer settings saved");
}

static void loadTimerSettings()
{
    g_tm.showCount = true; g_tm.showTime = true; g_tm.showIps = true; g_tm.showMilestones = true;
    g_tm.restoreRequested = false;
    std::ifstream ifs(pathInExeDir("tm.ini"));
    if (!ifs) return;
    std::string line;
    bool inSection = false;
    int active = 0;
    while (std::getline(ifs, line))
    {
        std::string t = line;
        // trim whitespace
        while (!t.empty() && (t[0] == ' ' || t[0] == '\r' || t[0] == '\n')) t.erase(0, 1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\n')) t.pop_back();
        if (t.empty()) continue;
        if (t[0] == '[') { inSection = (t == "[Timer]"); continue; }
        if (!inSection) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq), val = t.substr(eq + 1);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        int v = 0;
        try { v = std::stoi(val); } catch (...) {}
        if (key == "showcount") g_tm.showCount = (v != 0);
        else if (key == "showtime") g_tm.showTime = (v != 0);
        else if (key == "showips") g_tm.showIps = (v != 0);
        else if (key == "showmilestones") g_tm.showMilestones = (v != 0);
        else if (key == "active") active = v;
        else if (key == "started") g_tm.started = (v != 0);
        else if (key == "elapsedms") g_tm.elapsedMs = (double)v;
        else if (key == "movecount") g_tm.moveCount = v;
        else if (key == "boardsize") g_tm.boardSize = v;
    }
    if (active != 0) g_tm.restoreRequested = true;   // needs timer-mode session restore
    logMsg("timer settings loaded");
}

// Whether this board size has a timer-mode independent save (for "click timer mode again" restore)
static bool tmBoardSaveExists(int size)
{
    std::ifstream ifs(tmBoardFileFor(size));
    return ifs.good();
}

// ===== Timer-mode independent save = board (tm_board_N.dat) + meta (tm_meta_N.dat: time/count/milestones) =====
static std::string tmMetaFileFor(int size)
{
    static std::string p3 = pathInExeDir("tm_meta_3.dat");
    static std::string p4 = pathInExeDir("tm_meta_4.dat");
    static std::string p5 = pathInExeDir("tm_meta_5.dat");
    switch (size) { case 3: return p3; case 5: return p5; default: return p4; }
}
static bool tmMetaExists(int size) { std::ifstream ifs(tmMetaFileFor(size)); return ifs.good(); }

// Save the current timer meta (time/count/milestones/max power) to this size's meta file
static void tmSaveMeta(int size)
{
    std::ofstream ofs(tmMetaFileFor(size), std::ios::trunc);
    if (!ofs) return;
    ofs << g_tm.elapsedMs << "\n" << g_tm.moveCount << "\n" << g_tm.maxPow << "\n";
    for (int i = 0; i < 32; ++i) ofs << g_tm.milestoneMs[i] << "\n";
}

// Read time/count/milestones back from the meta file (false on failure)
static bool tmLoadMeta(int size)
{
    std::ifstream ifs(tmMetaFileFor(size));
    if (!ifs) return false;
    double e; int mc, mp;
    if (!(ifs >> e >> mc >> mp)) return false;
    g_tm.elapsedMs = e; g_tm.moveCount = mc; g_tm.maxPow = mp;
    for (int i = 0; i < 32; ++i) { int v; ifs >> v; g_tm.milestoneMs[i] = v; }
    return true;
}

// Delete one size's timer independent save (board + meta)
static void tmDeleteSave(int size)
{
    std::remove(tmBoardFileFor(size));
    std::remove(tmMetaFileFor(size).c_str());
}

// Save the current timed game's full independent save (board + meta)
static void tmWriteFullSave(Game2048& game)
{
    g_tm.boardSize = game.getBoardSize();
    game.saveToFile(tmBoardFileFor(g_tm.boardSize));
    tmSaveMeta(g_tm.boardSize);
}

// Restore the timer-mode session ("direct close while in timer mode" -> Active=1, auto-restored at startup):
// re-enter timer mode, restore the board (independent save) and time/count/milestones (meta file); timing starts on the first move.
static void tmRestoreSession(sf::RenderWindow& window, Game2048& game)
{
    loadTimerSettings();   // read Active -> restoreRequested
    if (!g_tm.restoreRequested) return;
    g_tm.restoreRequested = false;
    const int sz = g_tm.boardSize > 0 ? g_tm.boardSize : game.getBoardSize();
    if (sz >= MIN_BOARD_SIZE && sz <= MAX_BOARD_SIZE && game.getBoardSize() != sz)
    {
        switchBoardSize(game, sz);
    }
    g_tm.active = true;
    g_tm.settingsOpen = false;
    g_tm.confirmClose = false;
    g_tm.origW = window.getSize().x;
    g_tm.origH = window.getSize().y;
    // restore board + time/count/milestones (that size's independent save)
    game.loadFromFile(tmBoardFileFor(sz));
    tmLoadMeta(sz);
    // do not time yet: after the user's first move (arrow key/mouse slide), keep accumulating from the current time
    g_tm.started = false;
    tmApplyWindowWidth(window);
    logMsg("Timer mode session restored: elapsed=" + std::to_string((long long)g_tm.elapsedMs) +
           "ms moves=" + std::to_string(g_tm.moveCount));
}

// When the user clicks "timer mode" again and a full save exists for this size -> restore progress/time/count (timing starts on the first move).
// Returns whether restore succeeded; on failure the caller starts fresh.
static bool tmResumeFromSave(sf::RenderWindow& window, Game2048& game)
{
    const int sz = game.getBoardSize();
    if (!tmBoardSaveExists(sz) || !tmMetaExists(sz)) return false;
    g_tm.boardSize = sz;
    game.loadFromFile(tmBoardFileFor(sz));   // restore board
    tmLoadMeta(sz);                          // restore time/count/milestones
    g_tm.active = true;
    g_tm.settingsOpen = false;
    g_tm.confirmClose = false;
    g_tm.origW = window.getSize().x;
    g_tm.origH = window.getSize().y;
    g_tm.started = false;   // timing starts on the first move
    tmApplyWindowWidth(window);
    logMsg("Timer mode resumed from save: elapsed=" + std::to_string((long long)g_tm.elapsedMs) +
           "ms moves=" + std::to_string(g_tm.moveCount));
    return true;
}

// Menu "timer mode" button hit rect (set while drawing, used on interaction)
static sf::FloatRect g_menuTimerBtn;

// Forward declaration (defined after drawGame)
static void drawTimerMilestones(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                float boardX, float boardY, float boardPx, const SchemeData& scheme,
                                float scoreBoxH, float gapBtn, float statsRowY, float closeY);

// ========== Challenge 2048 helpers ==========
// Reset challenge session state (board untouched)
static void chResetState()
{
    g_ch.started = false;
    g_ch.finished = false;
    g_ch.won = false;
    g_ch.elapsedMs = 0.0;
    g_ch.moveCount = 0;
    g_ch.clock.restart();
    g_ch.confirmSwitchProject = -1;
}

// Start / reset the current project challenge: clear the board, start the timer (no history written)
static void chStart(Game2048& game)
{
    g_ch.active = true;
    chResetState();
    g_ch.settingsOpen = false;
    const ChallengeProject& proj = g_chProjects[g_ch.project];
    if (game.getBoardSize() != proj.boardSize)
        game.setBoardSizeNoHistory(proj.boardSize);
    else
        game.resetNoHistory();
    g_aiCacheReset = 1;   // new game: fully reset the AI engine in background against cross-game degradation
    logMsg("Challenge started: " + std::to_string(proj.boardSize) + "x" + std::to_string(proj.boardSize) +
           " target=" + std::to_string(proj.target));
}

// Forward declaration: saveChallengeProject is defined after chSelectProject, but chSelectProject needs it
static void saveChallengeProject();

// Select a challenge project and restart
static void chSelectProject(Game2048& game, int idx)
{
    if (idx < 0 || idx >= 6) return;
    g_ch.project = idx;
    chStart(game);
    saveChallengeProject();   // remember the user choice, restored on next launch
}

// ========== Challenge project persistence (challenge.ini) ==========
// Remembers the last challenge project; no file on first install -> default 4x4 2048 (project=0)
static void saveChallengeProject()
{
    std::ofstream ofs(CHALLENGE_SETTINGS_FILE, std::ios::trunc);
    if (!ofs) {
        logMsg(std::string("ERROR: saveChallengeProject FAILED - cannot write: ") + CHALLENGE_SETTINGS_FILE);
        return;
    }
    ofs << "[Challenge]\r\n";
    ofs << "Project=" << g_ch.project << "\r\n";
    ofs.close();
    logMsg(std::string("challenge saved: Project=") + std::to_string(g_ch.project));
}

static void loadChallengeProject()
{
    g_ch.project = 0;   // first-install default: 4x4 2048
    std::ifstream ifs(CHALLENGE_SETTINGS_FILE);
    if (!ifs) {
        logMsg(std::string("challenge.ini not found - default Project=0 (4x4 2048)"));
        return;
    }
    int proj = 0;
    std::string line;
    bool inSection = false;
    while (std::getline(ifs, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        std::string t = line.substr(start, end - start + 1);
        if (t.empty()) continue;
        if (t[0] == '[') { inSection = (t == "[Challenge]"); continue; }
        if (!inSection) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq), val = t.substr(eq + 1);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        if (key == "project") {
            try { proj = std::stoi(val); } catch (...) {}
        }
    }
    if (proj < 0 || proj >= 6) proj = 0;
    g_ch.project = proj;
    logMsg(std::string("challenge loaded: Project=") + std::to_string(proj));
}

// Close the challenge (mid-way close does not record history)
static void chClose(Game2048& game)
{
    if (!g_ch.active) return;
    g_ch.active = false;
    g_ch.settingsOpen = false;
    chResetState();
    // clear the board to the initial empty state (normal mode)
    game.resetNoHistory();
    logMsg("Challenge closed without history");
}

// Challenge end (success/fail): stop timing; history is written only on success
static void chFinish(Game2048& game)
{
    if (g_ch.finished) return;
    g_ch.finished = true;
    g_ch.settingsOpen = false;
    // note: challenge timing is accumulated per frame in the main loop (g_ch.elapsedMs += dt); no clock here or the time doubles
    // only success writes history (saveHistory sets the challenge tag on the game side)
    if (g_ch.won)
        saveHistory(game);
    game.saveToFile(SAVE_FILE);
}

// Check the challenge result (called after every move): target reached -> success; no moves -> fail
static void chCheckFinish(Game2048& game)
{
    if (!g_ch.active || g_ch.finished) return;
    const ChallengeProject& proj = g_chProjects[g_ch.project];
    if (game.getMaxTile() >= proj.target)
    {
        g_ch.won = true;
        chFinish(game);
        return;
    }
    if (!game.canMove())
    {
        g_ch.won = false;
        chFinish(game);
    }
}

// Post-move challenge handling: first move starts timing, count moves, check the result
static void chAfterUserMove(Game2048& game)
{
    if (!g_ch.active || g_ch.finished) return;
    if (!g_ch.started)
    {
        g_ch.started = true;
        g_ch.clock.restart();
    }
    g_ch.moveCount++;
    chCheckFinish(game);
}

// Challenge project label, e.g. "4x4 2048"
static std::string chProjectLabel(int proj)
{
    if (proj < 0 || proj >= 6) return "";
    const ChallengeProject& p = g_chProjects[proj];
    return std::to_string(p.boardSize) + "x" + std::to_string(p.boardSize) + " " + std::to_string(p.target);
}

// Timer mode end (no moves -> game over): record history and save
static void tmFinish(Game2048& game)
{
    if (!g_tm.active || g_tm.finished) return;
    g_tm.finished = true;
    g_tm.resultDismissed = false;   // show this result dialog
    g_tm.settingsOpen = false;
    g_tm.ips = (float)((double)g_tm.moveCount / std::max(1.0, g_tm.elapsedMs / 1000.0));
    saveHistory(game);            // record history (a timed game ends)
    game.saveToFile(SAVE_FILE);   // persist
    logMsg("Timer mode finished (game over)");
}

// Post-move timer-mode handling: first move starts timing, count moves, update IPS and milestones; no moves -> game over
static void tmAfterUserMove(Game2048& game)
{
    if (!g_tm.active || g_tm.finished) return;
    if (!g_tm.started)
    {
        g_tm.started = true;
        g_tm.clock.restart();
        g_tm.ipsWindowStart = 0.0;
        g_tm.ipsWindowMoves = 0;
    }
    g_tm.moveCount++;
    g_tm.ipsWindowMoves++;
    tmCheckMilestones(game);
    // no moves -> game over: show the end dialog and record history
    if (!game.canMove()) tmFinish(game);
}

// Challenge/timer time formatting: MM:SS.mmm (minutes are NOT mod 60; supports up to 10000 minutes)
static std::string chFormatTime(double ms)
{
    int total = (int)(ms + 0.5);
    int mm = total / 60000;
    int ss = (total / 1000) % 60;
    int mmm = total % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", mm, ss, mmm);
    return std::string(buf);
}

// ======= Board switching: each board keeps independent progress =======
// User need: freely switch 3x3/4x4/5x5 without losing scores; only "restart" clears the current board
// Implementation: each board size has its own slot file 2048slot_<size>.dat
//   Switch: save current board -> load the target size's slot (fresh if none)
//   The main save 2048save.dat always mirrors the active board, so startup restores "where you left off"
static void switchBoardSize(Game2048& game, int newSize)
{
    if (newSize < MIN_BOARD_SIZE) newSize = MIN_BOARD_SIZE;
    if (newSize > MAX_BOARD_SIZE) newSize = MAX_BOARD_SIZE;
    int curSize = game.getBoardSize();
    if (curSize == newSize) return;

    // stop AI first if running (avoid AI continuing on the wrong board after switching)
    if (g_aiMode != AI_INACTIVE) {
        g_aiMode = AI_INACTIVE;   // set inactive directly, no "AI ended" dialog (user-initiated switch)
        g_aiSettingsOpen = false;
        g_showAIEnd = false;
        game.setAIModeAnimations(false);
    }

    // timer-mode mid-way size switch: first auto-save the current timed game fully (progress/time), then switch
    if (g_tm.active)
    {
        g_tm.sizeElapsed[curSize - 3] = g_tm.elapsedMs;   // save this size's time/count (restored when switching back)
        g_tm.sizeMove[curSize - 3] = g_tm.moveCount;
        tmWriteFullSave(game);   // full independent save (board + time/count/milestones)
        saveTimerSettings();
        tmResetState();
        g_tm.settingsOpen = false;
        logMsg("Timer mode auto-saved on board size switch");
    }

    // 0) save the current theme (all boards share one theme; not slot-scoped)
    int savedScheme = game.getColorScheme();

    // 1) save the current board to its slot (keep progress)
    game.saveToFile(slotFileFor(curSize));
    logMsg("Board slot saved: " + std::to_string(curSize) + "x" + std::to_string(curSize) +
           " score=" + std::to_string(game.getScore()));

    // 2) switch board size (setBoardSize inits and clears the board)
    game.setBoardSize(newSize);

    // 3) try loading the target board: in timer mode, prefer that size's timer independent save (last progress); otherwise the normal slot;
    //    no save -> keep the post-init empty board (fresh game)
    bool loaded = false;
    if (g_tm.active) {
        loaded = game.loadFromFile(tmBoardFileFor(newSize));
        if (loaded)
            logMsg("Timer board restored: " + std::to_string(newSize) + "x" + std::to_string(newSize) +
                   " score=" + std::to_string(game.getScore()));
    }
    if (!loaded) {
        if (game.loadFromFile(slotFileFor(newSize))) {
            logMsg("Board slot restored: " + std::to_string(newSize) + "x" + std::to_string(newSize) +
                   " score=" + std::to_string(game.getScore()));
        } else {
            logMsg("Board slot empty, new game: " + std::to_string(newSize) + "x" + std::to_string(newSize));
        }
    }

    // timer mode: restore the target size's time/count (kept from last time; re-accumulates on the first move)
    if (g_tm.active && newSize >= 3 && newSize <= 5)
    {
        g_tm.elapsedMs = g_tm.sizeElapsed[newSize - 3];
        g_tm.moveCount = g_tm.sizeMove[newSize - 3];
        g_tm.started = false;
        g_tm.boardSize = newSize;
    }

    // restore the theme (a slot-stored theme never overrides the current global theme)
    game.setColorScheme(savedScheme);

    // 4) sync the main save (records the active board for next launch)
    game.saveToFile(SAVE_FILE);
}

// Restart: only clears the current board's slot; other sizes keep their progress
static void restartCurrentBoard(Game2048& game)
{
    // stop AI first if running
    if (g_aiMode != AI_INACTIVE) {
        g_aiMode = AI_INACTIVE;
        g_aiSettingsOpen = false;
        g_showAIEnd = false;
        game.setAIModeAnimations(false);
    }

    // timer-mode restart -> reset timer state (wait for the first move to re-time)
    if (g_tm.active)
    {
        tmResetState();
        g_tm.settingsOpen = false;
        logMsg("Timer mode reset on restart");
    }

    // clear the current board + score
    game.init();

    // new game: fully reset the AI engine in background (clear TT + restore defaults) against cross-game degradation
    g_aiCacheReset = 1;

    int curSize = game.getBoardSize();
    // overwrite the current board's slot (old progress cleared = "undo current score")
    game.saveToFile(slotFileFor(curSize));
    // sync the main save
    game.saveToFile(SAVE_FILE);
    logMsg("Restart current board: " + std::to_string(curSize) + "x" + std::to_string(curSize) +
           " (other boards retained)");
}

// ------- Pipelined AI step: background AI fully overlaps animation/STEP_DELAY; main thread never blocks -------
// Returns: true = consumed and moved (main loop should restart the STEP_DELAY clock)
//          false = skipped/not ready (main loop: poll again next frame in pending state)
//
// Consume policy (WASM design: main thread 100% non-blocking, never a synchronous fallback):
//   a. isAnimating=true -> return false immediately; let the animation run its 60ms
//   b. future.wait_for(0) == ready -> 0-block get() -> move -> kick next round -> true
//   c. future still running -> enter the s_aiConsumingPending per-frame poll state, restart clock, false
//   d. >250ms still not ready: NO synchronous fallback! (the previous sync getAllMoveScores call was the "not responding" root cause)
//      keep waiting on the background; >1500ms total -> consider the task stuck, force-restart prefetch; UI never blocks
static bool aiStep(Game2048& game)
{
    // 1. strictly yield during animation: these 80ms the main thread only runs updateAnimation/display
    if (game.isAnimating()) return false;

    if (g_aiBoardSize != game.getBoardSize())
    {
        g_aiBoardSize = game.getBoardSize();
        g_aiEngine.setBoardSize(g_aiBoardSize);
        if (g_prefetchFuture.valid()) { try { (void)g_prefetchFuture.get(); } catch (...) {} }
    }

    // === Dead-end detection: when AI cannot move, undo once and recompute ===
    // User logic: run the official algorithm as long as there is no dead end; on a dead end, undo once -> recompute -> continue
    // At most 12 consecutive undos, otherwise the position is hopeless and AI ends
    if (!game.canMove()) {
        if (g_aiUseUndo && game.canUndo() && g_aiUndoConsecutive < 20) {
            // on retry, skip the step that caused the dead end -> the undo is a real "second chance", no spinning
            g_aiSkipDir = g_aiLastMoveDir;
            game.undo();
            // do not increment immediately: count after a completed "undo + following move"
            g_aiUndoPending = true;
            markSaveDirty();
            logMsg("[AI] Deadlock → undo pending (will count after next move) #" + std::to_string(g_aiUndoConsecutive + 1) + "/20, re-search");
            const auto undoBoard = buildAiBoard(game);
            aiLaunchPrefetch(undoBoard, g_aiBoardSize, true);
            g_aiStepClock.restart();
            return false;
        }
        // cannot undo, or >20 consecutive undos = truly over
        game.setAIModeAnimations(false);
        aiEndSession();
        return false;
    }

    const auto board = buildAiBoard(game);
    std::vector<SolverAI2048::MoveScore> rankedMoves;
    float evalBefore = 0.0f;
    bool usedPrefetch = false;
    // future wait timing: short poll 250ms -> keep waiting; long timeout 1500ms -> force-kill and restart, never a synchronous block
    static sf::Clock s_notReadyClock;
    static bool s_waiting = false;
    static std::string s_lastPrefetchStats;

    // === 2. prefer consuming the precomputed result (0-block path) ===
    if (g_prefetchFuture.valid()) {
        auto status = g_prefetchFuture.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            try {
                PrefetchResult r = g_prefetchFuture.get();
                if (r.ok && r.aiBoardSize == g_aiBoardSize && r.startBoard.size() == board.size()) {
                    bool match = true;
                    for (size_t rr = 0; rr < board.size() && match; ++rr) {
                        if (r.startBoard[rr].size() != board[rr].size()) { match = false; break; }
                        for (size_t c = 0; c < board[rr].size(); ++c) {
                            if (r.startBoard[rr][c] != board[rr][c]) { match = false; break; }
                        }
                    }
                    if (match) {
                        rankedMoves    = std::move(r.ranked);
                        evalBefore     = r.evalBefore;
                        usedPrefetch   = true;
                        s_waiting = false;
                        s_lastPrefetchStats = std::move(r.statsString);
                    }
                }
            } catch (...) { usedPrefetch = false; }
        } else {
            if (!s_waiting) { s_notReadyClock.restart(); s_waiting = true; }
        }
    }

    // === 3. not ready: pure polling, never a synchronous block ===
    if (!usedPrefetch && g_prefetchFuture.valid()) {
        const auto waitMs = s_notReadyClock.getElapsedTime().asMilliseconds();
        if (waitMs >= 1500) {
            // --- background task abnormally slow (possible 0 empties + OS scheduling): force discard and restart ---
            // note: NEVER call synchronous getAllMoveScores here! That caused "not responding"
            try {
                // try non-blocking once; if truly unfinished, leave it
                if (g_prefetchFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    PrefetchResult r = g_prefetchFuture.get();
                    if (r.ok && r.aiBoardSize == g_aiBoardSize) {
                        bool match = true;
                        for (size_t rr = 0; rr < board.size() && match; ++rr)
                            for (size_t c = 0; c < board[rr].size(); ++c)
                                if (r.startBoard[rr][c] != board[rr][c]) { match = false; break; }
                        if (match) {
                            rankedMoves = std::move(r.ranked);
                            evalBefore  = r.evalBefore;
                            usedPrefetch = true;
                            s_lastPrefetchStats = std::move(r.statsString);
                        }
                    }
                }
            } catch (...) {}
            if (!usedPrefetch) {
                // still not ready: force-overwrite the old future, start a new round of background compute
                s_waiting = false;
                aiLaunchPrefetch(board, g_aiBoardSize, true);  // force=true: force overwrite
                s_aiConsumingPending = true;
                g_aiStepClock.restart();
                return false;
            }
            s_waiting = false;
        } else {
            // --- key: mark pending + restart clock; the next frame (~16ms) checks again ---
            s_aiConsumingPending = true;
            g_aiStepClock.restart();
            return false;
        }
    }

    // === 4. no future running (invalid); start a background task, this round consumes nothing ===
    if (!usedPrefetch && !g_prefetchFuture.valid()) {
        aiLaunchPrefetch(board, g_aiBoardSize);
        s_waiting = false;
        s_aiConsumingPending = true;
        g_aiStepClock.restart();
        return false;
    }

    // usedPrefetch=true: stats are packed by the worker thread
    if (usedPrefetch) {
        if (!s_lastPrefetchStats.empty()) {
            logMsg(s_lastPrefetchStats);
            s_lastPrefetchStats.clear();
        }
    } else {
        ZigSolverAI2048& engine = solverForThisThread(g_aiBoardSize);
        const std::string s = engine.getStatsString();
        if (!s.empty()) logMsg(s);
    }
    if (rankedMoves.empty()) { game.setAIModeAnimations(false); aiEndSession(); return false; }

    // ===== ziap native: execute in ranked order =====
    bool moved = false;
    const int N = (int)rankedMoves.size();

    for (int i = 0; i < N; ++i) {
        // 4x4 Solver4x4: engine dir -> main-program order (mapped in the main dispatcher)
        const int dir = (g_aiBoardSize == 4) ? solver4ToCppDir(rankedMoves[i].dir) : rankedMoves[i].dir;
        if (g_aiSkipDir >= 0 && dir == g_aiSkipDir) continue;   // undo retry: skip the step that caused the dead end
        if (game.move(aiDirToDx(dir), aiDirToDy(dir))) {
            g_aiLastMoveDir = dir;  g_aiSkipDir = -1;
            game.addRandomTile(); markSaveDirty();
            if (g_practiceActive) { g_practiceLastMoveDir = dir; g_lastMoveByAI = true; }   // record AI's actual direction (for practice deviation)
            // if an undo was pending, this move completes an "undo+move" cycle: count it and clear the flag
            if (g_aiUndoPending) {
                g_aiUndoConsecutive++;
                g_aiUndoPending = false;
                    logMsg("[AI] Completed undo+move cycle #" + std::to_string(g_aiUndoConsecutive) + "/12");
            }
            g_aiMoveCount++;   // AI move count
            aiRecordMilestones(game);   // record first-merge milestones (AI operation time)
            moved = true;
            break;
        }
    }
    if (!moved) {
        const int dirs[4] = {0, 1, 2, 3};
        for (int d = 0; d < 4; ++d) {
            if (g_aiSkipDir >= 0 && dirs[d] == g_aiSkipDir) continue;   // undo retry: skip the dead-end step
            if (game.move(aiDirToDx(dirs[d]), aiDirToDy(dirs[d]))) {
                g_aiLastMoveDir = dirs[d];  g_aiSkipDir = -1;
                game.addRandomTile(); markSaveDirty();
                if (g_practiceActive) { g_practiceLastMoveDir = dirs[d]; g_lastMoveByAI = true; }   // record AI's actual direction (practice deviation)
                if (g_aiUndoPending) {
                    g_aiUndoConsecutive++;
                    g_aiUndoPending = false;
                    logMsg("[AI] Completed undo+move cycle #" + std::to_string(g_aiUndoConsecutive) + "/12");
                }
                g_aiMoveCount++;   // AI move count
                aiRecordMilestones(game);   // record first-merge milestones (AI operation time)
                moved = true;
                break;
            }
        }
    }

    // === immediately kick the next precompute; the 80ms animation runs in parallel ===
    if (moved && game.canMove()) {
        const auto nextBoard = buildAiBoard(game);
        aiLaunchPrefetch(nextBoard, g_aiBoardSize);
    }

    // === all directions dead-ended: undo once and recompute ===
    if (!moved && !game.canMove()) {
        if (g_aiUseUndo && game.canUndo()) {
            // on retry, skip the step that caused the dead end
            g_aiSkipDir = g_aiLastMoveDir;
            game.undo();
            // mark pending; the following move completes the cycle count
            g_aiUndoPending = true;
            markSaveDirty();
            logMsg("[AI] All-dir deadlock → undo pending (will count after next move)");
            const auto undoBoard = buildAiBoard(game);
            aiLaunchPrefetch(undoBoard, g_aiBoardSize, true);
            g_aiStepClock.restart();
            return false;
        }
        // cannot undo = truly over
        game.setAIModeAnimations(false);
        aiEndSession();
        return false;
    }
    return true;  // consumed: main loop restarts the STEP clock
}

// ========== History records ==========
static const char* const HISTORY_DIR = []() -> const char* {
    static std::string p = pathInExeDir("History Record");
    return p.c_str();
}();
static const int MAX_HISTORY = 100;

struct HistoryEntry {
    int boardSize = 0;
    int colorScheme = 0;
    int score = 0;
    int board[MAX_BOARD_SIZE][MAX_BOARD_SIZE];
    std::string filename;
    std::string timeInfo;      // top-bar time info: "HH : MM : SS    YYYY - MM - DD [AI assisted]"
    int year = 0, month = 0, day = 0;   // date (the list "topic" field is now used for time)
    bool aiAssisted = false;   // whether this run was completed with AI assistance
    // replay data (new format; legacy files with moveCount=0 are view-only)
    int moveCount = 0;                      // move count
    int totalMs = 0;                        // total time in ms
    std::vector<std::vector<int>> frames;   // board after each step (flattened n*n); [0]=initial
    std::vector<int> frameScores;           // score after each step; size = moveCount+1
    std::vector<int> frameTimesMs;          // cumulative ms after each step; size = moveCount+1
    std::vector<int> dirs;                  // move direction per step; size = moveCount
    std::vector<int> animMs;                // animation ms per step; size = moveCount (manual 120 / AI by rate)
    // challenge tag: 0=normal, 1=challenge success record
    int  challenge = 0;
    int  challengeProject = 0;   // challenge project 0..5
    int  challengeMs = 0;        // challenge operation time in ms
    bool imported = false;       // externally imported (.txt/.vrs replay) -> pinned to list top + marked "imported"
    std::string importedName;    // original source filename before import (path stripped), for list display
};

struct HistoryDialogState {
    bool open = false;
    int scrollPos = 0;
    int selectedIdx = -1;
    int hoverIdx = -1;
    float listAreaX = 0.f, listAreaY = 0.f, listAreaW = 0.f, listAreaH = 0.f;
    float itemH = 0.f;
    float closeBtnX = 0.f, closeBtnY = 0.f, closeBtnW = 0.f, closeBtnH = 0.f;
    // bottom function keys: best records / close
    float maxBtnX = 0.f, maxBtnY = 0.f, maxBtnW = 0.f, maxBtnH = 0.f;
    bool  filterMax = false;    // best-records filter: only show the highest score of each board size
    std::vector<HistoryEntry> entries;
    // entries indices tracking the best score per board size (-1 = no record for that size yet)
    //   index 0 = 3×3, index 1 = 4×4, index 2 = 5×5
    int maxScoreIdxBySize[3] = { -1, -1, -1 };
    // entries indices tracking the fastest challenge record per project (-1 = none yet) [6 projects]
    int maxChallengeIdxByProject[6] = { -1, -1, -1, -1, -1, -1 };
    // ---- v3.2.15 history dialog top dual keys: 0=history 1=import history ----
    int  tab = 0;                        // 0=history (own records) 1=import history
    float tabHistX = 0.f, tabHistY = 0.f, tabHistW = 0.f, tabHistH = 0.f;   // top "history" key
    float tabImpX = 0.f, tabImpY = 0.f, tabImpW = 0.f, tabImpH = 0.f;       // top "import history" key
    // ---- import history panel ----
    std::vector<std::string> importFiles;          // selected import files (one entry = one filename)
    int   importScroll = 0;                        // import list scroll
    float selX = 0.f, selY = 0.f, selW = 0.f, selH = 0.f;                  // "choose file" button
    float loadAllX = 0.f, loadAllY = 0.f, loadAllW = 0.f, loadAllH = 0.f;  // "load all" button
    float impListX = 0.f, impListY = 0.f, impListW = 0.f, impListH = 0.f;  // import entry list area
    float impItemSpacing = 0.f;                     // import entry spacing (incl. gap, for click/hover hit-test)
    float impItemTopOffset = 0.f;                   // import list first-item top offset
    float impItemH = 0.f;                           // import entry visual height
    bool  importing = false;                       // importing in progress (prevents double clicks)
    // ---- v3.2.19 history entry delete: small corner delete button + confirm dialog ----
    bool  confirmDelete = false;                   // whether the delete confirm dialog is shown
    int   confirmDeleteIdx = -1;                   // entry index to delete (entries index)
    int   hoverDelIdx = -1;                        // hovered delete button (visIdx)
    int   hoverExpIdx = -1;                        // hovered export button (visIdx)
    // confirm dialog button geometry (drawing and click hit-test share it)
    float delYesX = 0.f, delYesY = 0.f, delYesW = 0.f, delYesH = 0.f;   // "yes" normal key
    float delNoX  = 0.f, delNoY  = 0.f, delNoW  = 0.f, delNoH  = 0.f;   // "no" guided highlight (default/highlight)
};

static HistoryDialogState g_historyDlg;   // global history dialog state

// ========== History replay state ==========
// After clicking a history entry, a playback control area appears below the board (track + time row + 3 function keys)
// AI button aspect ratio (the playback function-key row height matches the AI button row)
static const float AI_BTN_W = 4.6f;   // AI button: width = height * 4.6
struct PlaybackState {
    bool active = false;        // whether in playback mode
    bool playing = false;       // whether playing
    int  curFrame = 0;          // current frozen frame index (for track/replay start)
    int  appliedStep = -1;      // step whose animation was applied (-1 = not applied yet)
    int  entryIdx = -1;         // corresponding history entries index
    int  stepMode = 0;          // 0=original step length, 1=fixed step length
    float stepMul = 1.f;        // original step-length multiplier (1x~10x)
    float fixedMs = 80.f;       // fixed step length in ms/step (40..500)
    bool settingsOpen = false;  // playback settings dialog
    bool trackDrag = false;     // dragging the progress track
    int  sliderDrag = -1;       // dragging a settings dialog slider: -1 none, 0 original step, 1 fixed step
    float playMs = 0.f;         // playback position in ms (advances over time)
    int   colorScheme = 0;      // theme used during playback (imported history = current user theme, otherwise saved theme)
    sf::FloatRect track = sf::FloatRect();        // progress track hit area
    sf::FloatRect btnPlay, btnSettings, btnCancel;
    sf::FloatRect timeCur, timeTotal;   // time rects (original step length only)
    // playback settings dialog hit areas
    sf::FloatRect settingsRect;
    sf::FloatRect btnOrig, btnFixed, btnCloseSettings;
    sf::FloatRect origTrack, fixedTrack;     // the two slider tracks
    float origKnobMinCX = 0.f, origKnobMaxCX = 0.f;
    float fixedKnobMinCX = 0.f, fixedKnobMaxCX = 0.f;
    float rowY = 0.f;           // playback area start Y
    float rowH = 0.f;           // row height
    // quality marker small rect geometry (shared by drawing and click hit-test)
    float mkHalfW = 0.f;        // marker small rect half width
    float mkHalfH = 0.f;        // marker small rect half height
    float mkCenterY = 0.f;      // marker small rect center Y (progress track center)
} g_pb;

// Playback-dedicated board instance (carries per-step slide/merge/spawn animations)
static Game2048 g_pbGame;

// ===== Practice imported-history review: per-step quality analysis (background thread) =====
// Quality: 0=perfect 1=fair 2=poor 3=bad
static void practiceQualityWorker(int sz, std::vector<std::vector<int>> frames, std::vector<int> dirs)
{
    const int n = (int)dirs.size();
    std::vector<int> q(n, 1);
    ZigSolverAI2048& engine = solverForThisThread(sz);
    for (int k = 0; k < n; ++k)
    {
        std::vector<std::vector<int>> b(sz, std::vector<int>(sz));
        if (k < (int)frames.size())
        {
            const auto& fr = frames[k];
            for (int i = 0; i < sz && i < (int)fr.size() / sz; ++i)
                for (int j = 0; j < sz; ++j)
                    b[i][j] = fr[i * sz + j];
        }
        // evaluate the four direction results (fast heuristic evaluation)
        float e[4] = { -1e18f, -1e18f, -1e18f, -1e18f };
        for (int d = 0; d < 4; ++d)
        {
            auto bd2 = b;
            if (moveBoardPure(bd2, d)) e[d] = engine.evaluateBoard(bd2);
        }
        float mx = -1e18f, mn = 1e18f;
        for (int d = 0; d < 4; ++d) { mx = std::max(mx, e[d]); mn = std::min(mn, e[d]); }
        const int ud = (k < (int)dirs.size()) ? dirs[k] : -1;
        if (ud >= 0 && ud < 4 && e[ud] >= mx - 1e-4f) { q[k] = 0; continue; }   // perfect (loss ~ 0)
        const float span = mx - mn;
        const float loss = (span > 1e-6f) ? (mx - e[ud]) / span : 0.f;
        // history-fit tag grading (by relative loss %): 0-15% green / 15-50% yellow-green / 50-85% yellow / >85% orange-red
        if (loss <= 0.15f)      q[k] = 0;   // green
        else if (loss <= 0.5f)  q[k] = 1;   // yellow-green
        else if (loss <= 0.85f) q[k] = 2;   // yellow
        else                    q[k] = 3;   // orange-red
    }
    {
        std::lock_guard<std::mutex> lk(g_practiceQualityMutex);
        g_practiceQuality = std::move(q);
        g_practiceQualityReady = true;
    }
}

// Start quality analysis for a history entry (passes a data copy, avoiding main-thread mutation races)
static void startPracticeQualityAnalysis(int entryIdx)
{
    if (entryIdx < 0 || entryIdx >= (int)g_historyDlg.entries.size()) return;
    const HistoryEntry& entry = g_historyDlg.entries[entryIdx];
    if (entry.moveCount <= 0) return;
    g_practiceQualityReady = false;
    g_practiceQuality.clear();
    const int sz = entry.boardSize;
    const std::vector<std::vector<int>> frames = entry.frames;   // copy
    const std::vector<int> dirs = entry.dirs;                     // copy
    g_practiceQualityFuture = std::async(std::launch::async, [sz, frames, dirs]() {
        practiceQualityWorker(sz, frames, dirs);
    });
}

// ========== History playback settings persistence (playback.ini) ==========
// Remembers the last step-length mode / multiplier / fixed step; default original step length 1x
static const char* const PB_SETTINGS_FILE = []() -> const char* {
    static std::string p = pathInExeDir("playback.ini");
    return p.c_str();
}();

static void savePbSettings()
{
    std::ofstream ofs(PB_SETTINGS_FILE, std::ios::trunc);
    if (!ofs) return;
    ofs << "[Playback]\r\n";
    ofs << "Mode=" << g_pb.stepMode << "\r\n";
    ofs << "Mul=" << (int)std::lround(g_pb.stepMul) << "\r\n";
    ofs << "FixedMs=" << (int)g_pb.fixedMs << "\r\n";
    ofs.close();
}

static void loadPbSettings()
{
    g_pb.stepMode = 0; g_pb.stepMul = 1.f; g_pb.fixedMs = 80.f;
    std::ifstream ifs(PB_SETTINGS_FILE);
    if (!ifs) return;
    int mode = 0, mul = 1, fixedMs = 80;
    std::string line;
    bool inSection = false;
    while (std::getline(ifs, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        std::string t = line.substr(start, end - start + 1);
        if (t.empty()) continue;
        if (t[0] == '[') { inSection = (t == "[Playback]"); continue; }
        if (!inSection) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq), val = t.substr(eq + 1);
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        for (auto& c : val) c = (char)tolower((unsigned char)c);
        if (key == "mode") { try { mode = std::stoi(val); } catch (...) {} }
        else if (key == "mul") { try { mul = std::stoi(val); } catch (...) {} }
        else if (key == "fixedms") { try { fixedMs = std::stoi(val); } catch (...) {} }
    }
    g_pb.stepMode = (mode == 1) ? 1 : 0;
    if (mul < 1) mul = 1; if (mul > 10) mul = 10;
    g_pb.stepMul = (float)mul;
    if (fixedMs < 40) fixedMs = 40; if (fixedMs > 500) fixedMs = 500;
    g_pb.fixedMs = (float)fixedMs;
}

// ms -> "h:m:s:ms"
static void formatTimeMs(int ms, char* buf, int bufSize)
{
    int hh = ms / 3600000, mm = (ms / 60000) % 60, ss = (ms / 1000) % 60, rms = ms % 1000;
    std::snprintf(buf, bufSize, "%d:%02d:%02d:%03d", hh, mm, ss, rms);
}

// Actual playback time of one step (ms): faithfully restores the recorded real step time; only guards against zero duration
static float pbStepDurMs(const HistoryEntry& entry, int k)
{
    float d = (float)(entry.frameTimesMs[k + 1] - entry.frameTimesMs[k]);
    if (d < 1.f) d = 1.f;
    return d;
}

// Playback animation time of one step: prefers the recorded real animation time; legacy files without records -> 120ms
static float pbStepAnimMs(const HistoryEntry& entry, int k)
{
    if (k >= 0 && k < (int)entry.animMs.size() && entry.animMs[k] > 0)
        return (float)entry.animMs[k];
    return 120.f;
}

// Forward declaration: playback total time (defined below, called by updatePlayback)
static float pbTotalMs(const HistoryEntry& entry);

// Playback update: advances the play position over time and applies real move animations step by step
static void updatePlayback(float dt, Game2048& mainGame)
{
    if (!g_pb.active) return;
    const HistoryEntry* entry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
        ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
    if (!entry || entry->moveCount <= 0 || (int)entry->frames.size() != entry->moveCount + 1)
    {
        g_pb.playing = false;
        return;
    }
    // always advance the playback board animation (let an in-progress slide finish naturally on pause/drag)
    g_pbGame.updateAnimations(dt);
    if (!g_pb.playing || g_pb.trackDrag || g_pb.settingsOpen) return;

    // play time advances (by time, not by step count)
    g_pb.playMs += (g_pb.stepMode == 0) ? dt * 1000.f * g_pb.stepMul : dt * 1000.f;

    // compute the current step k (by time)
    int moveCount = entry->moveCount;
    int k = 0;
    if (g_pb.stepMode == 0)
    {
        while (k < moveCount)
        {
            float stepEnd = entry->frameTimesMs[k] + pbStepDurMs(*entry, k);
            if (g_pb.playMs < stepEnd) break;
            k++;
        }
    }
    else
    {
        int steps = (int)(g_pb.playMs / g_pb.fixedMs);
        if (steps >= moveCount) { g_pb.playing = false; g_pb.playMs = pbTotalMs(*entry); return; }
        k = steps;
    }
    if (k >= moveCount) { g_pb.playing = false; g_pb.playMs = pbTotalMs(*entry); return; }

    // apply the next step's move animation (if not yet applied)
    if (g_pb.appliedStep != k)
    {
        g_pb.appliedStep = k;
        g_pb.curFrame = k;
        g_pbGame.setBoardFrame(entry->boardSize,
                               g_pb.colorScheme,
                               entry->frameScores[k], entry->frames[k]);
        // animation time: original/fixed step both cap the animation at 120ms; the rest waits in place
        float animMs = (g_pb.stepMode == 0) ? std::min(pbStepAnimMs(*entry, k), 120.f) : std::min(g_pb.fixedMs, 120.f);
        g_pbGame.setAIAnimDuration(animMs / 1000.f);
        g_pbGame.applyPlaybackMove((k < (int)entry->dirs.size()) ? entry->dirs[k] : 0,
                                   entry->frames[k + 1]);
    }
}

// Shared by the playback progress drag and the settings dialog sliders: update the slider position by X
static void pbApplySlider(int which, float x)
{
    if (which == 0)
    {
        float span = g_pb.origKnobMaxCX - g_pb.origKnobMinCX;
        float t = span > 0.f ? (x - g_pb.origKnobMinCX) / span : 0.f;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        int mul = 1 + (int)std::lround(t * 9.f);
        if (mul < 1) mul = 1;
        if (mul > 10) mul = 10;
        if ((int)g_pb.stepMul != mul)
        {
            g_pb.stepMul = (float)mul;
            savePbSettings();
        }
    }
    else if (which == 1)
    {
        float span = g_pb.fixedKnobMaxCX - g_pb.fixedKnobMinCX;
        float t = span > 0.f ? (x - g_pb.fixedKnobMinCX) / span : 0.f;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        int ms = 40 + (int)std::lround(t * (500.f - 40.f));
        if (ms < 40) ms = 40;
        if (ms > 500) ms = 500;
        if ((int)g_pb.fixedMs != ms)
        {
            g_pb.fixedMs = (float)ms;
            savePbSettings();
        }
    }
}

// Playback total time (ms): original step uses the recorded total; fixed step = steps x fixed step
static float pbTotalMs(const HistoryEntry& entry)
{
    if (g_pb.stepMode == 0)
        return std::max((float)entry.totalMs, 1.f);   // use recorded total so the track can reach 100%
    return (float)entry.moveCount * g_pb.fixedMs;
}

// Frozen frame index for a given play time
static int pbFrameAtMs(const HistoryEntry& entry, float ms)
{
    if (g_pb.stepMode == 0)
    {
        int k = 0;
        while (k < entry.moveCount)
        {
            float stepEnd = entry.frameTimesMs[k] + pbStepDurMs(entry, k);
            if (ms < stepEnd) break;
            k++;
        }
        return k;
    }
    int steps = (int)(ms / g_pb.fixedMs);
    if (steps >= entry.moveCount) return entry.moveCount;
    return steps;
}

// Freeze the playback board to the frame matching the given time (track drag / jump)
static void pbSettleAtMs(const HistoryEntry& entry, float ms)
{
    int f = pbFrameAtMs(entry, ms);
    g_pb.curFrame = f;
    g_pb.appliedStep = -1;   // when replaying, start the animation of the step after this frame
    g_pb.playMs = ms;
    g_pbGame.setBoardFrame(entry.boardSize, g_pb.colorScheme, entry.frameScores[f], entry.frames[f]);
    g_pbGame.finishAnimation();
}

// Play time of a given frame: fixed step = frame x fixed step; original step = recorded real time
static float pbFrameTimeMs(const HistoryEntry& entry, int f)
{
    if (g_pb.stepMode == 1)
        return (float)f * g_pb.fixedMs;
    if (f >= 0 && f < (int)entry.frameTimesMs.size())
        return (float)entry.frameTimesMs[f];
    return 0.f;
}

// History playback: previous step / next step (auto-pause on click, inspect frame by frame)
static void pbStepBy(const HistoryEntry& entry, int delta)
{
    if (entry.moveCount <= 0) return;
    int f = g_pb.curFrame + delta;
    if (f < 0) f = 0;
    if (f > entry.moveCount) f = entry.moveCount;
    g_pb.playing = false;   // clicking previous/next auto-pauses
    if (f == g_pb.curFrame) { g_pb.playMs = pbFrameTimeMs(entry, f); return; }
    g_pb.curFrame = f;
    g_pb.appliedStep = -1;   // continue from the animation of the step after this frame
    g_pb.playMs = pbFrameTimeMs(entry, f);
    g_pbGame.setBoardFrame(entry.boardSize, g_pb.colorScheme, entry.frameScores[f], entry.frames[f]);
    g_pbGame.finishAnimation();
}

// Draw the playback control area below the board (track + time row [with centered play key] + function-key row)
static void drawPlaybackControls(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                 float boardX, float boardY, float boardPx, float gapBtn,
                                 const SchemeData& scheme)
{
    const sf::Color activeBlue(70, 130, 180);
    const sf::Color dark(119, 110, 101);
    const HistoryEntry* entry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
        ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
    int moveCount = entry ? entry->moveCount : 0;

    float baseBtnH = (boardPx - 2.f * gapBtn) / (AI_BTN_W * 3.f);
    float pbBtnH = baseBtnH * 1.4f;
    float rowY = boardY + boardPx + gapBtn;

    float trackRowH = pbBtnH * 0.5f;
    float timeH     = pbBtnH * 0.42f * 1.6f;   // time rect height = 1.6x the original
    float timeRowH  = pbBtnH;                  // time row height = playback settings key height (fits an equal-height play key)
    float totalPlayMs = entry ? pbTotalMs(*entry) : 0.f;
    float t = totalPlayMs > 0.f ? (g_pb.playMs / totalPlayMs) : 0.f;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;

    // --- row 1: progress track (thinner; moves with "time") ---
    float trackH = std::max(2.f, pbBtnH * 0.12f);   // thinner
    float trackY = rowY + (trackRowH - trackH) / 2.f;
    // hit area is taller than the visual track (a thin track stays easy to click/drag)
    float trackHitPad = std::max(4.f, pbBtnH * 0.12f);
    g_pb.track = sf::FloatRect(boardX, trackY - trackHitPad, boardPx, trackH + trackHitPad * 2.f);
    {
        sf::RectangleShape bg(sf::Vector2f(boardPx, trackH));
        bg.setPosition(boardX, trackY);
        bg.setFillColor(sf::Color(119, 110, 101, 90));
        window.draw(bg);
        if (t > 0.f)
        {
            sf::RectangleShape fill(sf::Vector2f(boardPx * t, trackH));
            fill.setPosition(boardX, trackY);
            fill.setFillColor(activeBlue);
            window.draw(fill);
        }
    }

    // --- practice imported-history: per-step quality markers overlaid on the track (0 perfect/1 fair/2 poor/3 bad, all marked) ---
    if (g_practiceActive && g_practiceImport && entry && entry->moveCount > 0)
    {
        std::vector<int> qCopy;
        {
            std::lock_guard<std::mutex> lk(g_practiceQualityMutex);
            if (!g_practiceQualityReady || (int)g_practiceQuality.size() != entry->moveCount) { /* analysis not finished */ }
            else qCopy = g_practiceQuality;
        }
        if ((int)qCopy.size() == entry->moveCount)
        {
            const int n = entry->moveCount;
            // four-color rects: width = track length / total steps (evenly filling the whole track), height = 3/5 of the original
            float mkW = (n > 0) ? (boardPx / (float)n) : boardPx;
            float mkH = trackH * 2.8f * (2.f / 3.f) * (3.f / 5.f);
            g_pb.mkHalfW   = mkW / 2.f;
            g_pb.mkHalfH   = mkH / 2.f;
            g_pb.mkCenterY = trackY + trackH / 2.f;   // track center
            for (int k = 0; k < n; ++k)
            {
                int qv = qCopy[k];
                sf::RectangleShape mk(sf::Vector2f(mkW, mkH));
                mk.setPosition(boardX + (float)k * mkW, g_pb.mkCenterY - mkH / 2.f);
                mk.setFillColor(practiceQualityColor(qv));
                window.draw(mk);
            }
        }
    }

    // --- progress indicator (control rect): drawn last, above all layers such as the quality markers ---
    {
        float knobW = std::max(4.f, pbBtnH * 0.10f);
        float knobH = trackH * 3.2f;
        sf::RectangleShape knob(sf::Vector2f(knobW, knobH));
        knob.setFillColor(activeBlue);
        knob.setOrigin(knobW / 2.f, knobH / 2.f);
        knob.setPosition(boardX + boardPx * t, trackY + trackH / 2.f);
        window.draw(knob);
    }

    // --- row 2: two time rects + centered play key + settings/cancel at both ends of the play key ---
    // play key height:width = 1:1.618 (narrower, so side keys fit); settings/cancel are smaller, at both ends of the play key
    float playH = pbBtnH;                      // play key height = playback settings key height
    float playW = playH * 1.618f;               // height:width = 1:1.618
    float timeBoxW = boardPx * 0.25f;          // time rect length ~ 1/2 the original
    float timeY = rowY + trackRowH + gapBtn + (timeRowH - timeH) / 2.f;
    float playY = rowY + trackRowH + gapBtn + (timeRowH - playH) / 2.f;
    g_pb.timeCur   = sf::FloatRect(boardX, timeY, timeBoxW, timeH);
    g_pb.timeTotal = sf::FloatRect(boardX + boardPx - timeBoxW, timeY, timeBoxW, timeH);
    float playLeft = boardX + (boardPx - playW) / 2.f;
    g_pb.btnPlay   = sf::FloatRect(playLeft, playY, playW, playH);
    // settings / cancel: small keys at both ends of the play key
    float sideGap = gapBtn * 2.4f;
    float sideH   = playH * 0.66f;
    float sideW   = sideH * 1.618f;
    float sideY   = playY + (playH - sideH) / 2.f;
    g_pb.btnSettings = sf::FloatRect(playLeft - sideGap - sideW, sideY, sideW, sideH);
    g_pb.btnCancel   = sf::FloatRect(playLeft + playW + sideGap, sideY, sideW, sideH);

    unsigned int tf = (unsigned int)(timeH * 0.55f);   // reasonably larger font
    if (g_pb.stepMode == 0)
    {
        int curMs = (int)(g_pb.playMs < totalPlayMs ? g_pb.playMs : totalPlayMs);
        char b1[64], b2[64];
        formatTimeMs(curMs, b1, sizeof(b1));
        formatTimeMs(entry ? entry->totalMs : 0, b2, sizeof(b2));
        sf::RectangleShape bx1(sf::Vector2f(g_pb.timeCur.width, g_pb.timeCur.height));
        bx1.setPosition(g_pb.timeCur.left, g_pb.timeCur.top);
        bx1.setFillColor(scheme.boardBg);
        window.draw(bx1);
        drawTextFB(window, font, toSFString(b1),
                   g_pb.timeCur.left + g_pb.timeCur.width / 2, g_pb.timeCur.top + g_pb.timeCur.height / 2,
                   tf, dark, sf::Text::Bold, 1);
        sf::RectangleShape bx2(sf::Vector2f(g_pb.timeTotal.width, g_pb.timeTotal.height));
        bx2.setPosition(g_pb.timeTotal.left, g_pb.timeTotal.top);
        bx2.setFillColor(scheme.boardBg);
        window.draw(bx2);
        drawTextFB(window, font, toSFString(b2),
                   g_pb.timeTotal.left + g_pb.timeTotal.width / 2, g_pb.timeTotal.top + g_pb.timeTotal.height / 2,
                   tf, dark, sf::Text::Bold, 1);
    }

    // centered play/pause key (triangle / double bar, same style as the AI switch)
    {
        sf::RectangleShape b(sf::Vector2f(playW, playH));
        b.setPosition(g_pb.btnPlay.left, g_pb.btnPlay.top);
        b.setFillColor(g_pb.playing ? activeBlue : scheme.boardBg);
        window.draw(b);
        float cx = g_pb.btnPlay.left + playW / 2;
        float cy = g_pb.btnPlay.top + playH / 2;
        sf::Color icol = g_pb.playing ? sf::Color::White : dark;
        if (g_pb.playing)
        {
            float barW = playH * 0.14f, barH = playH * 0.40f, barGap = playH * 0.20f;
            sf::RectangleShape bar1(sf::Vector2f(barW, barH));
            bar1.setPosition(cx - barGap - barW / 2, cy - barH / 2); bar1.setFillColor(icol); window.draw(bar1);
            sf::RectangleShape bar2(sf::Vector2f(barW, barH));
            bar2.setPosition(cx + barGap - barW / 2, cy - barH / 2); bar2.setFillColor(icol); window.draw(bar2);
        }
        else
        {
            float halfH = playH * 0.26f, halfW = halfH * 0.90f;
            sf::ConvexShape tri(3);
            tri.setPoint(0, sf::Vector2f(cx + halfW, cy));
            tri.setPoint(1, sf::Vector2f(cx - halfW, cy - halfH));
            tri.setPoint(2, sf::Vector2f(cx - halfW, cy + halfH));
            tri.setFillColor(icol); window.draw(tri);
        }
    }

    // --- settings / cancel drawing (small, at both ends of the play key) ---
    unsigned int sideFont = (unsigned int)(sideH * 0.62f);
    {
        sf::RectangleShape b(sf::Vector2f(g_pb.btnSettings.width, g_pb.btnSettings.height));
        b.setPosition(g_pb.btnSettings.left, g_pb.btnSettings.top);
        b.setFillColor(g_pb.settingsOpen ? activeBlue : scheme.boardBg);
        window.draw(b);
        drawTextFB(window, font,
                   (g_language == LANG_ZH) ? toSFString("设置") : sf::String("Set"),
                   g_pb.btnSettings.left + g_pb.btnSettings.width / 2,
                   g_pb.btnSettings.top + g_pb.btnSettings.height / 2,
                   sideFont, g_pb.settingsOpen ? sf::Color::White : dark, 0, 1);
    }
    {
        sf::RectangleShape b(sf::Vector2f(g_pb.btnCancel.width, g_pb.btnCancel.height));
        b.setPosition(g_pb.btnCancel.left, g_pb.btnCancel.top);
        b.setFillColor(scheme.boardBg);
        window.draw(b);
        drawTextFB(window, font, tr("Cancel", "取消"),
                   g_pb.btnCancel.left + g_pb.btnCancel.width / 2,
                   g_pb.btnCancel.top + g_pb.btnCancel.height / 2,
                   sideFont, dark, 0, 1);
    }
}

// Playback settings dialog: height = 3/8 board height, width = board width, bottom aligned with the board bottom
// Row 1: original step / fixed step (font = menu button font); row 2: sliders (both blue, slightly shorter)
// No close key: click the blank area or "playback settings" again to close
static void drawPlaybackSettings(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                 float boardX, float boardY, float boardPx, const SchemeData& scheme)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const sf::Color activeBlue(70, 130, 180);
    float popW = boardPx;
    float popH = boardPx * 0.375f;
    float popX = boardX;
    float popY = boardY + boardPx - popH;
    g_pb.settingsRect = sf::FloatRect(popX, popY, popW, popH);

    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 160));
    window.draw(overlay);

    sf::RectangleShape popBg(sf::Vector2f(popW, popH));
    popBg.setPosition(popX, popY);
    popBg.setFillColor(scheme.windowBg);
    popBg.setOutlineColor(activeBlue);
    popBg.setOutlineThickness(3.f);
    window.draw(popBg);

    float pad = popW * 0.04f;
    float gap = popW * 0.04f;
    unsigned int fRow = (unsigned int)(boardPx * 0.045f);   // matches the menu button font

    // row 1: original step / fixed step (no close key), centered as a whole
    // practice imported-history review: only fixed step (only it is adjustable)
    float btnW = (popW - 2.f * pad - gap) / 2.f;
    float btnH = popH * 0.22f;
    float row1Y = popY + popH * 0.12f;
    float x0 = popX + (popW - btnW * 2.f - gap) / 2.f;
    g_pb.btnOrig          = sf::FloatRect(x0, row1Y, btnW, btnH);
    g_pb.btnFixed         = sf::FloatRect(x0 + btnW + gap, row1Y, btnW, btnH);
    g_pb.btnCloseSettings = sf::FloatRect(0.f, 0.f, 0.f, 0.f);   // close key removed

    auto drawModeBtn = [&](const sf::FloatRect& r, const char* zh, const char* en, bool sel) {
        sf::RectangleShape b(sf::Vector2f(r.width, r.height));
        b.setPosition(r.left, r.top);
        b.setFillColor(sel ? activeBlue : scheme.boardBg);
        window.draw(b);
        drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                   r.left + r.width / 2, r.top + r.height / 2, fRow,
                   sel ? sf::Color::White : scheme.textDark, 0, 1);
    };
    if (g_practiceActive && g_practiceImport)
    {
        g_pb.btnOrig = sf::FloatRect(0.f, 0.f, 0.f, 0.f);
        g_pb.btnFixed = sf::FloatRect(x0, row1Y, popW - 2.f * pad, btnH);
        drawModeBtn(g_pb.btnFixed, "固定步长", "Fixed", true);
    }
    else
    {
        drawModeBtn(g_pb.btnOrig,  "原始步长", "Original", g_pb.stepMode == 0);
        drawModeBtn(g_pb.btnFixed, "固定步长", "Fixed",    g_pb.stepMode == 1);
    }

    // row 2: slider (slightly shorter, leaving room for the right number area; all unified blue)
    float row2Y = popY + popH * 0.58f;
    float labelW = popW * 0.18f;                 // right number area width
    float trackX = popX + pad;
    float trackMaxX = popX + popW - pad - labelW;
    float trackW = trackMaxX - trackX;
    float trackH = std::max(3.f, popH * 0.055f * 0.75f);   // track 25% thinner than before
    float hitH = std::max(trackH * 3.f, popH * 0.18f);
    float knobR = popH * 0.075f;                           // knob = 75% of the original
    unsigned int fLabel = (unsigned int)(popH * 0.13f);

    if (g_pb.stepMode == 0)
    {
        g_pb.origTrack = sf::FloatRect(trackX, row2Y - hitH / 2.f, trackW, hitH);
        g_pb.origKnobMinCX = trackX + knobR;
        g_pb.origKnobMaxCX = trackMaxX - knobR;
        if (g_pb.origKnobMaxCX < g_pb.origKnobMinCX) g_pb.origKnobMaxCX = g_pb.origKnobMinCX;
        float segT = (g_pb.stepMul - 1.f) / 9.f;
        float knobCX = g_pb.origKnobMinCX + segT * (g_pb.origKnobMaxCX - g_pb.origKnobMinCX);
        sf::RectangleShape tr(sf::Vector2f(trackW, trackH));
        tr.setPosition(trackX, row2Y - trackH / 2.f);
        tr.setFillColor(sf::Color(119, 110, 101, 90));
        window.draw(tr);
        sf::RectangleShape fill(sf::Vector2f(std::max(0.f, knobCX - trackX), trackH));
        fill.setPosition(trackX, row2Y - trackH / 2.f);
        fill.setFillColor(activeBlue);
        window.draw(fill);
        for (int i = 1; i < 10; ++i)  // tick marks (1x~10x, 10 segments)
        {
            float x = trackX + trackW * i / 10.f;
            sf::RectangleShape seg(sf::Vector2f(1.5f, trackH));
            seg.setPosition(x, row2Y - trackH / 2.f);
            seg.setFillColor(scheme.windowBg);
            window.draw(seg);
        }
        sf::CircleShape knob(knobR);
        knob.setFillColor(activeBlue);
        knob.setOrigin(knobR, knobR);
        knob.setPosition(knobCX, row2Y);
        window.draw(knob);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0fx", g_pb.stepMul);
        drawTextFB(window, font, toSFString(buf), trackMaxX + knobR + pad * 0.5f, row2Y, fLabel, activeBlue, sf::Text::Bold, 0);
    }
    else
    {
        g_pb.fixedTrack = sf::FloatRect(trackX, row2Y - hitH / 2.f, trackW, hitH);
        g_pb.fixedKnobMinCX = trackX + knobR;
        g_pb.fixedKnobMaxCX = trackMaxX - knobR;
        if (g_pb.fixedKnobMaxCX < g_pb.fixedKnobMinCX) g_pb.fixedKnobMaxCX = g_pb.fixedKnobMinCX;
        float fT = (g_pb.fixedMs - 40.f) / (500.f - 40.f);
        float knobCX = g_pb.fixedKnobMinCX + fT * (g_pb.fixedKnobMaxCX - g_pb.fixedKnobMinCX);
        sf::RectangleShape tr(sf::Vector2f(trackW, trackH));
        tr.setPosition(trackX, row2Y - trackH / 2.f);
        tr.setFillColor(sf::Color(119, 110, 101, 90));
        window.draw(tr);
        sf::RectangleShape fill(sf::Vector2f(std::max(0.f, knobCX - trackX), trackH));
        fill.setPosition(trackX, row2Y - trackH / 2.f);
        fill.setFillColor(activeBlue);
        window.draw(fill);
        sf::CircleShape knob(knobR);
        knob.setFillColor(activeBlue);
        knob.setOrigin(knobR, knobR);
        knob.setPosition(knobCX, row2Y);
        window.draw(knob);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dms", (int)g_pb.fixedMs);
        drawTextFB(window, font, toSFString(buf), trackMaxX + knobR + pad * 0.5f, row2Y, fLabel, activeBlue, sf::Text::Bold, 0);
    }
}

// Exit playback mode
static void exitPlayback(Game2048& game)
{
    g_pb.active = false;
    g_pb.playing = false;
    g_pb.settingsOpen = false;
    g_pb.trackDrag = false;
    g_pb.sliderDrag = -1;
    g_pb.entryIdx = -1;
    g_pb.curFrame = 0;
    g_pb.appliedStep = -1;
    g_pb.playMs = 0.f;
    g_pbGame.finishAnimation();   // clear the playback board animation
    game.exitHistoryView();
}

// ========== Custom color helper functions ==========

// Get the color pointer of the current edit target from the custom scheme
static sf::Color* ccGetColor(int target)
{
    SchemeData& cs = g_ccDlg.open ? g_ccDlg.workingScheme : SCHEMES[CUSTOM_SCHEME_IDX];
    if (target == CEDIT_WINDOW_BG)     return &cs.windowBg;
    if (target == CEDIT_BOARD_BG)      return &cs.boardBg;
    if (target == CEDIT_EMPTY_COLOR)   return &cs.emptyColor;
    if (target == CEDIT_SCOREBOX_COLOR) return &cs.scoreBoxColor;
    if (target == CEDIT_TEXT_LIGHT)    return &cs.textLight;
    if (target == CEDIT_TEXT_DARK)     return &cs.textDark;
    if (target == CEDIT_SYSTEM_TEXT)   return &cs.textDark;   // system text follows the dark text color
    if (target >= CEDIT_TILE_COLOR_START)
    {
        int power = target - CEDIT_TILE_COLOR_START;
        if (power >= 1 && power <= MAX_POWER)
            return &cs.tileColors[power];
    }
    return nullptr;
}

// Get the display name of a target (UTF-8 string in the current language)
static std::string ccGetTargetName(int target)
{
    bool zh = (g_language == LANG_ZH);
    if (target == CEDIT_WINDOW_BG)     return zh ? "窗口背景"   : "Window BG";
    if (target == CEDIT_BOARD_BG)      return zh ? "棋盘背景"   : "Board BG";
    if (target == CEDIT_EMPTY_COLOR)   return zh ? "空格"       : "Empty Tile";
    if (target == CEDIT_SCOREBOX_COLOR) return zh ? "分数框"    : "Score Box";
    if (target == CEDIT_TEXT_LIGHT)    return zh ? "浅色文字"   : "Light Text";
    if (target == CEDIT_TEXT_DARK)     return zh ? "深色文字"   : "Dark Text";
    if (target == CEDIT_SYSTEM_TEXT)   return zh ? "系统文字"   : "System Text";
    if (target >= CEDIT_TILE_COLOR_START)
    {
        int power = target - CEDIT_TILE_COLOR_START;
        if (power >= 1 && power <= MAX_POWER)
        {
            std::ostringstream ss;
            ss << (1 << power);
            return zh ? ("方块: " + ss.str()) : ("Box: " + ss.str());
        }
    }
    return "?";
}

// Total color item count (7 global colors + 26 tile colors)
static const int CC_TOTAL_ITEMS = 7 + MAX_POWER;

// Convert a list index 0..CC_TOTAL_ITEMS-1 to an edit target
static int ccIndexToTarget(int listIdx)
{
    if (listIdx < 0) return CEDIT_NONE;
    if (listIdx == 0) return CEDIT_WINDOW_BG;
    if (listIdx == 1) return CEDIT_BOARD_BG;
    if (listIdx == 2) return CEDIT_EMPTY_COLOR;
    if (listIdx == 3) return CEDIT_SCOREBOX_COLOR;
    if (listIdx == 4) return CEDIT_TEXT_LIGHT;
    if (listIdx == 5) return CEDIT_TEXT_DARK;
    if (listIdx == 6) return CEDIT_SYSTEM_TEXT;
    if (listIdx >= 7 && listIdx < 7 + MAX_POWER)
        return CEDIT_TILE_COLOR_START + (listIdx - 6); // power 1..MAX_POWER
    return CEDIT_NONE;
}

// Copy the current Custom scheme content into a save slot
static void ccPresetFromCurrent(CustomPreset& p)
{
    const SchemeData& cs = SCHEMES[CUSTOM_SCHEME_IDX];
    p.used = true;
    std::memcpy(p.tileColors, cs.tileColors, sizeof(p.tileColors));
    p.emptyColor = cs.emptyColor;
    p.windowBg = cs.windowBg;
    p.boardBg = cs.boardBg;
    p.scoreBoxColor = cs.scoreBoxColor;
    p.textLight = cs.textLight;
    p.textDark = cs.textDark;
    p.lightTextFromValue = cs.lightTextFromValue;
}

// Load a save slot into the Custom scheme
static void ccPresetToCurrent(const CustomPreset& p)
{
    if (!p.used) return;
    SchemeData& cs = SCHEMES[CUSTOM_SCHEME_IDX];
    std::memcpy(cs.tileColors, p.tileColors, sizeof(cs.tileColors));
    cs.emptyColor = p.emptyColor;
    cs.windowBg = p.windowBg;
    cs.boardBg = p.boardBg;
    cs.scoreBoxColor = p.scoreBoxColor;
    cs.textLight = p.textLight;
    cs.textDark = p.textDark;
    cs.lightTextFromValue = p.lightTextFromValue;
}

// Save/load the custom theme file (custom_theme.dat): persists SCHEMES[CUSTOM_SCHEME_IDX]
//   SchemeData holds a const char* name pointer and cannot be serialized whole; only the data fields are written
static void saveCustomTheme()
{
    const SchemeData& s = SCHEMES[CUSTOM_SCHEME_IDX];
    std::ofstream ofs(CUSTOM_THEME_FILE, std::ios::binary);
    if (!ofs) {
        logMsg(std::string("ERROR: saveCustomTheme FAILED - cannot write: ") + CUSTOM_THEME_FILE);
        return;
    }
    ofs.write((const char*)s.tileColors, sizeof(s.tileColors));
    ofs.write((const char*)&s.emptyColor, sizeof(s.emptyColor));
    ofs.write((const char*)&s.windowBg, sizeof(s.windowBg));
    ofs.write((const char*)&s.boardBg, sizeof(s.boardBg));
    ofs.write((const char*)&s.scoreBoxColor, sizeof(s.scoreBoxColor));
    ofs.write((const char*)&s.textLight, sizeof(s.textLight));
    ofs.write((const char*)&s.textDark, sizeof(s.textDark));
    ofs.write((const char*)&s.lightTextFromValue, sizeof(s.lightTextFromValue));
    logMsg("custom theme saved");
}

static void loadCustomTheme()
{
    std::ifstream ifs(CUSTOM_THEME_FILE, std::ios::binary);
    if (!ifs) {
        logMsg(std::string("custom_theme.dat not found - use default custom scheme"));
        return;
    }
    SchemeData& s = SCHEMES[CUSTOM_SCHEME_IDX];
    ifs.read((char*)s.tileColors, sizeof(s.tileColors));
    ifs.read((char*)&s.emptyColor, sizeof(s.emptyColor));
    ifs.read((char*)&s.windowBg, sizeof(s.windowBg));
    ifs.read((char*)&s.boardBg, sizeof(s.boardBg));
    ifs.read((char*)&s.scoreBoxColor, sizeof(s.scoreBoxColor));
    ifs.read((char*)&s.textLight, sizeof(s.textLight));
    ifs.read((char*)&s.textDark, sizeof(s.textDark));
    ifs.read((char*)&s.lightTextFromValue, sizeof(s.lightTextFromValue));
    if (s.lightTextFromValue < 0 || s.lightTextFromValue > MAX_POWER)
        s.lightTextFromValue = MAX_POWER;
    s.name = "";
    logMsg("custom theme loaded");
}

// Save/load the custom presets file
static bool ccSavePresetsToFile()
{
    std::ofstream ofs(CUSTOM_PRESETS_FILE, std::ios::binary);
    if (!ofs) return false;
    for (int i = 0; i < MAX_CUSTOM_PRESETS; ++i)
    {
        const CustomPreset& p = g_ccDlg.presets[i];
        ofs.write((const char*)&p.used, sizeof(p.used));
        if (p.used)
        {
            ofs.write((const char*)p.tileColors, sizeof(p.tileColors));
            ofs.write((const char*)&p.emptyColor, sizeof(sf::Color));
            ofs.write((const char*)&p.windowBg, sizeof(sf::Color));
            ofs.write((const char*)&p.boardBg, sizeof(sf::Color));
            ofs.write((const char*)&p.scoreBoxColor, sizeof(sf::Color));
            ofs.write((const char*)&p.textLight, sizeof(sf::Color));
            ofs.write((const char*)&p.textDark, sizeof(sf::Color));
            ofs.write((const char*)&p.lightTextFromValue, sizeof(int));
        }
    }
    // also save the current Custom scheme content so it is restored on the next launch
    const SchemeData& cs = SCHEMES[CUSTOM_SCHEME_IDX];
    ofs.write((const char*)cs.tileColors, sizeof(cs.tileColors));
    ofs.write((const char*)&cs.emptyColor, sizeof(sf::Color));
    ofs.write((const char*)&cs.windowBg, sizeof(sf::Color));
    ofs.write((const char*)&cs.boardBg, sizeof(sf::Color));
    ofs.write((const char*)&cs.scoreBoxColor, sizeof(sf::Color));
    ofs.write((const char*)&cs.textLight, sizeof(sf::Color));
    ofs.write((const char*)&cs.textDark, sizeof(sf::Color));
    ofs.write((const char*)&cs.lightTextFromValue, sizeof(int));
    return true;
}

static void ccLoadPresetsFromFile()
{
    std::ifstream ifs(CUSTOM_PRESETS_FILE, std::ios::binary);
    if (!ifs) return;
    // zero out
    for (int i = 0; i < MAX_CUSTOM_PRESETS; ++i)
        g_ccDlg.presets[i].used = false;
    for (int i = 0; i < MAX_CUSTOM_PRESETS; ++i)
    {
        CustomPreset& p = g_ccDlg.presets[i];
        ifs.read((char*)&p.used, sizeof(p.used));
        if (ifs.eof()) break;
        if (p.used)
        {
            ifs.read((char*)p.tileColors, sizeof(p.tileColors));
            ifs.read((char*)&p.emptyColor, sizeof(sf::Color));
            ifs.read((char*)&p.windowBg, sizeof(sf::Color));
            ifs.read((char*)&p.boardBg, sizeof(sf::Color));
            ifs.read((char*)&p.scoreBoxColor, sizeof(sf::Color));
            ifs.read((char*)&p.textLight, sizeof(sf::Color));
            ifs.read((char*)&p.textDark, sizeof(sf::Color));
            ifs.read((char*)&p.lightTextFromValue, sizeof(int));
        }
    }
    // read the Custom scheme current content
    SchemeData& cs = SCHEMES[CUSTOM_SCHEME_IDX];
    if (ifs.good())
    {
        ifs.read((char*)cs.tileColors, sizeof(cs.tileColors));
        ifs.read((char*)&cs.emptyColor, sizeof(sf::Color));
        ifs.read((char*)&cs.windowBg, sizeof(sf::Color));
        ifs.read((char*)&cs.boardBg, sizeof(sf::Color));
        ifs.read((char*)&cs.scoreBoxColor, sizeof(sf::Color));
        ifs.read((char*)&cs.textLight, sizeof(sf::Color));
        ifs.read((char*)&cs.textDark, sizeof(sf::Color));
        ifs.read((char*)&cs.lightTextFromValue, sizeof(int));
    }
}

// Whether the save-slot list is full
static bool ccPresetsFull()
{
    for (int i = 0; i < MAX_CUSTOM_PRESETS; ++i)
        if (!g_ccDlg.presets[i].used) return false;
    return true;
}

// Find the first empty slot, return its index; -1 if none
static int ccFindEmptySlot()
{
    for (int i = 0; i < MAX_CUSTOM_PRESETS; ++i)
        if (!g_ccDlg.presets[i].used) return i;
    return -1;
}

// ========== Theme import/export ==========
// Export a theme to a user-chosen file (binary fields identical to saveCustomTheme).
// ansiPath comes from GetSaveFileNameA (system ANSI encoding); convert to wide chars and write with CreateFileW
// to avoid MSVC std::ofstream encoding problems with Chinese paths.
static bool ccExportThemeToPath(const std::string& ansiPath, const SchemeData& s)
{
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansiPath.c_str(), (int)ansiPath.size(), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansiPath.c_str(), (int)ansiPath.size(), &wpath[0], wlen);
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        logMsg(std::string("ccExportThemeToPath CreateFileW failed (err=")
               + std::to_string(GetLastError()) + "): " + ansiPath);
        return false;
    }
    bool ok = true;
    DWORD w = 0;
    ok = ok && (WriteFile(hFile, s.tileColors, (DWORD)sizeof(s.tileColors), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.emptyColor, (DWORD)sizeof(sf::Color), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.windowBg, (DWORD)sizeof(sf::Color), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.boardBg, (DWORD)sizeof(sf::Color), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.scoreBoxColor, (DWORD)sizeof(sf::Color), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.textLight, (DWORD)sizeof(sf::Color), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.textDark, (DWORD)sizeof(sf::Color), &w, NULL) != FALSE);
    ok = ok && (WriteFile(hFile, &s.lightTextFromValue, (DWORD)sizeof(int), &w, NULL) != FALSE);
    CloseHandle(hFile);
    return ok;
}

// Read a theme from a user-chosen file, return true on success
static bool ccImportThemeFromPath(const std::string& ansiPath, SchemeData& out)
{
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansiPath.c_str(), (int)ansiPath.size(), nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wpath(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansiPath.c_str(), (int)ansiPath.size(), &wpath[0], wlen);
    HANDLE hFile = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        logMsg(std::string("ccImportThemeFromPath CreateFileW failed (err=")
               + std::to_string(GetLastError()) + "): " + ansiPath);
        return false;
    }
    bool ok = true;
    DWORD rd = 0;
    ok = ok && (ReadFile(hFile, out.tileColors, (DWORD)sizeof(out.tileColors), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.emptyColor, (DWORD)sizeof(sf::Color), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.windowBg, (DWORD)sizeof(sf::Color), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.boardBg, (DWORD)sizeof(sf::Color), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.scoreBoxColor, (DWORD)sizeof(sf::Color), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.textLight, (DWORD)sizeof(sf::Color), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.textDark, (DWORD)sizeof(sf::Color), &rd, NULL) != FALSE);
    ok = ok && (ReadFile(hFile, &out.lightTextFromValue, (DWORD)sizeof(int), &rd, NULL) != FALSE);
    CloseHandle(hFile);
    if (out.lightTextFromValue < 0 || out.lightTextFromValue > MAX_POWER)
        out.lightTextFromValue = MAX_POWER;
    out.name = "";
    return ok;
}

// Save the current board to history
static void saveHistory(const Game2048& game)
{
    // check the board has content (at least one non-zero tile)
    bool hasContent = false;
    for (int i = 0; i < game.getBoardSize() && !hasContent; ++i)
        for (int j = 0; j < game.getBoardSize() && !hasContent; ++j)
            if (game.getTile(i, j) != 0) hasContent = true;
    if (!hasContent) return;

    CreateDirectoryA(HISTORY_DIR, NULL);

    // scan existing files, sort by modification time (compatible with legacy .bin and new .dat)
    WIN32_FIND_DATAA findData;
    std::vector<std::pair<std::string, FILETIME>> files;
    const char* patterns[] = { "\\*.dat", "\\*.bin" };
    for (int p = 0; p < 2; ++p)
    {
        std::string searchPath = std::string(HISTORY_DIR) + patterns[p];
        HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    std::string fname = findData.cFileName;
                    FILETIME ft = findData.ftLastWriteTime;
                    files.push_back({ fname, ft });
                }
            } while (FindNextFileA(hFind, &findData));
            FindClose(hFind);
        }
    }

    // delete the oldest when over the cap
    if ((int)files.size() >= MAX_HISTORY)
    {
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            return CompareFileTime(&a.second, &b.second) < 0;
        });
        int toDelete = (int)files.size() - MAX_HISTORY + 1;
        for (int i = 0; i < toDelete; ++i)
        {
            std::string fullPath = std::string(HISTORY_DIR) + "\\" + files[i].first;
            std::remove(fullPath.c_str());
        }
    }

    // generate filename: history_timestamp.dat
    time_t now = std::time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    char nameBuf[128];
    strftime(nameBuf, sizeof(nameBuf), "history_%Y%m%d_%H%M%S.dat", &t);
    std::string fullPath = std::string(HISTORY_DIR) + "\\" + nameBuf;

    // ===== legacy full format (v1, no magic): full board per frame -> thumbnails/playback work directly, most compatible =====
    //   header: boardSize + colorScheme + score + final board + aiAssisted + moveCount + totalMs
    //   frames: (moveCount+1) x [ n*n full board + score + time ]
    //   tail: moveCount x direction + moveCount x animation + challenge tag(+project/duration)
    std::ofstream ofs(fullPath, std::ios::binary);
    if (!ofs) return;
    int bs = game.getBoardSize();
    int cs = game.getColorScheme();
    int sc = game.getScore();
    bool ai = game.isAiAssisted();
    int moveCount = game.recMoveCount();
    int totalMs   = game.recTotalMs();
    int chMarker  = (g_ch.active && g_ch.won && g_ch.finished) ? 1 : 0;

    ofs.write((const char*)&bs, sizeof(int));
    ofs.write((const char*)&cs, sizeof(int));
    ofs.write((const char*)&sc, sizeof(int));
    // final board (current game board, keeps the list thumbnail correct)
    int boardFinal[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    for (int i = 0; i < bs; ++i)
        for (int j = 0; j < bs; ++j)
            boardFinal[i][j] = game.getTile(i, j);
    ofs.write((const char*)boardFinal, sizeof(boardFinal));
    ofs.write((const char*)&ai, sizeof(bool));
    ofs.write((const char*)&moveCount, sizeof(int));
    ofs.write((const char*)&totalMs, sizeof(int));
    // full board + score + time per frame
    for (int f = 0; f <= moveCount; ++f)
    {
        for (int i = 0; i < bs * bs; ++i)
        {
            int v = game.recFrameVal(f, i);
            ofs.write((const char*)&v, sizeof(int));
        }
        int s = game.recScore(f);
        int t = game.recTime(f);
        ofs.write((const char*)&s, sizeof(int));
        ofs.write((const char*)&t, sizeof(int));
    }
    // direction per step
    for (int k = 0; k < moveCount; ++k)
    {
        int d = game.recDir(k);
        ofs.write((const char*)&d, sizeof(int));
    }
    // animation time per step
    for (int k = 0; k < moveCount; ++k)
    {
        int am = game.recAnimMs(k);
        ofs.write((const char*)&am, sizeof(int));
    }
    // challenge tag
    ofs.write((const char*)&chMarker, sizeof(int));
    if (chMarker == 1)
    {
        int cp = g_ch.project;
        int cm = (int)(g_ch.elapsedMs + 0.5);
        ofs.write((const char*)&cp, sizeof(int));
        ofs.write((const char*)&cm, sizeof(int));
    }
}

// Load the history record list
static void loadHistoryList()
{
    g_historyDlg.entries.clear();
    g_historyDlg.maxScoreIdxBySize[0] = -1;  // 3x3
    g_historyDlg.maxScoreIdxBySize[1] = -1;  // 4x4
    g_historyDlg.maxScoreIdxBySize[2] = -1;  // 5x5
    for (int i = 0; i < 6; ++i) g_historyDlg.maxChallengeIdxByProject[i] = -1;

    WIN32_FIND_DATAA findData;
    std::vector<std::pair<std::string, FILETIME>> files;
    const char* patterns[] = { "\\*.dat", "\\*.bin" };
    for (int p = 0; p < 2; ++p)
    {
        std::string searchPath = std::string(HISTORY_DIR) + patterns[p];
        HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                std::string fname = findData.cFileName;
                FILETIME ft = findData.ftLastWriteTime;
                files.push_back({ fname, ft });
            }
        } while (FindNextFileA(hFind, &findData));
        FindClose(hFind);
    }

    // sort by modification time descending (newest first)
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return CompareFileTime(&a.second, &b.second) > 0;
    });

    // track the best score per size separately
    int maxScoreBySize[3] = { -1, -1, -1 };

    for (const auto& f : files)
    {
        std::string fullPath = std::string(HISTORY_DIR) + "\\" + f.first;
        std::ifstream ifs(fullPath, std::ios::binary);
        if (!ifs) continue;

        HistoryEntry entry;
        entry.filename = f.first;
        entry.imported = (f.first.compare(0, 9, "imported_") == 0);   // externally imported replay file (imported_*.dat)

        char magic[4] = {};
        ifs.read(magic, 4);
        if (ifs.gcount() != 4) continue;

        if (memcmp(magic, "MH2D", 4) == 0)
        {
            // ===== lightweight compressed format (v2) parse: initial board (exponents) + per step (direction/changed cell/score/time/anim) -> rebuild frames =====
            char ver = 0; ifs.read(&ver, 1);
            char hb = 0, hc = 0, ha = 0;
            ifs.read(&hb, 1); ifs.read(&hc, 1); ifs.read(&ha, 1);
            entry.boardSize = (int)hb;
            entry.colorScheme = (int)hc;
            entry.aiAssisted = (ha != 0);
            int sc1 = 0, mc1 = 0, tms1 = 0;
            ifs.read((char*)&sc1, sizeof(sc1));
            ifs.read((char*)&mc1, sizeof(mc1));
            ifs.read((char*)&tms1, sizeof(tms1));
            entry.score = sc1;
            char chm = 0; ifs.read(&chm, 1);
            if (chm == 1)
            {
                char cp = 0; int cms = 0;
                ifs.read(&cp, 1);
                ifs.read((char*)&cms, sizeof(cms));
                entry.challenge = 1;
                entry.challengeProject = (int)cp;
                entry.challengeMs = cms;
            }
            if (entry.boardSize < MIN_BOARD_SIZE || entry.boardSize > MAX_BOARD_SIZE) continue;
            if (entry.colorScheme < 0 || entry.colorScheme >= TOTAL_SCHEMES) continue;
            entry.moveCount = mc1;
            entry.totalMs = tms1;
            bool okV2 = true;
            std::vector<int> fr(entry.boardSize * entry.boardSize, 0);
            for (int i = 0; i < entry.boardSize * entry.boardSize && okV2; ++i)
            {
                unsigned char e = 0;
                ifs.read((char*)&e, 1);
                if (ifs.gcount() != 1) { okV2 = false; break; }
                fr[i] = e ? (1 << e) : 0;
            }
            for (int bi = 0; bi < entry.boardSize; ++bi)
                for (int bj = 0; bj < entry.boardSize; ++bj)
                    entry.board[bi][bj] = fr[bi * entry.boardSize + bj];
            entry.frames.push_back(fr);
            entry.frameScores.push_back(0);
            entry.frameTimesMs.push_back(0);
            entry.dirs.resize(mc1);
            entry.animMs.clear();
            for (int k = 0; k < mc1 && okV2; ++k)
            {
                unsigned char d = 0;
                ifs.read((char*)&d, 1);
                if (ifs.gcount() != 1) { okV2 = false; break; }
                entry.dirs[k] = (int)d;
                unsigned char n = 0;
                ifs.read((char*)&n, 1);
                if (ifs.gcount() != 1) { okV2 = false; break; }
                std::vector<int> nf = fr;
                for (int j = 0; j < n && okV2; ++j)
                {
                    unsigned char pp = 0, pe = 0;
                    ifs.read((char*)&pp, 1);
                    ifs.read((char*)&pe, 1);
                    if (ifs.gcount() != 2) { okV2 = false; break; }
                    if (pp < (unsigned char)(entry.boardSize * entry.boardSize))
                        nf[pp] = pe ? (1 << pe) : 0;
                }
                int scf = 0, tmf = 0; unsigned short am = 0;
                if (okV2) { ifs.read((char*)&scf, sizeof(scf)); if (ifs.gcount() != sizeof(scf)) okV2 = false; }
                if (okV2) { ifs.read((char*)&tmf, sizeof(tmf)); if (ifs.gcount() != sizeof(tmf)) okV2 = false; }
                if (okV2) { ifs.read((char*)&am, sizeof(am));  if (ifs.gcount() != sizeof(am))  okV2 = false; }
                if (!okV2) break;
                fr = nf;
                entry.frames.push_back(fr);
                entry.frameScores.push_back(scf);
                entry.frameTimesMs.push_back(tmf);
                entry.animMs.push_back(am);
            }
            if (!okV2 || (int)entry.frames.size() != mc1 + 1)
            {
                entry.frames.clear(); entry.frameScores.clear(); entry.frameTimesMs.clear();
                entry.moveCount = 0; entry.totalMs = 0; entry.challenge = 0;
            }
            // thumbnail uses the final board (last frame); without replay data keep the initial board
            if (!entry.frames.empty())
            {
                const auto& last = entry.frames.back();
                for (int bi = 0; bi < entry.boardSize; ++bi)
                    for (int bj = 0; bj < entry.boardSize; ++bj)
                        entry.board[bi][bj] = last[bi * entry.boardSize + bj];
            }
        }
        else
        {
        // ===== legacy format (no magic): first 4 bytes are boardSize (int) =====
        int bsOld = 0;
        memcpy(&bsOld, magic, 4);
        entry.boardSize = bsOld;
        ifs.read((char*)&entry.colorScheme, sizeof(int));
        ifs.read((char*)&entry.score, sizeof(int));
        ifs.read((char*)entry.board, sizeof(entry.board));
        // read the AI-assisted flag (legacy files lack this field -> EOF, keep default false)
        ifs.read((char*)&entry.aiAssisted, sizeof(bool));

        if (entry.boardSize < MIN_BOARD_SIZE || entry.boardSize > MAX_BOARD_SIZE) continue;
        if (entry.colorScheme < 0 || entry.colorScheme >= TOTAL_SCHEMES) continue;

        // read replay data (legacy frame data; truncated files -> moveCount=0 -> view-only)
        int moveCount = 0, totalMs = 0;
        ifs.read((char*)&moveCount, sizeof(moveCount));
        if (ifs.gcount() == sizeof(moveCount) && moveCount > 0 && moveCount <= 100000)
        {
            ifs.read((char*)&totalMs, sizeof(totalMs));
            entry.totalMs = totalMs;
            int bs2 = entry.boardSize;
            bool ok = true;
            for (int f = 0; f <= moveCount; ++f)
            {
                std::vector<int> fr(bs2 * bs2, 0);
                for (int i = 0; i < bs2 * bs2; ++i)
                {
                    int v = 0;
                    ifs.read((char*)&v, sizeof(v));
                    if (ifs.gcount() != sizeof(v)) { ok = false; break; }
                    fr[i] = v;
                }
                int scf = 0, tmf = 0;
                if (ok) { ifs.read((char*)&scf, sizeof(scf)); if (ifs.gcount() != sizeof(scf)) ok = false; }
                if (ok) { ifs.read((char*)&tmf, sizeof(tmf)); if (ifs.gcount() != sizeof(tmf)) ok = false; }
                if (!ok) break;
                entry.frames.push_back(fr);
                entry.frameScores.push_back(scf);
                entry.frameTimesMs.push_back(tmf);
            }
            if (ok && (int)entry.frames.size() == moveCount + 1)
            {
                entry.moveCount = moveCount;
                // read per-step move direction (compatible with legacy files lacking direction data -> fall back to 0)
                entry.dirs.resize(moveCount);
                for (int k = 0; k < moveCount; ++k)
                {
                    int d = 0;
                    ifs.read((char*)&d, sizeof(d));
                    if (ifs.gcount() != sizeof(d)) d = 0;
                    entry.dirs[k] = d;
                }
                // per-step animation time (compatible with legacy files lacking this field -> empty; playback falls back to 120ms)
                entry.animMs.clear();
                for (int k = 0; k < moveCount; ++k)
                {
                    int am = 0;
                    ifs.read((char*)&am, sizeof(am));
                    if (ifs.gcount() != sizeof(am)) break;
                    entry.animMs.push_back(am);
                }
                // challenge tag (compatible with legacy files lacking this field -> 0)
                ifs.read((char*)&entry.challenge, sizeof(int));
                if (ifs.gcount() == sizeof(int) && entry.challenge == 1)
                {
                    ifs.read((char*)&entry.challengeProject, sizeof(int));
                    ifs.read((char*)&entry.challengeMs, sizeof(int));
                }
                else entry.challenge = 0;
            }
            else
            {
                entry.frames.clear(); entry.frameScores.clear(); entry.frameTimesMs.clear();
                entry.moveCount = 0; entry.totalMs = 0;
            }
        }
        }   // else: legacy format parse ends

        // parse the history time from the filename (strict validation):
        // only a valid YYYYMMDD[_-]HHMMSS in the filename is used; otherwise fall back to the file's last-modified time.
        // Arbitrary digits in a normal filename must not be treated as a date, otherwise an imported file's original
        // name could be misread as a bogus date (e.g. "year 56649, month 672, day 474").
        {
            std::string fn = f.first;
            entry.timeInfo = "";
            entry.year = entry.month = entry.day = 0;
            auto isDig = [&](size_t k) { return fn[k] >= '0' && fn[k] <= '9'; };
            bool dateSet = false;
            for (size_t i = 0; i + 14 <= fn.size(); ++i)
            {
                bool eight = true;
                for (int n = 0; n < 8; ++n) if (!isDig(i + n)) { eight = false; break; }
                if (!eight) continue;
                int YY = (fn[i+0]-'0')*1000+(fn[i+1]-'0')*100+(fn[i+2]-'0')*10+(fn[i+3]-'0');
                int MM = (fn[i+4]-'0')*10+(fn[i+5]-'0');
                int DD = (fn[i+6]-'0')*10+(fn[i+7]-'0');
                if (!(YY>=1990 && YY<=2099 && MM>=1 && MM<=12 && DD>=1 && DD<=31)) continue;
                size_t j = i + 8;
                if (j < fn.size() && (fn[j]=='_' || fn[j]=='-')) ++j;
                if (j + 6 > fn.size()) continue;
                bool six = true;
                for (int n = 0; n < 6; ++n) if (!isDig(j + n)) { six = false; break; }
                if (!six) continue;
                int HH=(fn[j+0]-'0')*10+(fn[j+1]-'0');
                int Mi=(fn[j+2]-'0')*10+(fn[j+3]-'0');
                int SS=(fn[j+4]-'0')*10+(fn[j+5]-'0');
                if (HH>23 || Mi>59 || SS>59) continue;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%02d : %02d : %02d    %04d - %02d - %02d",
                              HH, Mi, SS, YY, MM, DD);
                entry.timeInfo = buf;
                entry.year = YY; entry.month = MM; entry.day = DD;
                dateSet = true;
                break;
            }
            // no valid timestamp in the filename (e.g. an imported file with a normal name) -> use the last-modified time,
            // so a real date is shown instead of garbage
            if (!dateSet)
            {
                FILETIME local = f.second;
                SYSTEMTIME st;
                if (FileTimeToLocalFileTime(&f.second, &local) && FileTimeToSystemTime(&local, &st))
                {
                    entry.year = st.wYear; entry.month = st.wMonth; entry.day = st.wDay;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%02u : %02u : %02u    %04u - %02u - %02u",
                                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                                  (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay);
                    entry.timeInfo = buf;
                }
            }
        }

        // compute the best score per board size (3->0, 4->1, 5->2); challenge records do not count toward normal best scores
        int szIdx = entry.boardSize - 3;
        if (entry.challenge == 1)
        {
            // challenge: record the fastest time per project
            int proj = entry.challengeProject;
            if (proj >= 0 && proj < 6)
            {
                int prevIdx = g_historyDlg.maxChallengeIdxByProject[proj];
                if (prevIdx < 0 || entry.challengeMs < g_historyDlg.entries[prevIdx].challengeMs)
                    g_historyDlg.maxChallengeIdxByProject[proj] = (int)g_historyDlg.entries.size();
            }
        }
        else if (szIdx >= 0 && szIdx < 3 && entry.score > maxScoreBySize[szIdx])
        {
            maxScoreBySize[szIdx] = entry.score;
            g_historyDlg.maxScoreIdxBySize[szIdx] = (int)g_historyDlg.entries.size();
        }
        g_historyDlg.entries.push_back(entry);
    }

    // Sort all entries (incl. imported) by filename timestamp descending (chronological history order):
    // Imported files are sorted by their filename timestamp (not their on-disk "now" time), so old histories slot back in order
    {
        auto stampOf = [](const HistoryEntry& e) -> std::string {
            std::string fn = e.filename;
            size_t pos = fn.find('_');
            std::string stamp;
            if (pos != std::string::npos)
            {
                stamp = fn.substr(pos + 1);
                size_t dot = stamp.find_last_of('.');
                if (dot != std::string::npos) stamp = stamp.substr(0, dot);
            }
            for (auto& ch : stamp) if (ch == '-') ch = '_';
            return stamp;   // "YYYYMMDD_HHMMSS" (imported same-second _N suffixes sort naturally first)
        };
        std::stable_sort(g_historyDlg.entries.begin(), g_historyDlg.entries.end(),
            [&](const HistoryEntry& a, const HistoryEntry& b) { return stampOf(a) > stampOf(b); });
    }
    // imported history goes above the scrollbar (list top): pinned as a group; within the group sorted by timestamp desc (newest import first)
    std::stable_partition(g_historyDlg.entries.begin(), g_historyDlg.entries.end(),
        [](const HistoryEntry& e) { return e.imported; });
    // user-generated new history (most recent game) goes before earlier history (above imports); older imports one level lower, then chronological
    size_t impEnd = 0;
    while (impEnd < g_historyDlg.entries.size() && g_historyDlg.entries[impEnd].imported) ++impEnd;
    if (impEnd < g_historyDlg.entries.size())
    {
        HistoryEntry newest = std::move(g_historyDlg.entries[impEnd]);   // first entry after the import group = newest normal record
        g_historyDlg.entries.erase(g_historyDlg.entries.begin() + (ptrdiff_t)impEnd);
        g_historyDlg.entries.insert(g_historyDlg.entries.begin(), std::move(newest));
    }
    g_historyDlg.scrollPos = 0;   // back to the list top after sorting so the newest history/imports are visible

    // best-score/fastest-challenge indices must be recomputed after sorting
    g_historyDlg.maxScoreIdxBySize[0] = g_historyDlg.maxScoreIdxBySize[1] = g_historyDlg.maxScoreIdxBySize[2] = -1;
    for (int i = 0; i < 6; ++i) g_historyDlg.maxChallengeIdxByProject[i] = -1;
    {
        int maxScoreBySize[3] = { -1, -1, -1 };
        for (int ei = 0; ei < (int)g_historyDlg.entries.size(); ++ei)
        {
            const HistoryEntry& e = g_historyDlg.entries[ei];
            if (e.challenge == 1)
            {
                int proj = e.challengeProject;
                if (proj >= 0 && proj < 6)
                {
                    int prevIdx = g_historyDlg.maxChallengeIdxByProject[proj];
                    if (prevIdx < 0 || e.challengeMs < g_historyDlg.entries[prevIdx].challengeMs)
                        g_historyDlg.maxChallengeIdxByProject[proj] = ei;
                }
            }
            else
            {
                int szIdx = e.boardSize - 3;
                if (szIdx >= 0 && szIdx < 3 && e.score > maxScoreBySize[szIdx])
                {
                    maxScoreBySize[szIdx] = e.score;
                    g_historyDlg.maxScoreIdxBySize[szIdx] = ei;
                }
            }
        }
    }
}

// ========== Layout computation ==========
// All sizes scale with boardPx (board pixel size), so proportions stay fixed while resizing
//
// User-specified ratios:
//   menu/undo: width:height = 2:1
//   score bar:  height = menu height, width = menu width * 1.618
//   AI buttons: width:height = 4.6:1
//   total button-group width (incl. gaps) = board width
//   button gap = vertical gap between buttons and the board
//
// Derivation (GAP = button gap / button height = 0.25):
//   top-bar button total width = btnH*2 + G*btnH + btnH*2*1.618 + G*btnH + btnH*2
//                = btnH * (2 + 0.25 + 3.236 + 0.25 + 2)
//                = btnH * 7.736  = boardPx
//   => btnH      = boardPx / 7.736
//   => gapBtn    = boardPx * 0.25 / 7.736  = boardPx * 0.03232
//   => topBarH   = btnH / 0.80             = boardPx * 0.16160
//
//   AI buttons: aiBtnH * 4.6 * 3 + 2*gapBtn = boardPx
//   => aiBtnH    = (boardPx - 2*gapBtn) / 13.8
//                = boardPx * 0.06778
//   => aiRowH    = aiBtnH / 0.80           = boardPx * 0.08472
//
// Vertical layout:
//   no AI:  topBarH + gapBtn + boardPx + gapBtn  = boardPx * 1.22623
//   with AI:  topBarH + gapBtn + boardPx + gapBtn + aiRowH + gapBtn  = boardPx * 1.34528
static const float LAYOUT_PAD_RATIO      = 0.016f;   // window outer margin (relative to boardPx)
static const float LAYOUT_GAP_RATIO      = 0.012f;   // board tile gap
static const float BTN_GAP_RATIO        = 0.16f;    // button gap / button height
static const float BTN_IN_BAR           = 0.80f;    // button height / button-row height
static const float BTN_W_MENU           = 2.0f;     // menu/undo: width = height * 2
static const float BTN_W_SCORE          = 2.0f * 1.618f;  // score bar: width = height * 2 * 1.618
// AI_BTN_W is defined above (shared with the playback function-key row)

// Practice-mode overall ratio: board + analysis panel as one unit, height:width = 1:2.15, scales with the window
static const float PRACTICE_WHOLE_R        = 2.15f;   // whole width / whole height
// Gap between board and analysis panel = menu-to-board distance (gapBtn, defined below in computeGapBtnRatio); forward-declared
static float computeGapBtnRatio();
// analysis panel width = whole width - board width - gap
static float practicePanelW(float boardPx) { return boardPx * (PRACTICE_WHOLE_R - 1.f - computeGapBtnRatio()); }
// analysis panel X = board right edge + gap
static float practicePanelX(float boardX, float boardPx) { return boardX + boardPx + boardPx * computeGapBtnRatio(); }

// Layout ratios derived from boardPx (computed in computeBoardPx)
static float computeTopBarRatio()
{
    // topBarH / boardPx = 1 / ((2 + 2*1.618 + 2 + 2*BTN_GAP_RATIO) * BTN_IN_BAR)
    float sumW = BTN_W_MENU + BTN_W_SCORE + BTN_W_MENU + 2.f * BTN_GAP_RATIO;
    return 1.f / (sumW * BTN_IN_BAR);
}

static float computeGapBtnRatio()
{
    // gapBtn / boardPx = BTN_GAP_RATIO / sumW
    float sumW = BTN_W_MENU + BTN_W_SCORE + BTN_W_MENU + 2.f * BTN_GAP_RATIO;
    return BTN_GAP_RATIO / sumW;
}

static float computeAiRowRatio()
{
    // aiRowH / boardPx = 1.4 × ((1 - 2*gapBtnRatio) / 13.8)
    // the three AI function keys match mobile height (1.4x the original); row height = key height (no space below the keys)
    float gapR = computeGapBtnRatio();
    float aiBtnHRatio = 1.4f * (1.f - 2.f * gapR) / (AI_BTN_W * 3.f);
    return aiBtnHRatio;
}

// AI stats row (operation count / operation time rect) height = menu key height (scoreBoxH)
static float computeAiStatsRowRatio()
{
    return computeTopBarRatio() * BTN_IN_BAR;
}

// ===== Timer/AI milestone mode: big-block geometry =====
// Board + milestone bar viewed as one "height:width = 1:1.3" unit (stats row + top bar + board + milestone bar),
// scaling with the window and centered. Board-to-milestone-bar gap is fixed at gapBtn (menu-to-score-bar distance).
struct MilestoneGeo {
    float boardPx;   // board size (the big block derives everything from boardPx)
    float mileW;     // milestone bar width (= whole width - board - gapBtn)
    float wholeW;    // whole width (= 1.3 x whole height)
    float gapBtn;    // board <-> milestone bar gap
};
static MilestoneGeo computeMilestoneGeo(unsigned int winW, unsigned int winH)
{
    const float gapBtnR  = computeGapBtnRatio();
    float statsRowR = computeAiStatsRowRatio();      // stats row (above the top bar)
    if (g_tm.active) statsRowR *= 0.6f;              // timer-mode compact stats row (0.6x), matches drawing
    const float topBarR  = computeTopBarRatio();
    const bool aiActive  = (g_aiMode != AI_INACTIVE) && !g_pb.active;
    const float aiRowR   = aiActive ? computeAiRowRatio() : 0.f;
    const float gapAfter = aiActive ? gapBtnR : 0.f;
    // big-block height (boardPx units): stats row + gap + top bar + gap + board + [gap + AI row]
    const float totalHRatio = gapBtnR + statsRowR + gapBtnR + topBarR + gapBtnR + 1.f + gapAfter + aiRowR;
    // milestone width = 1/2 of the original (1:1.3 big-block derived value):
    //   mileHalfRatio = (1.3*totalHRatio - 1 - gapBtnR) / 2
    const float mileHalfRatio = (1.3f * totalHRatio - 1.f - gapBtnR) * 0.618f;
    // whole width = board + gapBtn + milestone (half width)
    const float wholeWRatio = 1.f + gapBtnR + mileHalfRatio;

    MilestoneGeo g;
    g.boardPx = std::min((float)winW / (wholeWRatio + 2.f * LAYOUT_PAD_RATIO),
                         (float)winH / (totalHRatio + 2.f * LAYOUT_PAD_RATIO));
    if (g.boardPx < 80.f) g.boardPx = 80.f;
    g.wholeW = g.boardPx * wholeWRatio;
    g.gapBtn = g.boardPx * gapBtnR;
    g.mileW  = g.boardPx * mileHalfRatio;
    if (g.mileW < g.boardPx * 0.15f) g.mileW = g.boardPx * 0.15f;   // lower-bound protection
    return g;
}

// Playback board (incl. function-key row) button height / boardPx
static float pbBtnRatio()
{
    float gapR = computeGapBtnRatio();
    return 1.4f * (1.f - 2.f * gapR) / (AI_BTN_W * 3.f);
}

// History playback layout:
//   top info row (height = function-key row height) above the top bar;
//   below the board: track row + time row (with centered play key) + function-key row + row gaps + bottom margin (= window margin)
static float computePbTopRowRatio()   // history time info row above the top bar (height = 75% of the original)
{
    return pbBtnRatio() * 0.75f;
}
static float computePbBelowRowRatio() // 2 rows below the board + gaps + bottom margin
{
    float gapR = computeGapBtnRatio();
    float btnR   = pbBtnRatio();            // function-key (play/settings/cancel row) height base
    float trackR = btnR * 0.5f;             // track row
    float timeR  = btnR;                    // time row height = function-key row height (play key equals settings key)
    return trackR + timeR + gapR;   // two rows + row gap (bottom margin handled by the global gapBtn margin)
}

// Compute the board pixel size (boardPx) from the window size and AI state
static float computeBoardPx(unsigned int winW, unsigned int winH, bool aiActive)
{
    const float topBarR  = computeTopBarRatio();
    const float gapBtnR  = computeGapBtnRatio();   // unified margin = menu-to-board gap (gapBtn)
    const float aiRowR   = aiActive ? computeAiRowRatio() : 0.f;
    const bool chStats = g_ch.active && !g_pb.active;
    float statsRowR = ((aiActive && (g_aiShowMoves || g_aiShowTime)) || chStats || g_tm.active) ? computeAiStatsRowRatio() : 0.f;
    if (g_tm.active && statsRowR > 0.f) statsRowR *= 0.6f;   // timer-mode compact stats row (0.6x), matches drawing
    const float pbTopR   = g_pb.active ? computePbTopRowRatio() : 0.f;
    const float pbBelowR = g_pb.active ? computePbBelowRowRatio() : 0.f;

    // practice mode: board + analysis panel as a whole, height:width = 1:2.15, the fixed-ratio whole scales centered in the window
    if (g_practiceActive)
    {
        float gapAfterBoard = (aiActive || g_pb.active) ? gapBtnR : 0.f;
        float totalHRatio = pbTopR + gapBtnR + statsRowR + gapBtnR + topBarR + gapBtnR + 1.f + gapAfterBoard
                          + aiRowR + pbBelowR;
        // practice mode (prediction panel on the right): bottom min gap = 1/4 original, right min gap = 1/4 original (top/left keep original)
        float padT = LAYOUT_PAD_RATIO;                // top/left min gap
        float padB = LAYOUT_PAD_RATIO * 0.25f;        // bottom/right min gap = 1/4 original
        float boardByH = (float)winH / (totalHRatio + padT + padB);      // vertical constraint
        float boardByW = (float)winW / (PRACTICE_WHOLE_R + padT + padB); // horizontal constraint (whole 1:2.15)
        return std::min(boardByW, boardByH);
    }

    // timer mode (milestones shown, not replaying): the whole big block (stats row + top bar + board + milestone bar)
    // is one height:width = 1:1.3 unit centered and scaled; AI operation shows no milestones -> normal layout
    if (g_tm.showMilestones && g_tm.active && !g_pb.active)
        return computeMilestoneGeo(winW, winH).boardPx;

    float boardPxByW = (float)winW / (1.f + 2.f * gapBtnR);   // board left/right margins = gapBtn
    // layout: top info row + gap + top bar + gap + board + [gap] + AI row/playback control row + bottom
    // initial (no AI/playback): board is the last element, bottom margin = board horizontal margin (gapBtn)
    float gapAfterBoard = (aiActive || g_pb.active) ? gapBtnR : 0.f;
    float totalHRatio = pbTopR + gapBtnR + statsRowR + gapBtnR + topBarR + gapBtnR + 1.f + gapAfterBoard
                      + aiRowR + pbBelowR;   // the bottom-most element of each mode is the board / AI keys / playback keys
    float boardPxByH = (float)winH / (totalHRatio + 2.f * LAYOUT_PAD_RATIO);   // gapBtn margins top and bottom, vertically centered
    return std::min(boardPxByW, boardPxByH);
}

// Whether the milestone panel should show (timer mode only, and not replaying; AI operation/prediction modes do not show it)
static bool milestoneVisible()
{
    return g_tm.showMilestones && g_tm.active && !g_pb.active;
}

// Sync window width: when milestones are shown (timer/AI mode), widen the window just enough to fit
// "board + gap + milestone panel (+ side min margins)"; restored to the previous (possibly narrower) width when the session ends.
// Called every frame in the main loop; only widens when the width is insufficient (avoids frequent/over widening).
static void tmApplyWindowWidth(sf::RenderWindow& window)
{
    const bool showMile = milestoneVisible();
    const unsigned int curW = window.getSize().x;
    const unsigned int curH = window.getSize().y;

    if (showMile)
    {
        if (g_tm.origW == 0) { g_tm.origW = curW; g_tm.origH = curH; }   // record the pre-session width

        RECT rc, wa;
        GetWindowRect((HWND)window.getSystemHandle(), &rc);
        SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
        const int sw = wa.right - wa.left;
        const bool fillsScreen = (rc.right - rc.left >= sw - 4);
        if (!fillsScreen)
        {
            // the width that exactly fits "height:width=1:1.3 big block (+ side min margins)" (width uncapped -> height decides board size)
            MilestoneGeo mg = computeMilestoneGeo(100000u, curH);
            unsigned int needW = (unsigned int)std::lround(mg.wholeW + 2.f * mg.boardPx * LAYOUT_PAD_RATIO);
            if (needW < 320) needW = 320;
            if (needW > (unsigned int)sw) needW = (unsigned int)sw;
            if (needW > curW)                       // only widen to fit, never shrink
                window.setSize(sf::Vector2u(needW, curH));
        }
    }
    else
    {
        // session ended: restore the pre-session (possibly narrower) window width and clear the record for the next session
        if (g_tm.origW > 0 && curW != g_tm.origW)
            window.setSize(sf::Vector2u(g_tm.origW, (g_tm.origH > 0) ? g_tm.origH : curH));
        g_tm.origW = 0; g_tm.origH = 0;
    }
}

struct TopBarLayout {
    float leftBoxX, leftBoxY, leftBoxW, leftBoxH;
    float scoreBoxX, scoreBoxY, scoreBoxW, scoreBoxH;
    float rightBoxX, rightBoxY, rightBoxW, rightBoxH;
    float topBarH;   // top bar total height
    float topBarY;   // top bar start Y
    float gapBtn;    // button gap (= vertical gap between buttons and the board)
};

// boardPx: the already-computed board pixel size; all button sizes scale from it
// offsetY: vertical start of the top bar in the window (whole centered offset)
// inverse: choose btnH so the button-group total width (incl. gaps) = boardPx
TopBarLayout computeTopBarLayout(unsigned int winW, unsigned int winH, float boardPx, float offsetY)
{
    TopBarLayout L;

    // sumW = btnH*(2 + 1.618*2 + 2) + 2*btnH*0.25 = btnH * 7.736
    float sumW = BTN_W_MENU + BTN_W_SCORE + BTN_W_MENU + 2.f * BTN_GAP_RATIO;
    float btnH = boardPx / sumW;

    L.scoreBoxH = btnH;
    L.gapBtn    = btnH * BTN_GAP_RATIO;
    L.topBarH   = btnH / BTN_IN_BAR;
    L.topBarY   = offsetY;

    // menu/undo: width:height = BTN_W_MENU : 1
    L.leftBoxW   = btnH * BTN_W_MENU;
    L.rightBoxW  = btnH * BTN_W_MENU;

    // score bar: width:height = BTN_W_SCORE : 1
    L.scoreBoxW  = btnH * BTN_W_SCORE;

    // button-group total width = boardPx (guaranteed by construction), left-aligned with the board left edge
    // practice mode: board + analysis panel as a whole (height:width = 1:2.15) centered in the window
    float boardX;
    if (g_practiceActive)
    {
        float wholeW = boardPx * PRACTICE_WHOLE_R;
        boardX = ((float)winW - wholeW) / 2.f;
        if (boardX < boardPx * LAYOUT_PAD_RATIO) boardX = boardPx * LAYOUT_PAD_RATIO;
    }
    else if (milestoneVisible())
    {
        // timer/AI mode: top-bar button group left-aligned with the board (the big block is centered in drawGame)
        MilestoneGeo mg = computeMilestoneGeo(winW, winH);
        boardX = ((float)winW - mg.wholeW) / 2.f;
        if (boardX < boardPx * LAYOUT_PAD_RATIO) boardX = boardPx * LAYOUT_PAD_RATIO;
    }
    else boardX = (winW - boardPx) / 2.f;

    L.leftBoxX   = boardX;
    L.scoreBoxX  = boardX + L.leftBoxW + L.gapBtn;
    L.rightBoxX  = boardX + L.leftBoxW + L.gapBtn + L.scoreBoxW + L.gapBtn;

    // buttons vertically centered in the top bar, plus offsetY
    L.leftBoxY   = offsetY + (L.topBarH - btnH) / 2.f;
    L.scoreBoxY  = L.leftBoxY;
    L.rightBoxY  = L.leftBoxY;

    return L;
}

void computeMenuLayout(unsigned int winW, unsigned int winH, float& menuW, float& menuH,
                        float& menuX, float& menuY, float& btnH, float& btnGap, float& btnStartY)
{
    // uses the same proportional layout as the game interface
    float boardPx = computeBoardPx(winW, winH, false);
    float pad = boardPx * LAYOUT_PAD_RATIO;

    menuW = boardPx * 0.75f;
    menuH = boardPx * 1.00f;
    menuX = (winW - menuW) / 2.f;
    menuY = (winH - menuH) / 2.f;
    btnH = menuH * 0.070f;
    btnGap = menuH * 0.017f;
    btnStartY = menuY + menuH * 0.21f;
}

// Unified tile number drawing (shared by main board / mini boards / build board):
//   1-2 digits 0.42, 3 digits 0.34, 4 digits 0.31 (slightly smaller than 3), 5 digits 0.28, 6+ digits wrapped to two lines 0.28
static void drawTileNumber(sf::RenderWindow& window, const sf::Font& font, const std::string& numStr,
                           float cx, float cy, float tileSize, const sf::Color& color)
{
    if (numStr.length() >= 6)
    {
        unsigned int fontSize = (unsigned int)(tileSize * 0.28f);
        int splitPos = std::min(5, (int)numStr.length() - 1);
        std::string line1 = numStr.substr(0, splitPos);
        std::string line2 = numStr.substr(splitPos);
        sf::Text t1(line1, font, fontSize);
        t1.setStyle(sf::Text::Bold); t1.setFillColor(color);
        sf::FloatRect b1 = t1.getLocalBounds();
        t1.setOrigin(b1.left + b1.width / 2, b1.top + b1.height / 2);
        t1.setPosition(cx, cy - tileSize * 0.18f);
        sf::Text t2(line2, font, fontSize);
        t2.setStyle(sf::Text::Bold); t2.setFillColor(color);
        sf::FloatRect b2 = t2.getLocalBounds();
        t2.setOrigin(b2.left + b2.width / 2, b2.top + b2.height / 2);
        t2.setPosition(cx, cy + tileSize * 0.18f);
        window.draw(t1);
        window.draw(t2);
    }
    else
    {
        int len = (int)numStr.length();
        float ratio = 0.42f;
        if (len >= 5)      ratio = 0.28f;
        else if (len >= 4) ratio = 0.31f;
        else if (len >= 3) ratio = 0.34f;
        unsigned int fontSize = (unsigned int)(tileSize * ratio);
        sf::Text num(numStr, font, fontSize);
        num.setStyle(sf::Text::Bold);
        sf::FloatRect bounds = num.getLocalBounds();
        num.setOrigin(bounds.left + bounds.width / 2, bounds.top + bounds.height / 2);
        num.setPosition(cx, cy);
        num.setFillColor(color);
        window.draw(num);
    }
}

// ========== Practice-mode right analysis panel ==========
static sf::Color practiceProbColor(float p)
{
    p = std::min(1.f, std::max(0.f, p));
    sf::Uint8 r = (sf::Uint8)(235.f + (70.f - 235.f) * p);     // red -> green
    sf::Uint8 g = (sf::Uint8)(70.f + (200.f - 70.f) * p);
    return sf::Color(r, g, 80);
}
static const char* dirZhName(int d) { static const char* n[4] = { "左", "右", "上", "下" }; return (d >= 0 && d < 4) ? n[d] : "-"; }
static const char* dirEnName(int d) { static const char* n[4] = { "Left", "Right", "Up", "Down" }; return (d >= 0 && d < 4) ? n[d] : "-"; }
static const char* dirArrowName(int d) { static const char* n[4] = { "←", "→", "↑", "↓" }; return (d >= 0 && d < 4) ? n[d] : "-"; }

// Per-step quality colors: 0=truly best (green) 1=next best (yellow-green) 2=second worst (yellow) 3=worst (orange-red)
static sf::Color practiceQualityColor(int qv)
{
    if (qv < 0) qv = 0;
    if (qv > 3) qv = 3;
    const sf::Color qCol[4] = { sf::Color(80, 200, 80),    // green
                                sf::Color(178, 214, 66),   // yellow-green
                                sf::Color(235, 205, 60),   // yellow
                                sf::Color(235, 85, 50) };  // orange-red
    return qCol[qv];
}

// Practice-mode board layout transforms (apply to the main board): 0=flip vertical 1=flip horizontal 2=CCW 90 deg 3=CW 90 deg
static void practiceTransformBoard(Game2048& game, int op)
{
    const int sz = game.getBoardSize();
    std::vector<std::vector<int>> b(sz, std::vector<int>(sz, 0));
    std::vector<std::vector<int>> n(sz, std::vector<int>(sz, 0));
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            b[i][j] = game.getTile(i, j);
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
        {
            if (op == 0)      n[i][j] = b[sz - 1 - i][j];      // flip vertical
            else if (op == 1) n[i][j] = b[i][sz - 1 - j];      // flip horizontal
            else if (op == 2) n[i][j] = b[j][sz - 1 - i];      // CCW 90 deg
            else              n[i][j] = b[sz - 1 - j][i];      // CW 90 deg
        }
    game.setBoardDirect(n, game.getScore());
    markSaveDirty();
    requestPracticePrediction(game);
}

// Draw the practice-mode right analysis panel (title / four mini boards / probability bars / operation bars / bottom function keys)
// Layout: title shares a row with the undo bar; mini boards top-aligned with the main board; probability/operation bars at the bottom; layout keys keep a large gap to the probability bars
static void drawPracticePanel(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                              float boardX, float boardY, float boardPx, float undoH, const SchemeData& scheme)
{
    const int sz = game.getBoardSize();

    pollPracticePrediction();   // sync background prediction results into the read cache
    pollMilestonePrediction();  // sync background milestone-likelihood results into the read cache

    // Auto-refresh the prediction when the board changes (keeps the analysis panel fresh under AI operation, etc.; replay uses the replay board in real time; no refresh while building)
    // "hide 4-step prediction" also keeps refreshing: the up/down/left/right bars show the "previous step" prediction (the previous position), which must follow moves in real time
    if (!g_practiceBuildOpen)
    {
        const Game2048& src = g_pb.active ? g_pbGame : game;   // replay mode uses the replay instance
        static uint64_t s_lastHash = 0;
        uint64_t h = 0;
        for (int i = 0; i < sz; ++i)
            for (int j = 0; j < sz; ++j)
                h = h * 131u + (unsigned int)src.getTile(i, j);
        if (h != s_lastHash)
        {
            s_lastHash = h;
            // replay: current op = the direction of the step before the currently displayed frame; per-step deviation is based on the previous frame board, updated with previous/next/play
            if (g_pb.active)
            {
                const HistoryEntry* pe = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                    ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                // current displayed frame: during auto-play the applied step count is +1 (applyPlaybackMove advanced
                // the board to the next frame); manual previous/next/drag sets appliedStep=-1 -> equals curFrame
                int fCur = (g_pb.appliedStep >= 0) ? g_pb.appliedStep + 1 : g_pb.curFrame;
                int lastDir = -1;
                if (pe && fCur > 0 && fCur - 1 < (int)pe->dirs.size())
                    lastDir = pe->dirs[fCur - 1];
                g_practiceLastMoveDir = lastDir;
                g_lastMoveByAI = false;   // history replay is handled as "user/evaluated" (not live AI), use engine evaluation
                // explicitly build the "previous frame board" (the replay instance canUndo is always false; take it from the history frames)
                std::vector<std::vector<int>> prev;
                if (lastDir >= 0 && pe && fCur - 1 < (int)pe->frames.size())
                {
                    const auto& pf = pe->frames[fCur - 1];
                    prev.assign(sz, std::vector<int>(sz, 0));
                    for (int i = 0; i < sz; ++i)
                        for (int j = 0; j < sz; ++j)
                            if (i * sz + j < (int)pf.size()) prev[i][j] = pf[i * sz + j];
                }
                std::vector<std::vector<int>> cur(sz, std::vector<int>(sz));
                for (int i = 0; i < sz; ++i)
                    for (int j = 0; j < sz; ++j)
                        cur[i][j] = src.getTile(i, j);
                requestPracticePredictionFor(cur, prev, lastDir);
                requestMilestonePrediction(cur, sz);   // milestone likelihood follows the replay frames
            }
            else
            {
                requestPracticePrediction(src);
                std::vector<std::vector<int>> cur(sz, std::vector<int>(sz));
                for (int i = 0; i < sz; ++i)
                    for (int j = 0; j < sz; ++j)
                        cur[i][j] = src.getTile(i, j);
                requestMilestonePrediction(cur, sz);   // milestone likelihood follows board changes
            }
        }
    }

    // panel area: board + analysis panel as a whole, height:width = 1:2.15, panel at a fixed gap right of the board
    float panelW = practicePanelW(boardPx);
    float panelX = practicePanelX(boardX, boardPx);
    g_practicePanel = sf::FloatRect(panelX, boardY, panelW, boardPx);

    const float px = panelX, pw = panelW;
    const float padX = pw * 0.04f;
    const float gapX = pw * 0.025f;
    const float btnH = undoH * 0.6f;          // function-key height = undo height x 0.6
    const float gapY = undoH * 0.14f;
    const float panelBottom = boardY + boardPx;

    // (5) bottom three function keys: import history / build position / close (pinned to the panel bottom, fixed size, no window distortion)
    float bottomBtnY = panelBottom - btnH;
    {
        const int btnCount = 3;
        float threeW = (pw - 2.f * padX - (btnCount - 1) * gapX) / (float)btnCount;
        g_practiceBtnImport = sf::FloatRect(px + padX, bottomBtnY, threeW, btnH);
        g_practiceBtnBuild  = sf::FloatRect(px + padX + threeW + gapX, bottomBtnY, threeW, btnH);
        g_practiceBtnClose  = sf::FloatRect(px + padX + 2.f * (threeW + gapX), bottomBtnY, threeW, btnH);
        auto drawBtn = [&](const sf::FloatRect& r, const char* zh, const char* en, bool on, sf::Color bg = sf::Color())
        {
            sf::RectangleShape b(sf::Vector2f(r.width, r.height));
            b.setPosition(r.left, r.top);
            b.setFillColor(on ? sf::Color(70, 130, 180) : (bg == sf::Color() ? scheme.boardBg : bg));
            window.draw(b);
            drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                       r.left + r.width / 2, r.top + r.height / 2, (unsigned int)(btnH * 0.5f),
                       on ? sf::Color::White : scheme.textDark, 0, 1);
        };
        drawBtn(g_practiceBtnImport, "导入历史", "Import", g_practiceImport);
        drawBtn(g_practiceBtnBuild,  "构建局面", "Build",  g_practiceBuildOpen);
        drawBtn(g_practiceBtnClose,  "关闭", "Close",  false);
    }

    // middle layout: mini-board title row / layout-transform key row; middle three prediction bars + function-key row + per-step loss line
    //   vertically centered and evenly distributed between the "flip vertical" row and the "import history" row
    float miniTop = boardY;                        // top-aligned with the main board
    float miniS = boardPx * (2.f / 9.f);           // mini board size (2/9 of the board width)
    float titleRowY = miniTop + miniS + gapY * 1.2f;                    // mini title row Y
    float layoutRowY = titleRowY + btnH * 0.55f + gapY * 0.5f;          // layout-transform key row Y (flip vertical etc.)
    float opRowH = btnH;
    float milestoneRowH = btnH * 0.8f;                                   // milestone-likelihood row height (taller = old per-step-loss height)
    float lossRowH = milestoneRowH * 0.8f;                               // per-step-loss row height (shorter = old milestone height)
    float fnKeyH = btnH * 0.9f;                                          // hide/refresh 4-step prediction key row height (slightly shorter)
    float probGridH = 2.f * btnH + gapY * 0.6f;                          // probability-bar group height (two rows + inner gap)
    float midH = fnKeyH + gapY + probGridH + gapY + opRowH + gapY + lossRowH + gapY + milestoneRowH;   // middle group total height
    float zoneTop = layoutRowY + btnH + gapY;                            // layout-key row bottom + gap
    float zoneBottom = bottomBtnY - gapY;                                // import-history row top - gap
    float midTop = zoneTop + std::max(0.f, (zoneBottom - zoneTop - midH) / 2.f);
    float fnKeyRowY = midTop;                                            // function-key row (above the probability bars)
    float probTopY = fnKeyRowY + fnKeyH + gapY;                          // probability-bar group top
    float opRowY = probTopY + probGridH + gapY;                          // operation-bar row
    float lossRowY = opRowY + opRowH + gapY;                             // per-step-loss row
    float milestoneRowY = lossRowY + lossRowH + gapY;                    // milestone-likelihood row

    // (4) one row of three operation bars: current / previous best / next best
    {
        float barH = btnH;
        float bw = (pw - 2.f * padX - 2.f * gapX) / 3.f;
        unsigned int opFont = (unsigned int)(barH * 0.34f);
        auto drawOpBar = [&](float x, float y, float w, const char* zh, const char* en, int dir,
                     unsigned int fontSize = 0, sf::Color borderColor = sf::Color(), bool disabled = false,
                     const float* probSrc = g_practiceProbs)
        {
            // disabled (hide 4-step prediction): next best greyed, no probability or direction shown
            if (disabled)
            {
                sf::RectangleShape tr(sf::Vector2f(w, barH));
                tr.setPosition(x, y);
                tr.setFillColor(sf::Color(200, 190, 180));
                window.draw(tr);
                return;
            }
            float p = (dir >= 0 && dir < 4 && probSrc) ? probSrc[dir] : 0.f;
            sf::Color pc = practiceProbColor(p);
            sf::RectangleShape tr(sf::Vector2f(w, barH));
            tr.setPosition(x, y);
            tr.setFillColor(scheme.boardBg);   // theme background color
            if (borderColor != sf::Color())
            {
                tr.setOutlineColor(borderColor);
                tr.setOutlineThickness(std::max(1.f, barH * 0.06f));
            }
            window.draw(tr);
            if (p > 0.f)
            {
                sf::RectangleShape fill(sf::Vector2f(w * p, barH));
                fill.setPosition(x, y);
                fill.setFillColor(pc);
                window.draw(fill);
            }
            // show the up/down/left/right text + a direction symbol: current = the direction the user just moved; previous/next best = the AI best direction
            std::string label;
            if (g_language == LANG_ZH)
                label = std::string(zh) + " " + std::string(dirZhName(dir)) + " " + std::string(dirArrowName(dir));
            else
                label = std::string(en) + " " + std::string(dirEnName(dir));
            unsigned int fnt = fontSize > 0 ? fontSize : opFont;
            drawTextFB(window, font, toSFString(label.c_str()),
                       x + w / 2, y + barH / 2, fnt, scheme.textDark, 0, 1);   // system text color
        };
        float x1 = px + padX;
        float x2 = x1 + bw + gapX;
        float x3 = x2 + bw + gapX;
        drawOpBar(x1, opRowY, bw, "当前", "Cur", g_practiceCurOp, 0, sf::Color(), false, g_practiceProbsPrev);
        drawOpBar(x2, opRowY, bw, "上一步最佳", "PrevBest", g_practiceBestOp, 0, sf::Color(), false, g_practiceProbsPrev);
        drawOpBar(x3, opRowY, bw, "下一步最佳", "NextBest", g_practiceNextBestOp,
                  (unsigned int)(opFont * 1.2f), sf::Color(70, 130, 180), g_practicePredHidden);

        // per-step deviation row (below "current"): per-step deviation 00.00% + performance (green/yellow-green/yellow/orange-red = excellent/good/fair/fair)
        {
            float lw = pw - 2.f * padX;
            float lx = px + padX;
            sf::RectangleShape lb(sf::Vector2f(lw, lossRowH));
            lb.setPosition(lx, lossRowY);
            lb.setFillColor(scheme.boardBg);
            lb.setOutlineColor(scheme.textDark);
            lb.setOutlineThickness(std::max(1.f, lossRowH * 0.03f));
            window.draw(lb);
            unsigned int lFont = (unsigned int)(milestoneRowH * 0.46f);   // matches the milestone-row font size
            float cy = lossRowY + lossRowH / 2.f;
            // left: "per-step expected deviation 00.00%" (always computed and shown)
            std::string lossStr;
            if (g_practiceLossPct < 0.f)
                lossStr = (g_language == LANG_ZH) ? "单步期望偏差 --.--%" : "Step Exp. Dev. --.--%";
            else
            {
                char lbuf[32];
                std::snprintf(lbuf, sizeof(lbuf), "%.2f%%", g_practiceLossPct);
                lossStr = std::string((g_language == LANG_ZH) ? "单步期望偏差 " : "Step Exp. Dev. ") + lbuf;
            }
            drawTextFB(window, font, toSFString(lossStr.c_str()), lx + lw * 0.04f, cy, lFont, scheme.textDark, 0, 0);
            // right: "performance excellent/good/fair" (worst also shows fair); darker colors for easier distinction
            const char* gName[4] = { "出色", "良好", "一般", "一般" };
            const char* gNameEn[4] = { "Excellent", "Good", "Average", "Average" };
            const sf::Color gTextCol[4] = { sf::Color(0, 150, 70),    // excellent: dark green
                                            sf::Color(125, 165, 30),  // good: olive yellow-green
                                            sf::Color(190, 145, 0),   // fair: dark golden
                                            sf::Color(205, 70, 35) }; // fair (worst): dark orange-red
            sf::Color gc = gTextCol[g_practiceLossGrade];
            std::string perStr;
            if (g_practiceLossPct < 0.f)   // no data (--.--%): the performance column shows -- too, avoiding a misleading "excellent"
                perStr = (g_language == LANG_ZH) ? "表现 --" : "Grade --";
            else
                perStr = (g_language == LANG_ZH)
                    ? std::string("表现 ") + gName[g_practiceLossGrade]
                    : std::string("Grade ") + gNameEn[g_practiceLossGrade];
            drawTextFB(window, font, toSFString(perStr.c_str()),
                       lx + lw * 0.5f, cy, lFont,
                       (g_practiceLossPct < 0.f) ? sf::Color(140, 140, 140) : gc,
                       sf::Text::Bold, 0);
        }

        // milestone-likelihood row (below the per-step deviation): two score-milestone achievement rates (e.g. 512/1024; the sliding window moves on once the smallest is achieved)
        {
            float lw = pw - 2.f * padX;
            float lx = px + padX;
            sf::RectangleShape mb(sf::Vector2f(lw, milestoneRowH));
            mb.setPosition(lx, milestoneRowY);
            mb.setFillColor(scheme.boardBg);
            mb.setOutlineColor(scheme.textDark);
            mb.setOutlineThickness(std::max(1.f, milestoneRowH * 0.03f));
            window.draw(mb);
            unsigned int mFont = (unsigned int)(milestoneRowH * 0.46f);
            float cy = milestoneRowY + milestoneRowH / 2.f;
            // left label
            drawTextFB(window, font,
                       (g_language == LANG_ZH) ? toSFString("里程碑可能性") : sf::String("Milestones"),
                       lx + lw * 0.04f, cy, mFont, scheme.textDark, 0, 0);
            // right milestones (one per actual count, each framed by the theme tile color of that value; computed independently)
            if (g_practiceMilestonePct[0] >= 0.f)
            {
                int cnt = g_practiceMilestoneCount;   // actual count (1-2), capped by the board science limit
                if (cnt < 1) cnt = 1;
                if (cnt > 2) cnt = 2;
                float right = lx + lw - lw * 0.04f;
                float boxH = milestoneRowH * 0.68f;
                for (int i = cnt - 1; i >= 0; --i)
                {
                    char mbuf[48];
                    std::snprintf(mbuf, sizeof(mbuf), "%lld: %.2f%%",
                                  (long long)g_practiceMilestoneVals[i], g_practiceMilestonePct[i]);
                    sf::String ms = toSFString(mbuf);
                    float mw = measureTextFB(font, ms, mFont);
                    float boxW = mw + milestoneRowH * 0.5f;
                    // frame color = the tile color of this value in the theme (2^power)
                    int power = 0; long long tv = g_practiceMilestoneVals[i];
                    while (tv > 1) { tv >>= 1; power++; }
                    sf::Color bc = (power >= 1 && power <= MAX_POWER) ? scheme.tileColors[power] : scheme.textDark;
                    sf::RectangleShape box(sf::Vector2f(boxW, boxH));
                    box.setPosition(right - boxW, cy - boxH / 2.f);
                    box.setFillColor(scheme.boardBg);
                    box.setOutlineColor(bc);
                    box.setOutlineThickness(std::max(1.f, boxH * 0.06f));
                    window.draw(box);
                    drawTextFB(window, font, ms, right - boxW / 2.f, cy, mFont, scheme.textDark, 0, 1);
                    right -= boxW + lw * 0.02f;
                }
            }
            else
            {
                drawTextFB(window, font,
                           (g_language == LANG_ZH) ? toSFString("计算中...") : sf::String("..."),
                           lx + lw - lw * 0.04f, cy, mFont, scheme.textDark, 0, 2);
            }
        }
    }

    // function-key row (above the probability bars): "hide 4-step prediction" / "refresh 4-step prediction" (refresh greyed when hidden)
    {
        float fkW = (pw - 2.f * padX - gapX) / 2.f;
        float fkY = fnKeyRowY;
        g_practiceHidePredBtn    = sf::FloatRect(px + padX, fkY, fkW, fnKeyH);
        g_practiceRefreshPredBtn = sf::FloatRect(px + padX + fkW + gapX, fkY, fkW, fnKeyH);
        unsigned int fkFont = (unsigned int)(fnKeyH * 0.42f);
        auto drawFk = [&](const sf::FloatRect& r, const char* zh, const char* en, bool on, bool disabled)
        {
            sf::RectangleShape b(sf::Vector2f(r.width, r.height));
            b.setPosition(r.left, r.top);
            b.setFillColor(on ? sf::Color(70, 130, 180) : scheme.boardBg);
            window.draw(b);
            drawTextFB(window, font,
                       (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                       r.left + r.width / 2, r.top + r.height / 2, fkFont,
                       on ? sf::Color::White : (disabled ? sf::Color(170, 160, 150) : scheme.textDark), 0, 1);
        };
        drawFk(g_practiceHidePredBtn,    "隐藏四步预测", "Hide Predict",  g_practicePredHidden, false);
        drawFk(g_practiceRefreshPredBtn, "刷新四步预测", "Refresh Predict", false, g_practicePredHidden);
    }

    // (3) four probability bars (2x2: up/down top row, left/right bottom row), green->red gradient (above the operation bars)
    {
        float barH = btnH;
        float bw = (pw - 2.f * padX - gapX) / 2.f;
        const int order[4] = { 2, 3, 0, 1 };   // up down left right
        for (int i = 0; i < 4; ++i)
        {
            int d = order[i];
            float bx = px + padX + (i % 2) * (bw + gapX);
            float by = probTopY + (i / 2) * (barH + gapY * 0.6f);
            // with "hide 4-step prediction": the four direction bars show each direction's score of the user's previous operation (the previous position), marked (previous step)
            //   -- this is intended: hiding the 4-step prediction means reviewing the previous position's scores, not a static analysis of the current position.
            //   otherwise (4-step prediction on): show the next-step prediction of the current position.
            const bool showPrev = g_practicePredHidden;
            float p = showPrev ? g_practiceProbsPrev[d] : g_practiceProbs[d];
            sf::Color pc = practiceProbColor(p);
            sf::RectangleShape tr(sf::Vector2f(bw, barH));
            tr.setPosition(bx, by);
            tr.setFillColor(scheme.boardBg);   // theme background color
            window.draw(tr);
            if (p > 0.f)
            {
                sf::RectangleShape fill(sf::Vector2f(bw * p, barH));
                fill.setPosition(bx, by);
                fill.setFillColor(pc);
                window.draw(fill);
            }
            std::string dlabel;
            if (g_language == LANG_ZH)
                dlabel = std::string(dirZhName(d)) + (showPrev ? "（上一步）" : "");
            else
                dlabel = std::string(dirEnName(d)) + (showPrev ? " (Prev)" : "");
            drawTextFB(window, font, toSFString(dlabel.c_str()),
                       bx + barH * 0.22f, by + barH / 2, (unsigned int)(barH * 0.52f), scheme.textDark, 0, 0);
            char pbuf[32];
            std::snprintf(pbuf, sizeof(pbuf), "%.3f%%", p * 100.f);   // three decimals
            // "score rate" label (same font size as the number) right next to the percentage, on its left
            unsigned int numSize = (unsigned int)(barH * 0.42f);
            sf::String numStr = toSFString(pbuf);
            sf::String lblStr = (g_language == LANG_ZH) ? toSFString("得分率") : sf::String("Score");
            float numRight = bx + bw - barH * 0.2f;
            float lblRight = numRight - measureTextFB(font, numStr, numSize) - barH * 0.1f;
            drawTextFB(window, font, lblStr, lblRight, by + barH / 2, numSize, scheme.textDark, 0, 2);
            drawTextFB(window, font, numStr, numRight, by + barH / 2, numSize, scheme.textDark, 0, 2);
        }
    }

    // (1) title: "best position prediction" - same row as the undo/score bar, width = 0.6x original, height = 0.6x original, centered, font same as the undo key, with a border
    const float topBarH = undoH / BTN_IN_BAR;                      // top bar total height
    const float titleY = boardY - topBarH - undoH * BTN_GAP_RATIO + (topBarH - undoH) / 2.f;   // the undo button row
    const float titleH = undoH * 0.6f;                             // title height = undo key height x 0.6
    const unsigned int titleFont = (unsigned int)(undoH * 0.42f);  // undo key font size
    {
        float titleW = (pw - 2.f * padX) * 0.6f;                   // width = 0.6x the original
        sf::RectangleShape titleBox(sf::Vector2f(titleW, titleH));
        titleBox.setPosition(px + (pw - titleW) / 2.f, titleY);
        titleBox.setFillColor(scheme.boardBg);
        titleBox.setOutlineColor(scheme.textDark);
        titleBox.setOutlineThickness(std::max(1.f, titleH * 0.03f));
        window.draw(titleBox);
        drawTextFB(window, font,
                   (g_language == LANG_ZH) ? toSFString("最佳局势预测") : sf::String("Best Position Predict"),
                   px + pw / 2.f, titleY + titleH / 2.f, titleFont, scheme.textDark, sf::Text::Bold, 1);
    }

    // (2)-a four mini boards (2/9 board width, spacing x3): top-aligned with the main board; title below aligned to the board left edge
    {
        float miniGap = boardPx * 0.03f;               // fixed gap between mini boards (original 0.01 x3)
        float totalMiniW = 4.f * miniS + 3.f * miniGap;
        float startX = px + (pw - totalMiniW) / 2.f;
        for (int i = 0; i < 4; ++i)
        {
            float mx = startX + i * (miniS + miniGap);
            sf::RectangleShape bg(sf::Vector2f(miniS, miniS));
            bg.setPosition(mx, miniTop);
            bg.setFillColor(scheme.boardBg);
            window.draw(bg);
            const auto& bd = g_practicePredBoard[i];
            if ((int)bd.size() == sz && !g_practicePredHidden)   // hide 4-step prediction: clear the mini-board numbers
            {
                float mg = miniS * LAYOUT_GAP_RATIO;
                float mt = (miniS - mg * (sz + 1)) / (float)sz;
                for (int r = 0; r < sz; ++r)
                    for (int c = 0; c < sz; ++c)
                    {
                        int v = bd[r][c];
                        float tx = mx + mg + c * (mt + mg);
                        float ty = miniTop + mg + r * (mt + mg);
                        sf::RectangleShape t(sf::Vector2f(mt, mt));
                        t.setPosition(tx, ty);
                        t.setFillColor(game.getTileColor(v));
                        window.draw(t);
                        if (v > 0 && !isFillValue(v))
                            drawTileNumber(window, font, std::to_string(v),
                                           tx + mt / 2, ty + mt / 2, mt, scheme.textDark);
                    }
            }
        }
        // title row: step-N prediction [direction small rect + up/down/left/right text] (slightly farther from the board;
        // aligned to the board left edge as a whole); with prediction hidden, title/direction/arrows all hidden
        if (!g_practicePredHidden)
        {
            const char* tzh[4] = { "第一步", "第二步", "第三步", "第四步" };
            const char* ten[4] = { "Step 1", "Step 2", "Step 3", "Step 4" };
            float miniTitleFontF = btnH * 0.36f;
            unsigned int miniTitleFont = (unsigned int)miniTitleFontF;
            float dirBoxS = miniTitleFontF * 1.05f;
            for (int i = 0; i < 4; ++i)
            {
                float mx = startX + i * (miniS + miniGap);
                int d = g_practicePredDirs[i];
                // group text (incl. up/down/left/right)
                std::string label;
                if (g_language == LANG_ZH)
                    label = std::string(tzh[i]) + (d >= 0 && d < 4 ? " " + std::string(dirZhName(d)) : "");
                else
                    label = std::string(ten[i]);
                sf::String lbl = toSFString(label.c_str());
                // aligned to the mini-board left edge as a whole (left-aligned so the direction rect closely follows the text)
                drawTextFB(window, font, lbl, mx, titleRowY, miniTitleFont, scheme.textDark, 0, 0);
                // direction small rect (with arrow) right after the text
                sf::Text measure(lbl, font, miniTitleFont);
                float textW = measure.getLocalBounds().width;
                float dirBoxX = mx + textW + 4.f;
                float dirBoxY = titleRowY - dirBoxS / 2.f;
                sf::RectangleShape dirBox(sf::Vector2f(dirBoxS, dirBoxS));
                dirBox.setPosition(dirBoxX, dirBoxY);
                dirBox.setFillColor(scheme.boardBg);
                dirBox.setOutlineColor(scheme.textDark);
                dirBox.setOutlineThickness(std::max(1.f, dirBoxS * 0.05f));
                window.draw(dirBox);
                if (d >= 0 && d < 4)
                    drawTextFB(window, font, toSFString(dirArrowName(d)),
                               dirBoxX + dirBoxS / 2.f, dirBoxY + dirBoxS / 2.f,
                               (unsigned int)(dirBoxS * 0.72f), scheme.textDark, 0, 1);
            }
        }
    }

    // (2)-b four board layout-transform keys: flip vertical / flip horizontal / CCW 90 / CW 90 (below the mini title row, with a large gap to the probability bars)
    {
        float layoutBtnH = btnH;
        float fourW = (pw - 2.f * padX - 3.f * gapX) / 4.f;
        g_practiceBtnFlipV   = sf::FloatRect(px + padX, layoutRowY, fourW, layoutBtnH);
        g_practiceBtnFlipH   = sf::FloatRect(px + padX + fourW + gapX, layoutRowY, fourW, layoutBtnH);
        g_practiceBtnRotCCW  = sf::FloatRect(px + padX + 2.f * (fourW + gapX), layoutRowY, fourW, layoutBtnH);
        g_practiceBtnRotCW   = sf::FloatRect(px + padX + 3.f * (fourW + gapX), layoutRowY, fourW, layoutBtnH);
        auto drawLayout = [&](const sf::FloatRect& r, const char* zh, const char* en)
        {
            sf::RectangleShape b(sf::Vector2f(r.width, r.height));
            b.setPosition(r.left, r.top);
            b.setFillColor(scheme.boardBg);
            b.setOutlineColor(scheme.textDark);
            b.setOutlineThickness(std::max(1.f, layoutBtnH * 0.03f));
            window.draw(b);
            drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                       r.left + r.width / 2, r.top + r.height / 2, (unsigned int)(layoutBtnH * 0.42f),
                       scheme.textDark, 0, 1);
        };
        drawLayout(g_practiceBtnFlipV,  "上下翻转",  "Flip V");
        drawLayout(g_practiceBtnFlipH,  "左右翻转",  "Flip H");
        drawLayout(g_practiceBtnRotCCW, "逆时针90°", "Rot CCW");
        drawLayout(g_practiceBtnRotCW,  "顺时针90°", "Rot CW");
    }
}

// ========== Build-position dialog ==========
// Max value power for a board size (the max selectable value when building)
static int practiceBuildMaxPow(int sz)
{
    if (sz == 3) return 10;    // 2^10 = 1024
    if (sz == 4) return 16;    // 2^16 = 65536
    return 25;                 // 2^25
}

// Build-position quality budget (scientific basis): the total value of the full merge chain 2^maxPow + 2^(maxPow-1) + ... + 2
// i.e. the total value of all tiles on the final board cannot exceed it, otherwise the position cannot be built through legal merges
static long long practiceBuildBudget(int maxPow) { return (1LL << (maxPow + 1)) - 2; }
// Total value of all tiles on the current board (fill/obstacle tiles carry no value)
static long long practiceBuildSum(const std::vector<std::vector<int>>& board)
{
    long long s = 0;
    for (const auto& row : board)
        for (int v : row)
            if (v > 0 && !isFillValue(v)) s += v;
    return s;
}

// Compute the build-position dialog layout (shared by drawing and interaction; returns the visible list item count)
// boardPx: main-board pixel size, used to lock the build board to 75% x 85% x boardPx (15% smaller)
static int computePracticeBuildLayout(unsigned int winW, unsigned int winH, int sz, float boardPx)
{
    // dialog fixed ratio: height:width = 1:1.618, capped by the window
    const float RATIO = 1.618f;
    float maxW = (float)winW * 0.9f;
    float maxH = (float)winH * 0.85f;
    float popH = maxH;
    float popW = popH * RATIO;
    if (popW > maxW) { popW = maxW; popH = popW / RATIO; }
    const float popX = ((float)winW - popW) / 2.f;
    const float popY = ((float)winH - popH) / 2.f;
    g_practiceBuildRect = sf::FloatRect(popX, popY, popW, popH);

    const float innerPad = popW * 0.045f;
    const float paletteW = popW * 0.20f;
    g_practiceBuildPaletteX = popX + innerPad;
    g_practiceBuildPaletteY = popY + innerPad;
    g_practiceBuildPaletteW = paletteW;
    g_practiceBuildPaletteH = popH - innerPad * 2.f - popH * 0.14f;   // bottom reserves the function-key row

    // value list: clear (0) + 2^1 .. 2^maxPow (idx 1..maxPow) [fill tiles removed]
    const int maxPow = practiceBuildMaxPow(sz);
    const int total = maxPow + 1;   // 0=clear, 1..maxPow=2^idx [fill tiles removed]
    const int visible = 8;
    g_practiceBuildItemH = g_practiceBuildPaletteH / (float)visible;
    int maxScroll = total - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (g_practiceBuildScroll > maxScroll) g_practiceBuildScroll = maxScroll;
    if (g_practiceBuildScroll < 0) g_practiceBuildScroll = 0;

    // board area: fixed at 75% x 85% x boardPx of the main board, centered
    const float targetBoardW = boardPx * 0.75f * 0.85f;
    const float cell = targetBoardW / (float)sz;
    g_practiceBuildCell = cell;
    // available area right edge
    const float availX = g_practiceBuildPaletteX + g_practiceBuildPaletteW + popW * 0.05f;
    const float availW = popX + popW - innerPad - availX;
    const float boardDrawW = cell * sz;
    g_practiceBuildBoardX = availX + (availW - boardDrawW) / 2.f;
    g_practiceBuildBoardY = g_practiceBuildPaletteY + (g_practiceBuildPaletteH - boardDrawW) / 2.f;

    // bottom function keys: save / cancel
    float btnW = popW * 0.16f;
    float btnH = popH * 0.08f;
    float btnY = popY + popH - innerPad - btnH;
    float totalW = btnW * 2.f + popW * 0.04f;
    float btnX = popX + (popW - totalW) / 2.f;
    g_practiceBuildSave   = sf::FloatRect(btnX, btnY, btnW, btnH);
    g_practiceBuildCancel = sf::FloatRect(btnX + btnW + popW * 0.04f, btnY, btnW, btnH);
    return visible;
}

// Open the build-position dialog: start from the current board
static void openPracticeBuild(const Game2048& game)
{
    const int sz = game.getBoardSize();
    g_practiceBuildBoard.assign(sz, std::vector<int>(sz, 0));
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            g_practiceBuildBoard[i][j] = game.getTile(i, j);
    g_practiceBuildSelPow = 1;   // default selected 2 [fill tiles removed]
    g_practiceBuildScroll = 0;
    g_practiceBuildOpen = true;
}

// Save the built position: apply it to the game board
static void savePracticeBuild(Game2048& game)
{
    game.setBoardDirect(g_practiceBuildBoard, game.getScore());
    g_practiceBuildOpen = false;
    markSaveDirty();
    requestPracticePrediction(game);
}

// Draw the build-position dialog (topmost layer)
static void drawPracticeBuildPopup(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                   const SchemeData& scheme, float boardPx)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const int sz = game.getBoardSize();
    const int visible = computePracticeBuildLayout(winW, winH, sz, boardPx);
    const sf::Color activeBlue(70, 130, 180);
    const int maxPow = practiceBuildMaxPow(sz);

    // semi-transparent overlay
    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(overlay);

    // dialog background
    sf::RectangleShape bg(sf::Vector2f(g_practiceBuildRect.width, g_practiceBuildRect.height));
    bg.setPosition(g_practiceBuildRect.left, g_practiceBuildRect.top);
    bg.setFillColor(scheme.windowBg);
    bg.setOutlineColor(activeBlue);
    bg.setOutlineThickness(3.f);
    window.draw(bg);

    // title
    drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString("构建局面") : sf::String("Build Position"),
               g_practiceBuildRect.left + g_practiceBuildRect.width / 2,
               g_practiceBuildRect.top + g_practiceBuildRect.height * 0.045f,
               (unsigned int)(g_practiceBuildRect.height * 0.05f), scheme.textDark, sf::Text::Bold, 1);

    // quality budget: remaining placeable value (full merge chain 2^maxPow + ... + 2 total - current board total)
    const long long budget = practiceBuildBudget(maxPow);
    const long long usedVal = practiceBuildSum(g_practiceBuildBoard);
    const long long remaining = budget - usedVal;
    {
        char rbuf[64];
        std::snprintf(rbuf, sizeof(rbuf), (g_language == LANG_ZH)
                      ? "剩余可放价值: %lld / %lld" : "Remaining: %lld / %lld", remaining, budget);
        drawTextFB(window, font, toSFString(rbuf),
                   g_practiceBuildRect.left + g_practiceBuildRect.width / 2,
                   g_practiceBuildRect.top + g_practiceBuildRect.height * 0.09f,
                   (unsigned int)(g_practiceBuildRect.height * 0.035f), scheme.textDark, 0, 1);
    }

    // left value list (scrollable; clear + fill + 2^1..2^maxPow; items over the remaining budget are greyed out)
    // idx: 0=clear, 1=fill (obstacle), 2..maxPow+1=2^(idx-1)
    unsigned int itemFont = (unsigned int)(g_practiceBuildItemH * 0.42f);
    for (int vi = 0; vi < visible; ++vi)
    {
        int idx = g_practiceBuildScroll + vi;
        if (idx > maxPow) break;
        float iy = g_practiceBuildPaletteY + vi * g_practiceBuildItemH;
        bool sel = (idx == g_practiceBuildSelPow);
        bool isClear = (idx == 0);
        int val = isClear ? 0 : (1 << idx);
        // replacement/placement exceeding the remaining budget is not selectable (clear/fill always selectable)
        bool affordable = isClear || ((long long)val <= remaining);
        sf::RectangleShape item(sf::Vector2f(g_practiceBuildPaletteW, g_practiceBuildItemH));
        item.setPosition(g_practiceBuildPaletteX, iy);
        item.setFillColor(sel ? activeBlue : (affordable ? scheme.boardBg : sf::Color(150, 145, 140)));
        window.draw(item);
        // color block
        sf::Color sw = isClear ? sf::Color(150, 150, 150) : game.getTileColor(val);
        float swS = g_practiceBuildItemH * 0.62f;
        sf::RectangleShape swatch(sf::Vector2f(swS, swS));
        swatch.setPosition(g_practiceBuildPaletteX + g_practiceBuildItemH * 0.14f,
                           iy + (g_practiceBuildItemH - swS) / 2.f);
        swatch.setFillColor(sw);
        window.draw(swatch);
        // value text
        std::string txt = isClear
            ? (g_language == LANG_ZH ? std::string("清除") : std::string("Clear"))
            : std::to_string(val);
        drawTextFB(window, font, toSFString(txt.c_str()),
                   g_practiceBuildPaletteX + g_practiceBuildItemH * 0.14f + swS + g_practiceBuildItemH * 0.16f,
                   iy + g_practiceBuildItemH / 2, itemFont,
                   sel ? sf::Color::White : scheme.textDark, 0, 0);
    }

    // right board
    sf::RectangleShape boardBg(sf::Vector2f(g_practiceBuildCell * sz, g_practiceBuildCell * sz));
    boardBg.setPosition(g_practiceBuildBoardX, g_practiceBuildBoardY);
    boardBg.setFillColor(scheme.boardBg);
    window.draw(boardBg);
    float gap = g_practiceBuildCell * 0.02f;
    for (int r = 0; r < sz; ++r)
        for (int c = 0; c < sz; ++c)
        {
            int v = g_practiceBuildBoard[r][c];
            float x = g_practiceBuildBoardX + c * g_practiceBuildCell;
            float y = g_practiceBuildBoardY + r * g_practiceBuildCell;
            sf::RectangleShape cell(sf::Vector2f(g_practiceBuildCell - gap * 2, g_practiceBuildCell - gap * 2));
            cell.setPosition(x + gap, y + gap);
            cell.setFillColor(game.getTileColor(v));
            if (isFillValue(v))
            {
                // fill (obstacle): draw a diagonal cross (no number)
                cell.setOutlineColor(sf::Color(200, 200, 210));
                cell.setOutlineThickness(1.5f);
                window.draw(cell);
                sf::Vertex line1[] = {
                    sf::Vertex(sf::Vector2f(x + gap + 3.f, y + gap + 3.f), sf::Color(200, 200, 210)),
                    sf::Vertex(sf::Vector2f(x + g_practiceBuildCell - gap - 3.f, y + g_practiceBuildCell - gap - 3.f), sf::Color(200, 200, 210)) };
                sf::Vertex line2[] = {
                    sf::Vertex(sf::Vector2f(x + g_practiceBuildCell - gap - 3.f, y + gap + 3.f), sf::Color(200, 200, 210)),
                    sf::Vertex(sf::Vector2f(x + gap + 3.f, y + g_practiceBuildCell - gap - 3.f), sf::Color(200, 200, 210)) };
                window.draw(line1, 2, sf::Lines);
                window.draw(line2, 2, sf::Lines);
                continue;
            }
            window.draw(cell);
            if (v > 0)
                drawTileNumber(window, font, std::to_string(v),
                               x + g_practiceBuildCell / 2, y + g_practiceBuildCell / 2,
                               g_practiceBuildCell - gap * 2, scheme.textDark);
        }

    // bottom function keys: save / cancel
    auto drawBottom = [&](const sf::FloatRect& r, const char* zh, const char* en, bool primary)
    {
        sf::RectangleShape b(sf::Vector2f(r.width, r.height));
        b.setPosition(r.left, r.top);
        b.setFillColor(primary ? activeBlue : scheme.boardBg);
        window.draw(b);
        drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                   r.left + r.width / 2, r.top + r.height / 2,
                   (unsigned int)(r.height * 0.48f),
                   primary ? sf::Color::White : scheme.textDark, 0, 1);
    };
    drawBottom(g_practiceBuildSave,   "保存", "Save", true);
    drawBottom(g_practiceBuildCancel, "取消", "Cancel", false);
}

// ========== Render functions ==========
// Challenge dialog forward declaration (defined after drawGame)
static void drawChallengeSettings(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                  float boardX, float boardY, float boardPx, const SchemeData& scheme);
static void drawChallengeResult(sf::RenderWindow& window, const Game2048& game, const sf::Font& font);
static void drawTimerResult(sf::RenderWindow& window, const Game2048& game, const sf::Font& font);

void drawGame(sf::RenderWindow& window, const Game2048& game, const sf::Font& font, bool menuOpen)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;

    // history view mode: render from the history data
    bool viewing = game.isViewingHistory();
    const SchemeData& scheme = viewing ? SCHEMES[game.getColorScheme()] : game.getScheme();
    int gridSize = viewing ? game.getViewBoardSize() : game.getBoardSize();
    int displayScore = viewing ? game.getViewScore() : game.getScore();

    window.clear(scheme.windowBg);

    // === proportional layout: all sizes based on boardPx, proportions fully fixed while resizing ===
    // playback mode is treated as no AI (AI buttons/stats row hidden; the playback control area takes over below the board)
    const bool aiActive = (g_aiMode != AI_INACTIVE) && !g_pb.active;
    const bool chActive = g_ch.active && !g_pb.active;   // challenge mode
    const bool showStatsRow = (aiActive && (g_aiShowMoves || g_aiShowTime)) || chActive || g_tm.active;
    const float boardPx = computeBoardPx(winW, winH, aiActive);

    // board tile gap (keeps the original ratio)
    const float gap = boardPx * LAYOUT_GAP_RATIO;
    const float pad = boardPx * LAYOUT_PAD_RATIO;

    // board horizontal position: practice mode centers the board + analysis panel whole (height:width = 1:2.15)
    float boardX;
    if (g_practiceActive)
    {
        float wholeW = boardPx * PRACTICE_WHOLE_R;
        boardX = ((float)winW - wholeW) / 2.f;
        if (boardX < pad) boardX = pad;
    }
    else if (milestoneVisible())
    {
        // timer/AI mode: the whole big block (board + milestone bar, height:width = 1:1.3) centered both horizontally and vertically
        MilestoneGeo mg = computeMilestoneGeo(winW, winH);
        boardX = ((float)winW - mg.wholeW) / 2.f;
        if (boardX < pad) boardX = pad;
    }
    else boardX = (winW - boardPx) / 2.f;

    // === compute the content area total height for vertical centering ===
    float topBarR = computeTopBarRatio();
    float gapBtnR = computeGapBtnRatio();
    float aiRowR = aiActive ? computeAiRowRatio() : 0.f;
    float statsRowR = showStatsRow ? computeAiStatsRowRatio() : 0.f;
    if (g_tm.active && statsRowR > 0.f) statsRowR *= 0.6f;   // timer-mode compact stats row (0.6x), matches drawing
    float pbTopR   = g_pb.active ? computePbTopRowRatio() : 0.f;
    float pbBelowR = g_pb.active ? computePbBelowRowRatio() : 0.f;
    float gapAfterBoard = (aiActive || g_pb.active) ? gapBtnR : 0.f;   // initial: board is the last element, bottom margin = board horizontal margin
    float totalHRatio = pbTopR + gapBtnR + statsRowR + gapBtnR + topBarR + gapBtnR + 1.f + gapAfterBoard
                      + aiRowR + pbBelowR;   // the bottom-most element of each mode is the board / AI keys / playback keys
    float totalContentH = boardPx * totalHRatio;
    // vertically centered; top/bottom min gap = left/right min distance (boardX)
    float offsetY = ((float)winH - totalContentH) / 2.f;
    if (offsetY < boardX) offsetY = boardX;
    if (offsetY + totalContentH > (float)winH) offsetY = ((float)winH - totalContentH) / 2.f;
    // practice mode (prediction panel on the right): board-to-bottom min gap = 1/4 original (bottom hugs the window; still centered when the window is tall enough)
    if (g_practiceActive)
    {
        float minBottom = pad * 0.25f;
        float yMax = (float)winH - totalContentH - minBottom;
        if (offsetY > yMax) offsetY = yMax;                     // bottom margin insufficient -> move up (top margin grows)
        if (offsetY < pad && yMax >= pad - 1.f) offsetY = pad;  // top margin insufficient and enough space -> move down (bottom shrinks)
        if (offsetY < 0.f) offsetY = 0.f;                       // absolute overflow guard
    }

    // === top-bar button layout (proportional to boardPx, vertically centered) ===
    TopBarLayout L = computeTopBarLayout(winW, winH, boardPx, offsetY);
    // playback top info row above the top bar: shift everything down by info-row height + gapBtn
    float pbTopInfoH = 0.f;
    if (g_pb.active)
    {
        pbTopInfoH = pbBtnRatio() * 0.75f * boardPx;   // height = 75% of the original
        float shift = pbTopInfoH + L.gapBtn;
        L.topBarY   += shift;
        L.leftBoxY  += shift;
        L.scoreBoxY += shift;
        L.rightBoxY += shift;
    }
    // stats row above the top bar: shift everything down by stats-row height + gapBtn; only reserve the row when
    // "show count" or "show time" is on (or challenge/timer mode), otherwise back to the original layout
    if (showStatsRow)
    {
        float statH = g_tm.active ? L.scoreBoxH * 0.6f : L.scoreBoxH;   // timer-mode compact stats row
        float shift = statH + L.gapBtn;
        L.topBarY   += shift;
        L.leftBoxY  += shift;
        L.scoreBoxY += shift;
        L.rightBoxY += shift;
    }

    // === playback top info row (history time info: time     date [AI assisted]) ===
    // length = board width, height = playback settings key height
    if (g_pb.active)
    {
        const HistoryEntry* entry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
            ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
        float infoH = pbTopInfoH;
        sf::RectangleShape infoBg(sf::Vector2f(boardPx, infoH));
        infoBg.setPosition(boardX, offsetY);
        infoBg.setFillColor(scheme.boardBg);
        window.draw(infoBg);
        if (entry)
        {
            std::string txt = entry->timeInfo;
            if (entry->aiAssisted)
                txt += (g_language == LANG_ZH) ? "  [AI 辅助]" : "  [AI-assisted]";
            // challenge record: append the [4x4 2048] challenge project tag in the same style as [AI assisted]
            if (entry->challenge == 1 && entry->challengeProject >= 0 && entry->challengeProject < 6)
            {
                const ChallengeProject& cp = g_chProjects[entry->challengeProject];
                txt += "  [" + std::to_string(cp.boardSize) + "×" + std::to_string(cp.boardSize)
                     + " " + std::to_string(cp.target) + "]";
            }
            if (!txt.empty())
            {
                unsigned int infoFont = (unsigned int)(infoH * 0.62f);
                drawTextFB(window, font, toSFString(txt.c_str()),
                           boardX + boardPx / 2.f, offsetY + infoH / 2.f,
                           infoFont, scheme.textDark, sf::Text::Bold, 1);
            }
        }
    }

    // === AI/challenge stats row (operation count / operation time) ===
    // above the top bar; timer mode: count+time above the top bar (total width = board width), IPS above the milestone bar
    if (showStatsRow)
    {
        float statsBoxH = L.scoreBoxH;
        float statsRowY = offsetY;   // stats row above the top bar
        float availW = boardPx - L.gapBtn;

        // timer mode: stats row height = 0.6x original
        if (g_tm.active) statsBoxH = L.scoreBoxH * 0.6f;

        bool showMoves, showTime, showIps;
        sf::String countStr, timeStr, ipsStr;
        if (chActive)
        {
            showMoves = true; showTime = true; showIps = false;
            countStr = (g_language == LANG_ZH)
                ? toSFString("次数： ") + sf::String(std::to_string(g_ch.moveCount))
                : sf::String("Moves: ") + sf::String(std::to_string(g_ch.moveCount));
            std::string t = chFormatTime(g_ch.elapsedMs);
            timeStr = (g_language == LANG_ZH)
                ? toSFString("时间： ") + toSFString(t.c_str())
                : sf::String("Time: ") + toSFString(t.c_str());
        }
        else if (g_tm.active)
        {
            // timer mode: count + main time (IPS above the milestone bar)
            showMoves = true;  showTime = true;  showIps = false;
            countStr = (g_language == LANG_ZH)
                ? toSFString("次数： ") + sf::String(std::to_string(g_tm.moveCount))
                : sf::String("Moves: ") + sf::String(std::to_string(g_tm.moveCount));
            std::string t2 = chFormatTime(g_tm.elapsedMs);
            timeStr = (g_language == LANG_ZH)
                ? toSFString("时间： ") + toSFString(t2.c_str())
                : sf::String("Time: ") + toSFString(t2.c_str());
        }
        else
        {
            showMoves = g_aiShowMoves; showTime = g_aiShowTime; showIps = false;
            countStr = (g_language == LANG_ZH)
                ? toSFString("次数： ") + sf::String(std::to_string(g_aiMoveCount))
                : sf::String("Moves: ") + sf::String(std::to_string(g_aiMoveCount));
            int msT = (int)g_aiElapsedMs;
            int thh = msT / 3600000, tmm = (msT / 60000) % 60, tss = (msT / 1000) % 60, tms = msT % 1000;
            char tbuf[64];
            std::snprintf(tbuf, sizeof(tbuf), "%d:%02d:%02d:%03d", thh, tmm, tss, tms);
            timeStr = (g_language == LANG_ZH)
                ? toSFString("时间： ") + toSFString(tbuf)
                : sf::String("Time: ") + toSFString(tbuf);
        }

        // ==== stats row ====
        // non-timer mode (challenge/AI): count + time (1:1.618)
        // timer mode: count + time (1:1.618, total width = board width, IPS above the milestone bar)
        {
            bool both = showMoves && showTime;
            float countW = showMoves ? (both ? availW / 2.618f : availW) : 0.f;
            float timeW  = showTime  ? (both ? availW * 1.618f / 2.618f : availW) : 0.f;
            float countX = boardX;
            float timeX  = (showMoves && showTime) ? countX + countW + L.gapBtn : boardX;

            if (showMoves)
            {
                sf::RectangleShape countBox(sf::Vector2f(countW, statsBoxH));
                countBox.setPosition(countX, statsRowY);
                countBox.setFillColor(scheme.boardBg);
                window.draw(countBox);
            }
            if (showTime)
            {
                sf::RectangleShape timeBox(sf::Vector2f(timeW, statsBoxH));
                timeBox.setPosition(timeX, statsRowY);
                timeBox.setFillColor(scheme.boardBg);
                window.draw(timeBox);
            }

            unsigned int uniFont = (unsigned int)(statsBoxH * 0.42f);
            auto fitFont = [&](const sf::String& s, float boxW) -> unsigned int
            {
                sf::Text t(s, font, uniFont);
                t.setStyle(sf::Text::Bold);
                sf::FloatRect b = t.getLocalBounds();
                float targetW = boxW * 0.92f;
                unsigned int f = uniFont;
                if (b.width > targetW)
                {
                    f = (unsigned int)(uniFont * targetW / b.width);
                    if (f < 8) f = 8;
                }
                return f;
            };
            unsigned int sharedFont = 0;
            if (showMoves && showTime)
                sharedFont = std::min(fitFont(countStr, countW), fitFont(timeStr, timeW));
            else if (showMoves)
                sharedFont = fitFont(countStr, countW);
            else if (showTime)
                sharedFont = fitFont(timeStr, timeW);

            if (showMoves)
                drawTextFB(window, font, countStr,
                           countX + statsBoxH * 0.15f, statsRowY + statsBoxH / 2,
                           sharedFont, scheme.textDark, sf::Text::Bold, 0);
            if (showTime)
                drawTextFB(window, font, timeStr,
                           timeX + timeW / 2, statsRowY + statsBoxH / 2,
                           sharedFont, scheme.textDark, sf::Text::Bold, 1);
        }
    }

    // board vertical position: top bar + gapBtn
    float boardY = L.topBarY + L.topBarH + L.gapBtn;
    float tileSize = (boardPx - gap * (gridSize + 1)) / gridSize;

    // menu/undo button font size: matches the top AI stats row (count/time) font (scoreBoxH * 0.42)
    unsigned int labelFontSize = (unsigned int)(L.scoreBoxH * 0.42f);

    // === left box "Menu" (playback shows "previous step", challenge shows "settings") ===
    sf::RectangleShape leftBox(sf::Vector2f(L.leftBoxW, L.scoreBoxH));
    leftBox.setPosition(L.leftBoxX, L.leftBoxY);
    leftBox.setFillColor(scheme.boardBg);
    window.draw(leftBox);

    drawTextFB(window, font,
               g_pb.active ? tr("Prev", "上一步") : tr("Menu", "菜单"),
               L.leftBoxX + L.leftBoxW / 2, L.leftBoxY + L.scoreBoxH / 2,
               labelFontSize, scheme.textDark, 0, 1);

    // === middle box score ===
    sf::RectangleShape scoreBox(sf::Vector2f(L.scoreBoxW, L.scoreBoxH));
    scoreBox.setPosition(L.scoreBoxX, L.scoreBoxY);
    scoreBox.setFillColor(scheme.scoreBoxColor);

    if (!game.canMove())
    {
        scoreBox.setOutlineColor(sf::Color::Red);
        scoreBox.setOutlineThickness(L.scoreBoxH * 0.06f);
    }
    window.draw(scoreBox);

    // score bar font adaptation: 9 digits take 92% of the box width
    std::ostringstream ss;
    ss << displayScore;
    std::string scoreStr = ss.str();

    // estimate font size by height first
    unsigned int scoreFontSize = (unsigned int)(L.scoreBoxH * 0.55f);
    sf::Text scoreText(scoreStr, font, scoreFontSize);
    scoreText.setStyle(sf::Text::Bold);
    scoreText.setFillColor(sf::Color::White);

    // measure the width; shrink proportionally when it exceeds 92% of the box width
    sf::FloatRect stBounds = scoreText.getLocalBounds();
    float targetW = L.scoreBoxW * 0.92f;
    if (stBounds.width > targetW)
    {
        scoreFontSize = (unsigned int)(scoreFontSize * targetW / stBounds.width);
        scoreText = sf::Text(scoreStr, font, scoreFontSize);
        scoreText.setStyle(sf::Text::Bold);
        scoreText.setFillColor(sf::Color::White);
        stBounds = scoreText.getLocalBounds();
    }

    scoreText.setOrigin(stBounds.left + stBounds.width / 2, stBounds.top + stBounds.height / 2);
    scoreText.setPosition(L.scoreBoxX + L.scoreBoxW / 2, L.scoreBoxY + L.scoreBoxH / 2);
    window.draw(scoreText);

    // === right box "Undo" (playback shows "next step", challenge/timer shows "close") ===
    sf::RectangleShape rightBox(sf::Vector2f(L.rightBoxW, L.scoreBoxH));
    rightBox.setPosition(L.rightBoxX, L.rightBoxY);
    rightBox.setFillColor((g_pb.active || chActive || g_tm.active) ? scheme.boardBg : (game.canUndo() ? scheme.boardBg : sf::Color(200, 190, 180)));
    window.draw(rightBox);

    drawTextFB(window, font,
               g_pb.active ? tr("Next", "下一步")
               : ((chActive || g_tm.active) ? tr("Close", "关闭") : tr("Undo", "撤销")),
               L.rightBoxX + L.rightBoxW / 2, L.rightBoxY + L.scoreBoxH / 2,
               labelFontSize,
               (g_pb.active || chActive || g_tm.active) ? scheme.textDark
               : (game.canUndo() ? scheme.textDark : sf::Color(170, 160, 150)), 0, 1);

    // AI running: cyan board border warning
    if (g_aiMode == AI_RUNNING)
    {
        float bt = boardPx * 0.015f;
        sf::RectangleShape aiBorder(sf::Vector2f(boardPx + bt * 2, boardPx + bt * 2));
        aiBorder.setPosition(boardX - bt, boardY - bt);
        aiBorder.setFillColor(sf::Color(60, 232, 255)); // cyan
        window.draw(aiBorder);
    }

    // practice imported-history playback: the board outer border is framed with a thin color ring matching the current step quality (green/yellow-green/yellow/orange-red)
    if (g_practiceActive && g_practiceImport && g_pb.active)
    {
        const HistoryEntry* pe = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
            ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
        std::vector<int> qCopy;
        {
            std::lock_guard<std::mutex> lk(g_practiceQualityMutex);
            if (pe && g_practiceQualityReady && (int)g_practiceQuality.size() == pe->moveCount)
                qCopy = g_practiceQuality;
        }
        int f = (g_pb.appliedStep >= 0) ? g_pb.appliedStep + 1 : g_pb.curFrame;   // current displayed frame; its quality = qCopy[f-1]
        if (!qCopy.empty() && f >= 1 && f <= (int)qCopy.size())
        {
            float bt = std::max(1.f, boardPx * 0.004f);   // thin border (1/2 thinner)
            sf::RectangleShape qBorder(sf::Vector2f(boardPx + bt * 2.f, boardPx + bt * 2.f));
            qBorder.setPosition(boardX - bt, boardY - bt);
            qBorder.setFillColor(practiceQualityColor(qCopy[f - 1]));
            window.draw(qBorder);
        }
    }

    // === board background ===
    sf::RectangleShape boardBg(sf::Vector2f(boardPx, boardPx));
    boardBg.setPosition(boardX, boardY);
    boardBg.setFillColor(scheme.boardBg);
    window.draw(boardBg);

    // === draw all tiles ===
    // extract the common drawing function to avoid duplicated code
    auto drawTile = [&](float x, float y, float sz, int value, const sf::Color* overrideColor = nullptr)
    {
        float off = (tileSize - sz) / 2.f;
        sf::RectangleShape tile(sf::Vector2f(sz, sz));
        tile.setPosition(x + off, y + off);
        if (overrideColor)
            tile.setFillColor(*overrideColor);
        else if (viewing)
        {
            const SchemeData& vs = SCHEMES[game.getColorScheme()];
            if (value == 0) tile.setFillColor(vs.emptyColor);
            else if (isFillValue(value)) tile.setFillColor(sf::Color(70, 70, 78));   // fill (obstacle): dark gray
            else { int p = 0, v = value; while (v > 1) { v >>= 1; p++; }
                tile.setFillColor(p >= 1 && p <= MAX_POWER ? vs.tileColors[p] : vs.emptyColor); }
        }
        else
            tile.setFillColor(game.getTileColor(value));
        window.draw(tile);

        if (value == 0 || isFillValue(value)) return;   // fill (obstacle): no number shown
        std::ostringstream numSs;
        numSs << value;
        std::string numStr = numSs.str();
        sf::Color textColor;
        if (viewing)
        {
            const SchemeData& vs = SCHEMES[game.getColorScheme()];
            textColor = value >= vs.lightTextFromValue ? vs.textLight : vs.textDark;
        }
        else
            textColor = game.getTextColor(value);
        if (overrideColor) textColor.a = overrideColor->a;

        // unified number rules shared with mini/build boards (6+ digits wrap; 4 digits slightly smaller than 3)
        drawTileNumber(window, font, numStr, x + tileSize / 2.f, y + tileSize / 2.f, sz, textColor);
    };

    // history view mode: render the history board directly
    if (viewing)
    {
        for (int i = 0; i < gridSize; ++i)
        {
            for (int j = 0; j < gridSize; ++j)
            {
                float x = boardX + gap + j * (tileSize + gap);
                float y = boardY + gap + i * (tileSize + gap);
                int value = game.getViewTile(i, j);
                drawTile(x, y, tileSize, value);
            }
        }

        // hint text (hidden in playback mode; the control area below hints instead)
        if (!g_pb.active)
        {
            sf::Text hint(tr("History View - Press any key or click to exit",
                             "历史回看 - 按任意键或点击退出"), font, (unsigned int)(boardPx * 0.03f));
            hint.setFillColor(sf::Color(119, 110, 101, 180));
            sf::FloatRect hBounds = hint.getLocalBounds();
            hint.setOrigin(hBounds.left + hBounds.width / 2, hBounds.top + hBounds.height / 2);
            hint.setPosition((float)winW / 2, boardY + boardPx + pad * 0.5f);
            window.draw(hint);
        }
    }
    else if (game.isAnimating())
    {
        const auto& anims = game.getAnimations();

        // 1. draw all static tiles first (skip animation target positions)
        // collect all animation target positions
        bool isAnimDest[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = { false };
        for (const auto& anim : anims)
        {
            int dc = (int)(anim.toCol + 0.5f);
            int dr = (int)(anim.toRow + 0.5f);
            if (dc >= 0 && dc < gridSize && dr >= 0 && dr < gridSize)
                isAnimDest[dr][dc] = true;
        }
        // newly spawned tile positions are also skipped (drawn separately)
        int newR = game.getNewTileRow();
        int newC = game.getNewTileCol();
        if (newR >= 0 && newC >= 0)
            isAnimDest[newR][newC] = true;

        for (int i = 0; i < gridSize; ++i)
        {
            for (int j = 0; j < gridSize; ++j)
            {
                if (isAnimDest[i][j]) continue; // skip animation positions
                int value = game.getTile(i, j);
                float x = boardX + gap + j * (tileSize + gap);
                float y = boardY + gap + i * (tileSize + gap);
                drawTile(x, y, tileSize, value);
            }
        }

        // 2. draw sliding tiles
        for (const auto& anim : anims)
        {
            float t = anim.progress;
            float eased = 1.f - (1.f - t) * (1.f - t);
            float col = anim.fromCol + (anim.toCol - anim.fromCol) * eased;
            float row = anim.fromRow + (anim.toRow - anim.fromRow) * eased;

            float tx = boardX + gap + col * (tileSize + gap);
            float ty = boardY + gap + row * (tileSize + gap);
            float sz = tileSize * anim.scale;

            // tiles consumed in a merge fade out at the end
            if (anim.dead && t > 0.7f)
            {
                float fadeT = (t - 0.7f) / 0.3f;
                sf::Color c = game.getTileColor(anim.value);
                c.a = (sf::Uint8)(255 * (1.f - fadeT));
                drawTile(tx, ty, sz, anim.value, &c);
            }
            else
            {
                drawTile(tx, ty, sz, anim.value);
            }
        }

        // 3. draw newly spawned tiles (scaling in)
        if (newR >= 0 && newC >= 0)
        {
            int val = game.getTile(newR, newC);
            if (val > 0)
            {
                float x = boardX + gap + newC * (tileSize + gap);
                float y = boardY + gap + newR * (tileSize + gap);
                float spawnT = anims.empty() ? 1.f : anims[0].progress;
                if (spawnT > 1.0f) spawnT = 1.0f;
                float spawnScale = 0.3f + spawnT * 0.7f;
                drawTile(x, y, tileSize * spawnScale, val);
            }
        }
    }
    else
    {
        // static render
        for (int i = 0; i < gridSize; ++i)
        {
            for (int j = 0; j < gridSize; ++j)
            {
                float x = boardX + gap + j * (tileSize + gap);
                float y = boardY + gap + i * (tileSize + gap);
                int value = game.getTile(i, j);
                drawTile(x, y, tileSize, value);
            }
        }
    }

    // === practice-mode right analysis panel ===
    if (g_practiceActive)
        drawPracticePanel(window, game, font, boardX, boardY, boardPx, L.scoreBoxH, scheme);

    // === AI control buttons (AI settings / play-pause / cancel) ===
    // width:height = 4.6:1, group total width (incl. gaps) = board width; button height matches mobile (base x 1.4)
    // playback mode does not draw the AI buttons (the playback control area takes over below)
    if (aiActive && !g_pb.active)
    {
        // boardPx = baseBtnH * 4.6 * 3 + 2 * gapBtn
        float baseBtnH = (boardPx - 2.f * L.gapBtn) / (AI_BTN_W * 3.f);
        float aiBtnH = baseBtnH * 1.4f;          // height x1.4 (mobile-matched)
        float aiBtnW = baseBtnH * AI_BTN_W;      // width unchanged (group total still = board width)
        float aiBtnY = boardY + boardPx + L.gapBtn;
        float aiStartX = boardX;
        unsigned int aiFontSize = (unsigned int)(baseBtnH * 0.60f * 1.3f);  // text x1.3

        g_aiBtnPause  = sf::FloatRect(aiStartX, aiBtnY, aiBtnW, aiBtnH);           // AI settings
        g_aiBtnStart  = sf::FloatRect(aiStartX + aiBtnW + L.gapBtn, aiBtnY, aiBtnW, aiBtnH);  // play/pause
        g_aiBtnCancel = sf::FloatRect(aiStartX + (aiBtnW + L.gapBtn) * 2, aiBtnY, aiBtnW, aiBtnH);

        const sf::Color activeBlue(70, 130, 180);

        // (1) AI settings button (reworked from the old pause key)
        {
            sf::RectangleShape btn(sf::Vector2f(g_aiBtnPause.width, g_aiBtnPause.height));
            btn.setPosition(g_aiBtnPause.left, g_aiBtnPause.top);
            btn.setFillColor(g_aiSettingsOpen ? activeBlue : scheme.boardBg);
            window.draw(btn);
            drawTextFB(window, font, tr("AI Settings", "AI 设置"),
                       g_aiBtnPause.left + g_aiBtnPause.width / 2, g_aiBtnPause.top + g_aiBtnPause.height / 2,
                       aiFontSize, g_aiSettingsOpen ? sf::Color::White : scheme.textDark, 0, 1);
        }
        // (2) play/pause button (triangle <-> double bar)
        {
            sf::RectangleShape btn(sf::Vector2f(g_aiBtnStart.width, g_aiBtnStart.height));
            btn.setPosition(g_aiBtnStart.left, g_aiBtnStart.top);
            bool running = (g_aiMode == AI_RUNNING);
            btn.setFillColor(running ? activeBlue : scheme.boardBg);
            window.draw(btn);
            float cx = g_aiBtnStart.left + g_aiBtnStart.width / 2;
            float cy = g_aiBtnStart.top + g_aiBtnStart.height / 2;
            sf::Color iconCol = running ? sf::Color::White : scheme.textDark;
            if (running)
            {
                // paused: two vertical bars (player pause icon)
                float barW = aiBtnH * 0.17f, barH = aiBtnH * 0.46f, barGap = aiBtnH * 0.24f;
                sf::RectangleShape bar1(sf::Vector2f(barW, barH));
                bar1.setPosition(cx - barGap - barW / 2, cy - barH / 2);
                bar1.setFillColor(iconCol);
                window.draw(bar1);
                sf::RectangleShape bar2(sf::Vector2f(barW, barH));
                bar2.setPosition(cx + barGap - barW / 2, cy - barH / 2);
                bar2.setFillColor(iconCol);
                window.draw(bar2);
            }
            else
            {
                // playing: triangle (player play icon)
                float halfH = aiBtnH * 0.30f, halfW = halfH * 0.90f;
                sf::ConvexShape tri(3);
                tri.setPoint(0, sf::Vector2f(cx + halfW, cy));
                tri.setPoint(1, sf::Vector2f(cx - halfW, cy - halfH));
                tri.setPoint(2, sf::Vector2f(cx - halfW, cy + halfH));
                tri.setFillColor(iconCol);
                window.draw(tri);
            }
        }
        // (3) cancel button
        {
            sf::RectangleShape btn(sf::Vector2f(g_aiBtnCancel.width, g_aiBtnCancel.height));
            btn.setPosition(g_aiBtnCancel.left, g_aiBtnCancel.top);
            btn.setFillColor(scheme.boardBg);
            window.draw(btn);
            drawTextFB(window, font, tr("Cancel", "取消"),
                       g_aiBtnCancel.left + g_aiBtnCancel.width / 2, g_aiBtnCancel.top + g_aiBtnCancel.height / 2,
                       aiFontSize, scheme.textDark, 0, 1);
        }
    }

    // === history playback control area (below the board: track + time rect + three function keys) ===
    if (g_pb.active)
        drawPlaybackControls(window, game, font, boardX, boardY, boardPx, L.gapBtn, scheme);
    // === playback settings dialog (original step / fixed step / close) ===
    if (g_pb.active && g_pb.settingsOpen)
        drawPlaybackSettings(window, game, font, boardX, boardY, boardPx, scheme);

    // === timer/AI mode: milestone slider bar (right of the board; window auto-widened) ===
    if (milestoneVisible())
        drawTimerMilestones(window, game, font, boardX, boardY, boardPx, scheme, L.scoreBoxH, L.gapBtn,
                            offsetY, L.rightBoxY);

    // === challenge project settings dialog (opened by "settings" in challenge mode) ===
    if (chActive && g_ch.settingsOpen)
        drawChallengeSettings(window, game, font, boardX, boardY, boardPx, scheme);
    // === challenge result dialog (success/fail; drawn last, on top) ===
    if (chActive && g_ch.finished)
        drawChallengeResult(window, game, font);
    // === timer-mode result dialog (game over, drawn last on top; timer mode stays after closing) ===
    if (g_tm.active && g_tm.finished && !g_tm.resultDismissed)
        drawTimerResult(window, game, font);

    // === AI settings dialog (opened by the AI settings button) ===
    // width = board width, height = 1/2 board, bottom aligned with the board bottom
    if (aiActive && g_aiSettingsOpen)
    {
        float popW = boardPx;
        float popH = boardPx * 0.42f;
        float popX = boardX;
        float popY = boardY + boardPx - popH;   // bottom aligned with the board bottom

        // semi-transparent overlay (click outside to close)
        sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
        overlay.setFillColor(sf::Color(0, 0, 0, 120));
        window.draw(overlay);

        sf::RectangleShape pop(sf::Vector2f(popW, popH));
        pop.setPosition(popX, popY);
        pop.setFillColor(scheme.windowBg);
        pop.setOutlineColor(sf::Color(70, 130, 180));
        pop.setOutlineThickness(3.f);
        window.draw(pop);

        const float padX = popW * 0.06f;
        const float valueW = popW * 0.13f;
        const unsigned int titleFont = (unsigned int)(boardPx * 0.050f);   // matches the menu button font
        const unsigned int lblFont   = (unsigned int)(boardPx * 0.050f);   // matches the menu button font
        const unsigned int valFont   = (unsigned int)(boardPx * 0.048f);
        const float commonBtnH = popH * 0.16f;   // shared height for tier keys / display keys

        // (1) rate title row (same left edge as "compute depth", moved down toward the slider)
        float rateTitleY = popY + popH * 0.16f;
        drawTextFB(window, font,
                   (g_language == LANG_ZH) ? toSFString("AI 操作速度") : sf::String("AI Speed"),
                   popX + padX, rateTitleY, titleFont, scheme.textDark, sf::Text::Bold, 0);

        // (1)b thin rect frame: frames the title + slider (rows 1+2 area; bottom edge 0.45 covers the slider knob)
        {
            float boxX = popX + padX * 0.5f;
            float boxY = popY + popH * 0.04f;
            float boxW = popW - padX;
            float boxH = popH * 0.45f - popH * 0.04f;
            sf::RectangleShape box(sf::Vector2f(boxW, boxH));
            box.setPosition(boxX, boxY);
            box.setFillColor(sf::Color::Transparent);
            box.setOutlineColor(sf::Color(70, 130, 180, 120));
            box.setOutlineThickness(2.5f);
            window.draw(box);
        }

        // (2) rate slider row (longer track, knob 25% smaller than before)
        {
            float rowC = popY + popH * 0.375f;   // 4 equal rows: row 2 = slider
            float trackX = popX + padX;
            float trackMaxX = popX + popW - padX - valueW;
            float trackW = trackMaxX - trackX;
            float trackH = std::max(2.f, popH * 0.045f * 0.5f);   // track 1/2 thinner
            float knobR = popH * 0.0675f * 0.75f;                // solid knob 25% smaller
            float knobMin = trackX + knobR;
            float knobMax = trackMaxX - knobR;
            if (knobMax < knobMin) knobMax = knobMin;
            // taller hit area (the track itself is thin; the click/drag detection zone is larger for easier picking)
            float hitH = std::max(trackH * 3.f, popH * 0.15f);
            g_aiRateTrack = sf::FloatRect(trackX, rowC - hitH / 2.f, trackW, hitH);
            g_aiRateKnobMinCX = knobMin;
            g_aiRateKnobMaxCX = knobMax;

            sf::RectangleShape track(sf::Vector2f(trackW, trackH));
            track.setPosition(trackX, rowC - trackH / 2.f);
            track.setFillColor(sf::Color(119, 110, 101, 90));
            window.draw(track);

            float rateT = (float)(g_aiRate - AI_RATE_MIN) / (float)(AI_RATE_MAX - AI_RATE_MIN);
            float knobCX = knobMin + rateT * (knobMax - knobMin);
            float fillW = knobCX - trackX;
            if (fillW > 0.f)
            {
                sf::RectangleShape fill(sf::Vector2f(fillW, trackH));
                fill.setPosition(trackX, rowC - trackH / 2.f);
                fill.setFillColor(sf::Color(70, 130, 180));
                window.draw(fill);
            }
            sf::CircleShape knob(knobR);
            knob.setFillColor(sf::Color(70, 130, 180));
            knob.setOrigin(knobR, knobR);
            knob.setPosition(knobCX, rowC);
            window.draw(knob);

            // show only the number (no "per second" unit)
            drawTextFB(window, font, sf::String(std::to_string(g_aiRate)),
                       trackMaxX + valueW, rowC, valFont, sf::Color(70, 130, 180), sf::Text::Bold, 2);
        }

        // (3) intelligence tier: search depth {3,5,8} (effective in both engine modes)
        {
            float rowC = popY + popH * 0.625f;   // 4 equal rows: row 3 = compute depth
            const char* lvlName[3]   = { "一档", "二档", "三档" };
            const char* lvlNameEn[3] = { "Lv.1", "Lv.2", "Lv.3" };
            float btnH = commonBtnH * 0.75f * 1.2f;            // height = 1.2x the original
            float btnW = commonBtnH * 0.75f * 1.618f * 1.10f * 1.6f;   // width = 1.6x the original
            float gap = popW * 0.03f;
            sf::String lvLabel = (g_language == LANG_ZH) ? toSFString("计算深度") : sf::String("AI Level");
            // title left-aligned (same left edge popX+padX as the "AI operation speed" title)
            drawTextFB(window, font, lvLabel, popX + padX, rowC, lblFont,
                       scheme.textDark, sf::Text::Bold, 0);
            float btnY = rowC - btnH / 2.f;
            unsigned int btnFont = (unsigned int)(popH * 0.10f);   // smaller font
            // tier keys right edge aligns with the dialog right edge (not too far right; no overlap with the left title)
            float totalW = 3.f * btnW + 2.f * gap;
            float btnX = popX + popW - padX - totalW;
            for (int i = 0; i < 3; ++i)
            {
                float x = btnX + i * (btnW + gap);
                g_aiLevelBtn[i] = sf::FloatRect(x, btnY, btnW, btnH);
                bool sel = (g_aiLevel == i);
                sf::RectangleShape b(sf::Vector2f(btnW, btnH));
                b.setPosition(x, btnY);
                b.setFillColor(sel ? sf::Color(70, 130, 180) : scheme.boardBg);
                window.draw(b);
                drawTextFB(window, font,
                           (g_language == LANG_ZH) ? toSFString(lvlName[i]) : sf::String(lvlNameEn[i]),
                           x + btnW / 2, rowC, btnFont,
                           sel ? sf::Color::White : scheme.textDark, 0, 1);
            }
        }

        // (4) display/undo row: three function keys (show count / show time / AI uses undo)
        //    the three keys split the length evenly, the whole row centered; blue when on
        {
            float rowC = popY + popH * 0.875f;   // 4 equal rows: row 4 = three function keys
            float sBtnH = commonBtnH;
            float sGap = popW * 0.04f;
            float sBtnW = (popW - 2.f * padX - 2.f * sGap) / 3.f;   // three keys evenly split
            float sTotalW = 3.f * sBtnW + 2.f * sGap;
            float sX0 = popX + (popW - sTotalW) / 2.f;
            float sY = rowC - sBtnH / 2.f;
            unsigned int sFont = (unsigned int)(popH * 0.10f);   // smaller font

            g_aiShowMovesBtn = sf::FloatRect(sX0, sY, sBtnW, sBtnH);
            g_aiShowTimeBtn  = sf::FloatRect(sX0 + (sBtnW + sGap), sY, sBtnW, sBtnH);
            g_aiUseUndoBtn   = sf::FloatRect(sX0 + 2.f * (sBtnW + sGap), sY, sBtnW, sBtnH);

            auto drawShowBtn = [&](const sf::FloatRect& r, const char* zh, const char* en, bool on)
            {
                sf::RectangleShape b(sf::Vector2f(r.width, r.height));
                b.setPosition(r.left, r.top);
                b.setFillColor(on ? sf::Color(70, 130, 180) : scheme.boardBg);
                window.draw(b);
                drawTextFB(window, font,
                           (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                           r.left + r.width / 2, rowC, sFont,
                           on ? sf::Color::White : scheme.textDark, 0, 1);
            };
            drawShowBtn(g_aiShowMovesBtn, "显示次数", "Show Moves", g_aiShowMoves);
            drawShowBtn(g_aiShowTimeBtn,  "显示时间", "Show Time",  g_aiShowTime);
            drawShowBtn(g_aiUseUndoBtn,   "AI使用撤回", "Use Undo", g_aiUseUndo);
        }





























    }

    // === menu dialog ===
    if (menuOpen)
    {
        float menuW, menuH, menuX, menuY, btnH, btnGap, btnStartY;
        computeMenuLayout(winW, winH, menuW, menuH, menuX, menuY, btnH, btnGap, btnStartY);

        sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
        overlay.setFillColor(sf::Color(0, 0, 0, 180));
        window.draw(overlay);

        sf::RectangleShape menuBg(sf::Vector2f(menuW, menuH));
        menuBg.setPosition(menuX, menuY);
        menuBg.setFillColor(scheme.windowBg);
        menuBg.setOutlineColor(scheme.boardBg);
        menuBg.setOutlineThickness(3.f);
        window.draw(menuBg);

        unsigned int menuFontSize = (unsigned int)(menuH * 0.045f);

        // menu title
        drawTextFB(window, font, tr("Settings", "设置"),
                   menuX + menuW / 2, menuY + menuH * 0.06f,
                   (unsigned int)(menuH * 0.08f), scheme.textDark, sf::Text::Bold, 1);

        // color schemes + challenge 2048 + timer mode button group (vertically centered between the "settings" title and the "board size" label)
        float titleBottom = menuY + menuH * 0.10f;                     // settings title bottom
        float sizeLabelY  = btnStartY + 2 * btnH + 3 * btnGap;          // board size label position
        float btnGroupH   = 3 * btnH + 2 * btnGap;
        float btnGroupTop = (titleBottom + sizeLabelY) / 2.f - btnGroupH / 2.f;

        // color scheme button (centered, moved up)
        float colorBtnW = menuW * 0.55f;
        float colorBtnH = btnH;
        float colorBtnX = menuX + (menuW - colorBtnW) / 2.f;
        float colorBtnY = btnGroupTop;
        sf::RectangleShape colorBtn(sf::Vector2f(colorBtnW, colorBtnH));
        colorBtn.setPosition(colorBtnX, colorBtnY);
        colorBtn.setFillColor(scheme.boardBg);
        colorBtn.setOutlineColor(scheme.boardBg);
        colorBtn.setOutlineThickness(1.f);
        window.draw(colorBtn);
        drawTextFB(window, font, tr("Color Scheme", "配色方案"),
                   colorBtnX + colorBtnW / 2, colorBtnY + colorBtnH / 2,
                   menuFontSize, scheme.textDark, 0, 1);

        // challenge 2048 button (centered, below the color schemes)
        float chBtnW = menuW * 0.55f;
        float chBtnH = btnH;
        float chBtnX = menuX + (menuW - chBtnW) / 2.f;
        float chBtnY = colorBtnY + colorBtnH + btnGap;
        bool chDisabled = g_practiceActive || g_tm.active;
        sf::RectangleShape chBtn(sf::Vector2f(chBtnW, chBtnH));
        chBtn.setPosition(chBtnX, chBtnY);
        chBtn.setFillColor(chDisabled ? sf::Color(140, 140, 140) : scheme.boardBg);
        window.draw(chBtn);
        drawTextFB(window, font, tr("Challenge 2048", "挑战2048"),
                   chBtnX + chBtnW / 2, chBtnY + chBtnH / 2,
                   menuFontSize,
                   chDisabled ? sf::Color(90, 90, 90) : scheme.textDark, 0, 1);

        // timer mode button (centered, below challenge 2048; click to enter timer mode)
        float tmBtnW = menuW * 0.55f;
        float tmBtnH = btnH;
        float tmBtnX = menuX + (menuW - tmBtnW) / 2.f;
        float tmBtnY = chBtnY + chBtnH + btnGap;
        bool tmDisabled = g_practiceActive || chActive;
        g_menuTimerBtn = sf::FloatRect(tmBtnX, tmBtnY, tmBtnW, tmBtnH);
        sf::RectangleShape tmBtn(sf::Vector2f(tmBtnW, tmBtnH));
        tmBtn.setPosition(tmBtnX, tmBtnY);
        tmBtn.setFillColor(tmDisabled ? sf::Color(140, 140, 140) : scheme.boardBg);
        window.draw(tmBtn);
        drawTextFB(window, font, tr("Timer Mode", "计时模式"),
                   tmBtnX + tmBtnW / 2, tmBtnY + tmBtnH / 2,
                   menuFontSize,
                   tmDisabled ? sf::Color(90, 90, 90) : scheme.textDark, 0, 1);

        // board size control
        drawTextFB(window, font, tr("Board Size", "棋盘大小"),
                   menuX + menuW / 2, sizeLabelY,
                   menuFontSize, scheme.textDark, 0, 1);

        // three board-size buttons: 3x3 / 4x4 / 5x5, equal width, centered; the current size is highlighted
        float sizeBtnY = sizeLabelY + btnGap * 2;
        float sizeBtnW = menuW * 0.26f;
        float sizeBtnGap = menuW * 0.04f;
        float sizeBtnStartX = menuX + menuW * 0.07f;  // 7% symmetric margin on both sides
        int curSize = game.getBoardSize();
        for (int i = 0; i < 3; ++i)
        {
            int sz = 3 + i;  // 3, 4, 5
            float bx = sizeBtnStartX + i * (sizeBtnW + sizeBtnGap);
            bool active = (sz == curSize);

            sf::RectangleShape btn(sf::Vector2f(sizeBtnW, btnH));
            btn.setPosition(bx, sizeBtnY);
            btn.setFillColor(active ? scheme.scoreBoxColor : scheme.boardBg);
            window.draw(btn);

            std::ostringstream ss;
            ss << sz << "x" << sz;
            sf::Text t(ss.str(), font, menuFontSize);
            t.setStyle(sf::Text::Bold);
            t.setFillColor(active ? sf::Color::White : scheme.textDark);
            sf::FloatRect b = t.getLocalBounds();
            t.setOrigin(b.left + b.width / 2, b.top + b.height / 2);
            t.setPosition(bx + sizeBtnW / 2, sizeBtnY + btnH / 2);
            window.draw(t);
        }

        // restart button
        float restartBtnW = menuW * 0.4f;
        float restartBtnX = menuX + (menuW - restartBtnW) / 2.f;
        float restartBtnY = sizeBtnY + btnH + btnGap * 2;
        sf::RectangleShape restartBtn(sf::Vector2f(restartBtnW, btnH));
        restartBtn.setPosition(restartBtnX, restartBtnY);
        restartBtn.setFillColor(scheme.boardBg);
        window.draw(restartBtn);

        sf::Text restartText(tr("Restart", "重新开始"), font, menuFontSize);
        restartText.setFillColor(scheme.textDark);
        drawTextFB(window, font, tr("Restart", "重新开始"),
                   restartBtnX + restartBtnW / 2, restartBtnY + btnH / 2,
                   menuFontSize, scheme.textDark, 0, 1);

        // AI operation + practice buttons (two keys on one row, same width as history; AI operation two-state colors match undo)
        float aiOpGap = menuW * 0.04f;
        float aiOpBtnW = menuW * 0.4f;   // same width as the "history" button
        float aiOpBtnX = menuX + (menuW - (2.f * aiOpBtnW + aiOpGap)) / 2.f;
        float aiOpBtnY = restartBtnY + btnH + btnGap;
        bool aiAvail = (g_aiMode == AI_INACTIVE) && !g_tm.active;   // greyed out while timer mode is active
        sf::RectangleShape aiOpBtn(sf::Vector2f(aiOpBtnW, btnH));
        aiOpBtn.setPosition(aiOpBtnX, aiOpBtnY);
        aiOpBtn.setFillColor(aiAvail ? sf::Color(70, 130, 180) : sf::Color(200, 190, 180));
        window.draw(aiOpBtn);
        drawTextFB(window, font, tr("AI Operation", "AI 操作"),
                   aiOpBtnX + aiOpBtnW / 2, aiOpBtnY + btnH / 2,
                   menuFontSize, aiAvail ? sf::Color::White : sf::Color(170, 160, 150), 0, 1);
        // prediction button (blue when practice mode is on; greyed out while timer/challenge is active)
        float practiceBtnX = aiOpBtnX + aiOpBtnW + aiOpGap;
        bool practiceDisabled = g_tm.active || chActive;
        sf::RectangleShape practiceBtn(sf::Vector2f(aiOpBtnW, btnH));
        practiceBtn.setPosition(practiceBtnX, aiOpBtnY);
        practiceBtn.setFillColor(g_practiceActive ? sf::Color(70, 130, 180)
                                : (practiceDisabled ? sf::Color(200, 190, 180) : scheme.boardBg));
        window.draw(practiceBtn);
        g_practiceBtn = sf::FloatRect(practiceBtnX, aiOpBtnY, aiOpBtnW, btnH);
        drawTextFB(window, font, tr("Predict", "预测"),
                   practiceBtnX + aiOpBtnW / 2, aiOpBtnY + btnH / 2,
                   menuFontSize,
                   g_practiceActive ? sf::Color::White
                   : (practiceDisabled ? sf::Color(170, 160, 150) : scheme.textDark), 0, 1);
        // history button
        float histBtnW = menuW * 0.4f;
        float histBtnX = menuX + (menuW - histBtnW) / 2.f;
        float histBtnY = aiOpBtnY + btnH + btnGap;
        sf::RectangleShape histBtn(sf::Vector2f(histBtnW, btnH));
        histBtn.setPosition(histBtnX, histBtnY);
        histBtn.setFillColor(scheme.boardBg);
        window.draw(histBtn);

        sf::Text histText(tr("History", "历史记录"), font, menuFontSize);
        histText.setFillColor(scheme.textDark);
        drawTextFB(window, font, tr("History", "历史记录"),
                   histBtnX + histBtnW / 2, histBtnY + btnH / 2,
                   menuFontSize, scheme.textDark, 0, 1);

        // language toggle button: shows the other language name (click to switch)
        float langBtnW = menuW * 0.4f;
        float langBtnX = menuX + (menuW - langBtnW) / 2.f;
        float langBtnY = histBtnY + btnH + btnGap;
        sf::RectangleShape langBtn(sf::Vector2f(langBtnW, btnH));
        langBtn.setPosition(langBtnX, langBtnY);
        langBtn.setFillColor(scheme.boardBg);
        window.draw(langBtn);

        sf::Text langText((g_language == LANG_ZH) ? sf::String("English") : toSFString("中文"),
                          font, menuFontSize);
        langText.setFillColor(scheme.textDark);
        drawTextFB(window, font,
                   (g_language == LANG_ZH) ? sf::String("English") : toSFString("中文"),
                   langBtnX + langBtnW / 2, langBtnY + btnH / 2,
                   menuFontSize, scheme.textDark, 0, 1);

        // about button (opens the technical document)
        float closeBtnW = menuW * 0.4f;
        float closeBtnH = btnH;
        float closeBtnX = menuX + (menuW - closeBtnW) / 2.f;
        float closeBtnY = menuY + menuH - closeBtnH - btnGap * 1.0f;
        sf::RectangleShape closeBtn(sf::Vector2f(closeBtnW, closeBtnH));
        closeBtn.setPosition(closeBtnX, closeBtnY);
        closeBtn.setFillColor(scheme.scoreBoxColor);
        window.draw(closeBtn);

        sf::Text closeText(tr("About", "关于"), font, menuFontSize);
        closeText.setFillColor(scheme.textDark);
        drawTextFB(window, font, tr("About", "关于"),
                   closeBtnX + closeBtnW / 2, closeBtnY + closeBtnH / 2,
                   menuFontSize, scheme.textDark, 0, 1);
    }
}

// ========== Confirm dialog rendering ==========
void drawConfirmDialog(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                       const char* en, const char* zh)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const SchemeData& scheme = game.getScheme();

    float dlgW = winW * 0.60f;
    float dlgH = winH * 0.32f;
    // minimum size cap, prevents being too small
    if (dlgW < 320.f) dlgW = 320.f;
    if (dlgH < 180.f) dlgH = 180.f;
    float dlgX = (winW - dlgW) / 2.f;
    float dlgY = (winH - dlgH) / 2.f;

    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 200));
    window.draw(overlay);

    sf::RectangleShape dlgBg(sf::Vector2f(dlgW, dlgH));
    dlgBg.setPosition(dlgX, dlgY);
    dlgBg.setFillColor(scheme.windowBg);
    dlgBg.setOutlineColor(scheme.boardBg);
    dlgBg.setOutlineThickness(3.f);
    window.draw(dlgBg);

    // adaptive font size: based on the dialog available height
    float availH = dlgH * 0.6f;  // text area available height
    float maxFontByH = availH * 0.25f;
    float maxFontByW = dlgW * 0.06f;
    float msgFontSize = std::min(maxFontByH, maxFontByW);
    if (msgFontSize > 48.f) msgFontSize = 48.f;
    if (msgFontSize < 14.f) msgFontSize = 14.f;

    // pick the message text in the current language
    std::string msg = (g_language == LANG_ZH) ? std::string(zh) : std::string(en);
    bool isZh = (g_language == LANG_ZH);
    float maxTextW = dlgW * 0.82f;
    std::vector<std::string> lines;

    // Chinese chars are full-width (width ~ font size); English chars ~ 0.55 font size
    float charW = isZh ? (msgFontSize * 1.0f) : (msgFontSize * 0.55f);
    if (charW <= 0) charW = 1.f;
    int maxCharsPerLine = (int)(maxTextW / charW);
    if (maxCharsPerLine < 4) maxCharsPerLine = 4;

    if (isZh)
    {
        // Chinese wraps by character (UTF-8 char boundaries)
        size_t pos = 0;
        while (pos < msg.size())
        {
            size_t end = pos;
            int cnt = 0;
            while (end < msg.size() && cnt < maxCharsPerLine)
            {
                size_t next = end + 1;
                while (next < msg.size() && (msg[next] & 0xC0) == 0x80) ++next;
                end = next;
                ++cnt;
            }
            lines.push_back(msg.substr(pos, end - pos));
            pos = end;
            if (lines.size() > 10) break;
        }
    }
    else
    {
        // English wraps by word
        size_t pos = 0;
        while (pos < msg.size())
        {
            size_t endPos = pos + maxCharsPerLine;
            if (endPos >= msg.size())
            {
                lines.push_back(msg.substr(pos));
                break;
            }
            size_t brkPos = msg.rfind("?.! ", endPos);
            if (brkPos == std::string::npos || brkPos <= pos)
                brkPos = msg.rfind(' ', endPos);
            if (brkPos == std::string::npos || brkPos <= pos)
                brkPos = endPos;
            else if (msg[brkPos] == ' ') brkPos--;
            lines.push_back(msg.substr(pos, brkPos - pos + 1));
            pos = brkPos + 1;
            if (lines.size() > 10)
            {
                lines.push_back(msg.substr(pos));
                break;
            }
        }
    }

    int lineCount = (int)lines.size();
    if (lineCount == 0) { lines.push_back(msg); lineCount = 1; }

    // compute the total text height and center vertically
    float lineH = msgFontSize * 1.35f;
    float totalTextH = lineH * lineCount;
    float textY = dlgY + dlgH * 0.38f - totalTextH * 0.5f + lineH * 0.5f;
    if (textY < dlgY + 10.f) textY = dlgY + 10.f;

    for (int i = 0; i < lineCount; ++i)
    {
        // Chinese lines need toSFString to display correctly as sf::String
        sf::String lineStr = isZh ? toSFString(lines[i].c_str()) : sf::String(lines[i]);
        drawTextFB(window, font, lineStr,
                   dlgX + dlgW / 2, textY + i * lineH,
                   (unsigned int)msgFontSize, scheme.textDark, sf::Text::Bold, 1);
    }

    // button area: adaptive size, guaranteed not squeezed
    float btnW = dlgW * 0.25f;
    if (btnW < 70.f) btnW = 70.f;
    if (btnW > 140.f) btnW = 140.f;
    float btnH = dlgH * 0.18f;
    if (btnH < 36.f) btnH = 36.f;
    if (btnH > 60.f) btnH = 60.f;
    float btnGap = dlgW * 0.08f;
    if (btnGap < 12.f) btnGap = 12.f;
    float btnY = dlgY + dlgH - btnH - dlgH * 0.12f;
    float totalBtnsW = btnW * 2 + btnGap;
    float startX = dlgX + (dlgW - totalBtnsW) / 2.f;

    float btnFontSize = msgFontSize * 0.85f;
    if (btnFontSize < 12.f) btnFontSize = 12.f;

    // Yes button
    sf::RectangleShape yesBtn(sf::Vector2f(btnW, btnH));
    yesBtn.setPosition(startX, btnY);
    yesBtn.setFillColor(scheme.scoreBoxColor);
    window.draw(yesBtn);
    drawTextFB(window, font, tr("Yes", "是"),
               startX + btnW / 2, btnY + btnH / 2,
               (unsigned int)btnFontSize, scheme.textDark, 0, 1);

    // No button
    sf::RectangleShape noBtn(sf::Vector2f(btnW, btnH));
    noBtn.setPosition(startX + btnW + btnGap, btnY);
    noBtn.setFillColor(scheme.boardBg);
    window.draw(noBtn);
    drawTextFB(window, font, tr("No", "否"),
               startX + btnW + btnGap + btnW / 2, btnY + btnH / 2,
               (unsigned int)btnFontSize, scheme.textDark, 0, 1);
}

// ========== Font dialog rendering ==========
struct FontDialogState {
    bool open;
    int scrollPos;
    int selectedIdx;
    int hoverIdx;
    float itemH;
    float listAreaX, listAreaY, listAreaW, listAreaH;
    float barX, barY, barW, barH;
    float thumbY, thumbH;
    float applyBtnX, applyBtnY, applyBtnW, applyBtnH;
    float cancelBtnX, cancelBtnY, cancelBtnW, cancelBtnH;
    float resetBtnX, resetBtnY, resetBtnW, resetBtnH;
    float prevBtnX, prevBtnY, prevBtnW, prevBtnH;
};

void drawFontDialog(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                    FontDialogState& dlg)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const SchemeData& scheme = game.getScheme();

    float dlgW = winW * 0.85f;
    float dlgH = winH * 0.90f;
    float dlgX = (winW - dlgW) / 2.f;
    float dlgY = (winH - dlgH) / 2.f;

    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 200));
    window.draw(overlay);

    sf::RectangleShape dlgBg(sf::Vector2f(dlgW, dlgH));
    dlgBg.setPosition(dlgX, dlgY);
    dlgBg.setFillColor(scheme.windowBg);
    dlgBg.setOutlineColor(scheme.boardBg);
    dlgBg.setOutlineThickness(3.f);
    window.draw(dlgBg);

    unsigned int titleSize = (unsigned int)(dlgH * 0.06f);
    float headerY = dlgY + dlgH * 0.05f;
    float headerH = dlgH * 0.06f;
    float listAreaRight = dlgX + dlgW * 0.86f;

    // button + title on the same row
    float smallBtnW = dlgW * 0.14f;
    float smallBtnH = dlgH * 0.045f;
    float smallBtnGap = dlgW * 0.025f;
    float headerBtnStartX = dlgX + dlgW * 0.08f;

    dlg.resetBtnX = headerBtnStartX;
    dlg.resetBtnY = headerY + (headerH - smallBtnH) / 2.f;
    dlg.resetBtnW = smallBtnW;
    dlg.resetBtnH = smallBtnH;

    sf::RectangleShape resetBtn(sf::Vector2f(smallBtnW, smallBtnH));
    resetBtn.setPosition(dlg.resetBtnX, dlg.resetBtnY);
    resetBtn.setFillColor(scheme.boardBg);
    window.draw(resetBtn);

    drawTextFB(window, font, tr("Default", "默认"),
               dlg.resetBtnX + smallBtnW / 2, dlg.resetBtnY + smallBtnH / 2,
               (unsigned int)(smallBtnH * 0.5f), scheme.textDark, 0, 1);

    dlg.prevBtnX = headerBtnStartX + smallBtnW + smallBtnGap;
    dlg.prevBtnY = headerY + (headerH - smallBtnH) / 2.f;
    dlg.prevBtnW = smallBtnW;
    dlg.prevBtnH = smallBtnH;

    sf::RectangleShape prevBtn(sf::Vector2f(smallBtnW, smallBtnH));
    prevBtn.setPosition(dlg.prevBtnX, dlg.prevBtnY);
    prevBtn.setFillColor(scheme.boardBg);
    window.draw(prevBtn);

    drawTextFB(window, font, tr("Previous", "上一个"),
               dlg.prevBtnX + smallBtnW / 2, dlg.prevBtnY + smallBtnH / 2,
               (unsigned int)(smallBtnH * 0.5f), scheme.textDark, 0, 1);

    // title right-aligned with the list area right edge
    drawTextFB(window, font, tr("Select Font", "选择字体"),
               listAreaRight, headerY + headerH / 2.f,
               titleSize, scheme.textDark, sf::Text::Bold, 2);

    float listAreaX = dlgX + dlgW * 0.08f;
    float listAreaY = dlgY + dlgH * 0.12f;
    float listAreaW = dlgW * 0.78f;
    float listAreaH = dlgH * 0.60f;
    float barW = 10.f;

    dlg.listAreaX = listAreaX;
    dlg.listAreaY = listAreaY;
    dlg.listAreaW = listAreaW;
    dlg.listAreaH = listAreaH;

    sf::RectangleShape listBg(sf::Vector2f(listAreaW, listAreaH));
    listBg.setPosition(listAreaX, listAreaY);
    listBg.setFillColor(scheme.scoreBoxColor);
    listBg.setOutlineColor(scheme.boardBg);
    listBg.setOutlineThickness(1.f);
    window.draw(listBg);

    dlg.itemH = listAreaH / 12.f;
    int totalItems = (int)g_systemFonts.size();
    int visibleCount = (int)(listAreaH / dlg.itemH);
    if (visibleCount > totalItems) visibleCount = totalItems;
    if (dlg.scrollPos > totalItems - visibleCount)
        dlg.scrollPos = std::max(0, totalItems - visibleCount);

    dlg.barX = listAreaX + listAreaW + 2.f;
    dlg.barY = listAreaY;
    dlg.barW = barW;
    dlg.barH = listAreaH;

    bool needScrollbar = totalItems > visibleCount;
    float itemAreaW = needScrollbar ? listAreaW - (barW + 4.f) : listAreaW;

    if (needScrollbar)
    {
        float thumbH = listAreaH * visibleCount / totalItems;
        float maxScroll = (float)(totalItems - visibleCount);
        dlg.thumbH = thumbH;
        dlg.thumbY = listAreaY + (listAreaH - thumbH) * dlg.scrollPos / maxScroll;

        sf::RectangleShape bar(sf::Vector2f(barW, listAreaH));
        bar.setPosition(dlg.barX, dlg.barY);
        bar.setFillColor(scheme.boardBg);
        window.draw(bar);

        sf::RectangleShape thumb(sf::Vector2f(barW, thumbH));
        thumb.setPosition(dlg.barX, dlg.thumbY);
        thumb.setFillColor(scheme.textDark);
        window.draw(thumb);
    }

    int startIdx = dlg.scrollPos;
    int endIdx = std::min(startIdx + visibleCount, totalItems);
    for (int i = startIdx; i < endIdx; ++i)
    {
        float itemY = listAreaY + (i - startIdx) * dlg.itemH;
        bool selected = (i == dlg.selectedIdx);
        bool hover = (i == dlg.hoverIdx);

        sf::RectangleShape itemBg(sf::Vector2f(itemAreaW, dlg.itemH));
        itemBg.setPosition(listAreaX, itemY);
        itemBg.setFillColor(selected ? scheme.boardBg :
            (hover ? sf::Color(scheme.boardBg.r, scheme.boardBg.g, scheme.boardBg.b, 180) : scheme.scoreBoxColor));
        if (selected)
        {
            itemBg.setOutlineColor(scheme.textDark);
            itemBg.setOutlineThickness(2.f);
        }
        window.draw(itemBg);

        sf::Font itemFont;
        unsigned int itemFontSize = (unsigned int)(dlg.itemH * 0.65f);
        sf::Text fontName;
        if (itemFont.loadFromFile(g_systemFonts[i].path))
        {
            fontName = sf::Text(g_systemFonts[i].name, itemFont, itemFontSize);
        }
        else
        {
            fontName = sf::Text(g_systemFonts[i].name, font, itemFontSize);
        }
        fontName.setFillColor(scheme.textDark);
        sf::FloatRect fnBounds = fontName.getLocalBounds();
        fontName.setOrigin(fnBounds.left, fnBounds.top + fnBounds.height / 2);
        fontName.setPosition(listAreaX + 10.f, itemY + dlg.itemH / 2);
        window.draw(fontName);
    }

    float previewY = listAreaY + listAreaH + dlgH * 0.02f;
    float previewH = dlgH * 0.10f;
    float previewW = dlgW * 0.84f;
    float previewX = dlgX + dlgW * 0.08f;

    sf::RectangleShape previewBg(sf::Vector2f(previewW, previewH));
    previewBg.setPosition(previewX, previewY);
    previewBg.setFillColor(scheme.scoreBoxColor);
    previewBg.setOutlineColor(scheme.boardBg);
    previewBg.setOutlineThickness(1.f);
    window.draw(previewBg);

    if (dlg.selectedIdx >= 0 && dlg.selectedIdx < totalItems)
    {
        sf::Font previewFont;
        if (previewFont.loadFromFile(g_systemFonts[dlg.selectedIdx].path))
        {
            // preview text: mixed Chinese/English, tests the font's Chinese support
            sf::String previewText = toSFString("2048 Game 张晨曦");
            if (!fontSupportsChinese(g_systemFonts[dlg.selectedIdx].path))
            {
                // make sure the fallback font is loaded
                if (!g_fallbackLoaded)
                {
                    std::string fb = findChineseFont();
                    if (!fb.empty()) g_fallbackLoaded = g_fallbackFont.loadFromFile(fb);
                }
                // chunked rendering: ASCII uses the preview font, Chinese uses the fallback font
                bool prevUsing = g_usingFallback;
                g_usingFallback = true; // force the chunked path
                float tw = drawTextFB(window, previewFont, previewText,
                                      previewX + previewW / 2, previewY + previewH / 2,
                                      (unsigned int)(previewH * 0.35f), scheme.textDark, 0, 1);
                g_usingFallback = prevUsing;
            }
            else
            {
                sf::Text preview(previewText, previewFont, (unsigned int)(previewH * 0.35f));
                preview.setFillColor(scheme.textDark);
                sf::FloatRect pBounds = preview.getLocalBounds();
                preview.setOrigin(pBounds.left + pBounds.width / 2, pBounds.top + pBounds.height / 2);
                preview.setPosition(previewX + previewW / 2, previewY + previewH / 2);
                window.draw(preview);
            }
        }
    }

    float btnW = dlgW * 0.3f;
    float btnH = dlgH * 0.08f;
    float btnGap = dlgW * 0.05f;
    float btnY = dlgY + dlgH - btnH - dlgH * 0.06f;
    float totalBtnsW = btnW * 2 + btnGap;
    float btnStartX = dlgX + (dlgW - totalBtnsW) / 2.f;

    dlg.applyBtnX = btnStartX;
    dlg.applyBtnY = btnY;
    dlg.applyBtnW = btnW;
    dlg.applyBtnH = btnH;

    sf::RectangleShape applyBtn(sf::Vector2f(btnW, btnH));
    applyBtn.setPosition(btnStartX, btnY);
    applyBtn.setFillColor(scheme.scoreBoxColor);
    window.draw(applyBtn);

    drawTextFB(window, font, tr("Apply", "应用"),
               btnStartX + btnW / 2, btnY + btnH / 2,
               (unsigned int)(btnH * 0.5f), scheme.textDark, 0, 1);

    dlg.cancelBtnX = btnStartX + btnW + btnGap;
    dlg.cancelBtnY = btnY;
    dlg.cancelBtnW = btnW;
    dlg.cancelBtnH = btnH;

    sf::RectangleShape cancelBtn(sf::Vector2f(btnW, btnH));
    cancelBtn.setPosition(btnStartX + btnW + btnGap, btnY);
    cancelBtn.setFillColor(scheme.boardBg);
    window.draw(cancelBtn);

    drawTextFB(window, font, tr("Cancel", "取消"),
               btnStartX + btnW + btnGap + btnW / 2, btnY + btnH / 2,
               (unsigned int)(btnH * 0.5f), scheme.textDark, 0, 1);
}

// ========== History dialog rendering ==========

// ---- external replay text import (.txt/.vrs, 2048verse etc.) ----
// Parse the move sequence in the text -> rebuild 4x4 game frames -> save as imported_*.dat (pinned to the list top + marked "imported")

// Single move token (word/single letter/arrow char) -> this game's direction (0 left 1 right 2 up 3 down); -1 = not a move
static int moveTokenToDir(const std::string& tok)
{
    if (tok.empty()) return -1;
    std::string w;
    for (char ch : tok) w += (char)tolower((unsigned char)ch);
    if (w == "up" || w == "u") return 2;
    if (w == "down" || w == "d") return 3;
    if (w == "left" || w == "l") return 0;
    if (w == "right" || w == "r") return 1;
    if (w == "w") return 2;   // wasd
    if (w == "s") return 3;
    if (w == "a") return 0;
    if (tok == "↑") return 2;   // UTF-8 up arrow
    if (tok == "↓") return 3;
    if (tok == "←") return 0;
    if (tok == "→") return 1;
    return -1;
}

// Detect the replay board size: look for "NxN" / "NÃN" / size hints in the content, default 4
static int detectReplaySize(const std::string& content)
{
    for (int s = 3; s <= 8; ++s)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%dx%d", s, s);
        if (content.find(buf) != std::string::npos) return s;
        std::snprintf(buf, sizeof(buf), "%d×%d", s, s);
        if (content.find(buf) != std::string::npos) return s;
    }
    return 0;   // not found -> caller defaults to 4
}

// Parse JSON-like replay content: extract the board size and the move sequence
// Supports: size/boardSize/grid numeric fields; moves/dirs/history/sequence arrays (direction strings or numbers 0-3)
static bool parseReplayJson(const std::string& c, std::vector<int>& dirs, int& outSize)
{
    const size_t n = c.size();
    // extract the integer after a size field (3-8)
    const char* sizeKeys[] = { "size", "boardsize", "grid" };
    for (const char* key : sizeKeys)
    {
        size_t pos = 0;
        while ((pos = c.find('"', pos)) != std::string::npos)
        {
            size_t e = c.find('"', pos + 1);
            if (e == std::string::npos) break;
            std::string tok = c.substr(pos + 1, e - pos - 1);
            for (auto& ch : tok) ch = (char)tolower((unsigned char)ch);
            if (tok == key)
            {
                size_t q = e + 1;
                while (q < n && (c[q] == ':' || c[q] == ' ' || c[q] == '\t')) q++;
                if (q < n && c[q] == ':')
                {
                    ++q;
                    while (q < n && (c[q] == ' ' || c[q] == '\t')) ++q;
                    int v = 0; bool digit = false;
                    while (q < n && isdigit((unsigned char)c[q])) { v = v * 10 + (c[q] - '0'); ++q; digit = true; }
                    if (digit && v >= 3 && v <= 8) outSize = v;
                }
            }
            pos = e + 1;
        }
    }
    // extract moves: direction strings (inside quotes) or numbers 0-3 in arrays
    bool inArray = false;
    size_t i = 0;
    while (i < n)
    {
        char cch = c[i];
        if (cch == '"')
        {
            size_t e = c.find('"', i + 1);
            if (e == std::string::npos) break;
            int d = moveTokenToDir(c.substr(i + 1, e - i - 1));
            if (d >= 0) dirs.push_back(d);
            i = e + 1;
            continue;
        }
        if (cch == '[') inArray = true;
        else if (cch == ']') inArray = false;
        else if (inArray && cch >= '0' && cch <= '3')
        {
            static const int kDigitToDir[4] = { 2, 1, 3, 0 };   // digits 0/1/2/3 -> up/right/down/left
            dirs.push_back(kDigitToDir[cch - '0']);
        }
        ++i;
    }
    return !dirs.empty();
}

// Parse plain-text move sequences (arrows/UDLR/wasd/words/digits 0-3)
// With density validation: if the content has too many non-move letters (descriptive text/JSON keys), refuse to avoid producing garbage steps
static std::vector<int> parseReplayTextMoves(const std::string& content)
{
    // detect the encoding scheme: an 'a'/'w' means WASD (w=up s=down a=left d=right); otherwise default UDLR (u=up d=down l=left r=right)
    bool wasd = false;
    for (char ch : content)
    {
        char c = (char)tolower((unsigned char)ch);
        if (c == 'a' || c == 'w') { wasd = true; break; }
    }
    std::vector<int> dirs;
    const size_t n = content.size();
    size_t i = 0;
    while (i < n)
    {
        unsigned char c = (unsigned char)content[i];
        // UTF-8 arrows: up E2 86 91 / down E2 86 93 / left E2 86 90 / right E2 86 92
        if (c == 0xE2 && i + 2 < n && (unsigned char)content[i + 1] == 0x86)
        {
            unsigned char a = (unsigned char)content[i + 2];
            if (a == 0x91) dirs.push_back(2);        // up arrow
            else if (a == 0x93) dirs.push_back(3);   // down arrow
            else if (a == 0x90) dirs.push_back(0);   // left arrow
            else if (a == 0x92) dirs.push_back(1);   // right arrow
            i += 3;
            continue;
        }
        if (std::isalpha(c))
        {
            size_t j = i;
            while (j < n && std::isalpha((unsigned char)content[j])) ++j;
            std::string word = content.substr(i, j - i);
            for (auto& ch : word) ch = (char)tolower((unsigned char)ch);
            if (word == "up" || word == "down" || word == "left" || word == "right")
            {
                if (word == "up") dirs.push_back(2);
                else if (word == "down") dirs.push_back(3);
                else if (word == "left") dirs.push_back(0);
                else dirs.push_back(1);
            }
            else
            {
                for (char ch : word)
                {
                    if (ch == 'u') dirs.push_back(2);
                    else if (ch == 'd') dirs.push_back(wasd ? 1 : 3);
                    else if (ch == 'l') dirs.push_back(0);
                    else if (ch == 'r') dirs.push_back(1);
                    else if (ch == 'w') dirs.push_back(2);
                    else if (ch == 's') dirs.push_back(3);
                    else if (ch == 'a') dirs.push_back(0);
                }
            }
            i = j;
            continue;
        }
        if (c >= '0' && c <= '3')
        {
            static const int kDigitToDir[4] = { 2, 1, 3, 0 };   // digits 0/1/2/3 -> up/right/down/left
            dirs.push_back(kDigitToDir[c - '0']);
            ++i;
            continue;
        }
        ++i;
    }
    // density validation: move letters are too rare among alpha chars -> descriptive text/structured data, refuse
    int alpha = 0, moveAlpha = 0;
    for (char ch : content)
    {
        if (std::isalpha((unsigned char)ch))
        {
            ++alpha;
            char c2 = (char)tolower((unsigned char)ch);
            if (std::strchr("udlrwas", c2)) ++moveAlpha;
        }
    }
    if (alpha > 8 && (float)moveAlpha / (float)alpha < 0.5f)
        return {};
    return dirs;
}

// 2048verse-specific binary replay ("4x4-1_..."): every 3 bytes pack into 3 7-bit indexes into a per-file 128-char table -> move directions
// Only the move sequence is extracted (the in-step new tile is later rebuilt by this game's rules), for replayFromMoves.
// Returns true with outSize valid; outSize=0 means this game does not support that size (e.g. 2x4/3x4/non-square).
static bool parseVersereplay(const std::string& content, std::vector<int>& dirs, int& outSize)
{
    static int rev[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i) rev[i] = -1;
        // 128-char table (latin-1 byte value -> index), order must match 2048verse:
        // 0-9(0..9) a-z(10..35) A-Z(36..61)
        // À..Æ(0xC0..0xC6=62..68) Ç..Ï(0xC7..0xCF=69..77)
        // Ð..ß(0xD0..0xDF=78..93) à..ï(0xE0..0xEF=94..109)
        // ð..ÿ(0xF0..0xFF=110..125) ¤(0xA4=126) ¾(0xBE=127)
        const unsigned char seg[][28] = {
            { 0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0 },
            { 0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0 },
            { 0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0 },
            { 0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0 },
            { 0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,0 },
            { 0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0 },
            { 0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,0 },
            { 0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,0 },
        };
        int idx = 0;
        for (int s = 0; s < 8; ++s)
            for (int k = 0; seg[s][k]; ++k)
                rev[seg[s][k]] = idx++;
        rev[0xA4] = idx++;   // ¤
        rev[0xBE] = idx++;   // ¾
        init = true;
    }

    // header format "WxH-<theme>_<data>" (e.g. 4x4-1_)
    const size_t n = content.size();
    if (n < 8) return false;
    size_t p = 0;
    while (p < n && content[p] >= '0' && content[p] <= '9') ++p;
    if (p == 0 || p + 1 >= n || content[p] != 'x') return false;
    size_t p2 = p + 1;
    while (p2 < n && content[p2] >= '0' && content[p2] <= '9') ++p2;
    if (p2 == p + 1 || p2 >= n || content[p2] != '-') return false;
    int w = content[0] - '0';
    for (size_t k = 1; k < p; ++k) w = w * 10 + (content[k] - '0');
    int h = content[p + 1] - '0';
    for (size_t k = p + 2; k < p2; ++k) h = h * 10 + (content[k] - '0');

    size_t us = content.find('_', p2);
    if (us == std::string::npos) return false;
    const size_t headerEnd = us + 1;   // end of 4x4-1_

    // 2048verse vrs format: "WxH-<theme>_" + 128-byte per-file custom lookup table + 3-byte chunks
    // each file has its own independent 128-byte latin-1 lookup table!
    if (headerEnd + 128 > n) return false;
    const unsigned char* lookup = (const unsigned char*)content.data() + headerEnd;

    outSize = (w == h && h >= 3 && h <= MAX_BOARD_SIZE) ? h : 0;

    // data starts at headerEnd + 128
    const size_t dataStart = headerEnd + 128;
    const size_t numChunks = (n - dataStart) / 3;
    if (numChunks < 3) return false;

    // move encoding:
    //   3 bytes split into 3 7-bit values (0..127), each an index into the lookup table
    //   lookup[idx] yields one byte (0..255); the 3 bytes form a 21-bit integer
    //   low 2 bits = direction (2048verse: 0=up 1=down 2=left 3=right)
    //   mapped to this game: 0=left 1=right 2=up 3=down
    static const int mvToDir[4] = { 2, 3, 0, 1 };
    const unsigned char* d = (const unsigned char*)content.data() + dataStart;
    dirs.clear();
    dirs.reserve(numChunks - 2);
    for (size_t k = 2; k < numChunks; ++k)
    {
        const unsigned char* c = d + k * 3;
        unsigned i0 = c[0] >> 1;
        unsigned i1 = ((c[0] & 1) << 6) | (c[1] >> 2);
        unsigned i2 = ((c[1] & 3) << 5) | (c[2] >> 3);
        if (i0 >= 128 || i1 >= 128 || i2 >= 128) { dirs.clear(); return false; }
        unsigned v0 = lookup[i0];
        unsigned v1 = lookup[i1];
        unsigned v2 = lookup[i2];
        unsigned bin = (v0 << 14) | (v1 << 7) | v2;
        dirs.push_back(mvToDir[bin & 3]);
    }
    return !dirs.empty();
}

// 2048verse legacy binary replay full parse: "WxH-<theme>_" + 128-byte per-file lookup + 3-byte chunks
// Same format as parseVersereplay, but extracts full events (direction + spawn position/value + time),
// building a vrs::PkuReplay for replayFromPkuEvents to rebuild precisely (keeps real times, no more 120ms estimates).
// Direction encoding matches PKU: 0=up 1=down 2=left 3=right.
static bool parseVersereplayFull(const std::string& content, vrs::PkuReplay& replay)
{
    static int rev[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i) rev[i] = -1;
        const unsigned char seg[][28] = {
            { 0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0 },
            { 0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0 },
            { 0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0 },
            { 0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0 },
            { 0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,0 },
            { 0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,0 },
            { 0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,0 },
            { 0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF,0 },
        };
        int idx = 0;
        for (int s = 0; s < 8; ++s)
            for (int k = 0; seg[s][k]; ++k)
                rev[seg[s][k]] = idx++;
        rev[0xA4] = idx++;
        rev[0xBE] = idx++;
        init = true;
    }

    const size_t n = content.size();
    if (n < 8) return false;
    size_t p = 0;
    while (p < n && content[p] >= '0' && content[p] <= '9') ++p;
    if (p == 0 || p + 1 >= n || content[p] != 'x') return false;
    size_t p2 = p + 1;
    while (p2 < n && content[p2] >= '0' && content[p2] <= '9') ++p2;
    if (p2 == p + 1 || p2 >= n || content[p2] != '-') return false;
    int w = content[0] - '0';
    for (size_t k = 1; k < p; ++k) w = w * 10 + (content[k] - '0');
    int h = content[p + 1] - '0';
    for (size_t k = p + 2; k < p2; ++k) h = h * 10 + (content[k] - '0');

    size_t us = content.find('_', p2);
    if (us == std::string::npos) return false;
    const size_t headerEnd = us + 1;
    if (headerEnd + 128 > n) return false;
    const unsigned char* lookup = (const unsigned char*)content.data() + headerEnd;

    const size_t dataStart = headerEnd + 128;
    const size_t numChunks = (n - dataStart) / 3;
    if (numChunks < 3) return false;

    replay.rows = w;
    replay.cols = h;
    replay.mode = "classic";
    replay.format = "verse-binary";
    replay.initialSpawnCount = 2;
    replay.events.clear();
    replay.events.reserve(numChunks);

    const unsigned char* d = (const unsigned char*)content.data() + dataStart;
    for (size_t k = 0; k < numChunks; ++k)
    {
        const unsigned char* c = d + k * 3;
        unsigned i0 = c[0] >> 1;
        unsigned i1 = ((c[0] & 1) << 6) | (c[1] >> 2);
        unsigned i2 = ((c[1] & 3) << 5) | (c[2] >> 3);
        if (i0 >= 128 || i1 >= 128 || i2 >= 128) { replay.events.clear(); return false; }
        unsigned v0 = lookup[i0];
        unsigned v1 = lookup[i1];
        unsigned v2 = lookup[i2];
        unsigned bin = (v0 << 14) | (v1 << 7) | v2;

        vrs::PkuEvent ev;
        vrs::decodePkuPacked(bin, ev);
        ev.initial = (k < (size_t)replay.initialSpawnCount);
        replay.events.push_back(ev);
    }

    // validity check: at least the initial spawn + 1 move; move event directions must be 0..3
    if (replay.events.size() <= (size_t)replay.initialSpawnCount) return false;
    int validMoves = 0;
    for (size_t k = replay.initialSpawnCount; k < replay.events.size(); ++k)
        if ((replay.events[k].dir & 3) == replay.events[k].dir) ++validMoves;
    return validMoves > 0;
}

// Parse replay content -> move sequence + board size (default 4, supports 3-5); explicitSize = whether the content states a size
static bool parseReplayContent(const std::string& content, std::vector<int>& dirs, int& outSize, bool& explicitSize)
{
    dirs.clear();
    int detected = detectReplaySize(content);
    explicitSize = (detected >= 3 && detected <= 5);
    if (explicitSize) outSize = detected;
    else outSize = 4;

    // JSON-like content (quotes + colon/brackets) -> structured parse
    bool jsonLike = content.find('"') != std::string::npos &&
                    (content.find('{') != std::string::npos || content.find('[') != std::string::npos ||
                     content.find(':') != std::string::npos);
    if (jsonLike)
    {
        if (parseReplayJson(content, dirs, outSize))
        {
            explicitSize = true;   // a JSON size field was recognized (even if default 4, honor the recognition)
            return true;
        }
        return false;
    }

    dirs = parseReplayTextMoves(content);
    return !dirs.empty();
}

// Rebuild an NxN game from a move sequence (defined later; forward-declared for bestReplaySize)
static bool replayFromMoves(const std::vector<int>& dirs, int sz, HistoryEntry& entry, uint64_t seed);

// No explicit size: try rebuilding as 3/4/5 and pick the size with the most valid moves (auto-detect 3x3/4x4/5x5)
static int bestReplaySize(const std::vector<int>& dirs, uint64_t seed)
{
    int bestSz = 4, bestCnt = -1;
    for (int sz = 3; sz <= MAX_BOARD_SIZE; ++sz)
    {
        HistoryEntry tmp;
        if (replayFromMoves(dirs, sz, tmp, seed))
        {
            if (tmp.moveCount > bestCnt) { bestCnt = tmp.moveCount; bestSz = sz; }
        }
    }
    return bestSz;
}

// Pure move with scoring (returns the merge score of this move; an invalid move returns 0 and leaves the board unchanged)
static int moveBoardScored(std::vector<std::vector<int>>& b, int dir)
{
    const int n = (int)b.size();
    int gained = 0;
    auto slideLine = [&](std::vector<int>& l)
    {
        std::vector<int> out;
        for (int v : l) if (v) out.push_back(v);
        for (size_t k = 0; k + 1 < out.size(); ++k)
            if (out[k] == out[k + 1]) { out[k] *= 2; gained += out[k]; out.erase(out.begin() + k + 1); }
        std::fill(l.begin(), l.end(), 0);
        for (size_t k = 0; k < out.size() && k < l.size(); ++k) l[k] = out[k];
    };
    std::vector<std::vector<int>> temp = b;
    for (int i = 0; i < n; ++i)
    {
        if (dir == 0)      { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[i][j]; slideLine(l); for (int j = 0; j < n; ++j) temp[i][j] = l[j]; }
        else if (dir == 1) { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[i][n - 1 - j]; slideLine(l); for (int j = 0; j < n; ++j) temp[i][n - 1 - j] = l[j]; }
        else if (dir == 2) { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[j][i]; slideLine(l); for (int j = 0; j < n; ++j) temp[j][i] = l[j]; }
        else if (dir == 3) { std::vector<int> l(n); for (int j = 0; j < n; ++j) l[j] = temp[n - 1 - j][i]; slideLine(l); for (int j = 0; j < n; ++j) temp[n - 1 - j][i] = l[j]; }
    }
    if (temp == b) return -1;   // invalid move (board unchanged)
    b = std::move(temp);
    return gained;
}

// Deterministic random 2(90%)/4(10%) spawn (seed = file content hash, so the same file replays identically)
static void replaySpawn(std::vector<std::vector<int>>& b, int sz, std::mt19937& rng)
{
    std::vector<int> empty;
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            if (b[i][j] == 0) empty.push_back(i * sz + j);
    if (empty.empty()) return;
    std::uniform_int_distribution<int> pos(0, (int)empty.size() - 1);
    const int idx = empty[pos(rng)];
    const int val = (rng() % 10 == 0) ? 4 : 2;
    b[idx / sz][idx % sz] = val;
}

// FNV-1a content hash -> deterministic seed
static uint64_t fnvHash64(const std::string& s)
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// Rebuild an NxN game from a move sequence (standard 2048: two initial tiles, random 2/4 after each step), filling the HistoryEntry replay data
static bool replayFromMoves(const std::vector<int>& dirs, int sz, HistoryEntry& entry, uint64_t seed)
{
    if (dirs.empty() || dirs.size() > 500000) return false;
    if (sz < 3 || sz > MAX_BOARD_SIZE) return false;
    std::mt19937 rng((uint32_t)seed);
    std::vector<std::vector<int>> b(sz, std::vector<int>(sz, 0));
    replaySpawn(b, sz, rng);
    replaySpawn(b, sz, rng);

    auto flatten = [&]() {
        std::vector<int> fr(sz * sz, 0);
        for (int i = 0; i < sz; ++i)
            for (int j = 0; j < sz; ++j)
                fr[i * sz + j] = b[i][j];
        return fr;
    };

    entry.boardSize = sz;
    entry.colorScheme = 0;
    entry.score = 0;
    entry.moveCount = 0;
    entry.totalMs = 0;
    entry.frames.push_back(flatten());
    entry.frameScores.push_back(0);
    entry.frameTimesMs.push_back(0);

    int score = 0, t = 0;
    for (size_t k = 0; k < dirs.size(); ++k)
    {
        int d = dirs[k];
        if (d < 0 || d > 3) continue;
        int gained = moveBoardScored(b, d);
        if (gained < 0) continue;   // skip invalid moves (gained==0 means a slide-only move, still valid)
        if (gained > 0) score += gained;
        t += 120;                    // 120ms per step estimate
        entry.dirs.push_back(d);
        entry.animMs.push_back(120);
        replaySpawn(b, sz, rng);
        entry.frames.push_back(flatten());
        entry.frameScores.push_back(score);
        entry.frameTimesMs.push_back(t);
        entry.moveCount++;
        if (entry.moveCount >= 500000) break;
    }
    if (entry.moveCount == 0) return false;
    entry.score = score;
    entry.totalMs = t;
    // final board (for static viewing)
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            entry.board[i][j] = b[i][j];
    return true;
}

// Rebuild an NxN game from a PKU 2048 .vrs event sequence (with exact spawn position/value/time),
// filling the HistoryEntry replay data. PKU directions: 0=up 1=down 2=left 3=right -> this game: 0=left 1=right 2=up 3=down
static bool replayFromPkuEvents(const vrs::PkuReplay& pku, HistoryEntry& entry)
{
    if (pku.events.empty()) return false;
    const int sz = pku.rows;
    if (sz < 3 || sz > MAX_BOARD_SIZE || pku.cols != sz) return false;   // square 3/4/5 only
    if (pku.mode != "classic") return false;   // classic pow2 mode only (other modes merge differently)

    static const int pkuToCpp[4] = { 2, 3, 0, 1 }; // PKU up/down/left/right -> C++ left/right/up/down

    std::vector<std::vector<int>> b(sz, std::vector<int>(sz, 0));

    auto flatten = [&]() {
        std::vector<int> fr(sz * sz, 0);
        for (int i = 0; i < sz; ++i)
            for (int j = 0; j < sz; ++j)
                fr[i * sz + j] = b[i][j];
        return fr;
    };

    entry.boardSize = sz;
    entry.colorScheme = 0;
    entry.score = 0;
    entry.moveCount = 0;
    entry.totalMs = 0;

    int score = 0;
    long long totalMs = 0;   // accumulate in long long so very long games (hundreds of hours) do not overflow int
    const long long TOTAL_MS_CAP = 2000000000LL;  // safe value within int range (~23.1 days)
    int initCount = pku.initialSpawnCount;
    if (initCount < 1) initCount = 2;

    // initial spawn event: place the tile directly (no move)
    for (int i = 0; i < initCount && i < (int)pku.events.size(); ++i)
    {
        const vrs::PkuEvent& ev = pku.events[i];
        if (ev.spawn.x >= 0 && ev.spawn.x < sz && ev.spawn.y >= 0 && ev.spawn.y < sz)
        {
            if (b[ev.spawn.y][ev.spawn.x] == 0)
                b[ev.spawn.y][ev.spawn.x] = (ev.spawn.value == 4) ? 4 : 2;
        }
        if (ev.timeMs > 0) totalMs = std::min(totalMs + ev.timeMs, TOTAL_MS_CAP);
    }

    // frame 0 = board after the initial spawn
    entry.frames.push_back(flatten());
    entry.frameScores.push_back(0);
    entry.frameTimesMs.push_back((int)std::min(totalMs, TOTAL_MS_CAP));

    // later events: move + spawn
    for (size_t k = initCount; k < pku.events.size(); ++k)
    {
        const vrs::PkuEvent& ev = pku.events[k];
        int d = pkuToCpp[ev.dir & 3];
        int gained = moveBoardScored(b, d);
        if (gained < 0)
        {
            // invalid move: still record the time (if any), but no new frame
            if (ev.timeMs > 0) totalMs += ev.timeMs;
            continue;
        }
        if (gained > 0) score += gained;
        if (ev.timeMs > 0) totalMs += ev.timeMs;
        else totalMs += 120;   // no recorded time -> 120ms estimate

        // place the spawn tile (exact position/value)
        if (ev.spawn.x >= 0 && ev.spawn.x < sz && ev.spawn.y >= 0 && ev.spawn.y < sz)
        {
            if (b[ev.spawn.y][ev.spawn.x] == 0)
                b[ev.spawn.y][ev.spawn.x] = (ev.spawn.value == 4) ? 4 : 2;
        }

        entry.dirs.push_back(d);
        entry.animMs.push_back((ev.timeMs > 0) ? ev.timeMs : 120);
        entry.frames.push_back(flatten());
        entry.frameScores.push_back(score);
        entry.frameTimesMs.push_back((int)std::min(totalMs, TOTAL_MS_CAP));
        entry.moveCount++;
        if (entry.moveCount >= 200000) break;
    }

    if (entry.moveCount == 0) return false;
    entry.score = score;
    entry.totalMs = (int)std::min(totalMs, TOTAL_MS_CAP);
    for (int i = 0; i < sz; ++i)
        for (int j = 0; j < sz; ++j)
            entry.board[i][j] = b[i][j];
    return true;
}

// Write a HistoryEntry to the History Record directory in the standard .dat format
static bool writeHistoryEntryFile(const HistoryEntry& entry, const std::string& fullPath)
{
    std::ofstream ofs(fullPath, std::ios::binary);
    if (!ofs) return false;
    int bs = entry.boardSize, cs = entry.colorScheme, sc = entry.score;
    ofs.write((const char*)&bs, sizeof(bs));
    ofs.write((const char*)&cs, sizeof(cs));
    ofs.write((const char*)&sc, sizeof(sc));
    for (int i = 0; i < MAX_BOARD_SIZE; ++i)
        ofs.write((const char*)entry.board[i], MAX_BOARD_SIZE * sizeof(int));
    bool ai = entry.aiAssisted;
    ofs.write((const char*)&ai, sizeof(ai));
    int moveCount = entry.moveCount, totalMs = entry.totalMs;
    ofs.write((const char*)&moveCount, sizeof(moveCount));
    ofs.write((const char*)&totalMs, sizeof(totalMs));
    for (int f = 0; f <= moveCount; ++f)
    {
        for (int i = 0; i < bs * bs; ++i)
        {
            int v = (f < (int)entry.frames.size() && i < (int)entry.frames[f].size()) ? entry.frames[f][i] : 0;
            ofs.write((const char*)&v, sizeof(v));
        }
        int scf = (f < (int)entry.frameScores.size()) ? entry.frameScores[f] : 0;
        ofs.write((const char*)&scf, sizeof(scf));
        int tmf = (f < (int)entry.frameTimesMs.size()) ? entry.frameTimesMs[f] : 0;
        ofs.write((const char*)&tmf, sizeof(tmf));
    }
    for (int k = 0; k < moveCount; ++k)
    {
        int d = (k < (int)entry.dirs.size()) ? entry.dirs[k] : 0;
        ofs.write((const char*)&d, sizeof(d));
    }
    for (int k = 0; k < moveCount; ++k)
    {
        int am = (k < (int)entry.animMs.size()) ? entry.animMs[k] : 120;
        ofs.write((const char*)&am, sizeof(am));
    }
    int chMarker = 0;
    ofs.write((const char*)&chMarker, sizeof(chMarker));
    return true;
}

// Parse and import a .vrs/.txt replay; returns true on success.
// outFile (optional): on success returns the written .dat filename (for auto-locating the playback after import).
// outReason (optional): failure reason; 1=board size unsupported (other than 3/4/5), the rest are parse failures.
static std::string makeImportedDstPath(const std::string& srcName);

static bool importReplayFile(const std::string& src, const std::string& srcName,
                             std::string* outFile = nullptr, int* outReason = nullptr)
{
    if (outReason) *outReason = 0;
    // safe read for Chinese paths: convert to wide chars and read with CreateFileW, avoiding MSVC std::ifstream ANSI issues
    int wlen = MultiByteToWideChar(CP_ACP, 0, src.c_str(), (int)src.size(), nullptr, 0);
    if (wlen <= 0) { logMsg(std::string("[import] path conv failed: ") + src); return false; }
    std::wstring wsrc(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, src.c_str(), (int)src.size(), &wsrc[0], wlen);
    HANDLE hFile = CreateFileW(wsrc.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        logMsg(std::string("[import] CreateFileW failed (err=") + std::to_string(GetLastError()) + "): " + src);
        return false;
    }
    LARGE_INTEGER fsize;
    if (!GetFileSizeEx(hFile, &fsize) || fsize.QuadPart <= 0 || fsize.QuadPart > 100 * 1024 * 1024) {
        CloseHandle(hFile);
        logMsg("[import] bad file size");
        return false;
    }
    std::string content((size_t)fsize.QuadPart, '\0');
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, content.data(), (DWORD)content.size(), &bytesRead, NULL)) {
        CloseHandle(hFile);
        logMsg("[import] ReadFile failed");
        return false;
    }
    CloseHandle(hFile);
    content.resize(bytesRead);
    logMsg(std::string("[import] loaded ") + std::to_string(bytesRead) + " bytes from " + src);

    // ===== encoding detection and conversion =====
    // UTF-16 LE BOM (FF FE) or UTF-16 BE BOM (FE FF) -> convert to UTF-8
    if (content.size() >= 2 &&
        ((unsigned char)content[0] == 0xFF && (unsigned char)content[1] == 0xFE))
    {
        logMsg("[import] UTF-16 LE BOM detected, converting to UTF-8");
        std::wstring wstr;
        for (size_t i = 2; i + 1 < content.size(); i += 2) {
            wchar_t wc = (unsigned char)content[i] | ((unsigned char)content[i+1] << 8);
            if (wc != 0) wstr.push_back(wc);
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string utf8(utf8Len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &utf8[0], utf8Len, nullptr, nullptr);
            content = utf8;
        }
    }
    else if (content.size() >= 2 &&
             ((unsigned char)content[0] == 0xFE && (unsigned char)content[1] == 0xFF))
    {
        logMsg("[import] UTF-16 BE BOM detected, converting to UTF-8");
        std::wstring wstr;
        for (size_t i = 2; i + 1 < content.size(); i += 2) {
            wchar_t wc = ((unsigned char)content[i] << 8) | (unsigned char)content[i+1];
            if (wc != 0) wstr.push_back(wc);
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string utf8(utf8Len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &utf8[0], utf8Len, nullptr, nullptr);
            content = utf8;
        }
    }
    // UTF-8 BOM (EF BB BF) -> strip
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF)
    {
        content = content.substr(3);
        logMsg("[import] UTF-8 BOM stripped");
    }

    // ===== file header diagnostics (first 64 bytes hex + ASCII) =====
    {
        std::string hexStr, asciiStr;
        size_t diagLen = std::min(content.size(), (size_t)64);
        for (size_t i = 0; i < diagLen; ++i) {
            unsigned char c = (unsigned char)content[i];
            char hx[4];
            snprintf(hx, sizeof(hx), "%02X ", c);
            hexStr += hx;
            asciiStr += (c >= 32 && c < 127) ? (char)c : '.';
        }
        logMsg(std::string("[import] header hex: ") + hexStr);
        logMsg(std::string("[import] header ascii: ") + asciiStr);
    }

    std::vector<int> dirs;
    int sz = 4;
    bool explicitSize = false;
    // .vrs: try the 2048verse binary built-in decode first, then the lightweight C++ parser (JSON/base64/text), finally text parse
    std::string ext = srcName;
    size_t dot = ext.find_last_of('.');
    ext = (dot == std::string::npos) ? "" : ext.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)tolower(c); });
    if (ext == ".vrs")
    {
        // try the PKU 2048 new format first (pku2048.cn): verse-new / pku-v2 / pku-v1
        // with exact spawn position/value/time -> rebuild precisely frame by frame with replayFromPkuEvents (no random seed)
        vrs::PkuReplay pku;
        std::string pkuErr;
        if (vrs::parsePku(content, pku, pkuErr) && !pku.events.empty())
        {
            logMsg(std::string("[import] PKU parser hit: format=") + pku.format +
                   " mode=" + pku.mode + " size=" + std::to_string(pku.rows) + "x" + std::to_string(pku.cols) +
                   " events=" + std::to_string(pku.events.size()));
            // square 3/4/5 classic pow2 mode only (other modes have different merge rules/board shapes)
            if (pku.rows == pku.cols && pku.rows >= 3 && pku.rows <= MAX_BOARD_SIZE && pku.mode == "classic")
            {
                HistoryEntry pkuEntry;
                pkuEntry.imported = true;
                {
                    size_t sl2 = srcName.find_last_of("\\/");
                    pkuEntry.importedName = (sl2 == std::string::npos) ? srcName : srcName.substr(sl2 + 1);
                }
                if (replayFromPkuEvents(pku, pkuEntry))
                {
                    logMsg(std::string("[import] PKU exact replay: ") + srcName + " -> " +
                           std::to_string(pkuEntry.moveCount) + " moves (size " +
                           std::to_string(pkuEntry.boardSize) + ", score " + std::to_string(pkuEntry.score) + ")");
                    CreateDirectoryA(HISTORY_DIR, NULL);
                    std::string dst = makeImportedDstPath(srcName);   // keep the original filename (extension stripped)
                    if (writeHistoryEntryFile(pkuEntry, dst))
                    {
                        logMsg(std::string("[import] PKU replay saved: ") + dst);
                        if (outFile) *outFile = dst;
                        return true;
                    }
                    logMsg(std::string("[import] PKU replay write failed: ") + dst);
                }
                else
                {
                    logMsg("[import] PKU replayFromPkuEvents failed (no valid moves), falling back");
                }
            }
            else
            {
                logMsg(std::string("[import] PKU replay unsupported (size=") + std::to_string(pku.rows) +
                       "x" + std::to_string(pku.cols) + " mode=" + pku.mode + "), falling back to direction-only");
                // non-classic/non-square: degrade to a direction sequence (random-seed rebuild, may be imprecise)
                dirs.clear();
                static const int pkuToCpp[4] = { 2, 3, 0, 1 };
                for (size_t i = pku.initialSpawnCount; i < pku.events.size(); ++i)
                    dirs.push_back(pkuToCpp[pku.events[i].dir & 3]);
                sz = (pku.rows == pku.cols && pku.rows >= 3 && pku.rows <= MAX_BOARD_SIZE) ? pku.rows : 4;
                explicitSize = true;
            }
        }
        else
        {
            logMsg(std::string("[import] PKU parser miss (") + pkuErr + "), trying old verse binary");
        }

        // legacy 2048verse-specific binary replay ("4x4-1_..." + 128-byte per-file lookup)
        // first try the full-event parse (direction + spawn + time), rebuilt precisely with replayFromPkuEvents keeping real times;
        // on full-parse failure fall back to direction-only (parseVersereplay + replayFromMoves, 120ms per step estimate).
        if (dirs.empty())
        {
            vrs::PkuReplay versePku;
            if (parseVersereplayFull(content, versePku) && !versePku.events.empty())
            {
                logMsg(std::string("[import] verse-binary full parser hit: size=") +
                       std::to_string(versePku.rows) + "x" + std::to_string(versePku.cols) +
                       " events=" + std::to_string(versePku.events.size()));
                if (versePku.rows == versePku.cols && versePku.rows >= 3 &&
                    versePku.rows <= MAX_BOARD_SIZE && versePku.mode == "classic")
                {
                    HistoryEntry verseEntry;
                    verseEntry.imported = true;
                    {
                        size_t sl4 = srcName.find_last_of("\\/");
                        verseEntry.importedName = (sl4 == std::string::npos) ? srcName : srcName.substr(sl4 + 1);
                    }
                    if (replayFromPkuEvents(versePku, verseEntry))
                    {
                        logMsg(std::string("[import] verse-binary exact replay (time preserved): ") + srcName + " -> " +
                               std::to_string(verseEntry.moveCount) + " moves, " +
                               std::to_string(verseEntry.totalMs) + " ms (size " +
                               std::to_string(verseEntry.boardSize) + ", score " + std::to_string(verseEntry.score) + ")");
                        CreateDirectoryA(HISTORY_DIR, NULL);
                        std::string dst2 = makeImportedDstPath(srcName);   // keep the original filename (extension stripped)
                        if (writeHistoryEntryFile(verseEntry, dst2))
                        {
                            logMsg(std::string("[import] verse-binary replay saved: ") + dst2);
                            if (outFile) *outFile = dst2;
                            return true;
                        }
                        logMsg(std::string("[import] verse-binary replay write failed: ") + dst2);
                    }
                    else
                    {
                        logMsg("[import] verse-binary replayFromPkuEvents failed, falling back to direction-only");
                    }
                }
                else
                {
                    logMsg(std::string("[import] verse-binary unsupported (size=") + std::to_string(versePku.rows) +
                           "x" + std::to_string(versePku.cols) + " mode=" + versePku.mode + "), falling back");
                }
            }

            // fallback: direction-only parse
            std::vector<int> vd;
            int vsz = 4;
            bool vr_ok = parseVersereplay(content, vd, vsz);
            logMsg(std::string("[DEBUG] parseVersereplay ret=") + (vr_ok ? "true" : "false") +
                   " vd.size=" + std::to_string(vd.size()) + " vsz=" + std::to_string(vsz) +
                   " content.size=" + std::to_string(content.size()) +
                   " header=" + content.substr(0, 8));
            if (vr_ok)
            {
                if (vsz >= 3 && vsz <= MAX_BOARD_SIZE && !vd.empty())
                {
                    dirs = vd;
                    sz = vsz;
                    explicitSize = true;
                    logMsg(std::string("vrs built-in verse parser (direction-only): ") + srcName + " -> " +
                           std::to_string(dirs.size()) + " moves (size " + std::to_string(sz) + ")");
                }
                else
                {
                    logMsg(std::string("VRS verse replay rejected: unsupported board size (") + srcName + ")");
                    if (outReason) *outReason = 1;
                    return false;
                }
            }
            else
            {
                std::vector<vrs::Move> moves;
                std::string verr;
                if (vrs::parse(content, moves, sz, verr) && !moves.empty())
                {
                    dirs.reserve(moves.size());
                    for (const auto& m : moves) dirs.push_back(m.dir);
                    explicitSize = (sz >= 3 && sz <= MAX_BOARD_SIZE);
                    logMsg(std::string("vrs JS engine parsed: ") + srcName + " -> " +
                           std::to_string(dirs.size()) + " moves (size " + std::to_string(sz) + ")");
                }
                else
                {
                    logMsg(std::string("vrs JS engine failed (") + verr + "), fallback C++ parser for " + srcName);
                }
            }
        }
    }
    if (dirs.empty())
    {
        if (!parseReplayContent(content, dirs, sz, explicitSize)) return false;
    }
    if (dirs.empty()) return false;
    // this game supports 3/4/5 boards only; other sizes (6x6 etc.) cannot be replayed -> tell the user
    if (sz < 3 || sz > MAX_BOARD_SIZE)
    {
        logMsg(std::string("Replay import rejected: unsupported board size ") + std::to_string(sz) + " (" + srcName + ")");
        if (outReason) *outReason = 1;
        return false;
    }
    // no explicit size: auto-detect 3x3/4x4/5x5 (pick the size with the most valid moves)
    const uint64_t seed = fnvHash64(content);
    if (!explicitSize)
        sz = bestReplaySize(dirs, seed);

    HistoryEntry entry;
    entry.imported = true;
    {
        size_t sl3 = srcName.find_last_of("\\/");
        entry.importedName = (sl3 == std::string::npos) ? srcName : srcName.substr(sl3 + 1);
    }
    if (!replayFromMoves(dirs, sz, entry, seed)) return false;

    CreateDirectoryA(HISTORY_DIR, NULL);
    // keep the original filename (extension stripped) instead of turning into imported_timestamp and losing the name
    std::string dst = makeImportedDstPath(srcName);
    if (!writeHistoryEntryFile(entry, dst)) return false;
    if (outFile)
    {
        size_t sl = dst.find_last_of("\\/");
        *outFile = (sl == std::string::npos) ? dst : dst.substr(sl + 1);
    }
    logMsg(std::string("Imported replay: ") + srcName + " -> " + dst + " (size " + std::to_string(sz) + ", " + std::to_string(entry.moveCount) + " moves)");
    return true;
}

// Open the "About" technical document: Meter2048_Technical_Documentation.html ships with the installer,
// opened in the system default browser (HTML anchors/links are clickable)
static void openAboutDocumentation()
{
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = exePath;
    size_t pos = exeDir.find_last_of("\\/");
    exeDir = (pos == std::string::npos) ? std::string(".") : exeDir.substr(0, pos);
    // look for the technical document in the exe directory and its subdirectories
    const char* candidates[] = {
        "Meter2048_Technical_Documentation.html",
        "docs\\Meter2048_Technical_Documentation.html",
        "Documentation\\Meter2048_Technical_Documentation.html",
        "doc\\Meter2048_Technical_Documentation.html",
    };
    for (const char* name : candidates)
    {
        std::string p = exeDir + "\\" + name;
        if (std::filesystem::exists(p))
        {
            ShellExecuteA(NULL, "open", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return;
        }
    }
    logMsg("About documentation not found next to exe");
    MessageBoxA(NULL, "未找到技术文档 Meter2048_Technical_Documentation.html（应位于程序目录）",
                "关于", MB_OK | MB_ICONINFORMATION);
}

// UTF-8 Chinese message-box prompt (MessageBoxW displays Chinese correctly)
static void showInfoBox(const char* utf8Title, const char* utf8Msg)
{
    int tw = MultiByteToWideChar(CP_UTF8, 0, utf8Title, -1, nullptr, 0);
    int mw = MultiByteToWideChar(CP_UTF8, 0, utf8Msg, -1, nullptr, 0);
    std::wstring t(std::max(tw, 1), 0), m(std::max(mw, 1), 0);
    if (tw > 1) MultiByteToWideChar(CP_UTF8, 0, utf8Title, -1, &t[0], tw);
    if (mw > 1) MultiByteToWideChar(CP_UTF8, 0, utf8Msg, -1, &m[0], mw);
    MessageBoxW(NULL, m.c_str(), t.c_str(), MB_OK | MB_ICONWARNING);
}

// Generate the imported-file storage path: History Record\imported_<original filename (extension stripped)>.dat,
// keeping the original filename; same name appends _2/_3... to avoid overwrite (avoids imported_timestamp losing the name).
static std::string makeImportedDstPath(const std::string& srcName)
{
    std::string base = srcName;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    // sanitize illegal Windows filename characters
    std::string safe;
    safe.reserve(base.size());
    for (char ch : base)
    {
        if (ch=='<'||ch=='>'||ch==':'||ch=='"'||ch=='/'||ch=='\\'||ch=='|'||ch=='?'||ch=='*')
            safe += '_';
        else
            safe += ch;
    }
    if (safe.empty() || safe == "." || safe == "..") safe = "imported";
    if (safe.size() > 120) safe = safe.substr(0, 120);
    std::string dst = std::string(HISTORY_DIR) + "\\imported_" + safe + ".dat";
    int dup = 0;
    while (std::filesystem::exists(dst))
    {
        ++dup;
        dst = std::string(HISTORY_DIR) + "\\imported_" + safe + "_" + std::to_string(dup) + ".dat";
    }
    return dst;
}

// Directly import the file at a given path (called when clicking an entry in the import history panel)
// outFile (optional): on success returns the written/copied .dat filename (for auto-locating the playback after import).
// outReason (optional): failure reason; 1=board size unsupported (other than 3/4/5), the rest are parse/copy failures.
static bool importHistoryFile(const std::string& src, const std::string& name,
                              std::string* outFile = nullptr, int* outReason = nullptr)
{
    logMsg(std::string("[importHistoryFile] ENTRY src=") + src + " name=" + name);
    if (outReason) *outReason = 0;
    std::string lower = name;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    size_t dot = lower.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : lower.substr(dot);
    logMsg(std::string("[importHistoryFile] ext=") + ext);

    // .txt/.vrs -> parse the text replay (2048verse etc.); otherwise copy as .dat/.bin
    if (ext == ".txt" || ext == ".vrs")
    {
        if (importReplayFile(src, name, outFile, outReason)) return true;
        logMsg(std::string("Replay import failed: ") + name);
        // unsupported board size -> caller shows a clear message, no generic error box
        if (outReason && *outReason == 1) return false;
        showInfoBox("导入失败",
                    "无法导入回放文件：\n\n"
                    "请确认是有效的回放/移动序列文件（.txt/.vrs）。\n"
                    "若为特殊编码（如 base64 回放码）或专有二进制格式，请提供样例文件以便适配。");
        return false;
    }

    CreateDirectoryA(HISTORY_DIR, NULL);
    // .dat/.bin copy: first check the embedded board size (first int of the header); special sizes get a specific hint
    if (ext == ".dat" || ext == ".bin")
    {
        std::ifstream hdr(src, std::ios::binary);
        if (hdr)
        {
            int bs = 0;
            hdr.read((char*)&bs, sizeof(bs));
            if (bs >= 1 && bs <= 64 && (bs < 3 || bs > MAX_BOARD_SIZE))
            {
                logMsg(std::string("History file rejected: unsupported board size ") + std::to_string(bs) + " (" + name + ")");
                if (outReason) *outReason = 1;
                return false;
            }
        }
    }
    std::string dst = std::string(HISTORY_DIR) + "\\" + name;
    if (std::filesystem::exists(dst))
    {
        char ts[64];
        time_t now = std::time(nullptr);
        struct tm t;
        localtime_s(&t, &now);
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &t);
        size_t dot2 = name.find_last_of('.');
        std::string stem = (dot2 == std::string::npos) ? name : name.substr(0, dot2);
        std::string ext2  = (dot2 == std::string::npos) ? ".dat" : name.substr(dot2);
        dst = std::string(HISTORY_DIR) + "\\" + stem + "_" + ts + ext2;
    }
    if (!CopyFileA(src.c_str(), dst.c_str(), FALSE))
    {
        logMsg(std::string("History file copy failed: ") + src + " -> " + dst);
        showInfoBox("导入失败", "复制历史文件失败，请确认文件可访问且未被占用。");
        return false;
    }
    if (outFile)
    {
        size_t sl = dst.find_last_of("\\/");
        *outFile = (sl == std::string::npos) ? dst : dst.substr(sl + 1);
    }
    return true;
}

// Whether an import file is a supported type (.vrs/.dat/.bin, case-insensitive; .txt already filtered out).
// Used to directly filter out unsupported files during batch import.
static bool isSupportedImportFile(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == ".vrs" || ext == ".dat" || ext == ".bin";
}

// Export a history entry as a .dat file (reloadable through "import history").
// ansiPath comes from GetSaveFileNameA. To avoid MSVC std::ofstream failing on Chinese paths,
// write to an ASCII temp file in the exe directory first, then CopyFileA to the user target (CopyFileA supports system ANSI/Chinese paths).
static bool exportHistoryEntryToFile(const HistoryEntry& entry, const std::string& ansiPath)
{
    std::string tmp = getExeDir() + "\\~export_tmp.dat";
    if (!writeHistoryEntryFile(entry, tmp)) return false;
    BOOL ok = CopyFileA(tmp.c_str(), ansiPath.c_str(), FALSE);
    DeleteFileA(tmp.c_str());
    if (!ok)
    {
        logMsg(std::string("exportHistoryEntryToFile CopyFileA failed (err=")
               + std::to_string(GetLastError()) + "): " + ansiPath);
        return false;
    }
    return true;
}

// Start history playback (shared by clicking a history entry / auto-open after import)
// entryIdx is a g_historyDlg.entries index; with replay data it enters playback, otherwise degrades to static viewing
static void startHistoryPlayback(Game2048& game, int entryIdx)
{
    if (entryIdx < 0 || entryIdx >= (int)g_historyDlg.entries.size()) return;
    const HistoryEntry& entry = g_historyDlg.entries[entryIdx];
    if (g_pb.active) exitPlayback(game);
    g_historyDlg.open = false;
    if (entry.moveCount > 0 && (int)entry.frames.size() == entry.moveCount + 1)
    {
        // start playback: show the playback control area below the board (the playback instance carries the animations, per that game's board size 3x3/4x4/5x5)
        // pause the AI (avoid the AI still auto-playing in the background during playback)
        if (g_aiMode == AI_RUNNING) { g_aiMode = AI_PAUSED; g_aiTiming = false; }
        g_aiSettingsOpen = false;
        g_pb.active = true;
        g_pb.playing = false;
        g_pb.curFrame = 0;
        g_pb.appliedStep = -1;
        g_pb.entryIdx = entryIdx;
        g_pb.settingsOpen = false;
        g_pb.trackDrag = false;
        g_pb.sliderDrag = -1;
        g_pb.playMs = 0.f;
        g_pb.colorScheme = entry.imported ? game.getColorScheme() : entry.colorScheme;
        g_pbGame.setBoardFrame(entry.boardSize,
                               g_pb.colorScheme,
                               entry.frameScores[0], entry.frames[0]);
        game.exitHistoryView();
        // practice mode: imported-history review (fixed step + save the pre-import position + quality analysis)
        if (g_practiceActive)
        {
            g_practiceImport = true;
            g_pb.stepMode = 1;   // fixed step only
            g_practicePrevScore = game.getScore();
            int psz = game.getBoardSize();
            g_practicePrevBoard.assign(psz, std::vector<int>(psz, 0));
            for (int i = 0; i < psz; ++i)
                for (int j = 0; j < psz; ++j)
                    g_practicePrevBoard[i][j] = game.getTile(i, j);
            startPracticeQualityAnalysis(entryIdx);
            // sync the analysis panel to frame 0
            int sz0 = entry.boardSize;
            std::vector<std::vector<int>> bm0(sz0, std::vector<int>(sz0));
            const auto& fr0 = entry.frames[0];
            for (int i = 0; i < sz0 && i < (int)fr0.size() / sz0; ++i)
                for (int j = 0; j < sz0; ++j)
                    bm0[i][j] = fr0[i * sz0 + j];
            requestPracticePredictionFor(bm0);
        }
    }
    else
    {
        // legacy format without replay data: static viewing only
        game.enterHistoryView(entry.boardSize,
                              entry.imported ? game.getColorScheme() : entry.colorScheme,
                              entry.score, entry.board);
    }
}

// "Best records" filter: visible list length (1 per board size, sizes without records are not shown)
static int histVisibleCount(const HistoryDialogState& dlg)
{
    if (dlg.filterMax)
    {
        int n = 0;
        for (int i = 0; i < 3; ++i)
            if (dlg.maxScoreIdxBySize[i] >= 0) n++;
        for (int i = 0; i < 6; ++i)
            if (dlg.maxChallengeIdxByProject[i] >= 0) n++;
        return n;
    }
    return (int)dlg.entries.size();
}
// visible index -> actual entries index
static int histEntryAt(const HistoryDialogState& dlg, int visIdx)
{
    if (!dlg.filterMax)
        return visIdx;
    int n = 0;
    for (int i = 0; i < 3; ++i)
    {
        int idx = dlg.maxScoreIdxBySize[i];
        if (idx >= 0)
        {
            if (n == visIdx) return idx;
            n++;
        }
    }
    for (int i = 0; i < 6; ++i)
    {
        int idx = dlg.maxChallengeIdxByProject[i];
        if (idx >= 0)
        {
            if (n == visIdx) return idx;
            n++;
        }
    }
    return -1;
}

// Truncate text to a max width, appending "..." (same measurement rules as drawTextFB)
static sf::String truncateToWidth(const sf::Font& font, const sf::String& str, unsigned int size, float maxW)
{
    if (str.isEmpty() || maxW <= 0.f) return str;
    if (measureTextFB(font, str, size) <= maxW) return str;
    const sf::String ell("...");
    sf::String cur = str;
    while (cur.getSize() > 0)
    {
        cur.erase(cur.getSize() - 1);   // remove the last character
        if (measureTextFB(font, cur + ell, size) <= maxW) break;
    }
    return cur + ell;
}

void drawHistoryDialog(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                       HistoryDialogState& dlg)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const SchemeData& scheme = game.getScheme();

    // dialog size (slightly taller than wide so the list fully shows 5 entries; vertically centered in the window)
    float boardPx = computeBoardPx(winW, winH, false);
    const float pad = boardPx * LAYOUT_PAD_RATIO;
    float availW = winW - pad * 2;
    float availH = winH - pad * 2;
    float dlgW = std::min(availW, availH / 1.2f);   // fixed width:height = 1:1.2
    float dlgH = dlgW * 1.2f;
    float dlgX = (winW - dlgW) / 2.f;
    float dlgY = (winH - dlgH) / 2.f;

    // semi-transparent background
    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 180));
    window.draw(overlay);

    // dialog body
    sf::RectangleShape dlgBg(sf::Vector2f(dlgW, dlgH));
    dlgBg.setPosition(dlgX, dlgY);
    dlgBg.setFillColor(scheme.windowBg);
    dlgBg.setOutlineColor(scheme.boardBg);
    dlgBg.setOutlineThickness(3.f);
    window.draw(dlgBg);

    // top dual function keys: left "history" / right "import history" (open their own panels)
    // small, the group horizontally centered; no title above, only these two keys
    {
        const float tabH = dlgH * 0.05f;
        const float tabGap = dlgW * 0.06f;
        const float tabW = dlgW * 0.3f;
        const float groupW = tabW * 2.f + tabGap;
        const float tabY = dlgY + dlgH * 0.028f;
        const float tabX0 = dlgX + (dlgW - groupW) / 2.f;
        g_historyDlg.tabHistX = tabX0;          g_historyDlg.tabHistY = tabY;
        g_historyDlg.tabHistW = tabW;           g_historyDlg.tabHistH = tabH;
        g_historyDlg.tabImpX  = tabX0 + tabW + tabGap;  g_historyDlg.tabImpY = tabY;
        g_historyDlg.tabImpW  = tabW;           g_historyDlg.tabImpH = tabH;
        const sf::Color activeBlue(70, 130, 180);
        auto drawTab = [&](float x, float y, float w, float h, const char* zh, const char* en, bool active) {
            sf::RectangleShape b(sf::Vector2f(w, h));
            b.setPosition(x, y);
            b.setFillColor(active ? activeBlue : scheme.boardBg);
            b.setOutlineColor(scheme.boardBg);
            b.setOutlineThickness(1.f);
            window.draw(b);
            drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                       x + w / 2, y + h / 2, (unsigned int)(h * 0.5f),
                       active ? sf::Color::White : scheme.textDark, 0, 1);
        };
        drawTab(g_historyDlg.tabHistX, g_historyDlg.tabHistY, g_historyDlg.tabHistW, g_historyDlg.tabHistH,
                "历史记录", "History", dlg.tab == 0);
        drawTab(g_historyDlg.tabImpX,  g_historyDlg.tabImpY,  g_historyDlg.tabImpW,  g_historyDlg.tabImpH,
                "导入历史", "Import", dlg.tab == 1);
    }

    // ===== import history panel =====
    if (dlg.tab == 1)
    {
        // import entry list area (between the top keys and the bottom two keys)
        const float tabBottom = g_historyDlg.tabImpY + g_historyDlg.tabImpH;
        const float listTop = tabBottom + dlgH * 0.02f;
        const float listBottom = dlgY + dlgH - dlgH * 0.115f;   // reserve the bottom two keys
        dlg.impListX = dlgX + dlgW * 0.04f;
        dlg.impListW = dlgW * 0.92f;
        dlg.impListY = listTop;
        dlg.impListH = listBottom - listTop;
        sf::RectangleShape impBg(sf::Vector2f(dlg.impListW, dlg.impListH));
        impBg.setPosition(dlg.impListX, dlg.impListY);
        impBg.setFillColor(scheme.boardBg);
        window.draw(impBg);

        if (dlg.importFiles.empty())
        {
            drawTextFB(window, font, tr("No imported files yet", "暂无待导入文件"),
                       dlgX + dlgW / 2, dlgY + dlgH / 2,
                       (unsigned int)(dlgH * 0.035f), scheme.textDark, 0, 1);
        }
        else
        {
            // entry = filename; the small font auto-shrinks with name length, wraps up to 3 lines
            const float itemSpacing = dlgH * 0.085f;
            const float itemTopOff = dlgH * 0.006f;
            const float itemH = dlgH * 0.075f;
            dlg.impItemSpacing = itemSpacing;
            dlg.impItemTopOffset = itemTopOff;
            dlg.impItemH = itemH;
            const int visCnt = (int)(dlg.impListH / itemSpacing);
            if (dlg.importScroll > (int)dlg.importFiles.size() - visCnt) dlg.importScroll = std::max(0, (int)dlg.importFiles.size() - visCnt);
            for (int i = 0; i < visCnt; ++i)
            {
                int idx = dlg.importScroll + i;
                if (idx >= (int)dlg.importFiles.size()) break;
                const float itemY = dlg.impListY + i * itemSpacing + itemTopOff;
                // take the filename (path stripped)
                std::string fn = dlg.importFiles[idx];
                size_t sl = fn.find_last_of("\\/");
                std::string disp = (sl == std::string::npos) ? fn : fn.substr(sl + 1);
                // hover highlight
                sf::FloatRect ir(dlg.impListX, itemY, dlg.impListW, itemH);
                bool hover = (dlg.hoverIdx == idx);
                if (hover)
                {
                    sf::RectangleShape hb(sf::Vector2f(dlg.impListW, itemH));
                    hb.setPosition(dlg.impListX, itemY);
                    hb.setFillColor(sf::Color(70, 130, 180, 60));
                    window.draw(hb);
                }
                // adaptive small font: shrink in steps when the width exceeds 90% of the list width (min 10px)
                const sf::String dispStr = sfStringFromAnsi(disp);   // ANSI->UTF-16, avoids garbled Chinese filenames
                unsigned int fsize = (unsigned int)(itemH * 0.42f);
                if (fsize < 10) fsize = 10;
                while (fsize > 10 && measureTextFB(font, dispStr, fsize) > dlg.impListW * 0.78f)
                    fsize -= 2;
                // wrap up to 3 lines: split by the available width
                const float lineH = itemH * 0.30f;
                int line = 0;
                sf::String remaining = dispStr;
                while (line < 3 && !remaining.isEmpty())
                {
                    // truncate char by char to the width
                    sf::String acc;
                    for (size_t k = 0; k < remaining.getSize(); ++k)
                    {
                        sf::String trial = acc + sf::String(remaining[k]);
                        if (measureTextFB(font, trial, fsize) > dlg.impListW * 0.78f) break;
                        acc = trial;
                    }
                    if (acc.isEmpty()) acc = sf::String(remaining[0]);
                    drawTextFB(window, font, acc,
                               dlg.impListX + dlg.impListW * 0.04f,
                               itemY + lineH * line + itemH * 0.5f - lineH * 0.5f,
                               fsize, scheme.textDark, 0, 0);   // left-aligned
                    // remove the already-drawn prefix characters
                    for (size_t k = 0; k < acc.getSize(); ++k) remaining.erase(0);
                    ++line;
                }
                // delete button (entry bottom-right, small circle + x; same geometry as the click hit-test)
                float iDelSize = std::max(13.f, itemH * 0.45f);
                float iDelX = dlg.impListX + dlg.impListW - iDelSize - 4.f;
                float iDelY = itemY + itemH - iDelSize - 4.f;
                sf::CircleShape idb(iDelSize * 0.5f);
                idb.setPosition(iDelX, iDelY);
                idb.setFillColor(sf::Color(168, 64, 64, 235));
                idb.setOutlineColor(sf::Color(255, 255, 255, 180));
                idb.setOutlineThickness(1.5f);
                window.draw(idb);
                {
                    float inset = iDelSize * 0.30f;
                    sf::VertexArray cross(sf::Lines, 4);
                    cross[0].position = sf::Vector2f(iDelX + inset, iDelY + inset);
                    cross[1].position = sf::Vector2f(iDelX + iDelSize - inset, iDelY + iDelSize - inset);
                    cross[2].position = sf::Vector2f(iDelX + iDelSize - inset, iDelY + inset);
                    cross[3].position = sf::Vector2f(iDelX + inset, iDelY + iDelSize - inset);
                    for (int li = 0; li < 4; ++li) cross[li].color = sf::Color::White;
                    window.draw(cross);
                }
            }
        }

        // bottom three keys: choose file / load all / close (centered)
        const float btnH = dlgH * 0.06f * 0.75f;
        const float btnGap = dlgW * 0.04f;
        const float btnW = dlgW * 0.20f;
        const float totalBtnW = btnW * 3.f + btnGap * 2.f;
        const float btnX0 = dlgX + (dlgW - totalBtnW) / 2.f;
        const float btnY = listBottom + dlgH * 0.015f;
        dlg.selX = btnX0; dlg.selY = btnY; dlg.selW = btnW; dlg.selH = btnH;
        dlg.loadAllX = btnX0 + btnW + btnGap; dlg.loadAllY = btnY; dlg.loadAllW = btnW; dlg.loadAllH = btnH;
        dlg.closeBtnX = btnX0 + (btnW + btnGap) * 2; dlg.closeBtnY = btnY; dlg.closeBtnW = btnW; dlg.closeBtnH = btnH;
        auto drawImpBtn = [&](float x, const char* zh, const char* en, bool closeStyle) {
            sf::RectangleShape b(sf::Vector2f(btnW, btnH));
            b.setPosition(x, btnY);
            b.setFillColor(closeStyle ? scheme.scoreBoxColor : scheme.boardBg);
            b.setOutlineColor(scheme.boardBg);
            b.setOutlineThickness(1.f);
            window.draw(b);
            drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                       x + btnW / 2, btnY + btnH / 2, (unsigned int)(btnH * 0.5f),
                       closeStyle ? sf::Color::White : scheme.textDark, 0, 1);
        };
        drawImpBtn(dlg.selX, "选择文件", "Select Files", false);
        drawImpBtn(dlg.loadAllX, "全部加载", "Load All", false);
        drawImpBtn(dlg.closeBtnX, "关闭", "Close", true);
        return;   // the import panel is self-contained
    }

    // ===== history panel (the software's own operation history) =====
    // list area (between the top dual keys and the bottom two keys; no title)
    float listY = g_historyDlg.tabHistY + g_historyDlg.tabHistH + dlgH * 0.02f;
    float listH = dlgH * 0.80f;  // larger list share
    float listW = dlgW * 0.92f;
    float listX = dlgX + (dlgW - listW) / 2.f;

    dlg.listAreaX = listX;
    dlg.listAreaY = listY;
    dlg.listAreaW = listW;
    dlg.listAreaH = listH;

    // list background
    sf::RectangleShape listBg(sf::Vector2f(listW, listH));
    listBg.setPosition(listX, listY);
    listBg.setFillColor(scheme.boardBg);
    window.draw(listBg);

   
    float itemH = listH * 0.135f * 1.46f;
    dlg.itemH = itemH;
    int totalItems = histVisibleCount(dlg);
    int visibleCount = (int)(listH / itemH);
    if (visibleCount > totalItems && totalItems > 0) visibleCount = totalItems;
    if (dlg.scrollPos > totalItems - visibleCount)
        dlg.scrollPos = std::max(0, totalItems - visibleCount);

    // hover tooltip (full filename): collected in the loop, drawn on top after the loop
    sf::String tipName;
    float tipX = 0.f, tipY = 0.f, tipW = 0.f;

    // entry rect inner padding
    const float itemPadX = 6.f;
    const float itemPadY = 4.f;
    float itemInnerW = listW - itemPadX * 2.f;
    float itemInnerH = itemH - itemPadY * 2.f;

    // delete button size (entry bottom-right small circle, global; shared by drawing/hit-test/filename avoidance)
    const float delSize = std::max(15.f, itemInnerH * 0.27f);
    // export button: left of the delete button with a gap ("a bit farther"); same circle size as delete, refined look
    const float expGap = delSize * 0.5f;
    const float expSize = delSize;   // same size as the delete button

    // draw visible entries (scroll model like the font dialog)
    int startIdx = dlg.scrollPos;
    int endIdx = std::min(startIdx + visibleCount, totalItems);
    for (int visIdx = startIdx; visIdx < endIdx; ++visIdx)
    {
        int idx = histEntryAt(dlg, visIdx);
        if (idx < 0) continue;
        float itemY = listY + (visIdx - startIdx) * itemH;

        const HistoryEntry& entry = dlg.entries[idx];
        // best-score grouping by board size (3x3/4x4/5x5 each has its own champion color)
        int szIdx = entry.boardSize - 3;
        bool isMaxOfSize = (szIdx >= 0 && szIdx < 3 && idx == dlg.maxScoreIdxBySize[szIdx]);
        bool isHover = (visIdx == dlg.hoverIdx);

        float itemX = listX + itemPadX;
        float drawY = itemY + itemPadY;

        // entry background (best score: tile fill, colored per board size -> different boards naturally differ)
        sf::RectangleShape itemBg(sf::Vector2f(itemInnerW, itemInnerH));
        itemBg.setPosition(itemX, drawY);
        if (isMaxOfSize)
        {
            // fill color: lightened highlight of the scheme (alpha ~ 200, keeps text readable)
            //   3x3 red scheme:   (255, 160, 160) <- border (220, 50, 50)
            //   4x4 yellow scheme: (255, 235, 130) <- border (255, 215, 0)
            //   5x5 light green:  (180, 240, 180) <- border (144, 238, 144)
            if (entry.boardSize == 3)
                itemBg.setFillColor(sf::Color(255, 160, 160, 200));       // 3x3 red
            else if (entry.boardSize == 5)
                itemBg.setFillColor(sf::Color(180, 240, 180, 200));       // 5x5 light green
            else
                itemBg.setFillColor(sf::Color(255, 235, 130, 200));       // 4x4 yellow
        }
        else if (isHover)
            itemBg.setFillColor(sf::Color(238, 228, 218, 220));
        else
            itemBg.setFillColor(sf::Color(238, 228, 218, 160));
        window.draw(itemBg);

        // import tag: blue box + "imported" text (overlaid on the entry top-right, same style as "AI assisted")
        // when the entry already has a tag (AI assisted / challenge), the import tag goes to the second line (below it)
        if (entry.imported)
        {
            bool hasOtherTag = entry.aiAssisted ||
                               (entry.challenge == 1 && entry.challengeProject >= 0 && entry.challengeProject < 6);
            float impTagW = itemInnerW * ((g_language == LANG_ZH) ? 0.16f : 0.22f);
            float impTagH = itemInnerH * 0.30f;
            float impTagX = itemX + itemInnerW - impTagW - 6.f;
            float impTagY = drawY + 4.f + (hasOtherTag ? (impTagH + 4.f) : 0.f);   // existing tag -> second line
            sf::RectangleShape impTag(sf::Vector2f(impTagW, impTagH));
            impTag.setPosition(impTagX, impTagY);
            impTag.setFillColor(sf::Color(60, 130, 230, 220));   // same blue as "AI assisted"
            impTag.setOutlineColor(sf::Color(30, 80, 200));
            impTag.setOutlineThickness(1.5f);
            window.draw(impTag);
            unsigned int impFontSize = (unsigned int)(impTagH * 0.52f);
            sf::String impStr = (g_language == LANG_ZH) ? toSFString("导入") : sf::String("Import");
            drawTextFB(window, font, impStr,
                       impTagX + impTagW / 2.f, impTagY + impTagH / 2.f,
                       impFontSize, sf::Color::White, sf::Text::Bold, 1);
        }

        // AI-assisted tag: blue box + text (overlaid on the entry top-right)
        //   localized tag text: "AI-assisted"
        if (entry.aiAssisted)
        {
            // English "AI-assisted" has more chars, so the tag box is wider
            float aiTagW = itemInnerW * ((g_language == LANG_ZH) ? 0.22f : 0.30f);
            float aiTagH = itemInnerH * 0.30f;
            float aiTagX = itemX + itemInnerW - aiTagW - 6.f;
            float aiTagY = drawY + 4.f;   // first line; the "imported" tag already moved itself to the second line
            sf::RectangleShape aiTag(sf::Vector2f(aiTagW, aiTagH));
            aiTag.setPosition(aiTagX, aiTagY);
            aiTag.setFillColor(sf::Color(60, 130, 230, 220));
            aiTag.setOutlineColor(sf::Color(30, 80, 200));
            aiTag.setOutlineThickness(1.5f);
            window.draw(aiTag);

            unsigned int aiFontSize = (unsigned int)(aiTagH * 0.52f);
            // Chinese must go through toSFString (UTF-8->UTF-16); sf::String(const char*) would misread it as Latin-1
            sf::String aiStr = (g_language == LANG_ZH)
                ? toSFString("AI 辅助")
                : sf::String("AI-assisted");
            drawTextFB(window, font, aiStr,
                       aiTagX + aiTagW / 2.f, aiTagY + aiTagH / 2.f,
                       aiFontSize, sf::Color::White, sf::Text::Bold, 1);
        }

        // challenge project tag: orange box + project text (same position as the AI tag, top-right)
        if (entry.challenge == 1 && entry.challengeProject >= 0 && entry.challengeProject < 6)
        {
            std::string projLabel = chProjectLabel(entry.challengeProject);
            sf::String chTagStr = (g_language == LANG_ZH)
                ? toSFString(("挑战 " + projLabel).c_str())
                : sf::String("CH ") + sf::String(projLabel);
            float chTagW = itemInnerW * 0.34f * 0.75f;   // tag length = 0.75x the original
            float chTagH = itemInnerH * 0.30f;
            float chTagX = itemX + itemInnerW - chTagW - 6.f;
            float chTagY = drawY + 4.f;
            sf::RectangleShape chTag(sf::Vector2f(chTagW, chTagH));
            chTag.setPosition(chTagX, chTagY);
            chTag.setFillColor(sf::Color(235, 150, 40, 230));
            chTag.setOutlineColor(sf::Color(200, 110, 20));
            chTag.setOutlineThickness(1.5f);
            window.draw(chTag);
            unsigned int chFontSize = (unsigned int)(chTagH * 0.5f);
            drawTextFB(window, font, chTagStr,
                       chTagX + chTagW / 2.f, chTagY + chTagH / 2.f,
                       chFontSize, sf::Color::White, sf::Text::Bold, 1);
        }

        // mini board preview (left)
        float miniSize = itemH * 0.85f;
        float miniX = listX + 10.f;
        float miniY = itemY + (itemH - miniSize) / 2.f;

        int gs = entry.boardSize;
        float mgap = miniSize * 0.04f;
        float mtile = (miniSize - mgap * (gs + 1)) / gs;

        // mini board background
        sf::RectangleShape miniBg(sf::Vector2f(miniSize, miniSize));
        miniBg.setPosition(miniX, miniY);
        miniBg.setFillColor(scheme.boardBg);
        window.draw(miniBg);

        // mini board tiles
        const SchemeData& entryScheme = SCHEMES[entry.colorScheme];
        for (int i = 0; i < gs; ++i)
        {
            for (int j = 0; j < gs; ++j)
            {
                int val = entry.board[i][j];
                float tx = miniX + mgap + j * (mtile + mgap);
                float ty = miniY + mgap + i * (mtile + mgap);

                sf::RectangleShape tile(sf::Vector2f(mtile, mtile));
                tile.setPosition(tx, ty);
                if (val == 0)
                    tile.setFillColor(entryScheme.emptyColor);
                else
                {
                    int power = 0, v = val;
                    while (v > 1) { v >>= 1; power++; }
                    if (power >= 1 && power <= MAX_POWER)
                        tile.setFillColor(entryScheme.tileColors[power]);
                    else
                        tile.setFillColor(entryScheme.emptyColor);
                }
                window.draw(tile);

                // draw the number on the tile (mini version, same rules as the game board, 6+ digits round up)
                if (val > 0)
                {
                    sf::Color miniTextColor = val >= entryScheme.lightTextFromValue
                        ? entryScheme.textLight : entryScheme.textDark;
                    std::string numStr = std::to_string(val);
                    int len = (int)numStr.length();

                    if (len >= 6)
                    {
                        unsigned int miniFontSize = (unsigned int)(mtile * 0.28f);
                        int splitPos = std::min(5, len - 1);
                        std::string line1 = numStr.substr(0, splitPos);
                        std::string line2 = numStr.substr(splitPos);

                        sf::Text t1(line1, font, miniFontSize);
                        t1.setStyle(sf::Text::Bold);
                        t1.setFillColor(miniTextColor);
                        sf::FloatRect b1 = t1.getLocalBounds();
                        t1.setOrigin(b1.left + b1.width / 2, b1.top + b1.height / 2);
                        t1.setPosition(tx + mtile / 2.f, ty + mtile / 2.f - mtile * 0.18f);
                        window.draw(t1);

                        sf::Text t2(line2, font, miniFontSize);
                        t2.setStyle(sf::Text::Bold);
                        t2.setFillColor(miniTextColor);
                        sf::FloatRect b2 = t2.getLocalBounds();
                        t2.setOrigin(b2.left + b2.width / 2, b2.top + b2.height / 2);
                        t2.setPosition(tx + mtile / 2.f, ty + mtile / 2.f + mtile * 0.18f);
                        window.draw(t2);
                    }
                    else
                    {
                        unsigned int miniFontSize = (unsigned int)(mtile * 0.42f);
                        if (val >= 100)  miniFontSize = (unsigned int)(mtile * 0.34f);
                        if (val >= 10000) miniFontSize = (unsigned int)(mtile * 0.28f);

                        sf::Text numText(numStr, font, miniFontSize);
                        numText.setStyle(sf::Text::Bold);
                        numText.setFillColor(miniTextColor);
                        sf::FloatRect nBounds = numText.getLocalBounds();
                        numText.setOrigin(nBounds.left + nBounds.width / 2,
                                          nBounds.top + nBounds.height / 2);
                        numText.setPosition(tx + mtile / 2.f, ty + mtile / 2.f);
                        window.draw(numText);
                    }
                }
            }
        }

        // score and board size (right)
        float textX = miniX + miniSize + 12.f;

        // first line: score (challenge shows operation time)
        sf::String scoreStr;
        if (entry.challenge == 1)
        {
            std::string t = chFormatTime(entry.challengeMs);
            scoreStr = (g_language == LANG_ZH)
                ? toSFString("时间： ") + toSFString(t.c_str())
                : sf::String("Time: ") + toSFString(t.c_str());
        }
        else
        {
            scoreStr = (g_language == LANG_ZH)
                ? toSFString("分数： ") + sf::String(std::to_string(entry.score))    // Chinese: colon followed by a space
                : sf::String("Score: ") + sf::String(std::to_string(entry.score));
        }
        // second line: board size + history time (replaces the old "topic")
        sf::String infoStr;
        if (entry.year > 0)
        {
            if (g_language == LANG_ZH)
                infoStr = sf::String(std::to_string(entry.boardSize) + "x" + std::to_string(entry.boardSize) + "  ")
                        + toSFString((std::to_string(entry.year) + " 年 " + std::to_string(entry.month) + " 月 " + std::to_string(entry.day) + " 日").c_str());
            else
            {
                static const char* monthsEN[] = { "January", "February", "March", "April", "May", "June",
                                                  "July", "August", "September", "October", "November", "December" };
                int m = (entry.month >= 1 && entry.month <= 12) ? entry.month : 1;
                infoStr = sf::String(std::to_string(entry.boardSize) + "x" + std::to_string(entry.boardSize) + "  ")
                        + sf::String(monthsEN[m - 1]) + " " + std::to_string(entry.day) + ", " + std::to_string(entry.year);
            }
        }
        else
        {
            // legacy files with an unparseable date -> fall back to showing the scheme name
            infoStr = (g_language == LANG_ZH)
                ? sf::String(std::to_string(entry.boardSize) + "x" + std::to_string(entry.boardSize) + "  ") + toSFString(schemeNamesZh[entry.colorScheme])
                : sf::String(std::to_string(entry.boardSize) + "x" + std::to_string(entry.boardSize) + "  ") + sf::String(schemeNames[entry.colorScheme]);
        }

        if (entry.imported)
        {
            // imported entry: three lines of info (score / time / filename), smaller font
            unsigned int sFont = (unsigned int)(itemH * 0.24f);
            unsigned int tFont = (unsigned int)(itemH * 0.18f);
            unsigned int fFont = (unsigned int)(itemH * 0.16f);
            drawTextFB(window, font, scoreStr,
                       textX, itemY + itemH * 0.06f + sFont * 0.5f, sFont, scheme.textDark, sf::Text::Bold, 0);
            drawTextFB(window, font, infoStr,
                       textX, itemY + itemH * 0.34f + tFont * 0.5f, tFont, scheme.textDark, 0, 0);
            // third line: original source filename before import (ellipsis when too long; hover shows the full name)
            // the right side reserves space for the delete + export buttons so the filename is not overlapped
            float nameMaxW = (itemX + itemInnerW) - textX - (delSize + expGap + expSize + 12.f);
            std::string dispName = entry.importedName.empty() ? entry.filename : entry.importedName;
            // keep the original name: strip the imported_ prefix and extension, restoring the source filename
            if (dispName.size() >= 9 && dispName.compare(0, 9, "imported_") == 0)
                dispName = dispName.substr(9);
            size_t idot = dispName.find_last_of('.');
            if (idot != std::string::npos) dispName = dispName.substr(0, idot);
            sf::String fullName = sfStringFromAnsi(dispName);   // ANSI->UTF-16, imported Chinese source names display correctly
            sf::String shownName = truncateToWidth(font, fullName, fFont, nameMaxW);
            float nameY = itemY + itemH * 0.60f + fFont * 0.5f;
            drawTextFB(window, font, shownName, textX, nameY, fFont, scheme.textDark, 0, 0);
            if (shownName != fullName)
            {
                sf::Vector2i mpos = sf::Mouse::getPosition(window);
                float shownW = measureTextFB(font, shownName, fFont);
                sf::FloatRect nr(textX, itemY + itemH * 0.55f, shownW, itemH * 0.26f);
                if (mpos.x >= nr.left && mpos.x <= nr.left + nr.width &&
                    mpos.y >= nr.top && mpos.y <= nr.top + nr.height)
                {
                    tipName = fullName;
                    tipX = textX;
                    tipY = itemY + itemH * 0.62f + itemH * 0.22f;   // below the filename
                    float fullW = measureTextFB(font, fullName, fFont);
                    float maxTipW = (listX + listW) - tipX - 4.f;
                    tipW = std::min(fullW + 12.f, maxTipW);
                }
            }
        }
        else
        {
            // non-imported entry: two lines (score / time)
            unsigned int scoreFontSize = (unsigned int)(itemH * 0.32f);
            if (entry.challenge == 1)
                scoreFontSize = (unsigned int)(itemH * 0.32f * 0.75f);   // challenge-time font = 0.75x current
            drawTextFB(window, font, scoreStr,
                       textX, itemY + itemH * 0.15f + scoreFontSize * 0.5f,
                       scoreFontSize, scheme.textDark, sf::Text::Bold, 0);
            unsigned int infoFontSize = (unsigned int)(itemH * 0.22f);
            drawTextFB(window, font, infoStr,
                       textX, itemY + itemH * 0.55f + infoFontSize * 0.5f,
                       infoFontSize, scheme.textDark, 0, 0);
        }

        // delete button (entry rect bottom-right, small and neat: circle + x)
        // uses the same geometry formula as the click hit-test (see mouse-press handling)
        float delX = itemX + itemInnerW - delSize - 5.f;
        float delY = drawY + itemInnerH - delSize - 5.f;
        bool delHover = (visIdx == dlg.hoverDelIdx);
        sf::CircleShape delBtn(delSize * 0.5f);
        delBtn.setPosition(delX, delY);
        delBtn.setFillColor(delHover ? sf::Color(224, 64, 64) : sf::Color(168, 64, 64, 235));
        delBtn.setOutlineColor(delHover ? sf::Color(255, 255, 255, 240) : sf::Color(255, 255, 255, 180));
        delBtn.setOutlineThickness(1.5f);
        window.draw(delBtn);
        // x mark (two crossing thin lines, font-independent)
        {
            float inset = delSize * 0.30f;
            sf::VertexArray cross(sf::Lines, 4);
            cross[0].position = sf::Vector2f(delX + inset, delY + inset);
            cross[1].position = sf::Vector2f(delX + delSize - inset, delY + delSize - inset);
            cross[2].position = sf::Vector2f(delX + delSize - inset, delY + inset);
            cross[3].position = sf::Vector2f(delX + inset, delY + delSize - inset);
            for (int li = 0; li < 4; ++li) cross[li].color = sf::Color::White;
            window.draw(cross);
        }

        // export button (left of the delete button, a bit farther; small circle + down arrow, refined)
        // uses the same geometry formula as the click hit-test (see mouse-press handling)
        float expX = delX - expGap - expSize;
        float expY = delY;
        bool expHover = (visIdx == dlg.hoverExpIdx);
        sf::CircleShape expBtn(expSize * 0.5f);
        expBtn.setPosition(expX, expY);
        expBtn.setFillColor(expHover ? sf::Color(70, 130, 180) : sf::Color(47, 95, 145, 230));
        expBtn.setOutlineColor(expHover ? sf::Color(255, 255, 255, 240) : sf::Color(255, 255, 255, 180));
        expBtn.setOutlineThickness(1.5f);
        window.draw(expBtn);
        // hover on the export button: show an "export" tooltip above it
        if (expHover)
        {
            tipName = (g_language == LANG_ZH) ? toSFString("导出") : sf::String("Export");
            tipX = expX;
            tipY = expY - itemH * 0.26f;
            float tipTxtW = measureTextFB(font, tipName, (unsigned int)(itemH * 0.12f)) + 12.f;
            tipW = std::min(tipTxtW, (listX + listW) - tipX - 4.f);
        }
        // down arrow (export): a center vertical line + bottom converging lines forming a "down" glyph
        {
            float ex = expX + expSize / 2.f, ey = expY + expSize / 2.f;
            float er = expSize * 0.5f;
            sf::RectangleShape stem(sf::Vector2f(er * 0.16f, er * 0.55f));
            stem.setOrigin(stem.getSize().x / 2.f, 0.f);
            stem.setPosition(ex, ey - er * 0.32f);
            stem.setFillColor(sf::Color::White);
            window.draw(stem);
            sf::ConvexShape head(3);
            head.setPoint(0, sf::Vector2f(ex, ey + er * 0.42f));
            head.setPoint(1, sf::Vector2f(ex - er * 0.28f, ey + er * 0.08f));
            head.setPoint(2, sf::Vector2f(ex + er * 0.28f, ey + er * 0.08f));
            head.setFillColor(sf::Color::White);
            window.draw(head);
        }
    }

    // scrollbar
    if (totalItems > visibleCount)
    {
        float scrollBarW = 8.f;
        float scrollBarX = listX + listW - scrollBarW - 2.f;
        float scrollBarH = listH;
        sf::RectangleShape scrollTrack(sf::Vector2f(scrollBarW, scrollBarH));
        scrollTrack.setPosition(scrollBarX, listY);
        scrollTrack.setFillColor(sf::Color(187, 173, 160, 120));
        window.draw(scrollTrack);

        float thumbH = scrollBarH * visibleCount / totalItems;
        float maxScroll = (float)(totalItems - visibleCount);
        float thumbY = listY + (scrollBarH - thumbH) * dlg.scrollPos / maxScroll;
        sf::RectangleShape scrollThumb(sf::Vector2f(scrollBarW, thumbH));
        scrollThumb.setPosition(scrollBarX, thumbY);
        scrollThumb.setFillColor(sf::Color(119, 110, 101, 200));
        window.draw(scrollThumb);
    }

    // tooltip: an elongated box showing the full filename (drawn last, on top)
    if (!tipName.isEmpty())
    {
        float tipH = itemH * 0.24f;
        unsigned int tipFont = (unsigned int)(tipH * 0.5f);
        sf::RectangleShape tip(sf::Vector2f(tipW, tipH));
        tip.setPosition(tipX, tipY);
        tip.setFillColor(sf::Color(40, 40, 40, 245));
        tip.setOutlineColor(sf::Color(210, 210, 210));
        tip.setOutlineThickness(1.f);
        window.draw(tip);
        drawTextFB(window, font, tipName, tipX + tipW / 2.f, tipY + tipH / 2.f, tipFont, sf::Color::White, 0, 1);
    }

    // empty-list hint
    if (totalItems == 0)
    {
        drawTextFB(window, font, tr("No history records yet", "暂无历史记录"),
                   dlgX + dlgW / 2, dlgY + dlgH / 2,
                   (unsigned int)(dlgH * 0.04f), scheme.textDark, 0, 1);
    }

    // bottom two keys: best records / close (centered)
    // height = 0.75x the original, moved down (no contact with the list/scroll area), centered between the list area and the dialog bottom
    float btnH = dlgH * 0.06f * 0.75f;
    float btnGap = dlgW * 0.04f;
    float btnW = dlgW * 0.22f;
    float totalBtnW = btnW * 2.f + btnGap;
    float btnX0 = dlgX + (dlgW - totalBtnW) / 2.f;
    float areaTop = listY + listH + dlgH * 0.02f;      // gap from the list/scroll area
    float areaBottom = dlgY + dlgH - dlgH * 0.03f;
    float bandH = areaBottom - areaTop;
    float btnY = areaTop + (bandH - btnH) * 0.5f;      // centered between the scroll area and the dialog bottom

    dlg.maxBtnX  = btnX0;
    dlg.maxBtnY  = btnY;
    dlg.maxBtnW  = btnW;
    dlg.maxBtnH  = btnH;
    dlg.closeBtnX = btnX0 + btnW + btnGap;
    dlg.closeBtnY = btnY;
    dlg.closeBtnW = btnW;
    dlg.closeBtnH = btnH;

    const sf::Color activeBlue(70, 130, 180);
    auto drawBottomBtn = [&](float x, const char* zh, const char* en, bool highlight, bool whiteText) {
        sf::RectangleShape b(sf::Vector2f(btnW, btnH));
        b.setPosition(x, btnY);
        if (highlight)
            b.setFillColor(activeBlue);
        else if (whiteText)
            b.setFillColor(scheme.scoreBoxColor);
        else
            b.setFillColor(scheme.boardBg);
        window.draw(b);
        drawTextFB(window, font, (g_language == LANG_ZH) ? toSFString(zh) : sf::String(en),
                   x + btnW / 2, btnY + btnH / 2,
                   (unsigned int)(btnH * 0.5f),
                   highlight ? sf::Color::White : scheme.textDark, 0, 1);
    };
    drawBottomBtn(dlg.maxBtnX, "最高纪录", "Best",     dlg.filterMax, false);
    drawBottomBtn(dlg.closeBtnX, "关闭",   "Close",    false, true);

    // ===== delete confirm dialog (modal, overlaid on the history dialog) =====
    if (dlg.confirmDelete)
    {
        // semi-transparent overlay (full screen)
        sf::RectangleShape cfmOverlay(sf::Vector2f((float)winW, (float)winH));
        cfmOverlay.setFillColor(sf::Color(0, 0, 0, 160));
        window.draw(cfmOverlay);

        // confirm box (fixed golden ratio width:height = 1.618:1)
        const float kGOLD = 1.618f;
        float cfmW = dlgW * 0.62f;
        float cfmH = cfmW / kGOLD;
        if (cfmH > dlgH * 0.32f)          // height over the cap -> derive the width from the height, keeping the ratio
        {
            cfmH = dlgH * 0.32f;
            cfmW = cfmH * kGOLD;
        }
        float cfmX = dlgX + (dlgW - cfmW) / 2.f;
        float cfmY = dlgY + (dlgH - cfmH) / 2.f;
        sf::RectangleShape cfmBg(sf::Vector2f(cfmW, cfmH));
        cfmBg.setPosition(cfmX, cfmY);
        cfmBg.setFillColor(scheme.windowBg);
        cfmBg.setOutlineColor(scheme.boardBg);
        cfmBg.setOutlineThickness(3.f);
        window.draw(cfmBg);

        // prompt text (two lines, centered)
        unsigned int cfmFont = (unsigned int)(cfmH * 0.11f);
        if (cfmFont < 13.f) cfmFont = 13;
        if (cfmFont > 30.f) cfmFont = 30;
        sf::String line1 = (g_language == LANG_ZH) ? toSFString("确定删除这条历史记录吗？")
                                                   : sf::String("Delete this history record?");
        sf::String line2 = (g_language == LANG_ZH) ? toSFString("删除后不可恢复")
                                                   : sf::String("This cannot be undone");
        drawTextFB(window, font, line1, cfmX + cfmW / 2, cfmY + cfmH * 0.32f, cfmFont,
                   scheme.textDark, sf::Text::Bold, 1);
        drawTextFB(window, font, line2, cfmX + cfmW / 2, cfmY + cfmH * 0.52f,
                   (unsigned int)(cfmFont * 0.8f), sf::Color(180, 80, 80), 0, 1);

        // two buttons: left "yes" (normal), right "no" (guided highlight); both keys use the golden ratio 1.618:1
        float cfmBtnH = cfmH * 0.18f;
        if (cfmBtnH < 30.f) cfmBtnH = 30.f;
        if (cfmBtnH > 44.f) cfmBtnH = 44.f;
        float cfmBtnW = cfmBtnH * kGOLD;
        float cfmBtnGap = cfmW * 0.10f;
        float cfmBtnY = cfmY + cfmH * 0.68f;
        float cfmTotalW = cfmBtnW * 2 + cfmBtnGap;
        if (cfmTotalW > cfmW * 0.92f)     // overflow guard: shrink the height proportionally when too wide, keeping key ratio
        {
            cfmBtnH *= cfmW * 0.92f / cfmTotalW;
            cfmBtnW = cfmBtnH * kGOLD;
            cfmTotalW = cfmBtnW * 2 + cfmBtnGap;
        }
        float cfmBtnX0 = cfmX + (cfmW - cfmTotalW) / 2.f;
        unsigned int cfmBtnFont = (unsigned int)(cfmBtnH * 0.48f);
        if (cfmBtnFont < 13.f) cfmBtnFont = 13;

        // "yes" normal key
        dlg.delYesX = cfmBtnX0; dlg.delYesY = cfmBtnY; dlg.delYesW = cfmBtnW; dlg.delYesH = cfmBtnH;
        sf::RectangleShape yesBtn(sf::Vector2f(cfmBtnW, cfmBtnH));
        yesBtn.setPosition(dlg.delYesX, dlg.delYesY);
        yesBtn.setFillColor(scheme.boardBg);
        yesBtn.setOutlineColor(scheme.boardBg);
        yesBtn.setOutlineThickness(1.f);
        window.draw(yesBtn);
        drawTextFB(window, font, tr("Yes", "是"),
                   dlg.delYesX + cfmBtnW / 2, dlg.delYesY + cfmBtnH / 2,
                   cfmBtnFont, scheme.textDark, 0, 1);

        // "no" guided highlight (default highlight: blue fill + white text)
        dlg.delNoX = cfmBtnX0 + cfmBtnW + cfmBtnGap; dlg.delNoY = cfmBtnY;
        dlg.delNoW = cfmBtnW; dlg.delNoH = cfmBtnH;
        sf::RectangleShape noBtn(sf::Vector2f(cfmBtnW, cfmBtnH));
        noBtn.setPosition(dlg.delNoX, dlg.delNoY);
        noBtn.setFillColor(activeBlue);
        window.draw(noBtn);
        drawTextFB(window, font, tr("No", "否"),
                   dlg.delNoX + cfmBtnW / 2, dlg.delNoY + cfmBtnH / 2,
                   cfmBtnFont, sf::Color::White, sf::Text::Bold, 1);
    }
}

// ========== Mini board preview (generic, custom grid size support) ==========
static void drawMiniBoard(sf::RenderWindow& window, const sf::Font& font,
                          float miniX, float miniY, float miniSize,
                          const SchemeData& scheme, int gridSize = 4,
                          int highlightPower = -1)
{
    if (gridSize < 3) gridSize = 3;
    if (gridSize > 5) gridSize = 5;
    float mgap = miniSize * 0.04f;
    float mtile = (miniSize - mgap * (gridSize + 1)) / gridSize;

    sf::RectangleShape miniBg(sf::Vector2f(miniSize, miniSize));
    miniBg.setPosition(miniX, miniY);
    miniBg.setFillColor(scheme.boardBg);
    window.draw(miniBg);

    // generate demo tile values: assign powers in row*gridSize+col order
    int powerIdx = 1;
    for (int i = 0; i < gridSize; ++i)
    {
        for (int j = 0; j < gridSize; ++j)
        {
            int val = (1 << powerIdx);
            if (powerIdx > MAX_POWER) val = 0;
            float tx = miniX + mgap + j * (mtile + mgap);
            float ty = miniY + mgap + i * (mtile + mgap);
            sf::RectangleShape tile(sf::Vector2f(mtile, mtile));
            tile.setPosition(tx, ty);
            if (val == 0)
                tile.setFillColor(scheme.emptyColor);
            else
            {
                if (powerIdx >= 1 && powerIdx <= MAX_POWER)
                    tile.setFillColor(scheme.tileColors[powerIdx]);
                else
                    tile.setFillColor(scheme.emptyColor);
            }
            // highlight the selected power
            if (powerIdx == highlightPower && highlightPower > 0)
            {
                tile.setOutlineColor(sf::Color(255, 220, 80));
                tile.setOutlineThickness(2.f);
            }
            window.draw(tile);

            if (val > 0)
            {
                sf::Color tcol = val >= scheme.lightTextFromValue
                    ? scheme.textLight : scheme.textDark;
                std::string num = std::to_string(val);
                int len = (int)num.size();

                if (len >= 6)
                {
                    unsigned int fs = (unsigned int)(mtile * 0.28f);
                    int splitPos = std::min(5, len - 1);
                    std::string line1 = num.substr(0, splitPos);
                    std::string line2 = num.substr(splitPos);

                    sf::Text t1(line1, font, fs);
                    t1.setStyle(sf::Text::Bold);
                    t1.setFillColor(tcol);
                    sf::FloatRect b1 = t1.getLocalBounds();
                    t1.setOrigin(b1.left + b1.width / 2, b1.top + b1.height / 2);
                    t1.setPosition(tx + mtile / 2.f, ty + mtile / 2.f - mtile * 0.18f);
                    window.draw(t1);

                    sf::Text t2(line2, font, fs);
                    t2.setStyle(sf::Text::Bold);
                    t2.setFillColor(tcol);
                    sf::FloatRect b2 = t2.getLocalBounds();
                    t2.setOrigin(b2.left + b2.width / 2, b2.top + b2.height / 2);
                    t2.setPosition(tx + mtile / 2.f, ty + mtile / 2.f + mtile * 0.18f);
                    window.draw(t2);
                }
                else
                {
                    unsigned int fs = (unsigned int)(mtile * 0.42f);
                    if (val >= 100)  fs = (unsigned int)(mtile * 0.34f);
                    if (val >= 10000) fs = (unsigned int)(mtile * 0.28f);

                    sf::Text tt(num, font, fs);
                    tt.setStyle(sf::Text::Bold);
                    tt.setFillColor(tcol);
                    sf::FloatRect tb = tt.getLocalBounds();
                    tt.setOrigin(tb.left + tb.width / 2, tb.top + tb.height / 2);
                    tt.setPosition(tx + mtile / 2.f, ty + mtile / 2.f);
                    window.draw(tt);
                }
            }
            powerIdx++;
        }
    }
}

// ========== Timer mode: milestone panel (same height as the board, IPS on top, adaptive list) ==========
// Panel: top = board top (boardY), bottom = board bottom (boardY+boardPx), same height as the board
// Header shows only IPS (one line); below is the milestone list, height adapts to window scaling
// Milestone panel rewrite: top edge aligned to scoreBoxY (close key top), bottom edge aligned to the board bottom
// Top: IPS small rect (same height as the time tile), below: the milestone list
// Tile height = board height / 9, colored by power, unachieved shows no time, all 2^5..2^16 prelisted
// Timer-mode milestone cap (2^k tile), by board size:
//   3×3 → 1024 (2^10)；4×4 → 131072 (2^17)；5×5 → 2^26
static int tmMaxMilestonePow(int sz)
{
    return (sz == 3) ? 10 : (sz == 4 ? 17 : 26);
}

// Milestone panel: the IPS rect sits above the panel (same row as the "count/time" stats row),
// top edge = "close" rect top, bottom edge = board bottom; the list is exactly 10 items;
// each item = tile + first-achieved time after the tile (not exceeding the right edge)
static void drawTimerMilestones(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                float boardX, float boardY, float boardPx, const SchemeData& scheme,
                                float scoreBoxH, float gapBtn, float statsRowY, float closeY)
{
    const int sz     = game.getBoardSize();
    const int kMax   = tmMaxMilestonePow(sz);       // cap: 3->10, 4->17, 5->26
    const int kMin   = 5;                            // starts at 2^5 = 32
    const int totalItems = kMax - kMin + 1;

    // milestone bar width comes from the big-block geometry (height:width = 1:1.3 whole); gap to board = gapBtn
    MilestoneGeo mg = computeMilestoneGeo(window.getSize().x, window.getSize().y);
    const float mileW = mg.mileW;
    const float mileX = boardX + boardPx + mg.gapBtn;

    // === IPS rect: timer mode only (above the milestone bar, same row as the "count/time" stats row, width = milestone bar width) ===
    // AI mode has no IPS data, not drawn; the panel starts directly at the "close" row
    const bool showIpsRect = g_tm.active;
    const float ipsH = scoreBoxH * 0.6f;
    const float ipsY = statsRowY;
    if (showIpsRect)
    {
        sf::RectangleShape ipsBox(sf::Vector2f(mileW, ipsH));
        ipsBox.setPosition(mileX, ipsY);
        ipsBox.setFillColor(scheme.scoreBoxColor);
        window.draw(ipsBox);
        char ib[32];
        std::snprintf(ib, sizeof(ib), "IPS: %.1f", g_tm.ips);
        drawTextFB(window, font, toSFString(ib),
                   mileX + mileW / 2, ipsY + ipsH / 2,
                   (unsigned int)(ipsH * 0.42f), scheme.textDark, sf::Text::Bold, 1);
    }

    // === milestone panel: top edge = close button (right box) top, bottom edge = board bottom ===
    const float mileTopY = closeY;
    const float mileBotY = boardY + boardPx;
    const float mileH = mileBotY - mileTopY;
    g_tm.mileRect = sf::FloatRect(mileX, mileTopY, mileW, mileH);

    sf::RectangleShape mileBg(sf::Vector2f(mileW, mileH));
    mileBg.setPosition(mileX, mileTopY);
    mileBg.setFillColor(scheme.boardBg);
    window.draw(mileBg);

    // === milestone list: exactly 10 items (full list prelaid, truncated inside the panel) ===
    const int itemCount = 10;
    const float itemH = mileH / (float)itemCount;
    const float boxSize  = itemH * 0.80f;            // tile
    const float boxLeftPad = itemH * 0.10f;
    const float listTopY = mileTopY;                 // panel top = list top
    const float boxX = mileX + boxLeftPad;

    int vis = std::min(itemCount, totalItems);
    g_tm.mileScroll = std::max(0.f, std::min(g_tm.mileScroll, (float)(totalItems - vis)));
    g_tm.mileItemH = itemH;
    g_tm.mileVis = vis;
    g_tm.mileListTopOff = 0.f;                       // list starts at the panel top (for hit-testing)

    // time: right-aligned to the panel right edge (grows leftward as minute digits grow = "adding digits backward"), not exceeding the right edge and not overlapping the tile
    const float timeRight = mileX + mileW - itemH * 0.08f;        // margin at the panel right edge
    const float tileRightMin = boxX + boxSize + itemH * 0.10f;    // tile right + min gap
    const float timeMaxW = timeRight - tileRightMin;
    const unsigned int timeFont0 = (unsigned int)(itemH * 0.30f);

    for (int i = 0; i < vis; ++i)
    {
        int k = kMin + (int)g_tm.mileScroll + i;
        if (k > kMax) break;
        float itemY = listTopY + i * itemH;
        bool done = (g_tm.milestoneMs[k] > 0);

        // hover highlight
        if (g_tm.mileHover == k)
        {
            sf::RectangleShape hb(sf::Vector2f(mileW, itemH));
            hb.setPosition(mileX, itemY);
            hb.setFillColor(sf::Color(70, 130, 180, 50));
            window.draw(hb);
        }

        // tile color block (scheme tileColors[power], same as getTileColor)
        int val = 1 << k;
        float boxY = itemY + (itemH - boxSize) / 2.f;
        sf::RectangleShape box(sf::Vector2f(boxSize, boxSize));
        box.setPosition(boxX, boxY);
        if (k >= 1 && k <= MAX_POWER)
            box.setFillColor(scheme.tileColors[k]);
        else
            box.setFillColor(scheme.textDark);
        window.draw(box);

        // number inside the tile: same logic as custom colors (drawTileNumber)
        char nbuf[16];
        std::snprintf(nbuf, sizeof(nbuf), "%d", val);
        drawTileNumber(window, font, std::string(nbuf),
                       boxX + boxSize / 2.f, boxY + boxSize / 2.f,
                       boxSize, done ? scheme.textDark : scheme.textLight);

        // first-achieved time (MM:SS.mmm): right-aligned, unachieved shows faded --:--.---, auto-shrink when too wide to avoid overlap
        std::string ts = done ? tmFormatTime((double)g_tm.milestoneMs[k]) : "--:--.---";
        unsigned int tf = timeFont0;
        sf::Text tt(toSFString(ts.c_str()), font, tf);
        sf::FloatRect tb = tt.getLocalBounds();
        if (tb.width > timeMaxW && timeMaxW > 8.f)
        {
            tf = (unsigned int)(tf * timeMaxW / tb.width);
            if (tf < 6) tf = 6;
        }
        drawTextFB(window, font, toSFString(ts.c_str()), timeRight, itemY + itemH / 2.f,
                   tf, done ? scheme.textDark : sf::Color(130, 130, 138), 0, 2);
    }
}

static void drawChallengeSettings(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                                  float boardX, float boardY, float boardPx, const SchemeData& scheme)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    float popW = boardPx;
    float popH = boardPx * 0.55f;
    float popX = boardX;
    float popY = boardY + (boardPx - popH) / 2.f;
    g_ch.settingsRect = sf::FloatRect(popX, popY, popW, popH);

    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 120));
    window.draw(overlay);

    sf::RectangleShape pop(sf::Vector2f(popW, popH));
    pop.setPosition(popX, popY);
    pop.setFillColor(scheme.windowBg);
    pop.setOutlineColor(sf::Color(235, 150, 40));
    pop.setOutlineThickness(3.f);
    window.draw(pop);

    unsigned int titleFont = (unsigned int)(popH * 0.13f);
    drawTextFB(window, font, tr("Challenge Project", "挑战项目"),
               popX + popW / 2, popY + popH * 0.10f, titleFont, scheme.textDark, sf::Text::Bold, 1);

    // 6 project buttons: 2 rows x 3 columns
    float btnW = popW * 0.26f;
    float btnH = popH * 0.17f;
    float btnGapX = (popW - btnW * 3) / 4.f;
    float btnGapY = popH * 0.045f;
    float startY = popY + popH * 0.26f;
    unsigned int btnFont = (unsigned int)(popH * 0.08f);
    for (int i = 0; i < 6; ++i)
    {
        int row = i / 3, col = i % 3;
        float bx = popX + btnGapX + col * (btnW + btnGapX);
        float by = startY + row * (btnH + btnGapY);
        bool cur = (i == g_ch.project);
        g_ch.projBtn[i] = sf::FloatRect(bx, by, btnW, btnH);
        sf::RectangleShape b(sf::Vector2f(btnW, btnH));
        b.setPosition(bx, by);
        b.setFillColor(cur ? sf::Color(235, 150, 40) : scheme.boardBg);
        window.draw(b);
        const ChallengeProject& p = g_chProjects[i];
        std::string lbl = std::to_string(p.boardSize) + "x" + std::to_string(p.boardSize) + "  " + std::to_string(p.target);
        drawTextFB(window, font, toSFString(lbl.c_str()),
                   bx + btnW / 2, by + btnH / 2, btnFont,
                   cur ? sf::Color::White : scheme.textDark, sf::Text::Bold, 1);
    }

    // close button (bottom, centered)
    float closeW = popW * 0.3f;
    float closeH = popH * 0.13f;
    float closeX = popX + (popW - closeW) / 2.f;
    float closeY = popY + popH - closeH - popH * 0.05f;
    g_ch.btnClose = sf::FloatRect(closeX, closeY, closeW, closeH);
    sf::RectangleShape cb(sf::Vector2f(closeW, closeH));
    cb.setPosition(closeX, closeY);
    cb.setFillColor(scheme.scoreBoxColor);
    window.draw(cb);
    drawTextFB(window, font, tr("Close", "关闭"),
               closeX + closeW / 2, closeY + closeH / 2,
               (unsigned int)(closeH * 0.55f), scheme.textDark, 0, 1);

    // switch-project confirm dialog
    if (g_ch.confirmSwitchProject >= 0)
    {
        float cw = popW * 0.70f;
        float ch = popH * 0.40f;
        float cx = popX + (popW - cw) / 2.f;
        float cy = popY + (popH - ch) / 2.f;
        sf::RectangleShape cbg(sf::Vector2f(cw, ch));
        cbg.setPosition(cx, cy);
        cbg.setFillColor(scheme.windowBg);
        cbg.setOutlineColor(sf::Color(235, 150, 40));
        cbg.setOutlineThickness(2.f);
        window.draw(cbg);
        unsigned int msgFont = (unsigned int)(ch * 0.13f);
        drawTextFB(window, font, tr("Switching project will restart.\nContinue?", "切换项目将重新开始。\n是否继续？"),
                   cx + cw / 2, cy + ch * 0.30f, msgFont, scheme.textDark, sf::Text::Bold, 1);
        float btnW = cw * 0.30f;
        float btnH = ch * 0.22f;
        float btnY = cy + ch * 0.60f;
        float gap = cw * 0.10f;
        float total = btnW * 2 + gap;
        float startX = cx + (cw - total) / 2.f;
        g_ch.confirmYes = sf::FloatRect(startX, btnY, btnW, btnH);
        g_ch.confirmNo = sf::FloatRect(startX + btnW + gap, btnY, btnW, btnH);
        sf::RectangleShape yesBtn(sf::Vector2f(btnW, btnH));
        yesBtn.setPosition(startX, btnY);
        yesBtn.setFillColor(sf::Color(60, 180, 90));
        window.draw(yesBtn);
        drawTextFB(window, font, tr("Yes", "是"),
                   startX + btnW / 2, btnY + btnH / 2,
                   (unsigned int)(btnH * 0.55f), sf::Color::White, sf::Text::Bold, 1);
        sf::RectangleShape noBtn(sf::Vector2f(btnW, btnH));
        noBtn.setPosition(startX + btnW + gap, btnY);
        noBtn.setFillColor(sf::Color(180, 80, 80));
        window.draw(noBtn);
        drawTextFB(window, font, tr("No", "否"),
                   startX + btnW + gap + btnW / 2, btnY + btnH / 2,
                   (unsigned int)(btnH * 0.55f), sf::Color::White, sf::Text::Bold, 1);
    }
}

// ========== Challenge result dialog (success / fail) ==========
static void drawChallengeResult(sf::RenderWindow& window, const Game2048& game, const sf::Font& font)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const SchemeData& scheme = game.getScheme();
    const float GOLDEN = 2.f;
    float dlgW = winW * 0.55f;
    float dlgH = dlgW / GOLDEN;
    if (dlgH > winH * 0.85f) { dlgH = winH * 0.85f; dlgW = dlgH * GOLDEN; }
    float dlgX = (winW - dlgW) / 2.f;
    float dlgY = (winH - dlgH) / 2.f;

    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 180));
    window.draw(overlay);

    sf::Color borderCol = g_ch.won ? sf::Color(60, 180, 90) : sf::Color(220, 70, 60);
    sf::RectangleShape dlgBg(sf::Vector2f(dlgW, dlgH));
    dlgBg.setPosition(dlgX, dlgY);
    dlgBg.setFillColor(scheme.windowBg);
    dlgBg.setOutlineColor(borderCol);
    dlgBg.setOutlineThickness(4.f);
    window.draw(dlgBg);

    // title
    unsigned int titleSize = (unsigned int)(dlgH * 0.13f);
    if (titleSize < 20) titleSize = 20;
    if (titleSize > 60) titleSize = 60;
    sf::String titleStr = g_ch.won
        ? tr("Congratulations! Challenge completed!", "恭喜你挑战成功！")
        : tr("Challenge failed!", "挑战失败！");
    drawTextFB(window, font, titleStr, winW / 2.f, dlgY + dlgH * 0.30f,
               titleSize, g_ch.won ? sf::Color(60, 150, 70) : sf::Color(200, 60, 50), sf::Text::Bold, 1);

    // project + time
    std::string projLabel = chProjectLabel(g_ch.project);
    std::string timeStr = chFormatTime(g_ch.elapsedMs);
    std::string info = (g_language == LANG_ZH)
        ? ("项目： " + projLabel + "    用时： " + timeStr)
        : ("Project: " + projLabel + "    Time: " + timeStr);
    unsigned int infoSize = (unsigned int)(dlgH * 0.09f);
    if (infoSize < 14) infoSize = 14;
    drawTextFB(window, font, toSFString(info.c_str()), winW / 2.f, dlgY + dlgH * 0.58f,
               infoSize, scheme.textDark, 0, 1);

    drawTextFB(window, font, tr("Click to close", "点击关闭"),
               winW / 2.f, dlgY + dlgH * 0.80f,
               (unsigned int)(dlgH * 0.07f), scheme.textDark, 0, 1);
}

// ========== Timer-mode result dialog (game over) ==========
// Shown when no move is possible: time / operation count / highest tile; clicking anywhere only closes this dialog (timer mode stays,
// then clicking the red score box starts the next game; real exit goes through the menu "timer mode" -> "close" / confirm dialog)
static void drawTimerResult(sf::RenderWindow& window, const Game2048& game, const sf::Font& font)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    const SchemeData& scheme = game.getScheme();
    const float GOLDEN = 2.f;
    float dlgW = winW * 0.55f;
    float dlgH = dlgW / GOLDEN;
    if (dlgH > winH * 0.85f) { dlgH = winH * 0.85f; dlgW = dlgH * GOLDEN; }
    float dlgX = (winW - dlgW) / 2.f;
    float dlgY = (winH - dlgH) / 2.f;

    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 180));
    window.draw(overlay);

    sf::Color borderCol = sf::Color(220, 70, 60);
    sf::RectangleShape dlgBg(sf::Vector2f(dlgW, dlgH));
    dlgBg.setPosition(dlgX, dlgY);
    dlgBg.setFillColor(scheme.windowBg);
    dlgBg.setOutlineColor(borderCol);
    dlgBg.setOutlineThickness(4.f);
    window.draw(dlgBg);

    // title
    unsigned int titleSize = (unsigned int)(dlgH * 0.13f);
    if (titleSize < 20) titleSize = 20;
    if (titleSize > 60) titleSize = 60;
    drawTextFB(window, font, tr("Game Over!", "游戏结束！"), winW / 2.f, dlgY + dlgH * 0.28f,
               titleSize, sf::Color(200, 60, 50), sf::Text::Bold, 1);

    // time / count / highest tile
    std::string timeStr = tmFormatTime(g_tm.elapsedMs);
    std::string info = (g_language == LANG_ZH)
        ? ("用时： " + timeStr + "    操作次数： " + std::to_string(g_tm.moveCount)
           + "    最高方块： " + std::to_string(game.getMaxTile()))
        : ("Time: " + timeStr + "    Moves: " + std::to_string(g_tm.moveCount)
           + "    Max tile: " + std::to_string(game.getMaxTile()));
    unsigned int infoSize = (unsigned int)(dlgH * 0.09f);
    if (infoSize < 14) infoSize = 14;
    drawTextFB(window, font, toSFString(info.c_str()), winW / 2.f, dlgY + dlgH * 0.56f,
               infoSize, scheme.textDark, 0, 1);

    drawTextFB(window, font, tr("Click anywhere to continue; click the red score box to start next round",
                                "点击任意处继续；点击发红分数框开始下一局"),
               winW / 2.f, dlgY + dlgH * 0.80f,
               (unsigned int)(dlgH * 0.055f), scheme.textDark, 0, 1);
}

// ========== Custom color dialog rendering ==========
// Mini board (shape only, no numbers/text) -- for the theme list / custom list thumbnails
static void drawMiniBoardShape(sf::RenderWindow& window, const sf::Font& font,
                               float miniX, float miniY, float miniSize,
                               const SchemeData& scheme, int gridSize = 3)
{
    if (gridSize < 3) gridSize = 3;
    if (gridSize > 5) gridSize = 5;
    float mgap = miniSize * 0.05f;
    float mtile = (miniSize - mgap * (gridSize + 1)) / gridSize;

    sf::RectangleShape miniBg(sf::Vector2f(miniSize, miniSize));
    miniBg.setPosition(miniX, miniY);
    miniBg.setFillColor(scheme.boardBg);
    window.draw(miniBg);

    int powerIdx = 1;
    for (int i = 0; i < gridSize; ++i)
        for (int j = 0; j < gridSize; ++j)
        {
            float tx = miniX + mgap + j * (mtile + mgap);
            float ty = miniY + mgap + i * (mtile + mgap);
            sf::RectangleShape tile(sf::Vector2f(mtile, mtile));
            tile.setPosition(tx, ty);
            if (powerIdx >= 1 && powerIdx <= MAX_POWER)
                tile.setFillColor(scheme.tileColors[powerIdx]);
            else
                tile.setFillColor(scheme.emptyColor);
            window.draw(tile);
            ++powerIdx;
        }
}

void drawCustomColorDlg(sf::RenderWindow& window, const Game2048& game, const sf::Font& font,
                        CustomColorDlg& dlg)
{
    unsigned int winW = window.getSize().x;
    unsigned int winH = window.getSize().y;
    SchemeData& cs = SCHEMES[CUSTOM_SCHEME_IDX];

    // width = board width (boardPx), height:width = 1.2:1, centered in the window
    float boardPx = computeBoardPx(winW, winH, false);
    float dlgW = boardPx;
    float dlgH = dlgW * 1.2f;
    // scale down proportionally when it exceeds the window
    if (dlgH > (float)winH)
    {
        float scale = (float)winH / dlgH;
        dlgW *= scale;
        dlgH *= scale;
    }
    if (dlgW > (float)winW)
    {
        float scale = (float)winW / dlgW;
        dlgW *= scale;
        dlgH *= scale;
    }
    float dlgX = (winW - dlgW) / 2.f;
    float dlgY = (winH - dlgH) / 2.f;
    dlg.dlgX = dlgX; dlg.dlgY = dlgY; dlg.dlgW = dlgW; dlg.dlgH = dlgH;

    const SchemeData& base = SCHEMES[0]; // use Cool's text color for the title etc.

    // semi-transparent overlay
    sf::RectangleShape overlay(sf::Vector2f((float)winW, (float)winH));
    overlay.setFillColor(sf::Color(0, 0, 0, 180));
    window.draw(overlay);

    // dialog body
    sf::RectangleShape dlgBg(sf::Vector2f(dlgW, dlgH));
    dlgBg.setPosition(dlgX, dlgY);
    dlgBg.setFillColor(cs.windowBg);
    dlgBg.setOutlineColor(cs.boardBg);
    dlgBg.setOutlineThickness(3.f);
    window.draw(dlgBg);

    unsigned int titleSize = (unsigned int)(dlgH * 0.045f);
    drawTextFB(window, font, tr("Color Scheme", "配色方案"),
               dlgX + dlgW / 2, dlgY + dlgH * 0.04f,
               titleSize, cs.textDark, sf::Text::Bold, 1);   // system text

    // ========== top theme selection area (three zones) ==========
    // top 2/5: left = preset theme list | middle = custom list | right = game preview
    float topY = dlgY + dlgH * 0.07f;      // moved up as a whole
    float topH = dlgH * 0.385f;            // slightly compressed, leaving room for the title area below
    float topMargin = dlgW * 0.015f;
    float topGap = dlgW * 0.02f;
    // theme/custom list width = the color list width below (editInnerW * 0.30f); the preview gets the rest
    float listAreaW = (dlgW - 2 * topMargin) * 0.30f;
    float r1W = listAreaW;
    float r2W = listAreaW;
    float r3W = dlgW - 2 * topMargin - 2 * listAreaW - 2 * topGap;
    dlg.topAreaX = dlgX + topMargin; dlg.topAreaY = topY;
    dlg.topAreaW = dlgW - 2 * topMargin; dlg.topAreaH = topH;

    // --- zone 1: preset theme list (6 original + 11 added = 17) ---
    {
        float r1X = dlgX + topMargin;
        float r1Y = topY;
        float r1W = listAreaW;
        float r1H = topH;
        unsigned int secFont = (unsigned int)(r1H * 0.066f);   // 60% × 110%
        drawTextFB(window, font, tr("Themes", "主题"),
                   r1X + r1W / 2, r1Y + r1H * 0.07f,
                   secFont, cs.textDark, sf::Text::Bold, 1);
        float listTop = r1Y + r1H * 0.14f;
        float listH = r1H - listTop + r1Y - r1H * 0.02f;
        float itemH = listH * 0.16f;
        int totalItems = NUM_PRESET_SCHEMES;
        int visibleCount = (int)(listH / itemH);
        if (visibleCount > totalItems) visibleCount = totalItems;
        int maxScroll = totalItems - visibleCount;
        if (maxScroll < 0) maxScroll = 0;
        if (dlg.presetScroll > maxScroll) dlg.presetScroll = maxScroll;
        if (dlg.presetScroll < 0) dlg.presetScroll = 0;

        // first draw after opening: auto-locate the current theme (runs once)
        if (dlg.needScrollToCurrent)
        {
            int target = -1;
            int curScheme = game.getColorScheme();
            if (curScheme >= 0 && curScheme != CUSTOM_SCHEME_IDX && curScheme <= NUM_PRESET_SCHEMES)
            {
                for (int pi = 0; pi < NUM_PRESET_SCHEMES && target < 0; ++pi)
                {
                    int s = (pi < NUM_SCHEMES) ? pi : (CUSTOM_SCHEME_IDX + 1 + (pi - NUM_SCHEMES));
                    if (s == curScheme) target = pi;
                }
            }
            else if (curScheme == CUSTOM_SCHEME_IDX)
            {
                for (int pi = 0; pi < NUM_PRESET_SCHEMES && target < 0; ++pi)
                {
                    int s = (pi < NUM_SCHEMES) ? pi : (CUSTOM_SCHEME_IDX + 1 + (pi - NUM_SCHEMES));
                    if (schemeEquals(SCHEMES[CUSTOM_SCHEME_IDX], SCHEMES[s])) target = pi;
                }
            }
            if (target >= 0)
            {
                int sc = target - visibleCount / 2;
                if (sc < 0) sc = 0;
                if (sc > maxScroll) sc = maxScroll;
                dlg.presetScroll = sc;
            }
        }

        sf::RectangleShape listBg(sf::Vector2f(r1W, listH));
        listBg.setPosition(r1X, listTop);
        listBg.setFillColor(cs.boardBg);
        window.draw(listBg);

        // mouse hover detection (same hover effect as the color list below)
        int hoverVis = -1;
        {
            sf::Vector2i mpos = sf::Mouse::getPosition(window);
            if (mpos.x >= r1X && mpos.x <= r1X + r1W &&
                mpos.y >= listTop && mpos.y <= listTop + listH)
            {
                int hv = (int)((mpos.y - listTop) / itemH);
                if (hv >= 0 && hv < visibleCount) hoverVis = hv;
            }
        }

        // "current theme" tag rects of the used theme (collected in the loop, drawn after the scrollbar so they stay on top)
        std::vector<sf::FloatRect> curFrames;
        for (int vis = 0; vis < visibleCount; ++vis)
        {
            int idx = dlg.presetScroll + vis;
            int schemeIdx = (idx < NUM_SCHEMES) ? idx : (CUSTOM_SCHEME_IDX + 1 + (idx - NUM_SCHEMES));
            float iy = listTop + vis * itemH;
            // current active theme highlight: presets match by index; when a custom slot is active, compare colors against each preset to find its origin
            bool isCur = false;
            {
                int curScheme = game.getColorScheme();
                // current preset (0-5 or 7-32, excluding custom slot 6) matches by index; custom slot (6) matches by color
                if (curScheme >= 0 && curScheme != CUSTOM_SCHEME_IDX && curScheme <= NUM_PRESET_SCHEMES)
                    isCur = (schemeIdx == curScheme);
                else if (curScheme == CUSTOM_SCHEME_IDX)
                    isCur = schemeEquals(SCHEMES[CUSTOM_SCHEME_IDX], SCHEMES[schemeIdx]);
            }
            
            // click to select (preview only, not applied)
            bool isSelected = (dlg.selPresetIdx == idx);
            bool isHover = (vis == hoverVis);
            sf::RectangleShape ib(sf::Vector2f(r1W - 2, itemH - 1));
            ib.setPosition(r1X + 1, iy + 0.5f);
            if (isCur || isSelected)
                ib.setFillColor(sf::Color(255, 220, 140, 220));   // selected (currently applied or clicked): yellow
            else if (isHover)
                ib.setFillColor(sf::Color(255, 230, 205, 210));   // hover: light pink-yellow
            else
                ib.setFillColor(sf::Color(255, 242, 208, 165));   // default unselected: light yellow
            window.draw(ib);
            // left mini board
            float ms = itemH * 0.72f;
            float mx = r1X + 3.f;
            float my = iy + (itemH - ms) / 2.f;
            drawMiniBoardShape(window, font, mx, my, ms, SCHEMES[schemeIdx], 3);
            // name
            std::string nm = (g_language == LANG_ZH) ? schemeNamesZh[schemeIdx] : schemeNames[schemeIdx];
            unsigned int fs = (unsigned int)(itemH * 0.42f);
            drawTextFB(window, font, toSFString(nm.c_str()),
                       r1X + ms + 7.f, iy + itemH / 2, fs, cs.textDark, 0, 0);
            // record the "current theme" tag rect of the used theme (top-right; kept after closing and reopening)
            if (isCur)
            {
                float tagW = r1W * 0.38f;   // 80% of the current width
                float tagH = itemH * 0.51f;   // 50% taller than before
                curFrames.push_back(sf::FloatRect(r1X + r1W - tagW - 5.f, iy + 3.f, tagW, tagH));
            }
        }
        // scrollbar
        if (totalItems > visibleCount)
        {
            float sbw = 5.f;
            float sbx = r1X + r1W - sbw - 2.f;
            sf::RectangleShape st(sf::Vector2f(sbw, listH));
            st.setPosition(sbx, listTop);
            st.setFillColor(sf::Color(187, 173, 160, 120));
            window.draw(st);
            float thumbH = listH * visibleCount / totalItems;
            float ms2 = (float)maxScroll;
            float ty = listTop + (listH - thumbH) * (ms2 == 0 ? 0 : dlg.presetScroll / ms2);
            sf::RectangleShape tmb(sf::Vector2f(sbw, thumbH));
            tmb.setPosition(sbx, ty);
            tmb.setFillColor(sf::Color(119, 110, 101, 200));
            window.draw(tmb);
        }
        // draw the "current theme" tag on top (same style as the history AI tag)
        for (const sf::FloatRect& r : curFrames)
        {
            sf::RectangleShape tag(sf::Vector2f(r.width, r.height));
            tag.setPosition(r.left, r.top);
            tag.setFillColor(sf::Color(235, 150, 40, 230));
            tag.setOutlineColor(sf::Color(200, 110, 20));
            tag.setOutlineThickness(1.5f);
            window.draw(tag);
            unsigned int tagFont = (unsigned int)(r.height * 0.60f);   // slightly larger font
            sf::String tagStr = (g_language == LANG_ZH) ? toSFString("当前主题") : sf::String("Current");
            drawTextFB(window, font, tagStr,
                       r.left + r.width / 2.f, r.top + r.height / 2.f,
                       tagFont, sf::Color::White, sf::Text::Bold, 1);
        }
        dlg.presetItemX = r1X; dlg.presetItemY = listTop;
        dlg.presetItemW = r1W; dlg.presetItemH = itemH;
        dlg.presetListH = listH;
    }

    // --- zone 2: custom color list (20) ---
    {
        float r2X = dlgX + topMargin + r1W + topGap;
        float r2Y = topY;
        float r2W = listAreaW;
        float r2H = topH;
        unsigned int secFont = (unsigned int)(r2H * 0.066f);   // 60% × 110%
        drawTextFB(window, font, tr("Custom", "自定义"),
                   r2X + r2W / 2, r2Y + r2H * 0.07f,
                   secFont, cs.textDark, sf::Text::Bold, 1);
        float listTop = r2Y + r2H * 0.14f;
        float listH = r2H - listTop + r2Y - r2H * 0.02f;
        float itemH = listH * 0.16f;
        int totalItems = MAX_CUSTOM_PRESETS;
        int visibleCount = (int)(listH / itemH);
        if (visibleCount > totalItems) visibleCount = totalItems;
        int maxScroll = totalItems - visibleCount;
        if (maxScroll < 0) maxScroll = 0;
        if (dlg.customScroll > maxScroll) dlg.customScroll = maxScroll;
        if (dlg.customScroll < 0) dlg.customScroll = 0;

        // first draw after opening: auto-locate the current custom theme (runs once)
        if (dlg.needScrollToCurrent)
        {
            int target = -1;
            if (game.getColorScheme() == CUSTOM_SCHEME_IDX)
            {
                for (int ci = 0; ci < MAX_CUSTOM_PRESETS && target < 0; ++ci)
                {
                    const CustomPreset& p = dlg.presets[ci];
                    if (!p.used) continue;
                    SchemeData ps{};
                    std::memcpy(ps.tileColors, p.tileColors, sizeof(ps.tileColors));
                    ps.emptyColor = p.emptyColor; ps.windowBg = p.windowBg; ps.boardBg = p.boardBg;
                    ps.scoreBoxColor = p.scoreBoxColor; ps.textLight = p.textLight; ps.textDark = p.textDark;
                    ps.lightTextFromValue = p.lightTextFromValue; ps.name = "";
                    if (schemeEquals(SCHEMES[CUSTOM_SCHEME_IDX], ps)) target = ci;
                }
            }
            if (target >= 0)
            {
                int sc = target - visibleCount / 2;
                if (sc < 0) sc = 0;
                if (sc > maxScroll) sc = maxScroll;
                dlg.customScroll = sc;
            }
            dlg.needScrollToCurrent = false;   // both lists have been located
        }

        sf::RectangleShape listBg(sf::Vector2f(r2W, listH));
        listBg.setPosition(r2X, listTop);
        listBg.setFillColor(cs.boardBg);
        window.draw(listBg);

        // mouse hover detection (same hover effect as the theme/color lists)
        int hoverCustom = -1;
        {
            sf::Vector2i mpos = sf::Mouse::getPosition(window);
            if (mpos.x >= r2X && mpos.x <= r2X + r2W &&
                mpos.y >= listTop && mpos.y <= listTop + listH)
            {
                int hv = (int)((mpos.y - listTop) / itemH);
                if (hv >= 0 && hv < visibleCount) hoverCustom = hv;
            }
        }

        // "current theme" tag rects of the used theme (collected in the loop, drawn after the scrollbar so they stay on top)
        std::vector<sf::FloatRect> curFrames2;
        std::vector<std::pair<sf::FloatRect, bool>> delRects2;   // delete-button rects (drawn after the scrollbar so they sit above it)
        for (int vis = 0; vis < visibleCount; ++vis)
        {
            int idx = dlg.customScroll + vis;
            float iy = listTop + vis * itemH;
            const CustomPreset& p = dlg.presets[idx];
            // current applied highlight: the active custom slot matching the save is highlighted
            bool isCur = false;
            if (p.used && game.getColorScheme() == CUSTOM_SCHEME_IDX)
            {
                SchemeData ps{};
                std::memcpy(ps.tileColors, p.tileColors, sizeof(ps.tileColors));
                ps.emptyColor = p.emptyColor; ps.windowBg = p.windowBg; ps.boardBg = p.boardBg;
                ps.scoreBoxColor = p.scoreBoxColor; ps.textLight = p.textLight; ps.textDark = p.textDark;
                ps.lightTextFromValue = p.lightTextFromValue; ps.name = "";
                isCur = schemeEquals(SCHEMES[CUSTOM_SCHEME_IDX], ps);
            }
            // click to select (preview only, not applied)
            bool isSelected = (dlg.selCustomIdx == idx);
            bool isHover = (vis == hoverCustom);
            sf::RectangleShape ib(sf::Vector2f(r2W - 2, itemH - 1));
            ib.setPosition(r2X + 1, iy + 0.5f);
            if (isCur || isSelected)
                ib.setFillColor(sf::Color(255, 220, 140, 220));   // selected (currently applied or clicked): yellow
            else if (isHover)
                ib.setFillColor(sf::Color(255, 230, 205, 210));   // hover: light pink-yellow
            else
                ib.setFillColor(p.used ? sf::Color(238, 228, 218, 200) : sf::Color(238, 228, 218, 110)); // default: light yellow
            window.draw(ib);
            // mini board
            float ms = itemH * 0.72f;
            float mx = r2X + 3.f;
            float my = iy + (itemH - ms) / 2.f;
            if (p.used)
            {
                SchemeData ps{};
                std::memcpy(ps.tileColors, p.tileColors, sizeof(ps.tileColors));
                ps.emptyColor = p.emptyColor; ps.windowBg = p.windowBg; ps.boardBg = p.boardBg;
                ps.scoreBoxColor = p.scoreBoxColor; ps.textLight = p.textLight; ps.textDark = p.textDark;
                ps.lightTextFromValue = p.lightTextFromValue; ps.name = "";
                drawMiniBoardShape(window, font, mx, my, ms, ps, 3);
            }
            // name: custom n
            std::string nm = (g_language == LANG_ZH)
                ? std::string("自定义") + std::to_string(idx + 1)
                : std::string("Custom ") + std::to_string(idx + 1);
            unsigned int fs = (unsigned int)(itemH * 0.40f);
            drawTextFB(window, font, toSFString(nm.c_str()),
                       r2X + ms + 7.f, iy + itemH / 2, fs, cs.textDark, 0, 0);
            // delete button (collected, drawn after the scrollbar so they sit above it)
            dlg.customDelX[idx] = 0; dlg.customDelY[idx] = 0; dlg.customDelW[idx] = 0; dlg.customDelH[idx] = 0;
            if (p.used)
            {
                float delSize = itemH * 0.40f;
                float dx = r2X + r2W - delSize - 3.f;
                float dy = iy + (itemH - delSize) / 2.f;
                dlg.customDelX[idx] = dx; dlg.customDelY[idx] = dy;
                dlg.customDelW[idx] = delSize; dlg.customDelH[idx] = delSize;
                delRects2.push_back({ sf::FloatRect(dx, dy, delSize, delSize), true });
            }
            // record the "current theme" tag rect of the used theme (top-right; kept after closing and reopening)
            if (isCur)
            {
                float tagW = r2W * 0.30f;   // 80% of the current width
                float tagH = itemH * 0.51f;   // 50% taller than before
                float tagX = r2X + r2W - tagW - 5.f;
                // with a delete button the tag moves left, not covering the square delete key
                if (p.used)
                {
                    float delSize = itemH * 0.40f;
                    float delLeft = r2X + r2W - delSize - 3.f;
                    tagX = delLeft - tagW - 4.f;
                    if (tagX < r2X + 2) tagX = r2X + 2;
                }
                curFrames2.push_back(sf::FloatRect(tagX, iy + 3.f, tagW, tagH));
            }
        }
        // scrollbar
        if (totalItems > visibleCount)
        {
            float sbw = 5.f;
            float sbx = r2X + r2W - sbw - 2.f;
            sf::RectangleShape st(sf::Vector2f(sbw, listH));
            st.setPosition(sbx, listTop);
            st.setFillColor(sf::Color(187, 173, 160, 120));
            window.draw(st);
            float thumbH = listH * visibleCount / totalItems;
            float ms2 = (float)maxScroll;
            float ty = listTop + (listH - thumbH) * (ms2 == 0 ? 0 : dlg.customScroll / ms2);
            sf::RectangleShape tmb(sf::Vector2f(sbw, thumbH));
            tmb.setPosition(sbx, ty);
            tmb.setFillColor(sf::Color(119, 110, 101, 200));
            window.draw(tmb);
        }
        // delete buttons drawn uniformly (above the scrollbar)
        for (const auto& dr : delRects2)
        {
            if (!dr.second) continue;
            const sf::FloatRect& r = dr.first;
            sf::RectangleShape delBtn(sf::Vector2f(r.width, r.height));
            delBtn.setPosition(r.left, r.top);
            delBtn.setFillColor(sf::Color(220, 80, 80));
            window.draw(delBtn);
            sf::Text delT("X", font, (unsigned int)(r.height * 0.7f));
            delT.setFillColor(sf::Color::White);
            delT.setStyle(sf::Text::Bold);
            sf::FloatRect db = delT.getLocalBounds();
            delT.setOrigin(db.left + db.width / 2, db.top + db.height / 2);
            delT.setPosition(r.left + r.width / 2, r.top + r.height / 2);
            window.draw(delT);
        }

        // draw the "current theme" tag on top (same style as the history AI tag)
        for (const sf::FloatRect& r : curFrames2)
        {
            sf::RectangleShape tag(sf::Vector2f(r.width, r.height));
            tag.setPosition(r.left, r.top);
            tag.setFillColor(sf::Color(235, 150, 40, 230));
            tag.setOutlineColor(sf::Color(200, 110, 20));
            tag.setOutlineThickness(1.5f);
            window.draw(tag);
            unsigned int tagFont = (unsigned int)(r.height * 0.60f);   // slightly larger font
            sf::String tagStr = (g_language == LANG_ZH) ? toSFString("当前主题") : sf::String("Current");
            drawTextFB(window, font, tagStr,
                       r.left + r.width / 2.f, r.top + r.height / 2.f,
                       tagFont, sf::Color::White, sf::Text::Bold, 1);
        }
        dlg.customItemX = r2X; dlg.customItemY = listTop;
        dlg.customItemW = r2W; dlg.customItemH = itemH;
        dlg.customListH = listH;
    }

    // --- zone 3: game preview (5x5 board + menu/score/undo shapes, no numbers) ---
    {
        float r3X = dlgX + topMargin + r1W + topGap + listAreaW + topGap;
        float r3Y = topY;
        // r3W comes from an external variable
        float r3H = topH;
        unsigned int secFont = (unsigned int)(r3H * 0.066f);   // 60% × 110%
        drawTextFB(window, font, tr("Preview", "预览"),
                   r3X + r3W / 2, r3Y + r3H * 0.07f,
                   secFont, cs.textDark, sf::Text::Bold, 1);
        // thumbnail (10% larger than the available space, centered relative to the custom scrollbar and the color list right frame)
        float availW = r3W * 0.88f;
        float availH = r3H * 0.82f;
        float scale90 = 1.0f;
        // simulate the board layout: computed proportionally with boardPx
        // board size = thumbnail size, button sizes scale from the board
        // top bar = left menu + score bar + right undo, width = board width
        // score bar centered; left/right keys aligned with the board ends
        float thumbBoardSize = std::min(availW, availH * 0.65f) * scale90;
        float thumbGap = thumbBoardSize * 0.04f;
        float thumbTile = (thumbBoardSize - thumbGap * 6) / 5.f;
        // top bar height
        float thumbBtnH = thumbBoardSize * (1.f / ((2.f + 2.f*1.618f + 2.f + 2.f*0.16f) * 0.80f));
        float thumbBtnGap = thumbBtnH * 0.16f;
        // menu/score/undo widths by ratio
        float thumbMenuW = thumbBtnH * 2.f;
        float thumbScoreW = thumbBtnH * 2.f * 1.618f;
        float thumbUndoW = thumbBtnH * 2.f;
        // total width = menu + gap + score + gap + undo = board width
        float thumbBarTotalW = thumbMenuW + thumbBtnGap + thumbScoreW + thumbBtnGap + thumbUndoW;
        // scale down proportionally when the total width exceeds the board width
        if (thumbBarTotalW > thumbBoardSize)
        {
            float scale = thumbBoardSize / thumbBarTotalW;
            thumbMenuW *= scale; thumbScoreW *= scale; thumbUndoW *= scale;
            thumbBtnGap *= scale;
        }
        // top bar left-aligned, score centered, undo right-aligned = menu left, undo right, score centered
        float thumbMenuX = 0;
        float thumbScoreX = thumbMenuW + thumbBtnGap;
        float thumbUndoX = thumbMenuW + thumbBtnGap + thumbScoreW + thumbBtnGap;
        // whole thumbnail height = top bar + gap + board
        float thumbBarH = thumbBtnH / 0.80f;  // topBarH
        float thumbTotalH = thumbBarH + thumbBtnGap + thumbBoardSize;
        // thumbnail centered in the available area
        float thX = r3X + (r3W - availW) / 2.f + (availW - thumbBoardSize) / 2.f;
        float thY = r3Y + r3H * 0.15f + (availH - thumbTotalH) / 2.f;
        // the thumbnail colors follow the preview theme (windowBg/boardBg/scoreBoxColor follow workingScheme)
        const SchemeData& ws = dlg.open ? dlg.workingScheme : SCHEMES[CUSTOM_SCHEME_IDX];
        // window frame mimicking the window
        float framePad = 4.f;
        // inside the frame, the background is filled with the window bg (everything except the board and the three buttons)
        sf::RectangleShape thumbBg(sf::Vector2f(thumbBoardSize + 2*framePad, thumbTotalH + 2*framePad));
        thumbBg.setPosition(thX - framePad, thY - framePad);
        thumbBg.setFillColor(ws.windowBg);
        window.draw(thumbBg);
        sf::RectangleShape frame(sf::Vector2f(thumbBoardSize + 2*framePad, thumbTotalH + 2*framePad));
        frame.setPosition(thX - framePad, thY - framePad);
        frame.setFillColor(sf::Color::Transparent);
        frame.setOutlineColor(sf::Color(160, 150, 140));
        frame.setOutlineThickness(2.f);
        window.draw(frame);
        // top bar background (window base color, same as thumbBg)
        sf::RectangleShape topBarBg(sf::Vector2f(thumbBoardSize, thumbBarH));
        topBarBg.setPosition(thX, thY);
        topBarBg.setFillColor(ws.windowBg);
        window.draw(topBarBg);
        // left key (menu, no text, theme board base color)
        sf::RectangleShape mb(sf::Vector2f(thumbMenuW, thumbBtnH));
        mb.setPosition(thX + thumbMenuX, thY + (thumbBarH - thumbBtnH) / 2.f);
        mb.setFillColor(ws.boardBg);
        window.draw(mb);
        // score bar (centered, theme score base color)
        sf::RectangleShape sb(sf::Vector2f(thumbScoreW, thumbBtnH));
        sb.setPosition(thX + thumbScoreX, thY + (thumbBarH - thumbBtnH) / 2.f);
        sb.setFillColor(ws.scoreBoxColor);
        window.draw(sb);
        // right key (undo, no text, theme board base color)
        sf::RectangleShape ub(sf::Vector2f(thumbUndoW, thumbBtnH));
        ub.setPosition(thX + thumbUndoX, thY + (thumbBarH - thumbBtnH) / 2.f);
        ub.setFillColor(ws.boardBg);
        window.draw(ub);
        // 5x5 board (shape without numbers, showing 2^1..2^26, theme board base color)
        float boardTop = thY + thumbBarH + thumbBtnGap;
        sf::RectangleShape bgb(sf::Vector2f(thumbBoardSize, thumbBoardSize));
        bgb.setPosition(thX, boardTop);
        bgb.setFillColor(ws.boardBg);
        window.draw(bgb);
        int powerIdx = 1;
        for (int i = 0; i < 5; ++i)
        {
            for (int j = 0; j < 5; ++j)
            {
                float tx = thX + thumbGap + j * (thumbTile + thumbGap);
                float ty = boardTop + thumbGap + i * (thumbTile + thumbGap);
                sf::RectangleShape tile(sf::Vector2f(thumbTile, thumbTile));
                tile.setPosition(tx, ty);
                if (powerIdx >= 1 && powerIdx <= MAX_POWER)
                    tile.setFillColor(ws.tileColors[powerIdx]);
                else
                    tile.setFillColor(ws.emptyColor);
                window.draw(tile);
                ++powerIdx;
            }
        }
    }

    // ========== middle title area: "custom colors" ==========
    float subTitleY = topY + topH + dlgH * 0.015f;
    float subTitleH = dlgH * 0.03f;
    unsigned int subFont = (unsigned int)(topH * 0.066f);   // same font size as the three zone titles
    drawTextFB(window, font, tr("Custom Color", "自定义配色"),
               dlgX + dlgW / 2.f, subTitleY + subTitleH / 2.f,
               subFont, cs.textDark, sf::Text::Bold, 1);   // system text

    // right of the title: import/export small buttons (above the gradient box; the last "export" right edge aligns with the gradient box right edge)
    {
        float pickerRight = dlgX + dlgW - dlgW * 0.015f;   // same right edge as the HSV gradient box (color picker)
        sf::String expLabel = tr("Export", "导出");
        sf::String impLabel = tr("Import", "导入");
        float btnH = subTitleH * 0.9f * 1.2f;          // height = 1.2x the original
        float btnY = subTitleY + (subTitleH - btnH) / 2.f;
        unsigned int btnFont = (unsigned int)(btnH * 0.55f);   // font grows accordingly
        float padX = btnH * 0.35f;
        // button width by actual text width (still wide enough after switching to English); length = 2x the original
        auto textW = [&](const sf::String& s) -> float {
            sf::Text tmp(s, font, btnFont);
            return tmp.getLocalBounds().width;
        };
        float expW = (textW(expLabel) + padX) * 2.0f;
        float impW = (textW(impLabel) + padX) * 2.0f;
        float gapBtn = dlgW * 0.008f;
        float expX = pickerRight - expW;                    // export right edge = gradient box right edge
        float impX = expX - gapBtn - impW;
        g_ccDlg.exportBtnX = expX; g_ccDlg.exportBtnY = btnY;
        g_ccDlg.exportBtnW = expW; g_ccDlg.exportBtnH = btnH;
        g_ccDlg.importBtnX = impX; g_ccDlg.importBtnY = btnY;
        g_ccDlg.importBtnW = impW; g_ccDlg.importBtnH = btnH;
        auto drawSmall = [&](float bx, float bw, const sf::String& label) {
            sf::RectangleShape b(sf::Vector2f(bw, btnH));
            b.setPosition(bx, btnY);
            b.setFillColor(cs.boardBg);
            b.setOutlineColor(sf::Color(150, 140, 130));
            b.setOutlineThickness(1.f);
            window.draw(b);
            drawTextFB(window, font, label, bx + bw / 2.f, btnY + btnH / 2.f,
                       btnFont, cs.textDark, 0, 1);   // function-key text uses system text
        };
        drawSmall(impX, impW, impLabel);
        drawSmall(expX, expW, expLabel);
    }

    // ========== lower edit area (color picker + color list + RGB) ==========
    float editTopY = subTitleY + subTitleH + dlgH * 0.012f;
    // bottom button reserve area
    float closeBtnH = dlgH * 0.06f;
    float btnY = dlgY + dlgH - closeBtnH - dlgH * 0.02f;
    float editH = btnY - editTopY - dlgH * 0.02f;
    // inside the edit area: left = color list, right = color picker + RGB
    float editInnerW = dlgW - 2 * topMargin;
    float listX = dlgX + topMargin;
    float listW = editInnerW * 0.30f;
    float rightX = listX + listW + dlgW * 0.012f;
    float rightW = dlgX + dlgW - topMargin - rightX;
    float listY = editTopY;
    float listH = editH;

    dlg.listAreaX = listX; dlg.listAreaY = listY;
    dlg.listAreaW = listW; dlg.listAreaH = listH;

    sf::RectangleShape listBg(sf::Vector2f(listW, listH));
    listBg.setPosition(listX, listY);
    listBg.setFillColor(cs.boardBg);
    window.draw(listBg);

    float itemH = listH * 0.10f;
    dlg.itemH = itemH;
    int totalItems = CC_TOTAL_ITEMS;
    int visibleCount = (int)(listH / itemH);
    if (visibleCount > totalItems) visibleCount = totalItems;
    int maxScroll = totalItems - visibleCount;
    if (maxScroll < 0) maxScroll = 0;
    if (dlg.scrollPos > maxScroll) dlg.scrollPos = maxScroll;
    if (dlg.scrollPos < 0) dlg.scrollPos = 0;

    int startIdx = dlg.scrollPos;
    int endIdx = std::min(startIdx + visibleCount, totalItems);
    for (int idx = startIdx; idx < endIdx; ++idx)
    {
        int target = ccIndexToTarget(idx);
        float itemY = listY + (idx - startIdx) * itemH;
        bool isSelected = (target == dlg.editTarget);
        bool isHover = (idx == dlg.hoverIdx);

        sf::RectangleShape ib(sf::Vector2f(listW - 2, itemH - 1));
        ib.setPosition(listX + 1, itemY + 0.5f);
        if (isSelected)
            ib.setFillColor(sf::Color(255, 220, 140, 220));
        else if (isHover)
            ib.setFillColor(sf::Color(238, 228, 218, 200));
        else
            ib.setFillColor(sf::Color(238, 228, 218, 130));
        window.draw(ib);

        // left color swatch
        sf::Color* cp = ccGetColor(target);
        float swSize = itemH * 0.75f;
        float swX = listX + 4;
        float swY = itemY + (itemH - swSize) / 2;
        sf::RectangleShape sw(sf::Vector2f(swSize, swSize));
        sw.setPosition(swX, swY);
        sw.setFillColor(cp ? *cp : sf::Color::Black);
        sw.setOutlineColor(sf::Color(0, 0, 0, 80));
        sw.setOutlineThickness(1);
        window.draw(sw);

        // tile color: draw the number inside the swatch (same rules as the game board, 6+ digits round up)
        if (target >= CEDIT_TILE_COLOR_START)
        {
            int power = target - CEDIT_TILE_COLOR_START;
            int value = (power >= 1 && power <= MAX_POWER) ? (1 << power) : 0;
            std::string numStr = std::to_string(value);
            if (value > 0)
            {
                const SchemeData& ws = g_ccDlg.open ? g_ccDlg.workingScheme : SCHEMES[CUSTOM_SCHEME_IDX];
                sf::Color textColor = value >= ws.lightTextFromValue ? ws.textLight : ws.textDark;
                int len = (int)numStr.length();

                if (len >= 6)
                {
                    unsigned int fontSize = (unsigned int)(swSize * 0.28f);
                    int splitPos = std::min(5, len - 1);
                    std::string line1 = numStr.substr(0, splitPos);
                    std::string line2 = numStr.substr(splitPos);

                    sf::Text t1(line1, font, fontSize);
                    t1.setStyle(sf::Text::Bold);
                    t1.setFillColor(textColor);
                    sf::FloatRect b1 = t1.getLocalBounds();
                    t1.setOrigin(b1.left + b1.width / 2, b1.top + b1.height / 2);
                    t1.setPosition(swX + swSize / 2.f, swY + swSize / 2.f - swSize * 0.18f);
                    window.draw(t1);

                    sf::Text t2(line2, font, fontSize);
                    t2.setStyle(sf::Text::Bold);
                    t2.setFillColor(textColor);
                    sf::FloatRect b2 = t2.getLocalBounds();
                    t2.setOrigin(b2.left + b2.width / 2, b2.top + b2.height / 2);
                    t2.setPosition(swX + swSize / 2.f, swY + swSize / 2.f + swSize * 0.18f);
                    window.draw(t2);
                }
                else
                {
                    unsigned int fontSize = (unsigned int)(swSize * 0.42f);
                    if (len >= 3) fontSize = (unsigned int)(swSize * 0.34f);
                    if (len >= 5) fontSize = (unsigned int)(swSize * 0.28f);

                    sf::Text num(numStr, font, fontSize);
                    num.setStyle(sf::Text::Bold);
                    sf::FloatRect bounds = num.getLocalBounds();
                    num.setOrigin(bounds.left + bounds.width / 2, bounds.top + bounds.height / 2);
                    num.setPosition(swX + swSize / 2.f, swY + swSize / 2.f);
                    num.setFillColor(textColor);
                    window.draw(num);
                }
            }
            unsigned int fs = (unsigned int)(itemH * 0.45f);
            std::string tileNm = ccGetTargetName(target);
            drawTextFB(window, font, toSFString(tileNm.c_str()),
                       swX + swSize + 6, itemY + itemH / 2,
                       fs, cs.textDark, 0, 0);
        }
        else
        {
            std::string nm = ccGetTargetName(target);
            unsigned int fs = (unsigned int)(itemH * 0.45f);
            drawTextFB(window, font, toSFString(nm.c_str()),
                       swX + swSize + 6, itemY + itemH / 2,
                       fs, cs.textDark, 0, 0);
        }
    }

    // scrollbar
    if (totalItems > visibleCount)
    {
        float sbw = 6.f;
        float sbx = listX + listW - sbw - 2;
        float sbh = listH;
        sf::RectangleShape st(sf::Vector2f(sbw, sbh));
        st.setPosition(sbx, listY);
        st.setFillColor(sf::Color(187, 173, 160, 120));
        window.draw(st);
        float thumbH = sbh * visibleCount / totalItems;
        float ms = (float)maxScroll;
        float thumbY = listY + (sbh - thumbH) * (ms == 0 ? 0 : dlg.scrollPos / ms);
        sf::RectangleShape tmb(sf::Vector2f(sbw, thumbH));
        tmb.setPosition(sbx, thumbY);
        tmb.setFillColor(sf::Color(119, 110, 101, 200));
        window.draw(tmb);
    }

    // ========== middle-top: HSV color picker (width fills the right zone, height = half the edit area, leaving room for RGB) ==========
    float pickerY = listY;
    float rightStartX = listX + listW + dlgW * 0.015f;
    float rightEndX   = dlgX + dlgW - dlgW * 0.015f;
    float rightAvailW = rightEndX - rightStartX;
    float pickerMaxW  = rightAvailW;
    float previewSize = pickerMaxW;  // width by available space (no longer min, keeps width)
    // height = half the edit area height (the lower half is for RGB + preview)
    float pickerH = (listY + listH - pickerY) * 0.52f;
    float previewW    = previewSize;
    float previewX    = rightStartX + (rightAvailW - previewSize) / 2.f;
    float previewY    = pickerY;
    dlg.previewX = previewX; dlg.previewY = previewY; dlg.previewSize = previewSize;
    dlg.pickerH = pickerH;  // v6.1: store the picker actual height

    // cache the picker texture (created once)
    // vertical axis: 3-segment gradient, upper half: white -> pure color (S: 0->1, V=1); lower half: pure -> black (S=1, V: 1->0)
    // horizontal axis: Hue 0->360
    static sf::Texture huePickerTex;
    static bool huePickerInitialized = false;
    if (!huePickerInitialized)
    {
        const int PICKER_TEX_SIZE = 256;
        sf::Image img;
        img.create(PICKER_TEX_SIZE, PICKER_TEX_SIZE, sf::Color::White);
        for (int yy = 0; yy < PICKER_TEX_SIZE; ++yy)
        {
            float t = (float)yy / (PICKER_TEX_SIZE - 1);
            float s, v;
            if (t <= 0.5f)
            {
                // upper half: t=0 top (white) -> t=0.5 middle (pure hue)
                // S: 0 → 1, V: 1.0
                s = t / 0.5f;
                v = 1.0f;
            }
            else
            {
                // lower half: t=0.5 middle (pure hue) -> t=1 bottom (black)
                // S: 1.0, V: 1 → 0
                s = 1.0f;
                v = 1.0f - (t - 0.5f) / 0.5f;
            }
            for (int xx = 0; xx < PICKER_TEX_SIZE; ++xx)
            {
                float hue = (float)xx / (PICKER_TEX_SIZE - 1) * 360.f;
                sf::Color col = hsvToRgb(hue, s, v);
                img.setPixel(xx, yy, col);
            }
        }
        huePickerTex.loadFromImage(img);
        huePickerInitialized = true;
    }

    sf::Sprite hueSpr(huePickerTex);
    hueSpr.setPosition(previewX, previewY);
    // v6.1: non-uniform scaling - X axis by width, Y axis by actual height (2/3)
    float pickerScaleX = previewSize / 256.f;
    float pickerScaleY = pickerH / 256.f;
    hueSpr.setScale(pickerScaleX, pickerScaleY);
    window.draw(hueSpr);

    // indicator of the currently edited color
    if (dlg.editTarget != CEDIT_NONE)
    {
        sf::Color* cp = ccGetColor(dlg.editTarget);
        if (cp)
        {
            float hh, ss, vv; rgbToHsv(*cp, hh, ss, vv);
            float xpos = previewX + (hh / 360.f) * previewSize;
            // 3-segment gradient reverse-compute the Y coordinate (uses pickerH, not previewSize)
            float tpos;
            if (vv >= 0.999f)
            {
                tpos = ss * 0.5f;
            }
            else if (ss >= 0.999f)
            {
                tpos = 0.5f + (1.f - vv) * 0.5f;
            }
            else
            {
                float distUpper = 1.f - ss;
                float distLower = 1.f - vv;
                if (distUpper <= distLower)
                    tpos = ss * 0.5f;
                else
                    tpos = 0.5f + (1.f - vv) * 0.5f;
            }
            if (tpos < 0.f) tpos = 0.f; if (tpos > 1.f) tpos = 1.f;
            float ypos = previewY + tpos * pickerH;
            sf::CircleShape ind(6.f, 16);
            ind.setOrigin(6.f,6.f);
            ind.setPosition(xpos, ypos);
            ind.setFillColor(sf::Color::Transparent);
            ind.setOutlineColor(sf::Color::White);
            ind.setOutlineThickness(2.f);
            window.draw(ind);
        }
    }

    // hover preview indicator (directly uses the stored position, avoiding reverse-computation error)
    if (dlg.hoverPickerActive)
    {
        sf::CircleShape hoverInd(5.f, 16);
        hoverInd.setOrigin(5.f, 5.f);
        hoverInd.setPosition(dlg.hoverPickerX, dlg.hoverPickerY);
        hoverInd.setFillColor(sf::Color::White);
        hoverInd.setOutlineColor(sf::Color(60, 60, 60));
        hoverInd.setOutlineThickness(1.5f);
        window.draw(hoverInd);
    }

    // ========== middle-bottom: left = preview tile (shrunk 1/3), right = RGB input box (width 1/2), centered as a whole ==========
    float belowPickerY = previewY + pickerH + dlgH * 0.015f;
    float belowPickerH = listY + listH - belowPickerY;

    // v6.3: left/right split - left preview (35%), right RGB (65%)
    float leftAreaW = previewW * 0.35f;
    float rightAreaW = previewW - leftAreaW - dlgW * 0.012f;
    float areaGap = dlgW * 0.012f;

    // --- preview tile: shrunk by 1/3 (0.85 -> 0.567) ---
    float sampleSize = std::min(leftAreaW, belowPickerH) * 0.567f;

    // --- RGB row height: back to normal (not compressed) ---
    float spinRowH = belowPickerH * 0.18f;
    float spinRowGap = spinRowH * 0.15f;
    float rgbTotalH = 3 * spinRowH + 2 * spinRowGap;

    // --- centered as a whole ---
    float contentMaxH = std::max(sampleSize, rgbTotalH);
    float contentTotalW = leftAreaW + areaGap + rightAreaW;
    float contentStartX = previewX + (previewW - contentTotalW) / 2.f;
    float contentStartY = belowPickerY + (belowPickerH - contentMaxH) / 2.f;

    float leftAreaX = contentStartX;
    float rightAreaX = contentStartX + leftAreaW + areaGap;

    // ---------- left: color preview tile (centered) ----------
    float sampleX = leftAreaX + (leftAreaW - sampleSize) / 2.f;
    float sampleY = contentStartY + (contentMaxH - sampleSize) / 2.f;
    sf::Color curCol(200, 200, 200);
    if (dlg.hoverPickerActive)
    {
        curCol = dlg.hoverPreviewColor;
    }
    else if (dlg.editTarget != CEDIT_NONE)
    {
        sf::Color* cp = ccGetColor(dlg.editTarget);
        if (cp) curCol = *cp;
    }
    sf::RectangleShape sample(sf::Vector2f(sampleSize, sampleSize));
    sample.setPosition(sampleX, sampleY);
    sample.setFillColor(curCol);
    sample.setOutlineColor(cs.textDark);
    sample.setOutlineThickness(2);
    window.draw(sample);

    // hover RGB value (centered below the preview)
    if (dlg.hoverPickerActive)
    {
        unsigned int hfs = (unsigned int)(sampleSize * 0.25f);
        sf::Color hc = dlg.hoverPreviewColor;
        std::ostringstream hss;
        hss << (int)hc.r << ", " << (int)hc.g << ", " << (int)hc.b;
        sf::Text htxt(hss.str(), font, hfs);
        htxt.setFillColor(cs.textDark);
        sf::FloatRect htb = htxt.getLocalBounds();
        htxt.setOrigin(htb.left + htb.width / 2, htb.top + htb.height / 2);
        htxt.setPosition(leftAreaX + leftAreaW / 2, sampleY + sampleSize + hfs * 0.8f);
        window.draw(htxt);
    }

    // ---------- right: R/G/B vertical Spinner (arrows against the box + RGB colored, box length = 1.2x original) ----------
    float arrowW = spinRowH * 0.7f;
    float rgbStartY = contentStartY + (contentMaxH - rgbTotalH) / 2.f;

    const char* rgbLabels[3] = { "R", "G", "B" };
    sf::Color rgbColors[3] = { sf::Color(200, 40, 40), sf::Color(40, 180, 60), sf::Color(40, 80, 200) };

    for (int i = 0; i < 3; ++i)
    {
        float rowY = rgbStartY + i * (spinRowH + spinRowGap);
        float rowX = rightAreaX;

        // current edit target channel value (reads the edit target color directly)
        int curV = 0;
        {
            sf::Color* ccur = ccGetColor(dlg.editTarget);
            if (ccur) curV = (i == 0) ? (int)ccur->r : (i == 1) ? (int)ccur->g : (int)ccur->b;
        }

        // geometry: two arrows against the input box sides (zero gap), box length = 1.2x original
        float availBetween = rightAreaW - 2 * arrowW;
        float inputW = availBetween * 0.6f;            // 0.5 -> 1.2x
        float totalGroupW = 2 * arrowW + inputW;
        float groupX = rowX + (rightAreaW - totalGroupW) / 2.f;
        float leftX = groupX;                           // left arrow
        float inputX = groupX + arrowW;                 // input box
        float rightX = groupX + arrowW + inputW;        // right arrow

        // left arrow (decrease <) - against the input box left, RGB colored
        sf::ConvexShape leftTri(3);
        leftTri.setPoint(0, sf::Vector2f(2.f, spinRowH * 0.5f));
        leftTri.setPoint(1, sf::Vector2f(arrowW, spinRowH * 0.15f));
        leftTri.setPoint(2, sf::Vector2f(arrowW, spinRowH * 0.85f));
        leftTri.setPosition(leftX, rowY);
        leftTri.setFillColor(rgbColors[i]);      // v6.4: RGB colored
        leftTri.setOutlineColor(cs.textDark);
        leftTri.setOutlineThickness(1.f);
        window.draw(leftTri);
        dlg.spinLeftX[i] = leftX; dlg.spinLeftY[i] = rowY;
        dlg.spinLeftW[i] = arrowW; dlg.spinLeftH[i] = spinRowH;

        // right arrow (increase >) - against the input box right, RGB colored
        sf::ConvexShape rightTri(3);
        rightTri.setPoint(0, sf::Vector2f(arrowW - 2.f, spinRowH * 0.5f));
        rightTri.setPoint(1, sf::Vector2f(0.f, spinRowH * 0.15f));
        rightTri.setPoint(2, sf::Vector2f(0.f, spinRowH * 0.85f));
        rightTri.setPosition(rightX, rowY);
        rightTri.setFillColor(rgbColors[i]);     // v6.4: RGB colored
        rightTri.setOutlineColor(cs.textDark);
        rightTri.setOutlineThickness(1.f);
        window.draw(rightTri);
        dlg.spinRightX[i] = rightX; dlg.spinRightY[i] = rowY;
        dlg.spinRightW[i] = arrowW; dlg.spinRightH[i] = spinRowH;

        bool editing = (dlg.rgbEditing == i);
        sf::RectangleShape inputBox(sf::Vector2f(inputW, spinRowH));
        inputBox.setPosition(inputX, rowY);
        inputBox.setFillColor(editing ? sf::Color(255, 255, 255, 230) : cs.boardBg);
        inputBox.setOutlineColor(editing ? sf::Color(80, 120, 200) : sf::Color(150, 140, 130));
        inputBox.setOutlineThickness(1.f);
        window.draw(inputBox);

        // left color strip
        float colorTagW = spinRowH * 0.12f;
        sf::RectangleShape colorTag(sf::Vector2f(colorTagW, spinRowH));
        colorTag.setPosition(inputX, rowY);
        colorTag.setFillColor(rgbColors[i]);
        window.draw(colorTag);

        // R/G/B label
        unsigned int labFs = (unsigned int)(spinRowH * 0.55f);
        sf::Text labT(rgbLabels[i], font, labFs);
        labT.setFillColor(cs.textDark);   // system text
        labT.setStyle(sf::Text::Bold);
        sf::FloatRect lb = labT.getLocalBounds();
        labT.setOrigin(lb.left + lb.width / 2, lb.top + lb.height / 2);
        float labCenterX = inputX + colorTagW + (inputW - colorTagW) * 0.15f;
        labT.setPosition(labCenterX, rowY + spinRowH / 2.f);
        window.draw(labT);

        // value text
        std::string valStr;
        if (editing && !dlg.rgbInputStr.empty()) valStr = dlg.rgbInputStr;
        else valStr = std::to_string(curV);
        unsigned int vfs = (unsigned int)(spinRowH * 0.6f);
        sf::Text vt(valStr, font, vfs);
        vt.setFillColor(cs.textDark);
        sf::FloatRect vb = vt.getLocalBounds();
        vt.setOrigin(vb.left + vb.width / 2, vb.top + vb.height / 2);
        float valCenterX = inputX + colorTagW + (inputW - colorTagW) * 0.65f;
        vt.setPosition(valCenterX, rowY + spinRowH / 2.f);
        window.draw(vt);

        // draw the blinking cursor
        if (editing)
        {
            float blinkTime = dlg.rgbCursorClock.getElapsedTime().asSeconds();
            bool visible = ((int)(blinkTime * 2)) % 2 == 0;
            if (visible)
            {
                sf::Text cursorText(valStr, font, vfs);
                sf::FloatRect ctb = cursorText.getLocalBounds();
                float curX = valCenterX + ctb.width / 2.f + 2.f;
                float curY = rowY + (spinRowH - vfs) / 2.f;
                sf::RectangleShape cursor(sf::Vector2f(2.f, (float)vfs));
                cursor.setPosition(curX, curY);
                cursor.setFillColor(sf::Color(30, 30, 30));
                window.draw(cursor);
            }
        }

        // store the input-box rect for click detection
        if (i == 0) { dlg.rValX = inputX; dlg.rValY = rowY; dlg.rValW = inputW; dlg.rValH = spinRowH; }
        else if (i == 1) { dlg.gValX = inputX; dlg.gValY = rowY; dlg.gValW = inputW; dlg.gValH = spinRowH; }
        else { dlg.bValX = inputX; dlg.bValY = rowY; dlg.bValW = inputW; dlg.bValH = spinRowH; }
    }

    // ========== bottom buttons: Save / Apply / Close ==========
    // closeBtnH / btnY already computed in the edit-area layout; reused here
    float total3BtnW = dlgW * 0.72f;
    float btnGap = total3BtnW * 0.05f;
    float eachW = (total3BtnW - btnGap * 2) / 3.f;
    float startX = dlgX + (dlgW - total3BtnW) / 2.f;

    auto drawBtn = [&](float bx, float bw, const sf::String& label, bool primary)
    {
        sf::RectangleShape b(sf::Vector2f(bw, closeBtnH));
        b.setPosition(bx, btnY);
        b.setFillColor(primary ? cs.scoreBoxColor : cs.boardBg);
        window.draw(b);
        drawTextFB(window, font, label,
                   bx + bw / 2, btnY + closeBtnH / 2,
                   (unsigned int)(closeBtnH * 0.5f),
                   cs.textDark, 0, 1);   // function-key text unified as system text
    };
    float saveX = startX;
    float applyX = startX + eachW + btnGap;
    float closeX = startX + (eachW + btnGap) * 2;
    dlg.saveBtnX = saveX;  dlg.saveBtnY = btnY; dlg.saveBtnW = eachW; dlg.saveBtnH = closeBtnH;
    dlg.applyBtnX = applyX; dlg.applyBtnY = btnY; dlg.applyBtnW = eachW; dlg.applyBtnH = closeBtnH;
    dlg.closeBtnX = closeX; dlg.closeBtnY = btnY; dlg.closeBtnW = eachW; dlg.closeBtnH = closeBtnH;
    drawBtn(saveX, eachW, tr("Save", "保存"), false);
    drawBtn(applyX, eachW, tr("Apply", "应用"), false);
    drawBtn(closeX, eachW, tr("Close", "关闭"), true);

    (void)base;
}

// ========== Language config file lang.ini read/write ==========
//   Purpose: let the installer language choice directly affect the startup UI language
//   Load priority (high to low):
//     1. the language setting in 2048save.dat (the user's last in-game choice or save default)
//     2. lang.ini (written by the installer, fallback default on first launch)
//   Write: switching language in-game saves to lang.ini + 2048save.dat together
//   Format: [Language]\nLang=zh | en

static void loadLangIni()
{
    std::ifstream ifs(LANG_FILE);
    if (!ifs) {
        s_langIniFileExists = false;
        return;
    }
    s_langIniFileExists = true;
    std::string line;
    bool inLanguageSection = false;
    int loadedLang = LANG_EN;
    bool found = false;
    while (std::getline(ifs, line))
    {
        // trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(start, end - start + 1);
        if (trimmed.empty()) continue;
        // section marker
        if (trimmed[0] == '[')
        {
            inLanguageSection = (trimmed == "[Language]");
            continue;
        }
        if (!inLanguageSection) continue;
        // find Lang=zh / Lang=en (case-insensitive)
        if (trimmed.size() >= 5 &&
            (trimmed[0] == 'L' || trimmed[0] == 'l') &&
            (trimmed[1] == 'a' || trimmed[1] == 'a') &&
            (trimmed[2] == 'n' || trimmed[2] == 'n') &&
            (trimmed[3] == 'g' || trimmed[3] == 'g') &&
            trimmed[4] == '=')
        {
            std::string val = trimmed.substr(5);
            // lowercase
            for (auto& c : val) c = (char)tolower((unsigned char)c);
            if (val == "zh" || val == "chinese" || val == "chinesesimp" || val == "cn") {
                loadedLang = LANG_ZH;
                found = true;
            }
            else if (val == "en" || val == "english" || val == "uk" || val == "us") {
                loadedLang = LANG_EN;
                found = true;
            }
            break;
        }
    }
    if (found) {
        s_langIniLoaded = loadedLang;
        logMsg(std::string("lang.ini loaded: ") + (loadedLang == LANG_ZH ? "ZH" : "EN") +
               " (will only apply if save.dat has no language setting)");
    }
}

static void saveLangIni()
{
    std::ofstream ofs(LANG_FILE, std::ios::trunc);
    if (!ofs) {
        logMsg(std::string("ERROR: saveLangIni FAILED - cannot write: ") + LANG_FILE);
        return;
    }
    ofs << "[Language]\r\n";
    ofs << "Lang=" << (g_language == LANG_ZH ? "zh" : "en") << "\r\n";
    ofs.close();
    s_langIniFileExists = true;
    s_langIniLoaded = g_language;
    logMsg(std::string("lang.ini saved: ") + (g_language == LANG_ZH ? "ZH" : "EN"));
}

// ========== Dialog layout recomputation (refresh all dialog state coordinates on window Resized) ==========
static void recalcDialogLayouts(unsigned int winW, unsigned int winH, const Game2048& game,
                                FontDialogState& fontDlg)
{
    float boardPx = computeBoardPx(winW, winH, false);
    const float pad = boardPx * LAYOUT_PAD_RATIO;
    float topBarH = boardPx * computeTopBarRatio();

    // --- History Dialog ---
    if (g_historyDlg.open)
    {
        float availW = winW - pad * 2;
        float availH = winH - pad * 2;
        float dlgW = std::min(availW, availH / 1.2f);   // fixed width:height = 1:1.2
        float dlgH = dlgW * 1.2f;
        float dlgX = (winW - dlgW) / 2.f;
        float dlgY = (winH - dlgH) / 2.f;

        // top dual function keys (matching drawing: small, group centered)
        const float tabH = dlgH * 0.05f;
        const float tabGap = dlgW * 0.06f;
        const float tabW = dlgW * 0.3f;
        const float groupW = tabW * 2.f + tabGap;
        const float tabY = dlgY + dlgH * 0.028f;
        const float tabX0 = dlgX + (dlgW - groupW) / 2.f;
        g_historyDlg.tabHistX = tabX0;            g_historyDlg.tabHistY = tabY;
        g_historyDlg.tabHistW = tabW;             g_historyDlg.tabHistH = tabH;
        g_historyDlg.tabImpX  = tabX0 + tabW + tabGap;  g_historyDlg.tabImpY = tabY;
        g_historyDlg.tabImpW  = tabW;             g_historyDlg.tabImpH = tabH;

        // list area (between the top keys and the bottom two keys; no title)
        float listY = tabY + tabH + dlgH * 0.02f;
        float listH = dlgH * 0.80f;
        float listW = dlgW * 0.92f;
        float listX = dlgX + (dlgW - listW) / 2.f;

        g_historyDlg.listAreaX = listX;
        g_historyDlg.listAreaY = listY;
        g_historyDlg.listAreaW = listW;
        g_historyDlg.listAreaH = listH;
        g_historyDlg.itemH = listH * 0.135f * 1.46f;

        // Clamp scroll position
        int totalItems = histVisibleCount(g_historyDlg);
        int visibleCount = (int)(listH / g_historyDlg.itemH);
        if (visibleCount > totalItems && totalItems > 0) visibleCount = totalItems;
        int maxScroll = totalItems - visibleCount;
        if (maxScroll < 0) maxScroll = 0;
        if (g_historyDlg.scrollPos > maxScroll) g_historyDlg.scrollPos = maxScroll;
        if (g_historyDlg.scrollPos < 0) g_historyDlg.scrollPos = 0;

        // bottom two keys: best records / close (matching drawing)
        float btnH = dlgH * 0.06f * 0.75f;
        float btnGap = dlgW * 0.04f;
        float btnW = dlgW * 0.22f;
        float totalBtnW = btnW * 2.f + btnGap;
        float btnX0 = dlgX + (dlgW - totalBtnW) / 2.f;
        float areaTop = listY + listH + dlgH * 0.02f;
        float areaBottom = dlgY + dlgH - dlgH * 0.03f;
        float bandH = areaBottom - areaTop;
        float btnY = areaTop + (bandH - btnH) * 0.5f;

        g_historyDlg.maxBtnX = btnX0;
        g_historyDlg.maxBtnY = btnY;
        g_historyDlg.maxBtnW = btnW;
        g_historyDlg.maxBtnH = btnH;
        g_historyDlg.closeBtnX = btnX0 + btnW + btnGap;
        g_historyDlg.closeBtnY = btnY;
        g_historyDlg.closeBtnW = btnW;
        g_historyDlg.closeBtnH = btnH;

        // import history panel: choose file / load all / close three keys (matching drawing)
        if (g_historyDlg.tab == 1)
        {
            float tabBottom = tabY + tabH;
            float impListTop = tabBottom + dlgH * 0.02f;
            float impListBottom = dlgY + dlgH - dlgH * 0.115f;
            float iBtnH = dlgH * 0.06f * 0.75f;
            float iBtnGap = dlgW * 0.04f;
            float iBtnW = dlgW * 0.20f;
            float iTotal = iBtnW * 3.f + iBtnGap * 2.f;
            float iX0 = dlgX + (dlgW - iTotal) / 2.f;
            float iY = impListBottom + dlgH * 0.015f;
            g_historyDlg.selX = iX0;             g_historyDlg.selY = iY; g_historyDlg.selW = iBtnW; g_historyDlg.selH = iBtnH;
            g_historyDlg.loadAllX = iX0 + iBtnW + iBtnGap;
            g_historyDlg.loadAllY = iY; g_historyDlg.loadAllW = iBtnW; g_historyDlg.loadAllH = iBtnH;
            g_historyDlg.closeBtnX = iX0 + (iBtnW + iBtnGap) * 2;
            g_historyDlg.closeBtnY = iY; g_historyDlg.closeBtnW = iBtnW; g_historyDlg.closeBtnH = iBtnH;
            g_historyDlg.impListY = impListTop;
            g_historyDlg.impListH = impListBottom - impListTop;
        }
    }

    // --- Custom Color Dialog ---
    if (g_ccDlg.open)
    {
        // width = board width (boardPx), height:width = 1.1:1, centered in the window
        float boardPx = computeBoardPx(winW, winH, false);
        float dlgW = boardPx;
        float dlgH = dlgW * 1.1f;
        // scale down proportionally when it exceeds the window
        if (dlgH > (float)winH)
        {
            float scale = (float)winH / dlgH;
            dlgW *= scale;
            dlgH *= scale;
        }
        if (dlgW > (float)winW)
        {
            float scale = (float)winW / dlgW;
            dlgW *= scale;
            dlgH *= scale;
        }
        float dlgX = (winW - dlgW) / 2.f;
        float dlgY = (winH - dlgH) / 2.f;
        g_ccDlg.dlgX = dlgX; g_ccDlg.dlgY = dlgY; g_ccDlg.dlgW = dlgW; g_ccDlg.dlgH = dlgH;

        float listX = dlgX + dlgW * 0.02f;
        float listY = dlgY + dlgH * 0.09f;
        float listW = dlgW * 0.36f;
        float listH = dlgH * 0.55f;
        g_ccDlg.listAreaX = listX;
        g_ccDlg.listAreaY = listY;
        g_ccDlg.listAreaW = listW;
        g_ccDlg.listAreaH = listH;
        g_ccDlg.itemH = listH * 0.10f;

        // Clamp scroll position
        {
            int totalItems = CC_TOTAL_ITEMS;
            int visibleCount = (int)(listH / g_ccDlg.itemH);
            if (visibleCount > totalItems) visibleCount = totalItems;
            int maxScroll = totalItems - visibleCount;
            if (maxScroll < 0) maxScroll = 0;
            if (g_ccDlg.scrollPos > maxScroll) g_ccDlg.scrollPos = maxScroll;
            if (g_ccDlg.scrollPos < 0) g_ccDlg.scrollPos = 0;
        }

        // Preview / color picker (matching drawing: larger, wider square)
        float pickerY = listY;
        float rightStartX = listX + listW + dlgW * 0.015f;
        float rightEndX   = dlgX + dlgW - dlgW * 0.015f;
        float rightAvailW = rightEndX - rightStartX;
        float pickerMaxW  = rightAvailW;
        float pickerMaxH  = (listY + listH) - pickerY - dlgH * 0.04f;
        float previewSize = pickerMaxW;  // v6.1: width by available space
        float pickerH = std::min(previewSize * 0.6667f, pickerMaxH);
        g_ccDlg.previewX  = rightStartX + (rightAvailW - previewSize) / 2.f;
        g_ccDlg.previewY  = pickerY;
        g_ccDlg.previewSize = previewSize;
        g_ccDlg.pickerH = pickerH;  // v6.1: store the picker actual height

        // v6.3: row height restored, input box width 1/2, centered - matching drawing
        float belowPickerY = g_ccDlg.previewY + pickerH + dlgH * 0.015f;
        float belowPickerH = listY + listH - belowPickerY;
        float leftAreaW = previewSize * 0.35f;
        float rightAreaW = previewSize - leftAreaW - dlgW * 0.012f;
        float areaGap = dlgW * 0.012f;

        // preview tile shrunk 1/3
        float sampleSize = std::min(leftAreaW, belowPickerH) * 0.567f;
        // RGB row height: back to normal (not compressed)
        float spinRowH = belowPickerH * 0.18f;
        float spinRowGap = spinRowH * 0.15f;
        float rgbTotalH = 3 * spinRowH + 2 * spinRowGap;

        // centered as a whole
        float contentMaxH = std::max(sampleSize, rgbTotalH);
        float contentTotalW = leftAreaW + areaGap + rightAreaW;
        float contentStartX = g_ccDlg.previewX + (previewSize - contentTotalW) / 2.f;
        float contentStartY = belowPickerY + (belowPickerH - contentMaxH) / 2.f;

        float rightAreaX = contentStartX + leftAreaW + areaGap;
        float rgbStartY = contentStartY + (contentMaxH - rgbTotalH) / 2.f;
        float arrowW = spinRowH * 0.7f;

        for (int i = 0; i < 3; ++i)
        {
            float rowY = rgbStartY + i * (spinRowH + spinRowGap);

            // v6.4: against-the-box geometry (matching drawing)
            float availBetween = rightAreaW - 2 * arrowW;
            float inputW = availBetween * 0.5f;
            float inputX = rightAreaX + arrowW + (availBetween - inputW) / 2.f;
            float leftX = inputX - arrowW;
            float rightX = inputX + inputW;

            // Left arrow
            g_ccDlg.spinLeftX[i] = leftX;
            g_ccDlg.spinLeftY[i] = rowY;
            g_ccDlg.spinLeftW[i] = arrowW;
            g_ccDlg.spinLeftH[i] = spinRowH;

            // Right arrow
            g_ccDlg.spinRightX[i] = rightX;
            g_ccDlg.spinRightY[i] = rowY;
            g_ccDlg.spinRightW[i] = arrowW;
            g_ccDlg.spinRightH[i] = spinRowH;

            if (i == 0) { g_ccDlg.rValX = inputX; g_ccDlg.rValY = rowY; g_ccDlg.rValW = inputW; g_ccDlg.rValH = spinRowH; }
            else if (i == 1) { g_ccDlg.gValX = inputX; g_ccDlg.gValY = rowY; g_ccDlg.gValW = inputW; g_ccDlg.gValH = spinRowH; }
            else { g_ccDlg.bValX = inputX; g_ccDlg.bValY = rowY; g_ccDlg.bValW = inputW; g_ccDlg.bValH = spinRowH; }
        }

        // Brightness bar (below RGB spinners)
        float brightY = rgbStartY + rgbTotalH + dlgH * 0.04f;
        float brightW = dlgW * 0.40f;
        float brightX = dlgX + (dlgW - brightW) / 2.f;
        g_ccDlg.brightBarX = brightX; g_ccDlg.brightBarY = brightY;
        g_ccDlg.brightBarW = brightW; g_ccDlg.brightBarH = dlgH * 0.03f;

        // Slots area
        float slotsAreaY = listY + listH + dlgH * 0.02f;
        float slotH = dlgH * 0.10f;
        float slotW = (dlgW * 0.94f - dlgH * 0.04f) / 3;
        float slotStartX = dlgX + (dlgW - (slotW * 3 + dlgH * 0.04f)) / 2.f;
        float slotGap = dlgH * 0.02f;
        float delSize = slotH * 0.22f;
        for (int i = 0; i < MAX_CUSTOM_PRESETS; ++i)
        {
            int row = i / 3, col = i % 3;
            float sx = slotStartX + col * (slotW + slotGap);
            float sy = slotsAreaY + row * (slotH + slotGap * 0.5f);
            g_ccDlg.slotX[i] = sx; g_ccDlg.slotY[i] = sy;
            g_ccDlg.slotW[i] = slotW; g_ccDlg.slotH[i] = slotH;
            g_ccDlg.slotDelX[i] = sx + slotW - delSize - 4;
            g_ccDlg.slotDelY[i] = sy + 4;
            g_ccDlg.slotDelW[i] = delSize; g_ccDlg.slotDelH[i] = delSize;
        }

        // Bottom buttons
        float closeBtnW = dlgW * 0.2f;
        float closeBtnH = dlgH * 0.06f;
        float btnY = dlgY + dlgH - closeBtnH - dlgH * 0.025f;
        float total3BtnW = dlgW * 0.72f;
        float btnGap = total3BtnW * 0.05f;
        float eachW = (total3BtnW - btnGap * 2) / 3.f;
        float startX = dlgX + (dlgW - total3BtnW) / 2.f;
        g_ccDlg.saveBtnX = startX; g_ccDlg.saveBtnY = btnY; g_ccDlg.saveBtnW = eachW; g_ccDlg.saveBtnH = closeBtnH;
        g_ccDlg.applyBtnX = startX + eachW + btnGap; g_ccDlg.applyBtnY = btnY;
        g_ccDlg.applyBtnW = eachW; g_ccDlg.applyBtnH = closeBtnH;
        g_ccDlg.closeBtnX = startX + (eachW + btnGap) * 2; g_ccDlg.closeBtnY = btnY;
        g_ccDlg.closeBtnW = eachW; g_ccDlg.closeBtnH = closeBtnH;
    }

    // --- Font Dialog ---
    if (fontDlg.open)
    {
        float dlgW = winW * 0.85f;
        float dlgH = winH * 0.90f;
        float dlgX = (winW - dlgW) / 2.f;
        float dlgY = (winH - dlgH) / 2.f;

        float listAreaX = dlgX + dlgW * 0.08f;
        float listAreaY = dlgY + dlgH * 0.12f;
        float listAreaW = dlgW * 0.78f;
        float listAreaH = dlgH * 0.60f;

        fontDlg.listAreaX = listAreaX;
        fontDlg.listAreaY = listAreaY;
        fontDlg.listAreaW = listAreaW;
        fontDlg.listAreaH = listAreaH;
        fontDlg.itemH = listAreaH / 12.f;

        // Clamp scroll position
        {
            int totalItems = (int)g_systemFonts.size();
            int visibleCount = (int)(listAreaH / fontDlg.itemH);
            if (visibleCount > totalItems) visibleCount = totalItems;
            int maxScroll = totalItems - visibleCount;
            if (maxScroll < 0) maxScroll = 0;
            if (fontDlg.scrollPos > maxScroll) fontDlg.scrollPos = maxScroll;
            if (fontDlg.scrollPos < 0) fontDlg.scrollPos = 0;
        }

        fontDlg.barX = listAreaX + listAreaW + 2.f;
        fontDlg.barY = listAreaY;
        fontDlg.barW = 10.f;
        fontDlg.barH = listAreaH;

        float btnW = dlgW * 0.3f;
        float btnH = dlgH * 0.08f;
        float btnGap = dlgW * 0.05f;
        float btnY = dlgY + dlgH - btnH - dlgH * 0.06f;
        float totalBtnsW = btnW * 2 + btnGap;
        float btnStartX = dlgX + (dlgW - totalBtnsW) / 2.f;
        fontDlg.applyBtnX = btnStartX; fontDlg.applyBtnY = btnY;
        fontDlg.applyBtnW = btnW; fontDlg.applyBtnH = btnH;
        fontDlg.cancelBtnX = btnStartX + btnW + btnGap; fontDlg.cancelBtnY = btnY;
        fontDlg.cancelBtnW = btnW; fontDlg.cancelBtnH = btnH;

        float headerY = dlgY + dlgH * 0.05f;
        float headerH = dlgH * 0.06f;
        float smallBtnW = dlgW * 0.14f;
        float smallBtnH = dlgH * 0.045f;
        float smallBtnGap = dlgW * 0.025f;
        float headerBtnStartX = dlgX + dlgW * 0.08f;
        fontDlg.resetBtnX = headerBtnStartX;
        fontDlg.resetBtnY = headerY + (headerH - smallBtnH) / 2.f;
        fontDlg.resetBtnW = smallBtnW; fontDlg.resetBtnH = smallBtnH;
        fontDlg.prevBtnX = headerBtnStartX + smallBtnW + smallBtnGap;
        fontDlg.prevBtnY = headerY + (headerH - smallBtnH) / 2.f;
        fontDlg.prevBtnW = smallBtnW; fontDlg.prevBtnH = smallBtnH;
    }
}

// ========== Main function ==========
int main(int argc, char* argv[])
{
    try {
    // generate the 26-stage gradients of the added color schemes
    initAddedSchemes();

    // load the save first to get the window size
    Game2048 game;
    bool testMode = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--test") testMode = true;

    logMsg(std::string("=== Game started === exeDir=") + getExeDir() +
           " testMode=" + (testMode ? "1" : "0"));

    if (!testMode)
    {
        // read lang.ini first, recording only the installer-chosen value (not applied yet; the save wins)
        loadLangIni();

        // load AI settings (rate/tier, last user values; no config file -> default 30 + tier 3)
        loadAiSettings();

        // load history playback settings (step mode/multiplier/fixed step; no config file -> original step 1x)
        loadPbSettings();
        // load the challenge project choice (remembers the last chosen project; no config file -> default 4x4 2048)
        loadChallengeProject();

        // load timer-mode options (show count/main time/IPS/milestones; no config file -> all on)
        loadTimerSettings();

        // load the custom theme (persists SCHEMES[CUSTOM_SCHEME_IDX]; no file -> the default custom scheme)
        loadCustomTheme();

        // load 2048save.dat (loadLang=true: a language field in the save overrides g_language and sets s_saveHadLanguageField)
        bool saveLoaded = game.loadFromFile(SAVE_FILE, /*loadLang=*/true);

        if (!saveLoaded) {
            // first launch (no save) -> use the lang.ini default written by the installer
            g_language = s_langIniLoaded;
            logMsg(std::string("First run (no save.dat) - applying lang.ini default: ") +
                   (g_language == LANG_ZH ? "ZH" : "EN"));
        } else if (!s_saveHadLanguageField) {
            // legacy save without a language field (upgrade compat) -> use lang.ini to build the default
            if (s_langIniFileExists) {
                g_language = s_langIniLoaded;
                logMsg(std::string("Legacy save.dat - applying lang.ini fallback: ") +
                       (g_language == LANG_ZH ? "ZH" : "EN"));
            } else {
                logMsg(std::string("Legacy save.dat - using constructor default: ") +
                       (g_language == LANG_ZH ? "ZH" : "EN"));
            }
        } else {
            // normal: the save has a language field -> use it directly (the user's last in-game choice)
            logMsg(std::string("Save.dat language applied: ") +
                   (g_language == LANG_ZH ? "ZH" : "EN"));
        }
    }
    // font fixed to the default font in the software's font folder (manual switching no longer supported)
    g_currentFontPath = DEFAULT_FONT_PATH;
    // sync the font with the loaded language (Chinese mode auto-selects a Chinese font)
    applyLanguageFont();

    sf::RenderWindow window(
        sf::VideoMode(g_savedWindowW, g_savedWindowH),
        "Meter 2048 auto",
        sf::Style::Titlebar | sf::Style::Close | sf::Style::Resize
    );
    window.setFramerateLimit(60);
    window.setVerticalSyncEnabled(true);

    sf::Image icon;
    if (icon.loadFromFile(pathInExeDir("auto 2048.png")))
        window.setIcon(icon.getSize().x, icon.getSize().y, icon.getPixelsPtr());

    sf::Font font;
    font.loadFromFile(g_currentFontPath);

    // load the fallback font (default font in the game fonts dir, for Chinese fallback)
    {
        std::string fbPath = findGameFont();
        if (!fbPath.empty() && g_fallbackFont.loadFromFile(fbPath))
            g_fallbackLoaded = true;
    }
    // set the initial fallback flag
    g_usingFallback = (g_language == LANG_ZH && !fontSupportsChinese(g_currentFontPath));

    if (testMode)
    {
        game.setColorScheme(2); // Neon
        game.setupTestBoard();
        // print each tile's color to the console
        printf("=== Neon Theme Tile Colors ===\n");
        int powers[] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 };
        for (int idx = 0; idx < 17; ++idx)
        {
            int val = (1 << powers[idx]);
            sf::Color c = game.getTileColor(val);
            sf::Color tc = game.getTextColor(val);
            printf("2^%-2d = %10d  tile=RGB(%3d,%3d,%3d)  text=RGB(%3d,%3d,%3d)\n",
                powers[idx], val, c.r, c.g, c.b, tc.r, tc.g, tc.b);
        }
        printf("=== End ===\n");
        fflush(stdout);
    }
    else
    {
        if (!g_currentFontPath.empty())
        {
            font = sf::Font();
            font.loadFromFile(g_currentFontPath);
        }
    }

    bool menuOpen = false;
    bool showConfirm = false;
    bool showPresetFullAlert = false;   // preset slots full alert
    int pendingBoardSize = 0;
    bool screenshotTaken = false;
    FontDialogState fontDlg = {};
    enumerateSystemFonts();
    ccLoadPresetsFromFile(); // load the custom color presets at startup
    g_ccDlg.open = false;
    g_ccDlg.scrollPos = 0;
    g_ccDlg.editTarget = CEDIT_NONE;
    g_ccDlg.hoverIdx = -1;
    g_ccDlg.alertPresetFull = false;
    g_ccDlg.rgbEditing = -1;
    g_ccDlg.rgbInputStr = "";
    g_ccDlg.rgbLastValid = 0;

    sf::Vector2i mouseStart;
    bool isDragging = false;
    sf::Clock frameClock;

    // restore the last timer-mode session (tm.ini Active=1: re-enter timer mode, restore board and timing, time immediately)
    tmRestoreSession(window, game);

    while (window.isOpen())
    {
        sf::Event e;
        while (window.pollEvent(e))
        {
            if (e.type == sf::Event::Closed)
            {
                if (g_practiceActive && g_practiceOrigW > 0 && g_practiceOrigH > 0)
                {
                    // closing while in practice: restore the original window size first, avoiding persisting the 2.2x width
                    g_savedWindowW = g_practiceOrigW;
                    g_savedWindowH = g_practiceOrigH;
                    window.setSize(sf::Vector2u(g_practiceOrigW, g_practiceOrigH));
                }
                if (g_saveDirty) game.saveToFile(SAVE_FILE);
                if (g_tm.active)
                {
                    // direct software close: write a full independent save (board + time/count) + Active=1 -> next launch auto-restores timer mode
                    tmWriteFullSave(game);
                    saveTimerSettings();
                }
                ccSavePresetsToFile(); // save the custom color presets on exit
                g_saveDirty = false;
                logMsg("=== Game closed ===");
                window.close();
            }

            if (e.type == sf::Event::Resized)
            {
                const unsigned int MIN_W = 320, MIN_H = 380;
                sf::Vector2u curSize = window.getSize();
                if (curSize.x < MIN_W || curSize.y < MIN_H)
                {
                    unsigned int newW = std::max(curSize.x, MIN_W);
                    unsigned int newH = std::max(curSize.y, MIN_H);
                    window.setSize(sf::Vector2u(newW, newH));
                    continue;
                }
                g_savedWindowW = curSize.x;
                g_savedWindowH = curSize.y;
                markSaveDirty();
                sf::View view = window.getView();
                view.setSize((float)curSize.x, (float)curSize.y);
                view.setCenter((float)curSize.x / 2.f, (float)curSize.y / 2.f);
                window.setView(view);
                recalcDialogLayouts(curSize.x, curSize.y, game, fontDlg);
            }

            // ===== while a confirm dialog is open, only its input is handled =====
            if (showConfirm)
            {
                if (e.type == sf::Event::KeyPressed)
                {
                    if (showPresetFullAlert)
                    {
                        // preset slots full alert: any key closes
                        showConfirm = false;
                        showPresetFullAlert = false;
                    }
                    else
                    {
                        if (e.key.code == sf::Keyboard::Y || e.key.code == sf::Keyboard::Enter)
                        {
                            switchBoardSize(game, pendingBoardSize);
                            showConfirm = false;
                        }
                        else if (e.key.code == sf::Keyboard::N || e.key.code == sf::Keyboard::Escape)
                        {
                            showConfirm = false;
                        }
                    }
                }
                if (e.type == sf::Event::MouseButtonPressed && e.mouseButton.button == sf::Mouse::Left)
                {
                    sf::Vector2i pos = sf::Mouse::getPosition(window);
                    unsigned int winW = window.getSize().x;
                    unsigned int winH = window.getSize().y;

                    float dlgW = winW * 0.60f;
                    float dlgH = winH * 0.32f;
                    if (dlgW < 320.f) dlgW = 320.f;
                    if (dlgH < 180.f) dlgH = 180.f;
                    float dlgX = (winW - dlgW) / 2.f;
                    float dlgY = (winH - dlgH) / 2.f;
                    float btnW = dlgW * 0.25f;
                    if (btnW < 70.f) btnW = 70.f;
                    if (btnW > 140.f) btnW = 140.f;
                    float btnH = dlgH * 0.18f;
                    if (btnH < 36.f) btnH = 36.f;
                    if (btnH > 60.f) btnH = 60.f;
                    float btnGap = dlgW * 0.08f;
                    if (btnGap < 12.f) btnGap = 12.f;
                    float btnY = dlgY + dlgH - btnH - dlgH * 0.12f;
                    float totalBtnsW = btnW * 2 + btnGap;
                    float startX = dlgX + (dlgW - totalBtnsW) / 2.f;

                    if (showPresetFullAlert)
                    {
                        // preset slots full alert: yes/no any button closes
                        showConfirm = false;
                        showPresetFullAlert = false;
                    }
                    else
                    {
                        if (pos.x >= startX && pos.x <= startX + btnW &&
                            pos.y >= btnY && pos.y <= btnY + btnH)
                        {
                            switchBoardSize(game, pendingBoardSize);
                            showConfirm = false;
                        }
                        else if (pos.x >= startX + btnW + btnGap && pos.x <= startX + btnW + btnGap + btnW &&
                                 pos.y >= btnY && pos.y <= btnY + btnH)
                        {
                            showConfirm = false;
                        }
                    }
                }
                continue;
            }

            // ===== 5x5 AI warning dialog handling =====
            if (g_show5x5AiWarning)
            {
                if (e.type == sf::Event::KeyPressed)
                {
                    if (e.key.code == sf::Keyboard::Y || e.key.code == sf::Keyboard::Enter)
                    {
                        // user confirmed: start 5x5 AI
                        g_show5x5AiWarning = false;
                        g_aiMode = AI_PAUSED;
                        g_aiBoardSize = game.getBoardSize();
                        g_aiEngine.setBoardSize(g_aiBoardSize);
                        g_aiStepClock.restart();
                        g_showAIEnd = false;
                        g_aiCacheReset = 1;   // new game: clear the transposition table in the background to prevent degradation
                        aiResetSessionStats();   // new session: clear count/time stats
                        resetMilestoneState();   // new session: clear milestone records
                    }
                    else if (e.key.code == sf::Keyboard::N || e.key.code == sf::Keyboard::Escape)
                    {
                        g_show5x5AiWarning = false;
                    }
                    continue;
                }
                if (e.type == sf::Event::MouseButtonPressed && e.mouseButton.button == sf::Mouse::Left)
                {
                    sf::Vector2i pos = sf::Mouse::getPosition(window);
                    unsigned int winW = window.getSize().x;
                    unsigned int winH = window.getSize().y;

                    float dlgW = winW * 0.60f;
                    float dlgH = winH * 0.32f;
                    if (dlgW < 320.f) dlgW = 320.f;
                    if (dlgH < 180.f) dlgH = 180.f;
                    float dlgX = (winW - dlgW) / 2.f;
                    float dlgY = (winH - dlgH) / 2.f;
                    float btnW = dlgW * 0.25f;
                    if (btnW < 70.f) btnW = 70.f;
                    if (btnW > 140.f) btnW = 140.f;
                    float btnH = dlgH * 0.18f;
                    if (btnH < 36.f) btnH = 36.f;
                    if (btnH > 60.f) btnH = 60.f;
                    float btnGap = dlgW * 0.08f;
                    if (btnGap < 12.f) btnGap = 12.f;
                    float btnY = dlgY + dlgH - btnH - dlgH * 0.12f;
                    float totalBtnsW = btnW * 2 + btnGap;
                    float startX = dlgX + (dlgW - totalBtnsW) / 2.f;

                    // yes button: confirm, start AI
                    if (pos.x >= startX && pos.x <= startX + btnW &&
                        pos.y >= btnY && pos.y <= btnY + btnH)
                    {
                        g_show5x5AiWarning = false;
                        g_aiMode = AI_PAUSED;
                        g_aiBoardSize = game.getBoardSize();
                        g_aiEngine.setBoardSize(g_aiBoardSize);
                        g_aiStepClock.restart();
                        g_showAIEnd = false;
                        g_aiCacheReset = 1;   // new game: clear the transposition table in the background to prevent degradation
                        aiResetSessionStats();   // new session: clear count/time stats
                        resetMilestoneState();   // new session: clear milestone records
                    }
                    // no button: cancel
                    else if (pos.x >= startX + btnW + btnGap && pos.x <= startX + btnW + btnGap + btnW &&
                             pos.y >= btnY && pos.y <= btnY + btnH)
                    {
                        g_show5x5AiWarning = false;
                    }
                    continue;
                }
            }

            if (e.type == sf::Event::KeyPressed)
            {
                // font dialog open: keyboard handling
                if (fontDlg.open)
                {
                    if (e.key.code == sf::Keyboard::Escape)
                    {
                        fontDlg.open = false;
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Enter)
                    {
                        if (fontDlg.selectedIdx >= 0 && fontDlg.selectedIdx < (int)g_systemFonts.size())
                        {
                            font = sf::Font();
                            if (font.loadFromFile(g_systemFonts[fontDlg.selectedIdx].path))
                            {
                                g_previousFontPath = g_currentFontPath;
                                g_currentFontPath = g_systemFonts[fontDlg.selectedIdx].path;
                                // set the fallback flag: in Chinese mode with a non-Chinese font, drawTextFB uses the fallback
                                g_usingFallback = (g_language == LANG_ZH && !fontSupportsChinese(g_currentFontPath));
                                game.saveToFile(SAVE_FILE);
                            }
                        }
                        fontDlg.open = false;
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Up)
                    {
                        if (fontDlg.selectedIdx > 0) fontDlg.selectedIdx--;
                        if (fontDlg.selectedIdx < fontDlg.scrollPos)
                            fontDlg.scrollPos = fontDlg.selectedIdx;
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Down)
                    {
                        if (fontDlg.selectedIdx < (int)g_systemFonts.size() - 1) fontDlg.selectedIdx++;
                        float listAreaH = fontDlg.listAreaH;
                        float itemH = fontDlg.itemH;
                        int visibleCount = (int)(listAreaH / itemH);
                        if (fontDlg.selectedIdx >= fontDlg.scrollPos + visibleCount)
                            fontDlg.scrollPos = fontDlg.selectedIdx - visibleCount + 1;
                        continue;
                    }
                    continue;
                }

                // history dialog open: keyboard handling
                if (g_historyDlg.open)
                {
                    if (e.key.code == sf::Keyboard::Escape)
                    {
                        g_historyDlg.open = false;
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Up)
                    {
                        if (g_historyDlg.scrollPos > 0) g_historyDlg.scrollPos--;
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Down)
                    {
                        int totalItems = histVisibleCount(g_historyDlg);
                        int visibleCount = (int)(g_historyDlg.listAreaH / g_historyDlg.itemH);
                        if (visibleCount > totalItems) visibleCount = totalItems;
                        int maxScroll = totalItems - visibleCount;
                        if (maxScroll < 0) maxScroll = 0;
                        if (g_historyDlg.scrollPos < maxScroll) g_historyDlg.scrollPos++;
                        continue;
                    }
                    continue;
                }

                // custom color dialog open: keyboard handling
                if (g_ccDlg.open)
                {
                    if (e.key.code == sf::Keyboard::Escape)
                    {
                        g_ccDlg.open = false;
                        ccSavePresetsToFile();
                        game.saveToFile(SAVE_FILE);
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Up)
                    {
                        if (g_ccDlg.scrollPos > 0) g_ccDlg.scrollPos--;
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Down)
                    {
                        int totalItems = CC_TOTAL_ITEMS;
                        float availH = g_ccDlg.listAreaH;
                        float itemH = g_ccDlg.itemH;
                        int visibleCount = (int)(availH / itemH);
                        if (visibleCount > totalItems) visibleCount = totalItems;
                        int maxScroll = totalItems - visibleCount;
                        if (maxScroll < 0) maxScroll = 0;
                        if (g_ccDlg.scrollPos < maxScroll) g_ccDlg.scrollPos++;
                        continue;
                    }
                    continue;
                }

                // history playback mode: space = play/pause, left/right = previous/next step, other keys ignored (no exit)
                if (g_pb.active)
                {
                    const HistoryEntry* pbEntry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                        ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                    if (e.key.code == sf::Keyboard::Space)
                    {
                        if (pbEntry && pbEntry->moveCount > 0)
                        {
                            if (g_pb.playing)
                            {
                                g_pb.playing = false;
                            }
                            else
                            {
                                if (g_pb.playMs >= pbTotalMs(*pbEntry) - 1.f)  // finished -> replay from the start
                                {
                                    g_pb.playMs = 0.f;
                                    g_pb.appliedStep = -1;
                                    g_pb.curFrame = 0;
                                    g_pbGame.setBoardFrame(pbEntry->boardSize, g_pb.colorScheme,
                                                           pbEntry->frameScores[0], pbEntry->frames[0]);
                                    g_pbGame.finishAnimation();
                                }
                                g_pb.playing = true;
                            }
                        }
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Left)
                    {
                        if (pbEntry) pbStepBy(*pbEntry, -1);
                        continue;
                    }
                    if (e.key.code == sf::Keyboard::Right)
                    {
                        if (pbEntry) pbStepBy(*pbEntry, +1);
                        continue;
                    }
                    continue;   // other keys ignored
                }
                if (game.isViewingHistory())
                {
                    game.exitHistoryView();
                    continue;
                }

                const bool chActive = g_ch.active && !g_pb.active;
                switch (e.key.code)
                {
                case sf::Keyboard::Up:
                case sf::Keyboard::W:
                    if (!menuOpen && g_aiMode != AI_RUNNING && !(chActive && g_ch.finished) && game.move(0, -1))
                    { game.addRandomTile(); markSaveDirty(); chAfterUserMove(game); tmAfterUserMove(game);
                      if (g_practiceActive) { g_practiceLastMoveDir = 2; g_lastMoveByAI = false; requestPracticePrediction(game); } }
                    break;
                case sf::Keyboard::Down:
                case sf::Keyboard::S:
                    if (!menuOpen && g_aiMode != AI_RUNNING && !(chActive && g_ch.finished) && game.move(0, 1))
                    { game.addRandomTile(); markSaveDirty(); chAfterUserMove(game); tmAfterUserMove(game);
                      if (g_practiceActive) { g_practiceLastMoveDir = 3; g_lastMoveByAI = false; requestPracticePrediction(game); } }
                    break;
                case sf::Keyboard::Left:
                case sf::Keyboard::A:
                    if (!menuOpen && g_aiMode != AI_RUNNING && !(chActive && g_ch.finished) && game.move(-1, 0))
                    { game.addRandomTile(); markSaveDirty(); chAfterUserMove(game); tmAfterUserMove(game);
                      if (g_practiceActive) { g_practiceLastMoveDir = 0; g_lastMoveByAI = false; requestPracticePrediction(game); } }
                    break;
                case sf::Keyboard::Right:
                case sf::Keyboard::D:
                    if (!menuOpen && g_aiMode != AI_RUNNING && !(chActive && g_ch.finished) && game.move(1, 0))
                    { game.addRandomTile(); markSaveDirty(); chAfterUserMove(game); tmAfterUserMove(game);
                      if (g_practiceActive) { g_practiceLastMoveDir = 1; g_lastMoveByAI = false; requestPracticePrediction(game); } }
                    break;
                case sf::Keyboard::Z:
                    // challenge/timer mode forbid undo (timer-mode undo key is "close", same as challenge)
                    if (!chActive && !g_tm.active && g_aiMode != AI_RUNNING) { game.undo(); markSaveDirty(); }
                    break;
                case sf::Keyboard::R:
                    if (chActive) { chStart(game); }   // R in challenge = restart the current project
                    else if (g_tm.active) { tmResetState(); game.resetNoHistory(); g_aiCacheReset = 1; }   // R in timer = restart the session
                    else restartCurrentBoard(game);
                    break;
                case sf::Keyboard::F9:
                    game.setColorScheme(2); // Neon
                    game.setupTestBoard();
                    break;
                case sf::Keyboard::Num1: game.setColorScheme(0); game.saveToFile(SAVE_FILE); break;
                case sf::Keyboard::Num2: game.setColorScheme(1); game.saveToFile(SAVE_FILE); break;
                case sf::Keyboard::Num3: game.setColorScheme(2); game.saveToFile(SAVE_FILE); break;
                case sf::Keyboard::Num4: game.setColorScheme(3); game.saveToFile(SAVE_FILE); break;
                case sf::Keyboard::Num5: game.setColorScheme(4); game.saveToFile(SAVE_FILE); break;
                case sf::Keyboard::Num6: game.setColorScheme(5); game.saveToFile(SAVE_FILE); break;
                case sf::Keyboard::LBracket:
                    if (menuOpen)
                    {
                        int newSize = game.getBoardSize() - 1;
                        if (newSize >= MIN_BOARD_SIZE)
                            switchBoardSize(game, newSize);
                    }
                    break;
                case sf::Keyboard::RBracket:
                    if (menuOpen)
                    {
                        int newSize = game.getBoardSize() + 1;
                        if (newSize <= MAX_BOARD_SIZE)
                            switchBoardSize(game, newSize);
                    }
                    break;
                case sf::Keyboard::Space:
                    // space: toggle AI play/pause (same logic as the AI play button)
                    if (g_aiMode == AI_RUNNING)
                    {
                        g_aiMode = AI_PAUSED;
                        g_aiTiming = false;
                        g_aiStepClock.restart();
                        game.setAIModeAnimations(false);   // after pausing, manual play -> restore default animations
                    }
                    else if (g_aiMode == AI_PAUSED)
                    {
                        g_aiMode = AI_RUNNING;
                        g_aiTiming = true;
                        game.setAIAnimDuration(g_aiStepDelaySec);
                        game.markAiAssisted();
                        if (g_aiBoardSize != game.getBoardSize())
                        {
                            g_aiBoardSize = game.getBoardSize();
                            g_aiEngine.setBoardSize(g_aiBoardSize);
                        }
                        g_aiUndoConsecutive = 0;
                        g_aiUndoPending = false;
                        g_aiSkipDir = -1;  g_aiLastMoveDir = -1;
                        {
                            auto initBoard = buildAiBoard(game);
                            aiLaunchPrefetch(initBoard, g_aiBoardSize);
                        }
                        g_aiStepClock.restart();
                    }
                    break;
                case sf::Keyboard::Escape:
                    menuOpen = false;
                    break;
                case sf::Keyboard::Tab:
                    menuOpen = !menuOpen;
                    break;
                default:
                    break;
                }
            }

            // mouse click events
            if (e.type == sf::Event::MouseButtonPressed && e.mouseButton.button == sf::Mouse::Left)
            {
                sf::Vector2i pos = sf::Mouse::getPosition(window);
                unsigned int winW = window.getSize().x;
                unsigned int winH = window.getSize().y;

                // AI fully-ended dialog: click anywhere -> close the dialog, keep the end screen (board layout, score),
                // AI stays paused (control UI kept); the user plays the next game by themselves (click "start" -> new game
                // running AI automatically, or menu restart etc.). All three boards (3x3/4x4/5x5) behave the same.
                if (g_showAIEnd)
                {
                    g_showAIEnd = false;
                    g_aiStepClock.restart();
                    continue;   // do not clear the board; keep the end screen
                }

                // timer-mode "save?" confirm dialog: yes/no buttons (timing stops while shown)
                if (g_tm.confirmClose)
                {
                    auto inR = [&](const sf::FloatRect& r) {
                        return pos.x >= r.left && pos.x <= r.left + r.width &&
                               pos.y >= r.top && pos.y <= r.top + r.height;
                    };
                    if (inR(g_tm.confirmYesBtn)) tmConfirmCloseDecide(window, game, true);
                    else if (inR(g_tm.confirmNoBtn)) tmConfirmCloseDecide(window, game, false);
                    continue;   // block other interaction while the dialog is up
                }

                // font dialog open: only its input is handled
                if (fontDlg.open)
                {
                    float dlgW = winW * 0.85f;
                    float dlgH = winH * 0.82f;
                    float dlgX = (winW - dlgW) / 2.f;
                    float dlgY = (winH - dlgH) / 2.f;

                    // click Default (back to the default font)
                    if (pos.x >= fontDlg.resetBtnX && pos.x <= fontDlg.resetBtnX + fontDlg.resetBtnW &&
                        pos.y >= fontDlg.resetBtnY && pos.y <= fontDlg.resetBtnY + fontDlg.resetBtnH)
                    {
                        font = sf::Font();
                        if (font.loadFromFile(DEFAULT_FONT_PATH))
                        {
                            g_previousFontPath = g_currentFontPath;
                            g_currentFontPath = DEFAULT_FONT_PATH;
                            g_usingFallback = (g_language == LANG_ZH && !fontSupportsChinese(g_currentFontPath));
                            game.saveToFile(SAVE_FILE);
                            fontDlg.selectedIdx = -1;
                        }
                    }
                    // click Previous (back to the previous font)
                    else if (pos.x >= fontDlg.prevBtnX && pos.x <= fontDlg.prevBtnX + fontDlg.prevBtnW &&
                             pos.y >= fontDlg.prevBtnY && pos.y <= fontDlg.prevBtnY + fontDlg.prevBtnH)
                    {
                        font = sf::Font();
                        if (font.loadFromFile(g_previousFontPath))
                        {
                            std::string tmp = g_currentFontPath;
                            g_currentFontPath = g_previousFontPath;
                            g_previousFontPath = tmp;
                            g_usingFallback = (g_language == LANG_ZH && !fontSupportsChinese(g_currentFontPath));
                            game.saveToFile(SAVE_FILE);
                            fontDlg.selectedIdx = -1;
                        }
                    }
                    // click Apply
                    else if (pos.x >= fontDlg.applyBtnX && pos.x <= fontDlg.applyBtnX + fontDlg.applyBtnW &&
                        pos.y >= fontDlg.applyBtnY && pos.y <= fontDlg.applyBtnY + fontDlg.applyBtnH)
                    {
                        if (fontDlg.selectedIdx >= 0 && fontDlg.selectedIdx < (int)g_systemFonts.size())
                        {
                            font = sf::Font();
                            if (font.loadFromFile(g_systemFonts[fontDlg.selectedIdx].path))
                            {
                                g_previousFontPath = g_currentFontPath;
                                g_currentFontPath = g_systemFonts[fontDlg.selectedIdx].path;
                                // set the fallback flag
                                g_usingFallback = (g_language == LANG_ZH && !fontSupportsChinese(g_currentFontPath));
                                game.saveToFile(SAVE_FILE);
                            }
                        }
                        fontDlg.open = false;
                    }
                    // click Cancel
                    else if (pos.x >= fontDlg.cancelBtnX && pos.x <= fontDlg.cancelBtnX + fontDlg.cancelBtnW &&
                             pos.y >= fontDlg.cancelBtnY && pos.y <= fontDlg.cancelBtnY + fontDlg.cancelBtnH)
                    {
                        fontDlg.open = false;
                    }
                    // click the font list
                    else if (pos.x >= fontDlg.listAreaX && pos.x <= fontDlg.listAreaX + fontDlg.listAreaW &&
                             pos.y >= fontDlg.listAreaY && pos.y <= fontDlg.listAreaY + fontDlg.listAreaH)
                    {
                        int relY = (int)(pos.y - fontDlg.listAreaY);
                        int clickedIdx = fontDlg.scrollPos + (int)(relY / fontDlg.itemH);
                        if (clickedIdx >= 0 && clickedIdx < (int)g_systemFonts.size())
                        {
                            fontDlg.selectedIdx = clickedIdx;
                        }
                    }
                    // click outside to close
                    else if (pos.x < dlgX || pos.x > dlgX + dlgW ||
                             pos.y < dlgY || pos.y > dlgY + dlgH)
                    {
                        fontDlg.open = false;
                    }
                    continue;
                }

                // history dialog open: only its input is handled
                if (g_historyDlg.open)
                {
                    float boardPx = computeBoardPx(winW, winH, false);
                    const float pad = boardPx * LAYOUT_PAD_RATIO;
                    float availW = winW - pad * 2;
                    float availH = winH - pad * 2;
                    float dlgW = std::min(availW, availH / 1.2f);   // fixed width:height = 1:1.2
                    float dlgH = dlgW * 1.2f;
                    float dlgX = (winW - dlgW) / 2.f;
                    float dlgY = (winH - dlgH) / 2.f;

                    // click the top dual keys: history / import history switch
                    const bool hitTabHist = (pos.x >= g_historyDlg.tabHistX && pos.x <= g_historyDlg.tabHistX + g_historyDlg.tabHistW &&
                                             pos.y >= g_historyDlg.tabHistY && pos.y <= g_historyDlg.tabHistY + g_historyDlg.tabHistH);
                    const bool hitTabImp  = (pos.x >= g_historyDlg.tabImpX  && pos.x <= g_historyDlg.tabImpX  + g_historyDlg.tabImpW &&
                                             pos.y >= g_historyDlg.tabImpY  && pos.y <= g_historyDlg.tabImpY  + g_historyDlg.tabImpH);
                    if (hitTabHist)
                    {
                        if (g_historyDlg.tab != 0) { g_historyDlg.tab = 0; g_historyDlg.scrollPos = 0; }
                        continue;
                    }
                    if (hitTabImp)
                    {
                        if (g_historyDlg.tab != 1) { g_historyDlg.tab = 1; g_historyDlg.importScroll = 0; }
                        continue;
                    }
                    // ===== import history panel =====
                    if (g_historyDlg.tab == 1)
                    {
                        // click choose file: multi-select .vrs/.dat/.bin/.txt
                        if (pos.x >= g_historyDlg.selX && pos.x <= g_historyDlg.selX + g_historyDlg.selW &&
                            pos.y >= g_historyDlg.selY && pos.y <= g_historyDlg.selY + g_historyDlg.selH)
                        {
                            if (!g_historyDlg.importing)
                            {
                                g_historyDlg.importing = true;
                                char fileBuf[65536] = { 0 };
                                OPENFILENAMEA ofn;
                                std::memset(&ofn, 0, sizeof(ofn));
                                ofn.lStructSize = sizeof(ofn);
                                ofn.hwndOwner = GetActiveWindow();
                                ofn.lpstrFilter = "Replay/History Files (*.vrs;*.dat;*.bin)\0*.vrs;*.dat;*.bin\0All Files (*.*)\0*.*\0";
                                ofn.lpstrFile = fileBuf;
                                ofn.nMaxFile = sizeof(fileBuf);
                                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                                            OFN_ALLOWMULTISELECT | OFN_EXPLORER;
                                ofn.lpstrTitle = "选择要导入的 2048 录像/历史文件（.vrs/.dat/.bin）";
                                if (GetOpenFileNameA(&ofn))
                                {
                                    // multi-select: fileBuf = "dir\0file1\0file2\0\0"; single file = full path
                                    // distinction: on multi-select ofn.nFileOffset-1 is '\0' (dir/file separator);
                                    //            on single-select it is '\\' (path separator).
                                    const char* first = fileBuf;
                                    // multi-select test: nFileOffset-1 == '\0' (dir ends) means multi-select;
                                    // if it is '\\' (path separator right before the filename) it is single-select.
                                    // note: cannot use nFileOffset <= strlen(fileBuf): on multi-select
                                    // nFileOffset = strlen(dir)+1, which always satisfies that test and would misclassify multi-select as single-select.
                                    bool multiSelect = (ofn.nFileOffset > 0 &&
                                                        fileBuf[ofn.nFileOffset - 1] == '\0');
                                    if (multiSelect)
                                    {
                                        // multi-select: dir + several filenames; add only supported types, filter others
                                        std::string dir = fileBuf;
                                        const char* p = fileBuf + ofn.nFileOffset;
                                        while (*p)
                                        {
                                            std::string fp = dir + "\\" + p;
                                            if (isSupportedImportFile(fp))
                                                g_historyDlg.importFiles.push_back(fp);
                                            p += strlen(p) + 1;
                                        }
                                    }
                                    else
                                    {
                                        // single-select: the whole buffer is the full path; filter unsupported types
                                        if (isSupportedImportFile(first))
                                            g_historyDlg.importFiles.push_back(first);
                                    }
                                    g_historyDlg.importScroll = 0;
                                }
                                g_historyDlg.importing = false;
                            }
                        }
                        // click close
                        else if (pos.x >= g_historyDlg.closeBtnX && pos.x <= g_historyDlg.closeBtnX + g_historyDlg.closeBtnW &&
                                 pos.y >= g_historyDlg.closeBtnY && pos.y <= g_historyDlg.closeBtnY + g_historyDlg.closeBtnH)
                        {
                            g_historyDlg.open = false;
                        }
                        // click load all: auto-load every pending import file one by one
                        else if (pos.x >= g_historyDlg.loadAllX && pos.x <= g_historyDlg.loadAllX + g_historyDlg.loadAllW &&
                                 pos.y >= g_historyDlg.loadAllY && pos.y <= g_historyDlg.loadAllY + g_historyDlg.loadAllH)
                        {
                            if (!g_historyDlg.importFiles.empty())
                            {
                                std::vector<std::string> keep;
                                keep.reserve(g_historyDlg.importFiles.size());
                                bool anyLoaded = false;
                                for (const std::string& path : g_historyDlg.importFiles)
                                {
                                    std::string name = path.substr(path.find_last_of("\\/") + 1);
                                    std::string outFile;
                                    int reason = 0;
                                    if (importHistoryFile(path, name, &outFile, &reason))
                                        anyLoaded = true;   // success: remove from the list
                                    else
                                        keep.push_back(path);  // failure: keep for later
                                }
                                g_historyDlg.importFiles = keep;
                                g_historyDlg.importScroll = 0;
                                if (anyLoaded) loadHistoryList();
                            }
                        }
                        // click a file entry -> import that file and open it in history playback immediately
                        else if (pos.x >= g_historyDlg.impListX && pos.x <= g_historyDlg.impListX + g_historyDlg.impListW &&
                                 pos.y >= g_historyDlg.impListY && pos.y <= g_historyDlg.impListY + g_historyDlg.impListH)
                        {
                            // offset fix: entry visual start = impListY + impItemTopOffset, spacing = impItemSpacing
                            float spacing = g_historyDlg.impItemSpacing;
                            float topOff = g_historyDlg.impItemTopOffset;
                            if (spacing <= 0.f) { spacing = dlgH * 0.085f; topOff = dlgH * 0.006f; }
                            int relY = (int)(pos.y - g_historyDlg.impListY - topOff);
                            int visIdx = g_historyDlg.importScroll + (int)(relY / spacing);
                            if (visIdx >= 0 && visIdx < (int)g_historyDlg.importFiles.size())
                            {
                                // delete-button hit (entry bottom-right small circle), takes priority over import
                                float iSpacing = g_historyDlg.impItemSpacing;
                                float iTopOff = g_historyDlg.impItemTopOffset;
                                if (iSpacing <= 0.f) { iSpacing = dlgH * 0.085f; iTopOff = dlgH * 0.006f; }
                                float iItemH = (g_historyDlg.impItemH > 0) ? g_historyDlg.impItemH : dlgH * 0.075f;
                                float iItemY = g_historyDlg.impListY
                                            + (visIdx - g_historyDlg.importScroll) * iSpacing + iTopOff;
                                float iDelSize = std::max(13.f, iItemH * 0.45f);
                                float iDelX = g_historyDlg.impListX + g_historyDlg.impListW - iDelSize - 4.f;
                                float iDelY = iItemY + iItemH - iDelSize - 4.f;
                                if (pos.x >= iDelX && pos.x <= iDelX + iDelSize &&
                                    pos.y >= iDelY && pos.y <= iDelY + iDelSize)
                                {
                                    g_historyDlg.importFiles.erase(g_historyDlg.importFiles.begin() + visIdx);
                                    g_historyDlg.importScroll = 0;
                                    continue;
                                }
                                // not delete -> import the file and open it in history playback immediately
                                std::string path = g_historyDlg.importFiles[visIdx];
                                std::string name = path.substr(path.find_last_of("\\/") + 1);
                                std::string outFile;
                                int reason = 0;
                                if (importHistoryFile(path, name, &outFile, &reason))
                                {
                                    loadHistoryList();   // reload (incl. the new import)
                                    g_historyDlg.importFiles.erase(g_historyDlg.importFiles.begin() + visIdx);
                                    // an imported history opens in history playback on first import:
                                    if (!outFile.empty())
                                        for (int i = 0; i < (int)g_historyDlg.entries.size(); ++i)
                                            if (g_historyDlg.entries[i].filename == outFile)
                                            {
                                                startHistoryPlayback(game, i);
                                                break;
                                            }
                                }
                                else if (reason == 1)
                                {
                                    // special board sizes (other than 3x3/4x4/5x5) are not supported
                                    showInfoBox("暂不支持该棋盘尺寸",
                                                "该回放为特殊尺寸棋盘，当前版本暂不支持回放。\n\n"
                                                "本软件仅支持 3×3 / 4×4 / 5×5 棋盘。");
                                }
                            }
                        }
                        // click outside to close
                        else if (pos.x < dlgX || pos.x > dlgX + dlgW ||
                                 pos.y < dlgY || pos.y > dlgY + dlgH)
                        {
                            g_historyDlg.open = false;
                        }
                        continue;
                    }

                    // ===== history panel (the software's own operation history) =====
                    // while the delete confirm dialog is open: only its input is handled (modal)
                    if (g_historyDlg.confirmDelete)
                    {
                        // "yes": really delete
                        if (pos.x >= g_historyDlg.delYesX && pos.x <= g_historyDlg.delYesX + g_historyDlg.delYesW &&
                            pos.y >= g_historyDlg.delYesY && pos.y <= g_historyDlg.delYesY + g_historyDlg.delYesH)
                        {
                            int di = g_historyDlg.confirmDeleteIdx;
                            if (di >= 0 && di < (int)g_historyDlg.entries.size())
                            {
                                const std::string path = std::string(HISTORY_DIR) + "\\" + g_historyDlg.entries[di].filename;
                                DeleteFileA(path.c_str());
                                // if this entry is being played back -> exit playback
                                if (g_pb.active && g_pb.entryIdx == di)
                                {
                                    g_pb.active = false;
                                    g_pb.playing = false;
                                    game.exitHistoryView();
                                }
                                loadHistoryList();   // reload the list (incl. best-score/fastest-challenge index recompute)
                                g_historyDlg.confirmDelete = false;
                                g_historyDlg.confirmDeleteIdx = -1;
                            }
                        }
                        // "no": cancel (guided highlight = safe default)
                        else if (pos.x >= g_historyDlg.delNoX && pos.x <= g_historyDlg.delNoX + g_historyDlg.delNoW &&
                                 pos.y >= g_historyDlg.delNoY && pos.y <= g_historyDlg.delNoY + g_historyDlg.delNoH)
                        {
                            g_historyDlg.confirmDelete = false;
                            g_historyDlg.confirmDeleteIdx = -1;
                        }
                        // click outside the confirm box -> also cancel
                        else
                        {
                            g_historyDlg.confirmDelete = false;
                            g_historyDlg.confirmDeleteIdx = -1;
                        }
                        continue;
                    }
                    // click best records: toggle the filter showing the best score of each board size
                    if (pos.x >= g_historyDlg.maxBtnX && pos.x <= g_historyDlg.maxBtnX + g_historyDlg.maxBtnW &&
                        pos.y >= g_historyDlg.maxBtnY && pos.y <= g_historyDlg.maxBtnY + g_historyDlg.maxBtnH)
                    {
                        g_historyDlg.filterMax = !g_historyDlg.filterMax;
                        g_historyDlg.scrollPos = 0;
                    }
                    // click close
                    else if (pos.x >= g_historyDlg.closeBtnX && pos.x <= g_historyDlg.closeBtnX + g_historyDlg.closeBtnW &&
                             pos.y >= g_historyDlg.closeBtnY && pos.y <= g_historyDlg.closeBtnY + g_historyDlg.closeBtnH)
                    {
                        g_historyDlg.open = false;
                    }
                    // click a history entry (delete button first, otherwise open playback)
                    else if (pos.x >= g_historyDlg.listAreaX && pos.x <= g_historyDlg.listAreaX + g_historyDlg.listAreaW &&
                             pos.y >= g_historyDlg.listAreaY && pos.y <= g_historyDlg.listAreaY + g_historyDlg.listAreaH)
                    {
                        int relY = (int)(pos.y - g_historyDlg.listAreaY);
                        int visIdx = g_historyDlg.scrollPos + (int)(relY / g_historyDlg.itemH);
                        int entryIdx = histEntryAt(g_historyDlg, visIdx);
                        if (entryIdx >= 0 && entryIdx < (int)g_historyDlg.entries.size())
                        {
                            // delete-button geometry matching drawing
                            float itemPadX = 6.f, itemPadY = 4.f;
                            float itemInnerW = g_historyDlg.listAreaW - itemPadX * 2.f;
                            float itemInnerH = g_historyDlg.itemH - itemPadY * 2.f;
                            float itemX = g_historyDlg.listAreaX + itemPadX;
                            float itemY = g_historyDlg.listAreaY + (visIdx - g_historyDlg.scrollPos) * g_historyDlg.itemH;
                            float drawY = itemY + itemPadY;
                            float delSize = std::max(15.f, itemInnerH * 0.27f);
                            float expGap = delSize * 0.5f;
                            float expSize = delSize;   // same size as the delete button
                            float delX = itemX + itemInnerW - delSize - 5.f;
                            float delY = drawY + itemInnerH - delSize - 5.f;
                            // export button: left of the delete button, a bit farther
                            float expX = delX - expGap - expSize;
                            float expY = delY;
                            if (pos.x >= expX && pos.x <= expX + expSize &&
                                pos.y >= expY && pos.y <= expY + expSize)
                            {
                                // export this history entry as a .dat file
                                const HistoryEntry& entry = g_historyDlg.entries[entryIdx];
                                char fileBuf[4096] = { 0 };
                                OPENFILENAMEA ofn;
                                std::memset(&ofn, 0, sizeof(ofn));
                                ofn.lStructSize = sizeof(ofn);
                                ofn.hwndOwner = GetActiveWindow();
                                ofn.lpstrFilter = "2048 History Files (*.dat)\0*.dat\0All Files (*.*)\0*.*\0";
                                ofn.lpstrFile = fileBuf;
                                ofn.nMaxFile = sizeof(fileBuf);
                                // default English name: reuse the entry's original filename (ASCII); if it exists in the target dir, append a sequence number to avoid overwrite
                                std::string base = entry.filename;
                                if (base.empty()) base = "history_export";
                                size_t dotP = base.find_last_of('.');
                                if (dotP != std::string::npos) base = base.substr(0, dotP);
                                const std::string& exeDirP = getExeDir();
                                int seq = 1;
                                std::string defName;
                                while (true)
                                {
                                    defName = (seq == 1) ? base : (base + "_" + std::to_string(seq));
                                    if (GetFileAttributesA((exeDirP + "\\" + defName + ".dat").c_str())
                                            == INVALID_FILE_ATTRIBUTES) break;
                                    ++seq;
                                }
                                std::string defPath = exeDirP + "\\" + defName + ".dat";
                                strncpy_s(fileBuf, sizeof(fileBuf), defPath.c_str(), sizeof(fileBuf) - 1);
                                ofn.lpstrInitialDir = exeDirP.c_str();
                                ofn.lpstrDefExt = "dat";
                                ofn.lpstrTitle = (g_language == LANG_ZH) ? "导出该历史记录"
                                                                          : "Export this history record";
                                ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
                                if (GetSaveFileNameA(&ofn))
                                {
                                    if (!exportHistoryEntryToFile(entry, ofn.lpstrFile))
                                        logMsg("export history FAILED");
                                }
                            }
                            else if (pos.x >= delX && pos.x <= delX + delSize &&
                                     pos.y >= delY && pos.y <= delY + delSize)
                            {
                                g_historyDlg.confirmDelete = true;
                                g_historyDlg.confirmDeleteIdx = entryIdx;
                            }
                            else
                            {
                                startHistoryPlayback(game, entryIdx);
                            }
                        }
                    }
                    // click outside to close
                    else if (pos.x < dlgX || pos.x > dlgX + dlgW ||
                             pos.y < dlgY || pos.y > dlgY + dlgH)
                    {
                        g_historyDlg.open = false;
                    }
                    continue;
                }

                // custom color dialog open: only its input is handled
                if (g_ccDlg.open)
                {
                    bool handled = false;

                    // handle slider drag end first
                    g_ccDlg.draggingR = false;
                    g_ccDlg.draggingG = false;
                    g_ccDlg.draggingB = false;

                    // Close button
                    if (pos.x >= g_ccDlg.closeBtnX && pos.x <= g_ccDlg.closeBtnX + g_ccDlg.closeBtnW &&
                        pos.y >= g_ccDlg.closeBtnY && pos.y <= g_ccDlg.closeBtnY + g_ccDlg.closeBtnH)
                    {
                        g_ccDlg.open = false;
                        ccSavePresetsToFile();
                        game.saveToFile(SAVE_FILE);
                        handled = true;
                    }
                    // Save button: try saving the current Custom scheme to the first empty slot
                    else if (pos.x >= g_ccDlg.saveBtnX && pos.x <= g_ccDlg.saveBtnX + g_ccDlg.saveBtnW &&
                             pos.y >= g_ccDlg.saveBtnY && pos.y <= g_ccDlg.saveBtnY + g_ccDlg.saveBtnH)
                    {
                        int slot = ccFindEmptySlot();
                        if (slot < 0)
                        {
                            // slots full: show an alert
                            showConfirm = true;
                            showPresetFullAlert = true;
                        }
                        else
                        {
                            // save from workingScheme to the save slot
                            CustomPreset& p = g_ccDlg.presets[slot];
                            const SchemeData& ws = g_ccDlg.workingScheme;
                            p.used = true;
                            std::memcpy(p.tileColors, ws.tileColors, sizeof(p.tileColors));
                            p.emptyColor = ws.emptyColor;
                            p.windowBg = ws.windowBg;
                            p.boardBg = ws.boardBg;
                            p.scoreBoxColor = ws.scoreBoxColor;
                            p.textLight = ws.textLight;
                            p.textDark = ws.textDark;
                            p.lightTextFromValue = ws.lightTextFromValue;
                            ccSavePresetsToFile();
                        }
                        handled = true;
                    }
                    // Apply button: commit workingScheme into SCHEMES and apply it as the active game theme
                    else if (pos.x >= g_ccDlg.applyBtnX && pos.x <= g_ccDlg.applyBtnX + g_ccDlg.applyBtnW &&
                             pos.y >= g_ccDlg.applyBtnY && pos.y <= g_ccDlg.applyBtnY + g_ccDlg.applyBtnH)
                    {
                        // workingScheme → SCHEMES[CUSTOM_SCHEME_IDX]
                        SchemeData& dst = SCHEMES[CUSTOM_SCHEME_IDX];
                        const SchemeData& src = g_ccDlg.workingScheme;
                        dst.windowBg = src.windowBg;
                        dst.boardBg = src.boardBg;
                        dst.emptyColor = src.emptyColor;
                        dst.scoreBoxColor = src.scoreBoxColor;
                        dst.textLight = src.textLight;
                        dst.textDark = src.textDark;
                        dst.lightTextFromValue = src.lightTextFromValue;
                        for (int k = 0; k <= MAX_POWER; ++k)
                            dst.tileColors[k] = src.tileColors[k];
                        game.setColorScheme(CUSTOM_SCHEME_IDX);
                        saveCustomTheme();   // persist the custom theme so the current theme stays highlighted after restart
                        game.saveToFile(SAVE_FILE);
                        handled = true;
                    }
                    // export theme: save the (being-edited) theme to a user-chosen file
                    else if (g_ccDlg.exportBtnW > 0 &&
                             pos.x >= g_ccDlg.exportBtnX && pos.x <= g_ccDlg.exportBtnX + g_ccDlg.exportBtnW &&
                             pos.y >= g_ccDlg.exportBtnY && pos.y <= g_ccDlg.exportBtnY + g_ccDlg.exportBtnH)
                    {
                        char fileBuf[4096] = { 0 };
                        OPENFILENAMEA ofn;
                        std::memset(&ofn, 0, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = GetActiveWindow();
                        ofn.lpstrFilter = "2048 Theme Files (*.theme)\0*.theme\0All Files (*.*)\0*.*\0";
                        ofn.lpstrFile = fileBuf;
                        ofn.nMaxFile = sizeof(fileBuf);
                        // default English filename (Chinese names garble in the ANSI save dialog);
                        // if a same-named theme exists in the target dir (exe dir), append a sequence number
                        const std::string& exeDirP = getExeDir();
                        int seq = 1;
                        std::string defName;
                        while (true)
                        {
                            defName = (seq == 1) ? std::string("my_theme")
                                                 : std::string("my_theme") + std::to_string(seq);
                            std::string cand = exeDirP + "\\" + defName + ".theme";
                            if (GetFileAttributesA(cand.c_str()) == INVALID_FILE_ATTRIBUTES)
                                break;
                            ++seq;
                        }
                        std::string defPath = exeDirP + "\\" + defName + ".theme";
                        strncpy_s(fileBuf, sizeof(fileBuf), defPath.c_str(), sizeof(fileBuf) - 1);
                        ofn.lpstrInitialDir = exeDirP.c_str();
                        ofn.lpstrDefExt = "theme";
                        ofn.lpstrTitle = (g_language == LANG_ZH) ? "导出当前主题"
                                                                  : "Export current theme";
                        ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
                        if (GetSaveFileNameA(&ofn))
                        {
                            if (!ccExportThemeToPath(ofn.lpstrFile, g_ccDlg.workingScheme))
                                logMsg("export theme FAILED");
                        }
                        handled = true;
                    }
                    // import theme: read the file -> apply as the current theme directly and map to the custom list
                    else if (g_ccDlg.importBtnW > 0 &&
                             pos.x >= g_ccDlg.importBtnX && pos.x <= g_ccDlg.importBtnX + g_ccDlg.importBtnW &&
                             pos.y >= g_ccDlg.importBtnY && pos.y <= g_ccDlg.importBtnY + g_ccDlg.importBtnH)
                    {
                        char fileBuf[4096] = { 0 };
                        OPENFILENAMEA ofn;
                        std::memset(&ofn, 0, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = GetActiveWindow();
                        ofn.lpstrFilter = "2048 Theme Files (*.theme)\0*.theme\0All Files (*.*)\0*.*\0";
                        ofn.lpstrFile = fileBuf;
                        ofn.nMaxFile = sizeof(fileBuf);
                        ofn.lpstrTitle = (g_language == LANG_ZH) ? "导入主题，将直接应用"
                                                                  : "Import a theme (applied directly)";
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                        if (GetOpenFileNameA(&ofn) && ccImportThemeFromPath(ofn.lpstrFile, g_ccDlg.workingScheme))
                        {
                            const SchemeData& ws = g_ccDlg.workingScheme;
                            // 1) apply as the current theme directly
                            SchemeData& dst = SCHEMES[CUSTOM_SCHEME_IDX];
                            std::memcpy(dst.tileColors, ws.tileColors, sizeof(dst.tileColors));
                            dst.emptyColor = ws.emptyColor;
                            dst.windowBg = ws.windowBg;
                            dst.boardBg = ws.boardBg;
                            dst.scoreBoxColor = ws.scoreBoxColor;
                            dst.textLight = ws.textLight;
                            dst.textDark = ws.textDark;
                            dst.lightTextFromValue = ws.lightTextFromValue;
                            dst.name = "";
                            game.setColorScheme(CUSTOM_SCHEME_IDX);
                            saveCustomTheme();
                            game.saveToFile(SAVE_FILE);
                            // 2) map to the custom list: prefer an empty slot; if full, overwrite the selected slot; with no selection, overwrite slot 0
                            int slot = ccFindEmptySlot();
                            if (slot < 0)
                                slot = (g_ccDlg.selCustomIdx >= 0 && g_ccDlg.selCustomIdx < MAX_CUSTOM_PRESETS)
                                           ? g_ccDlg.selCustomIdx : 0;
                            CustomPreset& pr = g_ccDlg.presets[slot];
                            pr.used = true;
                            std::memcpy(pr.tileColors, ws.tileColors, sizeof(pr.tileColors));
                            pr.emptyColor = ws.emptyColor;
                            pr.windowBg = ws.windowBg;
                            pr.boardBg = ws.boardBg;
                            pr.scoreBoxColor = ws.scoreBoxColor;
                            pr.textLight = ws.textLight;
                            pr.textDark = ws.textDark;
                            pr.lightTextFromValue = ws.lightTextFromValue;
                            ccSavePresetsToFile();
                            g_ccDlg.selCustomIdx = slot;   // highlight the imported custom item
                            g_ccDlg.selPresetIdx = -1;
                            // scroll to that slot (if not visible)
                            if (g_ccDlg.customItemH > 0)
                            {
                                int visCnt = std::max(1, (int)(g_ccDlg.customListH / g_ccDlg.customItemH));
                                int maxScr = std::max(0, MAX_CUSTOM_PRESETS - visCnt);
                                g_ccDlg.customScroll = std::max(0, std::min(maxScr, slot - visCnt / 2));
                            }
                        }
                        handled = true;
                    }
                    // top theme list: click a theme -> load it into the working area preview, applied only on "apply"
                    if (!handled && g_ccDlg.presetItemW > 0 &&
                        pos.x >= g_ccDlg.presetItemX && pos.x <= g_ccDlg.presetItemX + g_ccDlg.presetItemW &&
                        pos.y >= g_ccDlg.presetItemY && pos.y <= g_ccDlg.presetItemY + g_ccDlg.presetListH)
                    {
                        int relY = (int)(pos.y - g_ccDlg.presetItemY);
                        int vis = (g_ccDlg.presetItemH > 0) ? (int)(relY / g_ccDlg.presetItemH) : 0;
                        int idx = g_ccDlg.presetScroll + vis;
                        if (idx >= 0 && idx < NUM_PRESET_SCHEMES)
                        {
                            int schemeIdx = (idx < NUM_SCHEMES) ? idx : (CUSTOM_SCHEME_IDX + 1 + (idx - NUM_SCHEMES));
                            // record the selection (yellow highlight on click, kept after apply)
                            g_ccDlg.selPresetIdx = idx;
                            g_ccDlg.selCustomIdx = -1;
                            // only loads into the working area (thumbnail previews immediately); applying needs the bottom "apply"
                            const SchemeData& src = SCHEMES[schemeIdx];
                            SchemeData& ws = g_ccDlg.workingScheme;
                            ws.windowBg = src.windowBg;
                            ws.boardBg = src.boardBg;
                            ws.emptyColor = src.emptyColor;
                            ws.scoreBoxColor = src.scoreBoxColor;
                            ws.textLight = src.textLight;
                            ws.textDark = src.textDark;
                            ws.lightTextFromValue = src.lightTextFromValue;
                            for (int k = 0; k <= MAX_POWER; ++k)
                                ws.tileColors[k] = src.tileColors[k];
                        }
                        handled = true;
                    }
                    // top custom list: delete button first, otherwise click to load into the working area
                    if (!handled && g_ccDlg.customItemW > 0 &&
                        pos.x >= g_ccDlg.customItemX && pos.x <= g_ccDlg.customItemX + g_ccDlg.customItemW &&
                        pos.y >= g_ccDlg.customItemY && pos.y <= g_ccDlg.customItemY + g_ccDlg.customListH)
                    {
                        int relY = (int)(pos.y - g_ccDlg.customItemY);
                        int vis = (g_ccDlg.customItemH > 0) ? (int)(relY / g_ccDlg.customItemH) : 0;
                        int idx = g_ccDlg.customScroll + vis;
                        if (idx >= 0 && idx < MAX_CUSTOM_PRESETS)
                        {
                            // delete button first
                            if (g_ccDlg.presets[idx].used &&
                                g_ccDlg.customDelW[idx] > 0 &&
                                pos.x >= g_ccDlg.customDelX[idx] && pos.x <= g_ccDlg.customDelX[idx] + g_ccDlg.customDelW[idx] &&
                                pos.y >= g_ccDlg.customDelY[idx] && pos.y <= g_ccDlg.customDelY[idx] + g_ccDlg.customDelH[idx])
                            {
                                g_ccDlg.presets[idx].used = false;
                                ccSavePresetsToFile();
                            }
                            else if (g_ccDlg.presets[idx].used)
                            {
                                // record the selection (yellow highlight on click, kept after apply)
                                g_ccDlg.selCustomIdx = idx;
                                g_ccDlg.selPresetIdx = -1;
                                // save slot -> workingScheme
                                const CustomPreset& p = g_ccDlg.presets[idx];
                                SchemeData& ws = g_ccDlg.workingScheme;
                                std::memcpy(ws.tileColors, p.tileColors, sizeof(ws.tileColors));
                                ws.emptyColor = p.emptyColor;
                                ws.windowBg = p.windowBg;
                                ws.boardBg = p.boardBg;
                                ws.scoreBoxColor = p.scoreBoxColor;
                                ws.textLight = p.textLight;
                                ws.textDark = p.textDark;
                                ws.lightTextFromValue = p.lightTextFromValue;
                            }
                        }
                        handled = true;
                    }
                    // color list click: pick the edit target
                    if (!handled &&
                        pos.x >= g_ccDlg.listAreaX && pos.x <= g_ccDlg.listAreaX + g_ccDlg.listAreaW &&
                        pos.y >= g_ccDlg.listAreaY && pos.y <= g_ccDlg.listAreaY + g_ccDlg.listAreaH)
                    {
                        int relY = (int)(pos.y - g_ccDlg.listAreaY);
                        int idx = g_ccDlg.scrollPos + (int)(relY / g_ccDlg.itemH);
                        if (idx >= 0 && idx < CC_TOTAL_ITEMS)
                        {
                            int target = ccIndexToTarget(idx);
                            if (target != CEDIT_NONE)
                                g_ccDlg.editTarget = target;
                        }
                        handled = true;
                    }
                    // spinner arrow click: first auto-select the hovered item as the edit target, then adjust the value
                    if (!handled)
                    {
                        // no edit target selected yet -> try auto-selecting
                        if (g_ccDlg.editTarget == CEDIT_NONE)
                        {
                            int tgt = CEDIT_NONE;
                            if (g_ccDlg.hoverIdx >= 0)
                                tgt = ccIndexToTarget(g_ccDlg.hoverIdx);
                            if (tgt == CEDIT_NONE)
                                tgt = ccIndexToTarget(g_ccDlg.scrollPos);
                            if (tgt != CEDIT_NONE) g_ccDlg.editTarget = tgt;
                        }

                        // Focus-loss commit: apply current buffered input before editing
                        if (g_ccDlg.rgbEditing != -1)
                        {
                            sf::Color* oldCp = ccGetColor(g_ccDlg.editTarget);
                            if (oldCp && !g_ccDlg.rgbInputStr.empty())
                            {
                                int newVal = std::atoi(g_ccDlg.rgbInputStr.c_str());
                                if (newVal < 0 || newVal > 255) newVal = g_ccDlg.rgbLastValid;   // out of range -> last valid value
                                if (g_ccDlg.rgbEditing == 0) oldCp->r = (sf::Uint8)newVal;
                                else if (g_ccDlg.rgbEditing == 1) oldCp->g = (sf::Uint8)newVal;
                                else oldCp->b = (sf::Uint8)newVal;
                            }
                            g_ccDlg.rgbEditing = -1;
                        }

                        // cp declared outside so the arrow clicks and the input-box click share it
                        sf::Color* cp = ccGetColor(g_ccDlg.editTarget);
                        // step: 10 while Shift is held, otherwise 1
                        int step = (sf::Keyboard::isKeyPressed(sf::Keyboard::LShift) ||
                                    sf::Keyboard::isKeyPressed(sf::Keyboard::RShift)) ? 10 : 1;

                        // detect the left/right arrow clicks of the three spinners
                        for (int i = 0; i < 3 && !handled; ++i)
                        {
                            // left arrow: decrease
                            if (g_ccDlg.spinLeftW[i] > 0 &&
                                pos.x >= g_ccDlg.spinLeftX[i] && pos.x <= g_ccDlg.spinLeftX[i] + g_ccDlg.spinLeftW[i] &&
                                pos.y >= g_ccDlg.spinLeftY[i] && pos.y <= g_ccDlg.spinLeftY[i] + g_ccDlg.spinLeftH[i])
                            {
                                if (cp)
                                {
                                    int v;
                                    if (i == 0)      { v = cp->r; v -= step; if (v < 0) v = 0; cp->r = (sf::Uint8)v; }
                                    else if (i == 1) { v = cp->g; v -= step; if (v < 0) v = 0; cp->g = (sf::Uint8)v; }
                                    else             { v = cp->b; v -= step; if (v < 0) v = 0; cp->b = (sf::Uint8)v; }
                                }
                                handled = true;
                            }
                            // right arrow: increase
                            else if (g_ccDlg.spinRightW[i] > 0 &&
                                pos.x >= g_ccDlg.spinRightX[i] && pos.x <= g_ccDlg.spinRightX[i] + g_ccDlg.spinRightW[i] &&
                                pos.y >= g_ccDlg.spinRightY[i] && pos.y <= g_ccDlg.spinRightY[i] + g_ccDlg.spinRightH[i])
                            {
                                if (cp)
                                {
                                    int v;
                                    if (i == 0)      { v = cp->r; v += step; if (v > 255) v = 255; cp->r = (sf::Uint8)v; }
                                    else if (i == 1) { v = cp->g; v += step; if (v > 255) v = 255; cp->g = (sf::Uint8)v; }
                                    else             { v = cp->b; v += step; if (v > 255) v = 255; cp->b = (sf::Uint8)v; }
                                }
                                handled = true;
                            }
                        }

                        // input-box click: enter edit mode
                        if (!handled)
                        {
                            if (cp && g_ccDlg.rValW > 0 && pos.x >= g_ccDlg.rValX && pos.x <= g_ccDlg.rValX + g_ccDlg.rValW &&
                                pos.y >= g_ccDlg.rValY && pos.y <= g_ccDlg.rValY + g_ccDlg.rValH)
                            {
                                g_ccDlg.rgbEditing = 0;
                                int v = cp->r;
                                g_ccDlg.rgbInputStr = std::to_string(v);
                                g_ccDlg.rgbLastValid = v;
                                g_ccDlg.rgbCursorClock.restart();
                                handled = true;
                            }
                            else if (cp && g_ccDlg.gValW > 0 && pos.x >= g_ccDlg.gValX && pos.x <= g_ccDlg.gValX + g_ccDlg.gValW &&
                                pos.y >= g_ccDlg.gValY && pos.y <= g_ccDlg.gValY + g_ccDlg.gValH)
                            {
                                g_ccDlg.rgbEditing = 1;
                                int v = cp->g;
                                g_ccDlg.rgbInputStr = std::to_string(v);
                                g_ccDlg.rgbLastValid = v;
                                g_ccDlg.rgbCursorClock.restart();
                                handled = true;
                            }
                            else if (cp && g_ccDlg.bValW > 0 && pos.x >= g_ccDlg.bValX && pos.x <= g_ccDlg.bValX + g_ccDlg.bValW &&
                                pos.y >= g_ccDlg.bValY && pos.y <= g_ccDlg.bValY + g_ccDlg.bValH)
                            {
                                g_ccDlg.rgbEditing = 2;
                                int v = cp->b;
                                g_ccDlg.rgbInputStr = std::to_string(v);
                                g_ccDlg.rgbLastValid = v;
                                g_ccDlg.rgbCursorClock.restart();
                                handled = true;
                            }
                        }
                    }
                    // preview color-picker click: pick the color directly + allow dragging
                    if (!handled)
                    {
                        // Focus-loss commit before interacting with color picker
                        if (g_ccDlg.rgbEditing != -1)
                        {
                            sf::Color* oldCp = ccGetColor(g_ccDlg.editTarget);
                            if (oldCp && !g_ccDlg.rgbInputStr.empty())
                            {
                                int newVal = std::atoi(g_ccDlg.rgbInputStr.c_str());
                                if (newVal < 0 || newVal > 255) newVal = g_ccDlg.rgbLastValid;   // out of range -> last valid value
                                if (g_ccDlg.rgbEditing == 0) oldCp->r = (sf::Uint8)newVal;
                                else if (g_ccDlg.rgbEditing == 1) oldCp->g = (sf::Uint8)newVal;
                                else oldCp->b = (sf::Uint8)newVal;
                            }
                            g_ccDlg.rgbEditing = -1;
                        }

                        // v6.1: hit area uses pickerH for the Y range (no longer a square)
                        if (pos.x >= g_ccDlg.previewX && pos.x <= g_ccDlg.previewX + g_ccDlg.previewSize &&
                            pos.y >= g_ccDlg.previewY && pos.y <= g_ccDlg.previewY + g_ccDlg.pickerH)
                        {
                            // ensure an edit target
                            if (g_ccDlg.editTarget == CEDIT_NONE)
                            {
                                int tgt = CEDIT_NONE;
                                if (g_ccDlg.hoverIdx >= 0)
                                    tgt = ccIndexToTarget(g_ccDlg.hoverIdx);
                                if (tgt == CEDIT_NONE)
                                    tgt = ccIndexToTarget(g_ccDlg.scrollPos);
                                if (tgt != CEDIT_NONE) g_ccDlg.editTarget = tgt;
                            }
                            sf::Color* cp = ccGetColor(g_ccDlg.editTarget);
                            if (cp)
                            {
                                float lx = (float)(pos.x - g_ccDlg.previewX);
                                float ly = (float)(pos.y - g_ccDlg.previewY);
                                float px = lx / g_ccDlg.previewSize; if (px < 0) px = 0; if (px > 1) px = 1;
                                // v6.1: Y normalized by pickerH
                                float py = ly / g_ccDlg.pickerH; if (py < 0) py = 0; if (py > 1) py = 1;
                                float hue = px * 360.f;
                                float s, v;
                                if (py <= 0.5f)
                                {
                                    // upper half: S: 0->1, V=1.0
                                    s = py / 0.5f;
                                    v = 1.0f;
                                }
                                else
                                {
                                    // lower half: S=1.0, V: 1->0
                                    s = 1.0f;
                                    v = 1.0f - (py - 0.5f) / 0.5f;
                                }
                                sf::Color col = hsvToRgb(hue, s, v);
                                sf::Uint8 r = col.r, g = col.g, b = col.b;
                                cp->r = r; cp->g = g; cp->b = b;
                                g_ccDlg.draggingPicker = true;
                                g_ccDlg.hoverPickerActive = true;
                                g_ccDlg.hoverPreviewColor = sf::Color(r, g, b);
                                g_ccDlg.hoverPickerX = (float)pos.x;
                                g_ccDlg.hoverPickerY = (float)pos.y;
                                // if currently editing a component, update input buffer
                                if (g_ccDlg.rgbEditing != -1)
                                {
                                    int val = (g_ccDlg.rgbEditing == 0) ? cp->r : (g_ccDlg.rgbEditing == 1) ? cp->g : cp->b;
                                    g_ccDlg.rgbInputStr = std::to_string(val);
                                }
                            }
                            handled = true;
                        }
                    }

                    // click outside to close
                    // Fallthrough: click inside dialog but not on any input/spinner → deactivate editing
                    if (g_ccDlg.rgbEditing != -1 && !handled)
                    {
                        // Check if click is NOT on any spinner arrow or input box
                        bool onSpinnerOrInput = false;
                        for (int i = 0; i < 3; ++i)
                        {
                            if (pos.x >= g_ccDlg.spinLeftX[i] && pos.x <= g_ccDlg.spinLeftX[i] + g_ccDlg.spinLeftW[i] &&
                                pos.y >= g_ccDlg.spinLeftY[i] && pos.y <= g_ccDlg.spinLeftY[i] + g_ccDlg.spinLeftH[i])
                                onSpinnerOrInput = true;
                            if (pos.x >= g_ccDlg.spinRightX[i] && pos.x <= g_ccDlg.spinRightX[i] + g_ccDlg.spinRightW[i] &&
                                pos.y >= g_ccDlg.spinRightY[i] && pos.y <= g_ccDlg.spinRightY[i] + g_ccDlg.spinRightH[i])
                                onSpinnerOrInput = true;
                        }
                        if (!onSpinnerOrInput &&
                            !((pos.x >= g_ccDlg.rValX && pos.x <= g_ccDlg.rValX + g_ccDlg.rValW &&
                               pos.y >= g_ccDlg.rValY && pos.y <= g_ccDlg.rValY + g_ccDlg.rValH) ||
                              (pos.x >= g_ccDlg.gValX && pos.x <= g_ccDlg.gValX + g_ccDlg.gValW &&
                               pos.y >= g_ccDlg.gValY && pos.y <= g_ccDlg.gValY + g_ccDlg.gValH) ||
                              (pos.x >= g_ccDlg.bValX && pos.x <= g_ccDlg.bValX + g_ccDlg.bValW &&
                               pos.y >= g_ccDlg.bValY && pos.y <= g_ccDlg.bValY + g_ccDlg.bValH)))
                        {
                            g_ccDlg.rgbEditing = -1;
                        }
                    }

                    if (!handled)
                    {
                        if (pos.x < g_ccDlg.dlgX || pos.x > g_ccDlg.dlgX + g_ccDlg.dlgW ||
                            pos.y < g_ccDlg.dlgY || pos.y > g_ccDlg.dlgY + g_ccDlg.dlgH)
                        {
                            g_ccDlg.open = false;
                            ccSavePresetsToFile();
                            game.saveToFile(SAVE_FILE);
                        }
                    }
                    continue;
                }

                if (menuOpen)
                {
                    float menuW, menuH, menuX, menuY, btnH, btnGap, btnStartY;
                    computeMenuLayout(winW, winH, menuW, menuH, menuX, menuY, btnH, btnGap, btnStartY);

                    // color scheme button (centered, between the "settings" title and "board size")
                    {
                        float titleBottom = menuY + menuH * 0.10f;
                        float sizeLabelY = btnStartY + 2 * btnH + 3 * btnGap;
                        float btnGroupH = 3 * btnH + 2 * btnGap;
                        float btnGroupTop = (titleBottom + sizeLabelY) / 2.f - btnGroupH / 2.f;
                        float colorBtnW = menuW * 0.55f;
                        float colorBtnH = btnH;
                        float colorBtnX = menuX + (menuW - colorBtnW) / 2.f;
                        float colorBtnY = btnGroupTop;
                        if (pos.x >= colorBtnX && pos.x <= colorBtnX + colorBtnW &&
                            pos.y >= colorBtnY && pos.y <= colorBtnY + colorBtnH)
                        {
                            g_ccDlg.open = true;
                            g_ccDlg.scrollPos = 0;
                            g_ccDlg.presetScroll = 0;
                            g_ccDlg.customScroll = 0;
                            g_ccDlg.editTarget = CEDIT_NONE;
                            g_ccDlg.hoverIdx = -1;
                            g_ccDlg.selPresetIdx = -1;
                            g_ccDlg.selCustomIdx = -1;
                            g_ccDlg.needScrollToCurrent = true;   // locate the current theme after opening
                            g_ccDlg.hoverPickerActive = false;
                            g_ccDlg.draggingPicker = false;
                            g_ccDlg.rgbEditing = -1;
                            g_ccDlg.brightness = 255;
                            g_ccDlg.draggingBright = false;
                            // if the current theme is not Custom, sync to Custom first
                            if (game.getColorScheme() != CUSTOM_SCHEME_IDX)
                            {
                                const SchemeData& cur = SCHEMES[game.getColorScheme()];
                                SchemeData& custom = SCHEMES[CUSTOM_SCHEME_IDX];
                                custom.windowBg = cur.windowBg;
                                custom.boardBg = cur.boardBg;
                                custom.emptyColor = cur.emptyColor;
                                custom.scoreBoxColor = cur.scoreBoxColor;
                                custom.textLight = cur.textLight;
                                custom.textDark = cur.textDark;
                                custom.lightTextFromValue = cur.lightTextFromValue;
                                for (int k = 0; k <= MAX_POWER; ++k)
                                    custom.tileColors[k] = cur.tileColors[k];
                            }
                            // sync SCHEMES[CUSTOM_SCHEME_IDX] -> workingScheme
                            const SchemeData& src = SCHEMES[CUSTOM_SCHEME_IDX];
                            SchemeData& dst = g_ccDlg.workingScheme;
                            dst.windowBg = src.windowBg;
                            dst.boardBg = src.boardBg;
                            dst.emptyColor = src.emptyColor;
                            dst.scoreBoxColor = src.scoreBoxColor;
                            dst.textLight = src.textLight;
                            dst.textDark = src.textDark;
                            dst.lightTextFromValue = src.lightTextFromValue;
                            for (int k = 0; k <= MAX_POWER; ++k)
                                dst.tileColors[k] = src.tileColors[k];
                            menuOpen = false;
                            continue;  // exit click handling immediately, avoiding interference from later buttons
                        }
                    }

                    // challenge 2048 button (centered, below the color schemes)
                    {
                        float titleBottom = menuY + menuH * 0.10f;
                        float sizeLabelY = btnStartY + 2 * btnH + 3 * btnGap;
                        float btnGroupH = 3 * btnH + 2 * btnGap;
                        float btnGroupTop = (titleBottom + sizeLabelY) / 2.f - btnGroupH / 2.f;
                        float chBtnW = menuW * 0.55f;
                        float chBtnH = btnH;
                        float chBtnX = menuX + (menuW - chBtnW) / 2.f;
                        float chBtnY = btnGroupTop + btnH + btnGap;
                        if (pos.x >= chBtnX && pos.x <= chBtnX + chBtnW &&
                            pos.y >= chBtnY && pos.y <= chBtnY + chBtnH)
                        {
                            if (g_practiceActive) continue;   // not clickable in practice mode
                            // exit playback/AI/history/timer (timer mode -> "save?" confirm)
                            if (g_pb.active) exitPlayback(game);
                            if (g_aiMode != AI_INACTIVE) { g_aiMode = AI_INACTIVE; g_aiSettingsOpen = false; g_showAIEnd = false; }
                            if (g_tm.active) tmRequestClose(window, game);
                            if (g_tm.active) continue;   // do not enter challenge before the confirm dialog is decided
                            chStart(game);
                            menuOpen = false;
                        }
                    }

                    // timer mode button (centered, below challenge 2048; click to enter timer mode)
                    {
                        float titleBottom = menuY + menuH * 0.10f;
                        float sizeLabelY = btnStartY + 2 * btnH + 3 * btnGap;
                        float btnGroupH = 3 * btnH + 2 * btnGap;
                        float btnGroupTop = (titleBottom + sizeLabelY) / 2.f - btnGroupH / 2.f;
                        float tmBtnW = menuW * 0.55f;
                        float tmBtnH = btnH;
                        float tmBtnX = menuX + (menuW - tmBtnW) / 2.f;
                        float tmBtnY = btnGroupTop + 2 * btnH + 2 * btnGap;
                        if (pos.x >= tmBtnX && pos.x <= tmBtnX + tmBtnW &&
                            pos.y >= tmBtnY && pos.y <= tmBtnY + tmBtnH)
                        {
                            if (g_practiceActive || (g_ch.active && !g_pb.active)) continue;   // not clickable in practice/challenge
                            // exit playback/AI/history
                            if (g_pb.active) exitPlayback(game);
                            if (g_aiMode != AI_INACTIVE) { g_aiMode = AI_INACTIVE; g_aiSettingsOpen = false; g_showAIEnd = false; }
                            if (g_tm.active) { tmRequestClose(window, game); menuOpen = false; }  // already active -> "save?" confirm
                            else if (tmResumeFromSave(window, game)) {
                                // full save exists -> restore last progress/time/count (timing starts on the first move)
                                menuOpen = false;
                            }
                            else { tmStart(window, game); menuOpen = false; }
                        }
                    }

                    // board-size buttons: 3x3 / 4x4 / 5x5 (layout matches drawing)
                    float sizeLabelY = btnStartY + 2 * btnH + 3 * btnGap;
                    float sizeBtnY = sizeLabelY + btnGap * 2;
                    float sizeBtnW = menuW * 0.26f;
                    float sizeBtnGap = menuW * 0.04f;
                    float sizeBtnStartX = menuX + menuW * 0.07f;
                    for (int i = 0; i < 3; ++i)
                    {
                        int sz = 3 + i;
                        float bx = sizeBtnStartX + i * (sizeBtnW + sizeBtnGap);
                        if (pos.x >= bx && pos.x <= bx + sizeBtnW &&
                            pos.y >= sizeBtnY && pos.y <= sizeBtnY + btnH)
                        {
                            if (sz != game.getBoardSize())
                                switchBoardSize(game, sz);
                            break;
                        }
                    }

                    // restart button (only clears the current board; other sizes keep their slot saves)
                    float restartBtnW = menuW * 0.4f;
                    float restartBtnX = menuX + (menuW - restartBtnW) / 2.f;
                    float restartBtnY = sizeBtnY + btnH + btnGap * 2;
                    if (pos.x >= restartBtnX && pos.x <= restartBtnX + restartBtnW &&
                        pos.y >= restartBtnY && pos.y <= restartBtnY + btnH)
                    {
                        restartCurrentBoard(game);
                    }

                    // AI operation + practice buttons (two keys on one row, same width as history)
                    float aiOpGap = menuW * 0.04f;
                    float aiOpBtnW = menuW * 0.4f;   // same width as the "history" button
                    float aiOpBtnX = menuX + (menuW - (2.f * aiOpBtnW + aiOpGap)) / 2.f;
                    float aiOpBtnY = restartBtnY + btnH + btnGap;
                    if (pos.x >= aiOpBtnX && pos.x <= aiOpBtnX + aiOpBtnW &&
                        pos.y >= aiOpBtnY && pos.y <= aiOpBtnY + btnH)
                    {
                        // timer mode active -> AI operation greyed out
                        if (g_tm.active) continue;
                        if (g_aiMode == AI_INACTIVE)
                        {
                            // 5x5 warning: the first click shows a resource-usage warning
                            if (game.getBoardSize() == 5 && !g_show5x5AiWarning)
                            {
                                g_show5x5AiWarning = true;
                                menuOpen = false;
                                break;  // do not start AI until the user confirms
                            }
                            g_aiMode = AI_PAUSED;
                            g_aiBoardSize = game.getBoardSize();
                            g_aiEngine.setBoardSize(g_aiBoardSize);
                            g_aiStepClock.restart();
                            g_showAIEnd = false;
                            g_aiCacheReset = 1;   // new game: clear the transposition table in the background to prevent degradation
                            aiResetSessionStats();   // new session: clear count/time stats
                            resetMilestoneState();   // new session: clear milestone records
                            menuOpen = false;
                        }
                    }
                    // practice button (click toggles practice mode in/out)
                    float practiceBtnX = aiOpBtnX + aiOpBtnW + aiOpGap;
                    if (pos.x >= practiceBtnX && pos.x <= practiceBtnX + aiOpBtnW &&
                        pos.y >= aiOpBtnY && pos.y <= aiOpBtnY + btnH)
                    {
                        // timer/challenge active -> prediction (practice) greyed out
                        if (g_tm.active || (g_ch.active && !g_pb.active)) continue;
                        if (g_practiceActive) exitPracticeMode(window);
                        else enterPracticeMode(window, game);
                        menuOpen = false;
                    }

                    // history button
                    float histBtnW = menuW * 0.4f;
                    float histBtnX = menuX + (menuW - histBtnW) / 2.f;
                    float histBtnY = aiOpBtnY + btnH + btnGap;
                    if (pos.x >= histBtnX && pos.x <= histBtnX + histBtnW &&
                        pos.y >= histBtnY && pos.y <= histBtnY + btnH)
                    {
                        if (g_pb.active) exitPlayback(game);   // exit playback before opening history
                        if (g_practiceActive && g_practiceImport) { g_practiceImport = false; g_practiceQualityReady = false; }
                        loadHistoryList();
                        g_historyDlg.open = true;
                        g_historyDlg.scrollPos = 0;
                        g_historyDlg.selectedIdx = -1;
                        g_historyDlg.hoverIdx = -1;
                        g_historyDlg.hoverDelIdx = -1;
                        g_historyDlg.hoverExpIdx = -1;
                        g_historyDlg.confirmDelete = false;
                        g_historyDlg.confirmDeleteIdx = -1;
                        menuOpen = false;
                    }

                    // language toggle button: click switches between Chinese/English
                    float langBtnW = menuW * 0.4f;
                    float langBtnX = menuX + (menuW - langBtnW) / 2.f;
                    float langBtnY = histBtnY + btnH + btnGap;
                    if (pos.x >= langBtnX && pos.x <= langBtnX + langBtnW &&
                        pos.y >= langBtnY && pos.y <= langBtnY + btnH)
                    {
                        g_language = (g_language == LANG_EN) ? LANG_ZH : LANG_EN;
                        applyLanguageFont();
                        font = sf::Font();
                        font.loadFromFile(g_currentFontPath);
                        saveLangIni();
                        markSaveDirty();
                        logMsg(std::string("Language -> ") + (g_language == LANG_ZH ? "ZH" : "EN") +
                               " font=" + g_currentFontPath);
                    }

                    // about button (opens the technical document)
                    float closeBtnW = menuW * 0.4f;
                    float closeBtnX = menuX + (menuW - closeBtnW) / 2.f;
                    float closeBtnY = menuY + menuH - btnH - btnGap * 1.0f;
                    if (pos.x >= closeBtnX && pos.x <= closeBtnX + closeBtnW &&
                        pos.y >= closeBtnY && pos.y <= closeBtnY + btnH)
                    {
                        menuOpen = false;
                        openAboutDocumentation();
                    }

                    // click outside to close
                    if (pos.x < menuX || pos.x > menuX + menuW ||
                        pos.y < menuY || pos.y > menuY + menuH)
                        menuOpen = false;
                }
                else
                {
                    // layout computation identical to drawGame (incl. playback/AI offsets), for the hit areas below
                    bool aiActive = (g_aiMode != AI_INACTIVE) && !g_pb.active;
                    const bool chActive = g_ch.active && !g_pb.active;
                    float boardPx = computeBoardPx(winW, winH, aiActive);
                    float topBarR = computeTopBarRatio();
                    float gapBtnR = computeGapBtnRatio();
                    float aiRowR = aiActive ? computeAiRowRatio() : 0.f;
                    bool showStatsRow = (aiActive && (g_aiShowMoves || g_aiShowTime)) || chActive || g_tm.active;
                    float statsRowR = showStatsRow ? computeAiStatsRowRatio() : 0.f;
                    if (g_tm.active && statsRowR > 0.f) statsRowR *= 0.6f;   // timer-mode compact stats row (0.6x), matches drawing
                    float pbTopR   = g_pb.active ? computePbTopRowRatio() : 0.f;
                    float pbBelowR = g_pb.active ? computePbBelowRowRatio() : 0.f;
                    float gapAfterBoard = (aiActive || g_pb.active) ? gapBtnR : 0.f;
                    float totalHRatio = pbTopR + gapBtnR + statsRowR + gapBtnR + topBarR + gapBtnR + 1.f + gapAfterBoard
                                      + aiRowR + pbBelowR;   // the bottom-most element of each mode is the board / AI keys / playback keys
                    float totalContentH = boardPx * totalHRatio;
                    // board horizontal position identical to drawGame (practice: board+panel whole 1:2.15 centered; timer/AI milestone whole centered)
                    float boardX;
                    {
                        const float pad = boardPx * LAYOUT_PAD_RATIO;
                        if (g_practiceActive)
                        {
                            float wholeW = boardPx * PRACTICE_WHOLE_R;
                            boardX = ((float)winW - wholeW) / 2.f;
                            if (boardX < pad) boardX = pad;
                        }
                        else if (milestoneVisible())
                        {
                            MilestoneGeo mg = computeMilestoneGeo(winW, winH);
                            boardX = ((float)winW - mg.wholeW) / 2.f;
                            if (boardX < pad) boardX = pad;
                        }
                        else boardX = (winW - boardPx) / 2.f;
                    }
                    float offsetY = ((float)winH - totalContentH) / 2.f;
                    if (offsetY < boardX) offsetY = boardX;
                    if (offsetY + totalContentH > (float)winH) offsetY = ((float)winH - totalContentH) / 2.f;
                    TopBarLayout L = computeTopBarLayout(winW, winH, boardPx, offsetY);
                    // playback top info row offset identical to drawGame, so top-bar hit areas match drawing
                    if (g_pb.active)
                    {
                        float shift = pbBtnRatio() * 0.75f * boardPx + L.gapBtn;
                        L.topBarY   += shift;
                        L.leftBoxY  += shift;
                        L.scoreBoxY += shift;
                        L.rightBoxY += shift;
                    }
                    // stats row offset identical to drawGame, so top-bar hit areas match drawing
                    if (showStatsRow)
                    {
                        float statH = g_tm.active ? L.scoreBoxH * 0.6f : L.scoreBoxH;
                        float shift = statH + L.gapBtn;
                        L.topBarY   += shift;
                        L.leftBoxY  += shift;
                        L.scoreBoxY += shift;
                        L.rightBoxY += shift;
                    }
                    float boardY = L.topBarY + L.topBarH + L.gapBtn;

                    // history playback mode: handle the playback control area / playback settings dialog
                    if (g_pb.active)
                    {
                        auto inRect = [&](const sf::FloatRect& r) {
                            return pos.x >= r.left && pos.x <= r.left + r.width &&
                                   pos.y >= r.top && pos.y <= r.top + r.height;
                        };
                        // playback settings dialog open: only its input is handled
                        if (g_pb.settingsOpen)
                        {
                            if (inRect(g_pb.btnOrig))
                            {
                                g_pb.stepMode = 0;
                                g_pb.sliderDrag = -1;
                                g_pb.appliedStep = -1;   // re-locate after switching the step mode
                                savePbSettings();
                                continue;
                            }
                            if (inRect(g_pb.btnFixed))
                            {
                                g_pb.stepMode = 1;
                                g_pb.sliderDrag = -1;
                                g_pb.appliedStep = -1;
                                savePbSettings();
                                continue;
                            }
                            if (g_pb.stepMode == 0 && inRect(g_pb.origTrack))
                            {
                                g_pb.sliderDrag = 0;
                                pbApplySlider(0, (float)pos.x);
                                continue;
                            }
                            if (g_pb.stepMode == 1 && inRect(g_pb.fixedTrack))
                            {
                                g_pb.sliderDrag = 1;
                                pbApplySlider(1, (float)pos.x);
                                continue;
                            }
                            // click outside the dialog: close the settings dialog
                            if (pos.x < g_pb.settingsRect.left || pos.x > g_pb.settingsRect.left + g_pb.settingsRect.width ||
                                pos.y < g_pb.settingsRect.top  || pos.y > g_pb.settingsRect.top  + g_pb.settingsRect.height)
                            {
                                g_pb.settingsOpen = false;
                                g_pb.sliderDrag = -1;
                                continue;
                            }
                            continue;
                        }

                        // playback settings
                        if (inRect(g_pb.btnSettings))
                        {
                            g_pb.settingsOpen = !g_pb.settingsOpen;
                            g_pb.sliderDrag = -1;
                            continue;
                        }
                        // play/pause toggle (same style as the AI switch, centered in the time row)
                        if (inRect(g_pb.btnPlay))
                        {
                            const HistoryEntry* entry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                                ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                            if (entry && entry->moveCount > 0)
                            {
                                if (g_pb.playing)
                                {
                                    g_pb.playing = false;
                                }
                                else
                                {
                                    if (g_pb.playMs >= pbTotalMs(*entry) - 1.f)  // finished -> replay from the start
                                    {
                                        g_pb.playMs = 0.f;
                                        g_pb.appliedStep = -1;
                                        g_pb.curFrame = 0;
                                        g_pbGame.setBoardFrame(entry->boardSize, entry->colorScheme,
                                                               entry->frameScores[0], entry->frames[0]);
                                        g_pbGame.finishAnimation();
                                    }
                                    g_pb.playing = true;
                                }
                            }
                            continue;
                        }
                        // cancel: exit playback (practice imported history restores the pre-import board)
                        if (inRect(g_pb.btnCancel))
                        {
                            exitPlayback(game);
                            if (g_practiceActive && g_practiceImport)
                            {
                                game.setBoardDirect(g_practicePrevBoard, g_practicePrevScore);
                                g_practiceImport = false;
                                g_practiceQualityReady = false;
                                requestPracticePrediction(game);
                            }
                            continue;
                        }
                        // track drag (locate by time; drag to any moment)
                        if (inRect(g_pb.track))
                        {
                            g_pb.trackDrag = true;
                            const HistoryEntry* entry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                                ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                            if (entry && entry->moveCount > 0 && g_pb.track.width > 0.f)
                            {
                                float t = (float)(pos.x - g_pb.track.left) / g_pb.track.width;
                                if (t < 0.f) t = 0.f;
                                if (t > 1.f) t = 1.f;
                                pbSettleAtMs(*entry, t * pbTotalMs(*entry));
                                // practice imported history: after jumping, sync the analysis panel (predict that frame)
                                if (g_practiceActive && g_practiceImport)
                                {
                                    int f = g_pb.curFrame;
                                    if (f >= 0 && f < (int)entry->frames.size())
                                    {
                                        int sz = entry->boardSize;
                                        std::vector<std::vector<int>> bm(sz, std::vector<int>(sz));
                                        const auto& fr = entry->frames[f];
                                        for (int i = 0; i < sz; ++i)
                                            for (int j = 0; j < sz; ++j)
                                                bm[i][j] = fr[i * sz + j];
                                        requestPracticePredictionFor(bm);
                                    }
                                }
                            }
                            continue;
                        }
                        // practice imported history: click a quality marker -> jump to that step and analyze in the panel
                        if (g_practiceActive && g_practiceImport)
                        {
                            const HistoryEntry* qEntry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                                ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                            std::vector<int> qCopy;
                            {
                                std::lock_guard<std::mutex> lk(g_practiceQualityMutex);
                                if (g_practiceQualityReady && (int)g_practiceQuality.size() == (qEntry ? qEntry->moveCount : 0))
                                    qCopy = g_practiceQuality;
                            }
                            if (qEntry && qEntry->moveCount > 0 && (int)qCopy.size() == qEntry->moveCount)
                            {
                                // four-color rect geometry matching drawing (evenly divided; if not drawn yet use the same formula)
                                const int n = qEntry->moveCount;
                                float segW = boardPx / (float)n;
                                float mkHalfH = g_pb.mkHalfH;
                                float mkCY    = g_pb.mkCenterY;
                                if (mkHalfH <= 0.f || mkCY <= 0.f)
                                {
                                    float bH = (boardPx - 2.f * L.gapBtn) / (AI_BTN_W * 3.f);
                                    float pH = bH * 1.4f;
                                    float tH = std::max(2.f, pH * 0.12f);
                                    float tPad = std::max(4.f, pH * 0.12f);
                                    mkHalfH = tH * 2.8f * (2.f / 3.f) * (3.f / 5.f) / 2.f;
                                    mkCY    = g_pb.track.top + tPad + tH / 2.f;
                                }
                                for (int k = 0; k < n; ++k)
                                {
                                    float segX = boardX + (float)k * segW;
                                    if (pos.x >= segX && pos.x <= segX + segW &&
                                        pos.y >= mkCY - mkHalfH && pos.y <= mkCY + mkHalfH)
                                    {
                                        int f = k + 1;
                                        g_pb.playing = false;
                                        g_pb.curFrame = f;
                                        g_pb.appliedStep = -1;
                                        g_pb.playMs = (g_pb.stepMode == 1) ? (float)f * g_pb.fixedMs
                                                                            : (float)qEntry->frameTimesMs[f];
                                        g_pbGame.setBoardFrame(qEntry->boardSize, qEntry->colorScheme,
                                                               qEntry->frameScores[f], qEntry->frames[f]);
                                        g_pbGame.finishAnimation();
                                        int sz = qEntry->boardSize;
                                        std::vector<std::vector<int>> bm(sz, std::vector<int>(sz));
                                        const auto& fr = qEntry->frames[f];
                                        for (int i = 0; i < sz; ++i)
                                            for (int j = 0; j < sz; ++j)
                                                bm[i][j] = fr[i * sz + j];
                                        requestPracticePredictionFor(bm);
                                        break;
                                    }
                                }
                            }
                            // after hitting a quality rect, continue with the other playback interactions; otherwise continue
                        }
                        // history playback: top-bar left key = previous step, right key = next step (auto-pause, frame by frame)
                        const HistoryEntry* pbEntry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                            ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                        if (pos.x >= L.leftBoxX && pos.x <= L.leftBoxX + L.leftBoxW &&
                            pos.y >= L.leftBoxY && pos.y <= L.leftBoxY + L.scoreBoxH)
                        {
                            if (pbEntry) pbStepBy(*pbEntry, -1);
                            if (g_practiceActive && g_practiceImport && pbEntry)
                            {
                                int f = g_pb.curFrame;
                                if (f >= 0 && f < (int)pbEntry->frames.size())
                                {
                                    int sz = pbEntry->boardSize;
                                    std::vector<std::vector<int>> bm(sz, std::vector<int>(sz));
                                    const auto& fr = pbEntry->frames[f];
                                    for (int i = 0; i < sz; ++i)
                                        for (int j = 0; j < sz; ++j)
                                            bm[i][j] = fr[i * sz + j];
                                    requestPracticePredictionFor(bm);
                                }
                            }
                            continue;
                        }
                        if (pos.x >= L.rightBoxX && pos.x <= L.rightBoxX + L.rightBoxW &&
                            pos.y >= L.rightBoxY && pos.y <= L.rightBoxY + L.scoreBoxH)
                        {
                            if (pbEntry) pbStepBy(*pbEntry, +1);
                            if (g_practiceActive && g_practiceImport && pbEntry)
                            {
                                int f = g_pb.curFrame;
                                if (f >= 0 && f < (int)pbEntry->frames.size())
                                {
                                    int sz = pbEntry->boardSize;
                                    std::vector<std::vector<int>> bm(sz, std::vector<int>(sz));
                                    const auto& fr = pbEntry->frames[f];
                                    for (int i = 0; i < sz; ++i)
                                        for (int j = 0; j < sz; ++j)
                                            bm[i][j] = fr[i * sz + j];
                                    requestPracticePredictionFor(bm);
                                }
                            }
                            continue;
                        }
                        // click anywhere else: do nothing (only "cancel" exits playback)
                        continue;
                    }

                    // history view mode: click anywhere to exit
                    if (game.isViewingHistory())
                    {
                        game.exitHistoryView();
                        continue;
                    }

                    // AI control buttons handled first (AI settings / play-pause / cancel)
                    if (g_aiMode != AI_INACTIVE)
                    {
                        auto inRect = [&](const sf::FloatRect& r) {
                            return pos.x >= r.left && pos.x <= r.left + r.width &&
                                   pos.y >= r.top && pos.y <= r.top + r.height;
                        };
                        // (1) AI settings button: open/close the settings dialog
                        if (inRect(g_aiBtnPause))
                        {
                            g_aiSettingsOpen = !g_aiSettingsOpen;
                            continue;
                        }
                        // (2) play/pause button: running -> pause; paused -> resume
                        if (inRect(g_aiBtnStart))
                        {
                            if (g_aiMode == AI_RUNNING)
                            {
                                g_aiMode = AI_PAUSED;
                                g_aiTiming = false;          // pause: stop timing
                                g_aiStepClock.restart();
                                // after pausing, manual play restores default (manual) animation speed; resuming AI uses AI speed
                                game.setAIModeAnimations(false);
                            }
                            else
                            {
                                // resume AI from pause (shared logic: RUNNING + prefetch + UI kept)
                                aiResumeRunning(game);
                            }
                            continue;
                        }
                        // (3) cancel button
                        if (inRect(g_aiBtnCancel))
                        {
                            // cancel: truly exit AI mode (UI disappears), score and tiles unchanged, session state cleaned
                            g_aiSettingsOpen = false;
                            g_aiTiming = false;           // stop timing (keep the last accumulated value on display)
                            game.setAIModeAnimations(false);  // restore the manual 150ms classic animation
                            // explicit exit: set INACTIVE (unlike the dead-end "keep UI" end), and wind down the future
                            g_aiMode = AI_INACTIVE;
                            s_aiConsumingPending = false;
                            if (g_prefetchFuture.valid()) {
                                auto st = g_prefetchFuture.wait_for(std::chrono::milliseconds(0));
                                if (st == std::future_status::ready) { try { (void)g_prefetchFuture.get(); } catch (...) {} }
                                g_prefetchFuture = std::future<PrefetchResult>();
                            }
                            continue;
                        }
                    }

                    // === AI settings dialog interaction: click outside to close / rate slider drag / tier select / toggles ===
                    if (g_aiSettingsOpen)
                    {
                        float popW = boardPx;
                        float popH = boardPx * 0.5f;
                        float popY = boardY + boardPx - popH;
                        bool insidePop = pos.x >= boardX && pos.x <= boardX + popW &&
                                         pos.y >= popY   && pos.y <= boardY + boardPx;
                        if (!insidePop)
                        {
                            g_aiSettingsOpen = false;
                            continue;
                        }
                        auto inRect = [&](const sf::FloatRect& t) {
                            return pos.x >= t.left && pos.x <= t.left + t.width &&
                                   pos.y >= t.top  && pos.y <= t.top  + t.height;
                        };
                        // (1) rate slider
                        if (g_aiRateTrack.width > 0.f && inRect(g_aiRateTrack))
                        {
                            g_aiRateDrag = true;
                            float span = g_aiRateKnobMaxCX - g_aiRateKnobMinCX;
                            float t = span > 0.f ? (pos.x - g_aiRateKnobMinCX) / span : 0.f;
                            if (t < 0.f) t = 0.f;
                            if (t > 1.f) t = 1.f;
                            aiApplyRate(AI_RATE_MIN + (int)std::lround(t * (AI_RATE_MAX - AI_RATE_MIN)));
                            if (g_aiMode == AI_RUNNING)
                                game.setAIAnimDuration(g_aiStepDelaySec);
                            continue;
                        }
                        // (2) intelligence tier: three small rects (tier 1/2/3 -> depth 3/5/8)
                        for (int i = 0; i < 3; ++i)
                        {
                            if (g_aiLevelBtn[i].width > 0.f && inRect(g_aiLevelBtn[i]))
                            {
                                if (i != g_aiLevel)
                                {
                                    g_aiLevel = i;
                                    g_aiDepth = g_aiLevelDepth[g_aiLevel];
                                    g_aiEngine.setDepth(g_aiDepth);   // tier -> ziap BFS budget
                                    saveAiSettings();   // save on change; restored on the next launch
                                }
                                break;
                            }
                        }
                        // (3) display toggles: show count / show time
                        if (g_aiShowMovesBtn.width > 0.f && inRect(g_aiShowMovesBtn))
                        {
                            g_aiShowMoves = !g_aiShowMoves;
                            saveAiSettings();
                            continue;
                        }
                        if (g_aiShowTimeBtn.width > 0.f && inRect(g_aiShowTimeBtn))
                        {
                            g_aiShowTime = !g_aiShowTime;
                            saveAiSettings();
                            continue;
                        }
                        // (4) AI uses undo
                        if (g_aiUseUndoBtn.width > 0.f && inRect(g_aiUseUndoBtn))
                        {
                            g_aiUseUndo = !g_aiUseUndo;
                            saveAiSettings();
                            continue;
                        }













                        // other areas in the dialog: only end dragging, no response
                        g_aiRateDrag = false;
                        continue;
                    }

                    // === practice-mode interaction ===
                    if (g_practiceActive)
                    {
                        auto inRectP = [&](const sf::FloatRect& t) {
                            return pos.x >= t.left && pos.x <= t.left + t.width &&
                                   pos.y >= t.top  && pos.y <= t.top  + t.height;
                        };
                        // build-position dialog (modal): value list / board / save / cancel
                        if (g_practiceBuildOpen)
                        {
                            computePracticeBuildLayout(winW, winH, game.getBoardSize(), boardPx);
                            const int maxPow = practiceBuildMaxPow(game.getBoardSize());
                            // value-list click (incl. clear)
                            if (pos.x >= g_practiceBuildPaletteX && pos.x <= g_practiceBuildPaletteX + g_practiceBuildPaletteW &&
                                pos.y >= g_practiceBuildPaletteY && pos.y <= g_practiceBuildPaletteY + g_practiceBuildPaletteH)
                            {
                                int vi = (int)((pos.y - g_practiceBuildPaletteY) / g_practiceBuildItemH);
                                int idx = g_practiceBuildScroll + vi;
                                if (idx >= 0 && idx <= maxPow)
                                    g_practiceBuildSelPow = idx;
                                continue;
                            }
                            // board click: place the currently selected value (fill uses no budget; over-budget not placed)
                            if (pos.x >= g_practiceBuildBoardX && pos.x <= g_practiceBuildBoardX + g_practiceBuildCell * game.getBoardSize() &&
                                pos.y >= g_practiceBuildBoardY && pos.y <= g_practiceBuildBoardY + g_practiceBuildCell * game.getBoardSize())
                            {
                                int sz = game.getBoardSize();
                                int c = (int)((pos.x - g_practiceBuildBoardX) / g_practiceBuildCell);
                                int r = (int)((pos.y - g_practiceBuildBoardY) / g_practiceBuildCell);
                                if (r >= 0 && r < sz && c >= 0 && c < sz)
                                {
                                    bool isClear = (g_practiceBuildSelPow == 0);
                                    // fill: increasing super-large values (FILL_BASE + counter*16), each new fill larger than the previous -> the AI can distinguish them
                                    int newVal = isClear ? 0 : (1 << g_practiceBuildSelPow);
                                    int oldVal = g_practiceBuildBoard[r][c];
                                    long long budget = practiceBuildBudget(practiceBuildMaxPow(sz));
                                    long long used = practiceBuildSum(g_practiceBuildBoard);
                                    if (isClear || used - oldVal + newVal <= budget)
                                        g_practiceBuildBoard[r][c] = newVal;
                                }
                                continue;
                            }
                            // save
                            if (inRectP(g_practiceBuildSave)) { savePracticeBuild(game); continue; }
                            // cancel
                            if (inRectP(g_practiceBuildCancel)) { g_practiceBuildOpen = false; continue; }
                            // click outside the dialog to close
                            if (!inRectP(g_practiceBuildRect)) { g_practiceBuildOpen = false; continue; }
                            continue;   // while the build dialog is open, other panel keys do not respond
                        }

                        // import history: open the history dialog
                        if (inRectP(g_practiceBtnImport))
                        {
                            loadHistoryList();
                            g_historyDlg.open = true;
                            g_historyDlg.scrollPos = 0;
                            g_historyDlg.selectedIdx = -1;
                            g_historyDlg.hoverIdx = -1;
                            g_historyDlg.hoverDelIdx = -1;
                            g_historyDlg.hoverExpIdx = -1;
                            g_historyDlg.confirmDelete = false;
                            g_historyDlg.confirmDeleteIdx = -1;
                            continue;
                        }
                        // build position: open the build dialog
                        if (inRectP(g_practiceBtnBuild))
                        {
                            openPracticeBuild(game);
                            continue;
                        }
                        // close: exit practice mode
                        if (inRectP(g_practiceBtnClose))
                        {
                            exitPracticeMode(window);
                            continue;
                        }
                        // board layout transforms: flip vertical / flip horizontal / CCW 90 / CW 90 (disabled in playback/build)
                        if (!g_pb.active && !g_practiceBuildOpen)
                        {
                            // hide 4-step prediction: on switch, mini-board numbers clear, next-best and refresh keys grey out; unhiding restores the prediction (state persisted)
                            if (inRectP(g_practiceHidePredBtn))
                            {
                                g_practicePredHidden = !g_practicePredHidden;
                                saveAiSettings();   // save on change, restored on the next launch
                                if (!g_practicePredHidden)
                                    requestPracticePrediction(game);
                                continue;
                            }
                            // refresh 4-step prediction: re-simulate with random 2/4 spawns (greyed when hidden)
                            if (!g_practicePredHidden && inRectP(g_practiceRefreshPredBtn))
                            {
                                requestPracticePrediction(game, true);
                                continue;
                            }
                            if (inRectP(g_practiceBtnFlipV))   { practiceTransformBoard(game, 0); continue; }
                            if (inRectP(g_practiceBtnFlipH))   { practiceTransformBoard(game, 1); continue; }
                            if (inRectP(g_practiceBtnRotCCW))  { practiceTransformBoard(game, 2); continue; }
                            if (inRectP(g_practiceBtnRotCW))   { practiceTransformBoard(game, 3); continue; }
                        }
                        // click panel blank: no handling (left to the board/top bar)
                    }

                    // === challenge project settings dialog interaction ===
                    if (chActive && g_ch.settingsOpen)
                    {
                        auto inRect2 = [&](const sf::FloatRect& t) {
                            return pos.x >= t.left && pos.x <= t.left + t.width &&
                                   pos.y >= t.top  && pos.y <= t.top  + t.height;
                        };
                        bool hit = false;
                        // confirm dialog open: handle confirm/cancel
                        if (g_ch.confirmSwitchProject >= 0)
                        {
                            if (inRect2(g_ch.confirmYes)) {
                                chSelectProject(game, g_ch.confirmSwitchProject);
                                g_ch.confirmSwitchProject = -1;
                                hit = true;
                            }
                            else if (inRect2(g_ch.confirmNo)) {
                                g_ch.confirmSwitchProject = -1;
                                hit = true;
                            }
                            else if (!inRect2(g_ch.settingsRect)) {
                                g_ch.confirmSwitchProject = -1;
                                hit = true;
                            }
                            if (hit) continue;
                        }
                        else
                        {
                            for (int i = 0; i < 6; ++i)
                                if (inRect2(g_ch.projBtn[i]))
                                {
                                    if (g_ch.started && i != g_ch.project)
                                    {
                                        // challenge already started, switching projects needs confirmation
                                        g_ch.confirmSwitchProject = i;
                                    }
                                    else
                                    {
                                        chSelectProject(game, i);
                                    }
                                    hit = true; break;
                                }
                            if (!hit && inRect2(g_ch.btnClose)) { g_ch.settingsOpen = false; hit = true; }
                            if (!hit && !inRect2(g_ch.settingsRect)) { g_ch.settingsOpen = false; hit = true; }
                        }
                        if (hit) continue;
                    }

                    // === challenge result dialog: click anywhere closes and exits the challenge ===
                    if (chActive && g_ch.finished)
                    {
                        chClose(game);
                        continue;
                    }

                    // === timer-mode result dialog (game over): click anywhere only closes the dialog, timer mode stays ===
                    // (the player may review the final score/layout/time on the kept interface; clicking the red score box
                    // starts the next game; real exit still goes through the menu "timer mode" -> "close" / confirm dialog)
                    if (g_tm.active && g_tm.finished && !g_tm.resultDismissed)
                    {
                        g_tm.resultDismissed = true;
                        continue;
                    }

                    if (pos.x >= L.leftBoxX && pos.x <= L.leftBoxX + L.leftBoxW &&
                        pos.y >= L.leftBoxY && pos.y <= L.leftBoxY + L.scoreBoxH)
                    {
                        if (chActive)
                        {
                            // challenge mode: left key = settings (opens the challenge project settings dialog)
                            if (!g_ch.finished) g_ch.settingsOpen = true;
                        }
                        else
                        {
                            // timer mode / normal mode: left key = open the system menu
                            if (g_pb.active) exitPlayback(game);   // exit playback before opening the menu
                            if (g_practiceActive && g_practiceImport) { g_practiceImport = false; g_practiceQualityReady = false; }
                            menuOpen = true;
                        }
                    }
                    else if (pos.x >= L.scoreBoxX && pos.x <= L.scoreBoxX + L.scoreBoxW &&
                             pos.y >= L.scoreBoxY && pos.y <= L.scoreBoxY + L.scoreBoxH)
                    {
                        // click when the score reaches the red frame (game over):
                        //   in an AI session (g_aiMode != INACTIVE) -> start a new game inside the AI control UI
                        //     (AI function keys kept; the user can click "start" to run a new game)
                        //   AI inactive (manual dead end) -> restart the game (original behavior)
                        // no red frame -> click does nothing
                        if (!game.canMove())
                        {
                            if (g_tm.active && g_tm.finished)
                            {
                                // timer mode: clicking the red score box starts the next game (timer mode kept; time/count/milestones reset, waits for the first move to re-time)
                                game.init();
                                tmResetState();
                                game.saveToFile(slotFileFor(game.getBoardSize()));
                                game.saveToFile(SAVE_FILE);
                                tmWriteFullSave(game);
                                saveTimerSettings();
                                continue;
                            }
                            if (g_aiMode != AI_INACTIVE)
                            {
                                // start a new game inside the AI control UI: clear the board, keep the AI function keys (paused)
                                game.init();
                                game.saveToFile(slotFileFor(game.getBoardSize()));
                                game.saveToFile(SAVE_FILE);
                                g_aiCacheReset = 1;
                                g_aiUndoConsecutive = 0;
                                g_aiUndoPending = false;
                                g_aiSkipDir = -1;  g_aiLastMoveDir = -1;
                                aiResetSessionStats();   // new game: count/time reset, no cross-game accumulation
                                g_aiStepClock.restart();
                                continue;
                            }
                            restartCurrentBoard(game);
                        }
                    }
                    else if (pos.x >= L.rightBoxX && pos.x <= L.rightBoxX + L.rightBoxW &&
                             pos.y >= L.rightBoxY && pos.y <= L.rightBoxY + L.scoreBoxH)
                    {
                        if (chActive)
                        {
                            // challenge mode: right key = close (mid-way close does not record history)
                            chClose(game);
                        }
                        else if (g_tm.active)
                        {
                            // timer mode: right key = close (with progress -> "save?" confirm)
                            tmRequestClose(window, game);
                        }
                        else if (g_aiMode != AI_RUNNING)
                        {
                            game.undo();
                            game.saveToFile(SAVE_FILE);
                            if (g_practiceActive) { g_practiceLastMoveDir = -1; g_lastMoveByAI = false; requestPracticePrediction(game); }
                        }
                    }
                    else
                    {
                        // manual slide forbidden while AI is running; operation forbidden after the challenge ends
                        if (g_aiMode != AI_RUNNING && !(chActive && g_ch.finished))
                        {
                            mouseStart = pos;
                            isDragging = true;
                        }
                    }
                }
            }

            // mouse slide (drag) events
            if (e.type == sf::Event::MouseButtonReleased && e.mouseButton.button == sf::Mouse::Left && isDragging)
            {
                sf::Vector2i mouseEnd = sf::Mouse::getPosition(window);
                int dx = mouseEnd.x - mouseStart.x;
                int dy = mouseEnd.y - mouseStart.y;

                if (abs(dx) > abs(dy) && abs(dx) > DRAG_THRESHOLD)
                {
                    if (game.move(dx > 0 ? 1 : -1, 0))
                    {
                        game.addRandomTile(); markSaveDirty(); chAfterUserMove(game); tmAfterUserMove(game);
                        if (g_practiceActive) { g_practiceLastMoveDir = (dx > 0) ? 1 : 0; g_lastMoveByAI = false; requestPracticePrediction(game); }
                    }
                }
                else if (abs(dy) > DRAG_THRESHOLD)
                {
                    if (game.move(0, dy > 0 ? 1 : -1))
                    {
                        game.addRandomTile(); markSaveDirty(); chAfterUserMove(game); tmAfterUserMove(game);
                        if (g_practiceActive) { g_practiceLastMoveDir = (dy > 0) ? 3 : 2; g_lastMoveByAI = false; requestPracticePrediction(game); }
                    }
                }
                isDragging = false;
            }

            // AI settings rate slider: release ends the drag
            if (e.type == sf::Event::MouseButtonReleased && e.mouseButton.button == sf::Mouse::Left)
            {
                g_aiRateDrag = false;
                g_pb.trackDrag = false;     // playback track drag ended
                g_pb.sliderDrag = -1;       // playback settings slider drag ended
            }
            // AI settings rate slider: drag update
            if (e.type == sf::Event::MouseMoved && g_aiRateDrag)
            {
                sf::Vector2i mpos(e.mouseMove.x, e.mouseMove.y);
                float span = g_aiRateKnobMaxCX - g_aiRateKnobMinCX;
                float t = span > 0.f ? (mpos.x - g_aiRateKnobMinCX) / span : 0.f;
                if (t < 0.f) t = 0.f;
                if (t > 1.f) t = 1.f;
                aiApplyRate(AI_RATE_MIN + (int)std::lround(t * (AI_RATE_MAX - AI_RATE_MIN)));
                if (g_aiMode == AI_RUNNING)
                    game.setAIAnimDuration(g_aiStepDelaySec);
            }

            // history playback track drag: follow the mouse to update the progress (locate by time)
            if (e.type == sf::Event::MouseMoved && g_pb.trackDrag && g_pb.active)
            {
                sf::Vector2i mpos(e.mouseMove.x, e.mouseMove.y);
                const HistoryEntry* entry = (g_pb.entryIdx >= 0 && g_pb.entryIdx < (int)g_historyDlg.entries.size())
                    ? &g_historyDlg.entries[g_pb.entryIdx] : nullptr;
                if (entry && entry->moveCount > 0 && g_pb.track.width > 0.f)
                {
                    float t = (float)(mpos.x - g_pb.track.left) / g_pb.track.width;
                    if (t < 0.f) t = 0.f;
                    if (t > 1.f) t = 1.f;
                    pbSettleAtMs(*entry, t * pbTotalMs(*entry));
                }
            }
            // playback settings slider drag: update multiplier/fixed step with the mouse
            if (e.type == sf::Event::MouseMoved && g_pb.sliderDrag >= 0 && g_pb.settingsOpen)
            {
                sf::Vector2i mpos(e.mouseMove.x, e.mouseMove.y);
                pbApplySlider(g_pb.sliderDrag, (float)mpos.x);
            }

            // custom color dialog: slider drag + list scrollbar/hover
            if (g_ccDlg.open)
            {
                if (e.type == sf::Event::MouseMoved)
                {
                    sf::Vector2i pos(e.mouseMove.x, e.mouseMove.y);
                    // hover
                    if (pos.x >= g_ccDlg.listAreaX && pos.x <= g_ccDlg.listAreaX + g_ccDlg.listAreaW &&
                        pos.y >= g_ccDlg.listAreaY && pos.y <= g_ccDlg.listAreaY + g_ccDlg.listAreaH)
                    {
                        int relY = (int)(pos.y - g_ccDlg.listAreaY);
                        int idx = g_ccDlg.scrollPos + (int)(relY / std::max(1.f, g_ccDlg.itemH));
                        if (idx >= 0 && idx < CC_TOTAL_ITEMS)
                            g_ccDlg.hoverIdx = idx;
                        else
                            g_ccDlg.hoverIdx = -1;
                    }
                    else
                    {
                        g_ccDlg.hoverIdx = -1;
                    }
                    // color-picker hover/drag (v6.1: Y range by pickerH)
                    if (pos.x >= g_ccDlg.previewX && pos.x <= g_ccDlg.previewX + g_ccDlg.previewSize &&
                        pos.y >= g_ccDlg.previewY && pos.y <= g_ccDlg.previewY + g_ccDlg.pickerH)
                    {
                        float lx = (float)(pos.x - g_ccDlg.previewX);
                        float ly = (float)(pos.y - g_ccDlg.previewY);
                        float px = lx / g_ccDlg.previewSize; if (px < 0) px = 0; if (px > 1) px = 1;
                        // v6.1: Y normalized by pickerH
                        float py = ly / g_ccDlg.pickerH; if (py < 0) py = 0; if (py > 1) py = 1;
                        float hue = px * 360.f;
                        float s, v;
                        if (py <= 0.5f)
                        {
                            // upper half: S: 0->1, V=1.0
                            s = py / 0.5f;
                            v = 1.0f;
                        }
                        else
                        {
                            // lower half: S=1.0, V: 1->0
                            s = 1.0f;
                            v = 1.0f - (py - 0.5f) / 0.5f;
                        }
                        sf::Color col = hsvToRgb(hue, s, v);
                        sf::Uint8 r = col.r, g = col.g, b = col.b;
                        g_ccDlg.hoverPreviewColor = sf::Color(r, g, b);
                        g_ccDlg.hoverPickerActive = true;
                        g_ccDlg.hoverPickerX = (float)pos.x;
                        g_ccDlg.hoverPickerY = (float)pos.y;

                        // while dragging the picker, apply the color to the edit target directly
                        if (g_ccDlg.draggingPicker && g_ccDlg.editTarget != CEDIT_NONE)
                        {
                            sf::Color* cp = ccGetColor(g_ccDlg.editTarget);
                            if (cp)
                            {
                                cp->r = r; cp->g = g; cp->b = b;
                            }
                        }
                    }
                    else
                    {
                        g_ccDlg.hoverPickerActive = false;
                    }
                    // slider drag removed (replaced by the spinner); keep the picker drag
                    if (g_ccDlg.editTarget != CEDIT_NONE && (g_ccDlg.draggingR || g_ccDlg.draggingG || g_ccDlg.draggingB))
                    {
                        // slider removed, reset the leftover flag
                        g_ccDlg.draggingR = false;
                        g_ccDlg.draggingG = false;
                        g_ccDlg.draggingB = false;
                    }
                }
                if (e.type == sf::Event::MouseWheelScrolled)
                {
                    float mx = (float)e.mouseWheelScroll.x;
                    float my = (float)e.mouseWheelScroll.y;
                    int delta = (int)e.mouseWheelScroll.delta;
                    // mouse over the top theme list -> scroll themes
                    if (g_ccDlg.presetItemW > 0 &&
                        mx >= g_ccDlg.presetItemX && mx <= g_ccDlg.presetItemX + g_ccDlg.presetItemW &&
                        my >= g_ccDlg.presetItemY && my <= g_ccDlg.presetItemY + g_ccDlg.presetListH)
                    {
                        int visibleCount = (int)(g_ccDlg.presetListH / std::max(1.f, g_ccDlg.presetItemH));
                        if (visibleCount > NUM_PRESET_SCHEMES) visibleCount = NUM_PRESET_SCHEMES;
                        int maxScroll = NUM_PRESET_SCHEMES - visibleCount;
                        if (maxScroll < 0) maxScroll = 0;
                        g_ccDlg.presetScroll -= delta;
                        if (g_ccDlg.presetScroll < 0) g_ccDlg.presetScroll = 0;
                        if (g_ccDlg.presetScroll > maxScroll) g_ccDlg.presetScroll = maxScroll;
                    }
                    // mouse over the top custom list -> scroll custom
                    else if (g_ccDlg.customItemW > 0 &&
                        mx >= g_ccDlg.customItemX && mx <= g_ccDlg.customItemX + g_ccDlg.customItemW &&
                        my >= g_ccDlg.customItemY && my <= g_ccDlg.customItemY + g_ccDlg.customListH)
                    {
                        int visibleCount = (int)(g_ccDlg.customListH / std::max(1.f, g_ccDlg.customItemH));
                        if (visibleCount > MAX_CUSTOM_PRESETS) visibleCount = MAX_CUSTOM_PRESETS;
                        int maxScroll = MAX_CUSTOM_PRESETS - visibleCount;
                        if (maxScroll < 0) maxScroll = 0;
                        g_ccDlg.customScroll -= delta;
                        if (g_ccDlg.customScroll < 0) g_ccDlg.customScroll = 0;
                        if (g_ccDlg.customScroll > maxScroll) g_ccDlg.customScroll = maxScroll;
                    }
                    else
                    {
                        int totalItems = CC_TOTAL_ITEMS;
                        int visibleCount = (int)(std::max(1.f, g_ccDlg.listAreaH) / std::max(1.f, g_ccDlg.itemH));
                        if (visibleCount > totalItems) visibleCount = totalItems;
                        int maxScroll = totalItems - visibleCount;
                        if (maxScroll < 0) maxScroll = 0;
                        g_ccDlg.scrollPos -= delta;
                        if (g_ccDlg.scrollPos < 0) g_ccDlg.scrollPos = 0;
                        if (g_ccDlg.scrollPos > maxScroll) g_ccDlg.scrollPos = maxScroll;
                    }
                }
                if (e.type == sf::Event::MouseButtonReleased && e.mouseButton.button == sf::Mouse::Left)
                {
                    g_ccDlg.draggingR = false;
                    g_ccDlg.draggingG = false;
                    g_ccDlg.draggingB = false;
                    g_ccDlg.draggingPicker = false;
                }

                // text input handling (SFML TextEntered carries all chars, incl. backspace)
                if (e.type == sf::Event::TextEntered && g_ccDlg.open && g_ccDlg.rgbEditing != -1)
                {
                    sf::Color* cp = ccGetColor(g_ccDlg.editTarget);
                    if (!cp) { g_ccDlg.rgbEditing = -1; continue; }

                    unsigned int ch = e.text.unicode;
                    bool changed = false;

                    if (ch == 8)  // Backspace (SFML sends unicode 8 via TextEntered)
                    {
                        if (!g_ccDlg.rgbInputStr.empty())
                        {
                            g_ccDlg.rgbInputStr.pop_back();
                            changed = true;
                        }
                    }
                    else if (ch >= '0' && ch <= '9')
                    {
                        if (g_ccDlg.rgbInputStr.size() < 3)
                        {
                            std::string test = g_ccDlg.rgbInputStr + (char)ch;
                            int val = std::atoi(test.c_str());
                            if (val <= 255)
                            {
                                g_ccDlg.rgbInputStr += (char)ch;
                                changed = true;
                            }
                        }
                    }

                    if (changed)
                    {
                        // update the color value live
                        int v = g_ccDlg.rgbInputStr.empty() ? 0 : std::atoi(g_ccDlg.rgbInputStr.c_str());
                        if (v < 0) v = 0; if (v > 255) v = 255;
                        if (g_ccDlg.rgbEditing == 0) cp->r = (sf::Uint8)v;
                        else if (g_ccDlg.rgbEditing == 1) cp->g = (sf::Uint8)v;
                        else cp->b = (sf::Uint8)v;
                        if (v > 0) g_ccDlg.rgbLastValid = v;
                        g_ccDlg.rgbCursorClock.restart();
                    }
                    continue;
                }

                if (e.type == sf::Event::KeyPressed && g_ccDlg.open && g_ccDlg.rgbEditing != -1)
                {
                    sf::Color* cp = ccGetColor(g_ccDlg.editTarget);
                    if (!cp) { g_ccDlg.rgbEditing = -1; continue; }

                    if (e.key.code == sf::Keyboard::Enter)
                    {
                        g_ccDlg.rgbEditing = -1;
                        continue;
                    }
                    else if (e.key.code == sf::Keyboard::Escape)
                    {
                        // cancel editing, restore the last valid value
                        int v = g_ccDlg.rgbLastValid;
                        if (g_ccDlg.rgbEditing == 0) cp->r = (sf::Uint8)v;
                        else if (g_ccDlg.rgbEditing == 1) cp->g = (sf::Uint8)v;
                        else cp->b = (sf::Uint8)v;
                        g_ccDlg.rgbEditing = -1;
                        continue;
                    }
                    else if (e.key.code == sf::Keyboard::Up || e.key.code == sf::Keyboard::Down)
                    {
                        int step = (e.key.shift) ? 10 : 1;
                        if (e.key.code == sf::Keyboard::Down) step = -step;
                        int v;
                        if (g_ccDlg.rgbEditing == 0)      { v = cp->r; }
                        else if (g_ccDlg.rgbEditing == 1) { v = cp->g; }
                        else                              { v = cp->b; }
                        v += step;
                        if (v < 0) v = 0; if (v > 255) v = 255;
                        if (g_ccDlg.rgbEditing == 0)      cp->r = (sf::Uint8)v;
                        else if (g_ccDlg.rgbEditing == 1) cp->g = (sf::Uint8)v;
                        else                              cp->b = (sf::Uint8)v;
                        g_ccDlg.rgbInputStr = std::to_string(v);
                        g_ccDlg.rgbLastValid = v;
                        g_ccDlg.rgbCursorClock.restart();
                        continue;
                    }
                    else if (e.key.code == sf::Keyboard::Left || e.key.code == sf::Keyboard::Right)
                    {
                        int curV = g_ccDlg.rgbInputStr.empty() ? 0 : std::atoi(g_ccDlg.rgbInputStr.c_str());
                        curV = std::max(0, std::min(255, curV));
                        if (g_ccDlg.rgbEditing == 0) cp->r = (sf::Uint8)curV;
                        else if (g_ccDlg.rgbEditing == 1) cp->g = (sf::Uint8)curV;
                        else cp->b = (sf::Uint8)curV;

                        int dir = (e.key.code == sf::Keyboard::Right) ? 1 : -1;
                        int next = g_ccDlg.rgbEditing + dir;
                        if (next < 0) next = 2;
                        if (next > 2) next = 0;
                        g_ccDlg.rgbEditing = next;
                        int v = (next == 0) ? cp->r : (next == 1) ? cp->g : cp->b;
                        g_ccDlg.rgbInputStr = std::to_string(v);
                        g_ccDlg.rgbLastValid = v;
                        g_ccDlg.rgbCursorClock.restart();
                        continue;
                    }
                }
            }

            // mouse wheel events - font list scroll
            if (e.type == sf::Event::MouseWheelScrolled && fontDlg.open)
            {
                float delta = e.mouseWheelScroll.delta;
                int totalItems = (int)g_systemFonts.size();
                float listAreaH = fontDlg.listAreaH;
                float itemH = fontDlg.itemH;
                int visibleCount = (int)(listAreaH / itemH);
                if (visibleCount > totalItems) visibleCount = totalItems;
                int maxScroll = totalItems - visibleCount;
                if (maxScroll < 0) maxScroll = 0;

                fontDlg.scrollPos -= (int)delta;
                if (fontDlg.scrollPos < 0) fontDlg.scrollPos = 0;
                if (fontDlg.scrollPos > maxScroll) fontDlg.scrollPos = maxScroll;
            }

            // mouse move - font list hover
            if (e.type == sf::Event::MouseMoved && fontDlg.open)
            {
                sf::Vector2i pos = sf::Mouse::getPosition(window);
                if (pos.x >= fontDlg.listAreaX && pos.x <= fontDlg.listAreaX + fontDlg.listAreaW &&
                    pos.y >= fontDlg.listAreaY && pos.y <= fontDlg.listAreaY + fontDlg.listAreaH)
                {
                    int relY = (int)(pos.y - fontDlg.listAreaY);
                    fontDlg.hoverIdx = fontDlg.scrollPos + (int)(relY / fontDlg.itemH);
                    if (fontDlg.hoverIdx >= (int)g_systemFonts.size())
                        fontDlg.hoverIdx = (int)g_systemFonts.size() - 1;
                }
                else
                {
                    fontDlg.hoverIdx = -1;
                }
            }

            // mouse wheel - timer/AI milestone slider bar scroll
            if (e.type == sf::Event::MouseWheelScrolled && milestoneVisible())
            {
                float mx = (float)e.mouseWheelScroll.x;
                float my = (float)e.mouseWheelScroll.y;
                const sf::FloatRect& r = g_tm.mileRect;
                if (r.width > 0 && mx >= r.left && mx <= r.left + r.width &&
                    my >= r.top && my <= r.top + r.height)
                {
                    // fixed milestone count: 2^5..tmMaxMilestonePow(sz)
                int totalItems = tmMaxMilestonePow(game.getBoardSize()) - 4;
                    int maxScroll = totalItems - g_tm.mileVis;
                    if (maxScroll < 0) maxScroll = 0;
                    g_tm.mileScroll -= (float)e.mouseWheelScroll.delta;
                    if (g_tm.mileScroll < 0) g_tm.mileScroll = 0;
                    if (g_tm.mileScroll > maxScroll) g_tm.mileScroll = (float)maxScroll;
                }
            }

            // mouse move - timer/AI milestone slider bar hover
            if (e.type == sf::Event::MouseMoved && milestoneVisible())
            {
                sf::Vector2i pos = sf::Mouse::getPosition(window);
                const sf::FloatRect& r = g_tm.mileRect;
                g_tm.mileHover = -1;
                if (r.width > 0 && g_tm.mileItemH > 0.f &&
                    pos.x >= r.left && pos.x <= r.left + r.width &&
                    pos.y >= r.top && pos.y <= r.top + r.height)
                {
                    int relY = (int)(pos.y - (r.top + g_tm.mileListTopOff));
                    if (relY < 0) relY = 0;
                    int k = 5 + (int)g_tm.mileScroll + (int)(relY / g_tm.mileItemH);
                    if (k <= tmMaxMilestonePow(game.getBoardSize())) g_tm.mileHover = k;
                }
            }

            // mouse wheel - history list scroll
            if (e.type == sf::Event::MouseWheelScrolled && g_historyDlg.open)
            {
                float delta = e.mouseWheelScroll.delta;
                int totalItems = histVisibleCount(g_historyDlg);
                int visibleCount = (int)(g_historyDlg.listAreaH / g_historyDlg.itemH);
                if (visibleCount > totalItems) visibleCount = totalItems;
                int maxScroll = totalItems - visibleCount;
                if (maxScroll < 0) maxScroll = 0;

                g_historyDlg.scrollPos -= (int)delta;
                if (g_historyDlg.scrollPos < 0) g_historyDlg.scrollPos = 0;
                if (g_historyDlg.scrollPos > maxScroll) g_historyDlg.scrollPos = maxScroll;
            }

            // mouse wheel - build-position value list scroll
            if (e.type == sf::Event::MouseWheelScrolled && g_practiceBuildOpen)
            {
                float mx = (float)e.mouseWheelScroll.x;
                float my = (float)e.mouseWheelScroll.y;
                if (mx >= g_practiceBuildPaletteX && mx <= g_practiceBuildPaletteX + g_practiceBuildPaletteW &&
                    my >= g_practiceBuildPaletteY && my <= g_practiceBuildPaletteY + g_practiceBuildPaletteH)
                {
                    int maxScroll = practiceBuildMaxPow(game.getBoardSize()) + 1 - 8;   // total = clear+fill+2^1..2^maxPow, 8 visible
                    if (maxScroll < 0) maxScroll = 0;
                    g_practiceBuildScroll -= (int)e.mouseWheelScroll.delta;
                    if (g_practiceBuildScroll < 0) g_practiceBuildScroll = 0;
                    if (g_practiceBuildScroll > maxScroll) g_practiceBuildScroll = maxScroll;
                }
            }

            // mouse move - history list / import list hover
            if (e.type == sf::Event::MouseMoved && g_historyDlg.open)
            {
                sf::Vector2i pos = sf::Mouse::getPosition(window);
                if (g_historyDlg.tab == 1)
                {
                    // import history list hover
                    if (pos.x >= g_historyDlg.impListX && pos.x <= g_historyDlg.impListX + g_historyDlg.impListW &&
                        pos.y >= g_historyDlg.impListY && pos.y <= g_historyDlg.impListY + g_historyDlg.impListH)
                    {
                        float spacing = g_historyDlg.impItemSpacing;
                        float topOff = g_historyDlg.impItemTopOffset;
                        if (spacing <= 0.f) { spacing = 50.f; topOff = 3.f; }
                        int relY = (int)(pos.y - g_historyDlg.impListY - topOff);
                        int visIdx = g_historyDlg.importScroll + (int)(relY / spacing);
                        if (visIdx >= 0 && visIdx < (int)g_historyDlg.importFiles.size())
                            g_historyDlg.hoverIdx = visIdx;
                        else
                            g_historyDlg.hoverIdx = -1;
                    }
                    else
                    {
                        g_historyDlg.hoverIdx = -1;
                    }
                }
                else if (pos.x >= g_historyDlg.listAreaX && pos.x <= g_historyDlg.listAreaX + g_historyDlg.listAreaW &&
                    pos.y >= g_historyDlg.listAreaY && pos.y <= g_historyDlg.listAreaY + g_historyDlg.listAreaH)
                {
                    int relY = (int)(pos.y - g_historyDlg.listAreaY);
                    g_historyDlg.hoverIdx = g_historyDlg.scrollPos + (int)(relY / g_historyDlg.itemH);
                    int visTotal = histVisibleCount(g_historyDlg);
                    if (g_historyDlg.hoverIdx >= visTotal)
                        g_historyDlg.hoverIdx = visTotal - 1;
                    // delete-button hover (bottom-right small circle)
                    g_historyDlg.hoverDelIdx = -1;
                    g_historyDlg.hoverExpIdx = -1;
                    if (g_historyDlg.hoverIdx >= 0)
                    {
                        int hv = g_historyDlg.hoverIdx;
                        float itemPadX = 6.f, itemPadY = 4.f;
                        float itemInnerW = g_historyDlg.listAreaW - itemPadX * 2.f;
                        float itemInnerH = g_historyDlg.itemH - itemPadY * 2.f;
                        float itemX = g_historyDlg.listAreaX + itemPadX;
                        float itemY = g_historyDlg.listAreaY + (hv - g_historyDlg.scrollPos) * g_historyDlg.itemH;
                        float drawY = itemY + itemPadY;
                        float delSize = std::max(15.f, itemInnerH * 0.27f);
                        float expGap = delSize * 0.5f;
                        float expSize = delSize;   // same size as the delete button
                        float delX = itemX + itemInnerW - delSize - 5.f;
                        float delY = drawY + itemInnerH - delSize - 5.f;
                        // export-button hover (left of the delete button)
                        float expX = delX - expGap - expSize;
                        float expY = delY;
                        if (pos.x >= expX && pos.x <= expX + expSize &&
                            pos.y >= expY && pos.y <= expY + expSize)
                            g_historyDlg.hoverExpIdx = hv;
                        else if (pos.x >= delX && pos.x <= delX + delSize &&
                                 pos.y >= delY && pos.y <= delY + delSize)
                            g_historyDlg.hoverDelIdx = hv;
                    }
                }
                else
                {
                    g_historyDlg.hoverIdx = -1;
                    g_historyDlg.hoverDelIdx = -1;
                    g_historyDlg.hoverExpIdx = -1;
                }
            }
        }

        flushSaveIfNeeded(game);
        float dt = frameClock.restart().asSeconds();
        game.updateAnimations(dt);

        // === window width sync: milestone panel (timer/AI) needs exactly fitting width; restored at session end ===
        tmApplyWindowWidth(window);

        // === history playback advance (track auto-slide / frame sync) ===
        updatePlayback(dt, game);

        // === AI session timing: accumulates while running, stops on pause (resume continues from the current value) ===
        if (g_aiMode == AI_RUNNING && g_aiTiming)
            g_aiElapsedMs += dt * 1000.f;

        // === challenge timing: accumulates after start until finished (settings dialog paused) ===
        if (g_ch.active && g_ch.started && !g_ch.finished && !g_ch.settingsOpen)
        {
            g_ch.elapsedMs += dt * 1000.0;
            // max supported 9999 minutes; auto-ends beyond (timeout without achieving, not recorded to history)
            if (g_ch.elapsedMs > 9999.0 * 60000.0)
            {
                g_ch.won = false;
                chFinish(game);
            }
        }

        // === timer mode: main time accumulation + IPS/MPS update (started and not finished; timing stops during the confirm dialog) ===
        if (g_tm.active && g_tm.started && !g_tm.finished && !g_tm.settingsOpen && !g_tm.confirmClose)
        {
            g_tm.elapsedMs += dt * 1000.0;
            // IPS: operations in the last 1s sliding window
            g_tm.ipsWindowStart += dt;
            if (g_tm.ipsWindowStart >= 1.0)
            {
                g_tm.ips = (float)g_tm.ipsWindowMoves / (float)g_tm.ipsWindowStart;
                g_tm.ipsWindowMoves = 0;
                g_tm.ipsWindowStart = 0.0;
            }
            if (g_tm.elapsedMs >= 10000.0 * 60000.0)
            {
                // 10000-minute cap reached -> auto (timeout) end: record history and save
                tmFinish(game);
            }
        }
        // IPS stays 0 while timer mode has not started (waiting for the first move)
        if (g_tm.active && !g_tm.started) g_tm.ips = 0.f;

        // === AI auto-step (high-frequency polling mode) ===
        // step cadence = max(animation duration, g_aiStepDelaySec):
        //   the clock keeps accumulating during animation (no longer reset every frame); when the animation ends and the interval reaches the rate, consume
        //   -- "moves per second" matches the rate slider, and the animation fills most of the step interval (looks natural)
        if (g_aiMode == AI_RUNNING && !g_showAIEnd)
        {
            if (!game.isAnimating() &&
                (g_aiStepClock.getElapsedTime().asSeconds() >= g_aiStepDelaySec ||
                 s_aiConsumingPending))   // not ready but entered the "poll pending each frame" state
            {
                const bool didConsume = aiStep(game);
                // inside aiStep: successful consume/sync fallback -> true; not ready -> false + restart the clock
                if (didConsume) {
                    g_aiStepClock.restart();
                    s_aiConsumingPending = false;
                }
            }
        }

        // playback mode renders the playback-dedicated board instance (carries per-step slide/merge/spawn animations)
        drawGame(window, (g_pb.active ? g_pbGame : game), font, menuOpen);
        if (g_historyDlg.open)
            drawHistoryDialog(window, game, font, g_historyDlg);
        if (g_ccDlg.open)
            drawCustomColorDlg(window, game, font, g_ccDlg);
        if (showConfirm)
        {
            if (showPresetFullAlert)
                drawConfirmDialog(window, game, font,
                    "Slots are full! Please delete one preset first.",
                    "存档已满！请先删除一个预设。");
            // switching boards no longer asks for confirmation (progress auto-kept in each size's slot save)
        }
        // 5x5 AI resource-usage warning dialog
        if (g_show5x5AiWarning)
        {
            drawConfirmDialog(window, game, font,
                "The 5x5 AI mode doesn't have a mature algorithm yet. Results may not be great. Proceed?",
                "当前 5×5 AI  模式暂时没有较为成熟的算法， 分数可能不理想，是否继续？");
        }
        // AI fully-ended dialog (drawn last to stay on top)
        unsigned int winW2 = window.getSize().x;
        unsigned int winH2 = window.getSize().y;
        if (g_showAIEnd)
        {
            sf::RectangleShape aiEndOverlay(sf::Vector2f((float)winW2, (float)winH2));
            aiEndOverlay.setFillColor(sf::Color(0, 0, 0, 180));
            window.draw(aiEndOverlay);

            // v6.4: fixed ratio height:width = 1:2
            const float GOLDEN = 2.f;
            float dlgW = winW2 * 0.55f;
            float dlgH = dlgW / GOLDEN;
            // window too short -> derive the width from the height
            if (dlgH > winH2 * 0.85f)
            {
                dlgH = winH2 * 0.85f;
                dlgW = dlgH * GOLDEN;
            }
            float dlgX = (winW2 - dlgW) / 2.f;
            float dlgY = (winH2 - dlgH) / 2.f;
            sf::RectangleShape aiEndDlg(sf::Vector2f(dlgW, dlgH));
            aiEndDlg.setPosition(dlgX, dlgY);
            aiEndDlg.setFillColor(game.getScheme().windowBg);
            aiEndDlg.setOutlineColor(sf::Color(70, 130, 180));
            aiEndDlg.setOutlineThickness(4.f);
            window.draw(aiEndDlg);

            // v6.4: text width adaptive - no more than 80% of the dialog width
            float maxTextW = dlgW * 0.80f;

            // title text
            unsigned int endFontSize = (unsigned int)(dlgH * 0.11f);
            if (endFontSize < 20) endFontSize = 20;
            if (endFontSize > 60) endFontSize = 60;
            sf::String endTitleStr = tr("AI operation completely ended", "AI 操作彻底结束");
            // measure the text width; shrink the font proportionally when it exceeds
            {
                sf::Text measureTxt(endTitleStr, font, endFontSize);
                float textW = measureTxt.getLocalBounds().width;
                if (textW > maxTextW && textW > 0.f)
                {
                    endFontSize = (unsigned int)(endFontSize * maxTextW / textW);
                    if (endFontSize < 14) endFontSize = 14;
                }
            }
            drawTextFB(window, font, endTitleStr,
                       winW2 / 2.f, dlgY + dlgH * 0.35f,
                       endFontSize, game.getScheme().textDark, sf::Text::Bold, 1);

            // hint text
            unsigned int hintFontSize = (unsigned int)(dlgH * 0.07f);
            if (hintFontSize < 14) hintFontSize = 14;
            sf::String endHintStr = tr("Click to close", "点击关闭");
            {
                sf::Text measureTxt(endHintStr, font, hintFontSize);
                float textW = measureTxt.getLocalBounds().width;
                if (textW > maxTextW && textW > 0.f)
                {
                    hintFontSize = (unsigned int)(hintFontSize * maxTextW / textW);
                    if (hintFontSize < 12) hintFontSize = 12;
                }
            }
            drawTextFB(window, font, endHintStr,
                       winW2 / 2.f, dlgY + dlgH * 0.72f,
                       hintFontSize, game.getScheme().textDark, 0, 1);
        }

        // === timer-mode "save?" confirm dialog (topmost; timing already stopped while shown) ===
        if (g_tm.confirmClose)
        {
            sf::RectangleShape tmOv(sf::Vector2f((float)winW2, (float)winH2));
            tmOv.setFillColor(sf::Color(0, 0, 0, 160));
            window.draw(tmOv);

            float cW = winW2 * 0.42f;
            float cH = cW * 0.62f;
            if (cW > winW2 * 0.9f) cW = winW2 * 0.9f;
            if (cH > winH2 * 0.5f) cH = winH2 * 0.5f;
            float cX = (winW2 - cW) / 2.f;
            float cY = (winH2 - cH) / 2.f;

            sf::RectangleShape cDlg(sf::Vector2f(cW, cH));
            cDlg.setPosition(cX, cY);
            cDlg.setFillColor(game.getScheme().windowBg);
            cDlg.setOutlineColor(sf::Color(70, 130, 180));
            cDlg.setOutlineThickness(3.f);
            window.draw(cDlg);

            // hint text (width-adaptive)
            float maxTw = cW * 0.82f;
            unsigned int cFont = (unsigned int)(cH * 0.10f);
            if (cFont < 16) cFont = 16;
            sf::String cMsg = tr("Save the current progress?",
                                 "是否保存当前进度？");
            {
                sf::Text mt(cMsg, font, cFont);
                float tw = mt.getLocalBounds().width;
                if (tw > maxTw && tw > 0.f) cFont = (unsigned int)(cFont * maxTw / tw);
                if (cFont < 12) cFont = 12;
            }
            drawTextFB(window, font, cMsg, winW2 / 2.f, cY + cH * 0.28f,
                       cFont, game.getScheme().textDark, sf::Text::Bold, 1);

            // button row: yes (save, filled emphasis) / no (discard)
            float btnW = cW * 0.30f;
            float btnH = cH * 0.24f;
            float btnY = cY + cH * 0.62f;
            float gapX = cW * 0.06f;
            float totalW = btnW * 2 + gapX;
            float btnX0 = winW2 / 2.f - totalW / 2.f;
            unsigned int btnFont = (unsigned int)(btnH * 0.5f);
            if (btnFont < 14) btnFont = 14;

            // yes (save) - filled emphasis (blue bg + white text)
            g_tm.confirmYesBtn = sf::FloatRect(btnX0, btnY, btnW, btnH);
            {
                sf::RectangleShape b(sf::Vector2f(btnW, btnH));
                b.setPosition(btnX0, btnY);
                b.setFillColor(sf::Color(70, 130, 180));
                window.draw(b);
                drawTextFB(window, font, tr("Yes", "是"),
                           btnX0 + btnW / 2.f, btnY + btnH / 2.f, btnFont, sf::Color::White, sf::Text::Bold, 1);
            }
            // no (discard)
            g_tm.confirmNoBtn = sf::FloatRect(btnX0 + btnW + gapX, btnY, btnW, btnH);
            {
                sf::RectangleShape b(sf::Vector2f(btnW, btnH));
                b.setPosition(btnX0 + btnW + gapX, btnY);
                b.setFillColor(game.getScheme().boardBg);
                b.setOutlineColor(game.getScheme().textDark);
                b.setOutlineThickness(1.f);
                window.draw(b);
                drawTextFB(window, font, tr("No", "否"),
                           btnX0 + btnW + gapX + btnW / 2.f, btnY + btnH / 2.f,
                           btnFont, game.getScheme().textDark, 0, 1);
            }
        }

        // === build-position dialog (topmost) ===
        if (g_practiceBuildOpen)
        {
            bool aiActiveLocal = (g_aiMode != AI_INACTIVE) && !g_pb.active;
            float bpLocal = computeBoardPx(winW2, winH2, aiActiveLocal);
            drawPracticeBuildPopup(window, game, font, game.getScheme(), bpLocal);
        }

        window.display();

        // test mode: auto screenshot after the first frame
        if (testMode && !screenshotTaken)
        {
            sf::Texture tex;
            tex.update(window);
            sf::Image img = tex.copyToImage();
            img.saveToFile("C:/test_neon.png");
            screenshotTaken = true;
        }
    }

    logMsg("=== Game exited normally ===");
    aiWorkerStop();   // stop the persistent AI search thread (engine/thread pool freed when the thread exits)
    return 0;
    } catch (const std::exception& e) {
        logMsg(std::string("EXCEPTION: ") + e.what());
        return 1;
    } catch (...) {
        logMsg("UNKNOWN EXCEPTION");
        return 1;
    }
}
