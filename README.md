# 2048 AI Software - Meter 2048 Auto

An assistive tool for **endless 2048** supporting **3×3 / 4×4 / 5×5** boards. The AI is a pure C++ algorithm (no endgame tablebase, no AI DLL) that solves the board in real time while you play.

[中文版说明 (README_zh.md)](README_zh.md)

## Features

- **AI Auto-Solve** — plays the best move for you, with an adjustable speed slider and a 3-level search depth (Fast / Balanced / Deep).
- **Real-Time Prediction Panel** — direction probability bars, per-step deviation, and milestone achievement rates (e.g. 8192 / 16384 / 32768) updated live.
- **Smart Undo** — consecutive-undo protection and a dead-end retry that skips the losing move.
- **History with Compression** — every session is stored as a compact `.dat` replay; every move can be reconstructed frame by frame (VRS-style). Import / export / single-entry delete / playback are all supported.
- **Timer Mode & Challenge 2048** — speed-run modes with full session persistence.
- **Board Editor & Custom Themes** — design positions, import/export color themes.
- **Free Board Switching** — jump between 3×3 / 4×4 / 5×5 without losing scores.
- **Bilingual UI** — Chinese / English; lightweight logs.

## 4×4 Success Rates

The 4×4 engine follows the classic Expectimax approach of [ziap/2048-ai](https://github.com/ziap/2048-ai). Official benchmark figures from that project:

| Budget | Speed | Avg Score | %32768 | %16384 | %8192 |
|--------|-------|-----------|--------|--------|-------|
| 2^15   | 1324.1 | 259989  | 3.6    | 58.2   | 91.6  |
| 2^17   | 384.0  | 319221  | 9.1    | 74.0   | 95.5  |
| 2^19   | 119.5  | 371887  | 17.6   | 84.6   | 98.1  |
| 2^21   | 26.8   | 419545  | 26.5   | 89.9   | 99.2  |

## Download

Grab the latest **Win64** package from the [Releases](../../releases) page. The package contains the exe, SFML runtimes, fonts, the icon and the full technical documentation — no installation required.

## Build from Source

Requirements: **Visual Studio 2022** (Desktop development with C++ workload), **SFML 2.6.1** (Visual C++ 17, x64), Windows 10 or later.

1. Clone this repository.
2. Download SFML 2.6.1 from <https://www.sfml-dev.org> and extract it, e.g. to `C:\SFML-2.6.1`.
3. Open `2048 game pro/2048 game pro.vcxproj` in Visual Studio 2022.
4. Project settings (**Release | x64**):
   - C/C++ → General → **Additional Include Directories**: `C:\SFML-2.6.1\include`
   - Linker → General → **Additional Library Directories**: `C:\SFML-2.6.1\lib`
   - Linker → Input → **Additional Dependencies**: `sfml-audio.lib;sfml-graphics.lib;sfml-network.lib;sfml-system.lib;sfml-window.lib;openal32.lib;opengl32.lib;winmm.lib`
5. Build **Release | x64**.
6. Copy the DLLs from `C:\SFML-2.6.1\bin` (plus the MSVC runtime DLLs if needed) next to the built exe, and keep the `game fonts` folder beside it.

## Documentation

A complete user guide ships inside the release package: `Meter2048_Technical_Documentation.html` (bilingual, with a sidebar).

## Credits

The 4×4 search design is based on [ziap/2048-ai](https://github.com/ziap/2048-ai).
