# Repository Guidelines

## Project Structure & Module Organization

Tinker Editor is a native Win32 editor written primarily in C. `src/tinkeredit.c` owns startup and the window procedure; UI subsystems live in `src/internal/*.inc`. Reusable modules use paired `.c`/`.h` files under `src/`. Text transformations are in `src/lua/`, while `src/menus.lua` defines the menu bar.

Only `src/tinkeredit.c` includes `src/internal/*.inc`; those components stay in the unity translation unit until they can be expressed through a small context structure. Every `src/*.c` must be listed in a repo-root `*.rsp` response file, and every `src/*.h` must be `#include`d from some `.c` or `.inc` file so a header left behind by a deleted or renamed source doesn't drift unnoticed. `check_structure.ps1` (run by `test.bat`) enforces all three rules.

Tests live in `tests/`; `test_preview.js` covers the preview bridge. Bundled extensions and Lua types live in `plugins/`. Preview assets are at the root and in `katex/`; dependencies are under `third_party/`. Treat `build/`, `dist/`, and root binaries as generated output.

## Build, Test, and Development Commands

- `build.bat` builds an speed optimized Windows release with MSVC and stages the complete runtime in `dist/`.
- `test.bat` compiles and runs all C tests, then runs the Node.js preview tests when Node is installed.
- `dist\tinkeredit.exe` launches the packaged build.
- `premake5 --zig gmake` generates an alternative make-based Zig build. `premake5 vs2022` generates a Visual Studio solution.

Visual Studio's Desktop development with C++ workload must be installed; the
scripts locate and initialize its x64 tools automatically. Run `test.bat`
before `build.bat` when preparing a reviewable change.

## Coding Style & Naming Conventions

Use C11, four-space indentation, same-line braces, and short functions. Follow existing names: `snake_case` for functions and locals, `g_camelCase` for shared state, and uppercase `IDM_*`/`ID_*` constants. Keep Win32 integration in C and prefer Lua for text transformations. Add command names to `menu_commands.c`, IDs to `command_ids.h`, and placement to `menus.lua`. No formatter is configured; builds use `-Wall -Wextra`, so keep them warning-free.

## Testing Guidelines

Name native tests `tests/test_<area>.c` and JavaScript tests `test_<area>.js`. Add focused regression coverage for behavior changes, especially encoding, multi-caret operations, menus, plugins, and preview messaging. There is no numeric coverage target. The acceptance standard is a clean `test.bat` run and successful release build.

## Commit & Pull Request Guidelines

Recent commits use short, direct summaries such as `added vertical splitter bar` and `fix file explorer`. Prefer concise imperative subjects, keep each commit scoped to one change, and avoid committing generated artifacts. Pull requests should explain the user-visible result, list validation performed, link relevant issues, and include screenshots for visible UI changes.

## Configuration & Security

Application and plugin settings belong in `%APPDATA%\tinkeredit\settings.json`; never package personal settings. Keep preview assets local and preserve the existing restrictions on raw HTML, URL schemes, and Lua plugin file/network access.
