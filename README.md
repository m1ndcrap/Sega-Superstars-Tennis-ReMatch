# Sega Superstars Tennis ReMatch

A native Windows port of the Xbox 360 version of Sega Superstars Tennis, recompiled with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (v0.10.0). The game's code runs as native x64. Its graphics run on a native Direct3D 12 renderer that translates the game's Xbox 360 shaders, not an emulated GPU.

## Features

- **Native D3D12 renderer**: internal resolution up to 2160p, render scale, FXAA/MSAA, shadows resolution, bloom toggle, brightness, VSync, a 30/60 FPS limit toggle, and an FPS counter. All of these are set in a new **VIDEO** page in Settings and in the pause menu.
- **Aspect ratios**: 4:3, 16:9, 16:10, 21:9 and 32:9. The 3D view is widened, and menu backgrounds fill the screen. The HUD adjust to each aspect ratio for a more natural look.
- **Keyboard and mouse support**, with rebindable keys (**CONTROLS -> KEY BINDINGS**). Button prompts follow the device you're using (Xbox, PlayStation or keyboard icons).
- **PlayStation controllers**: DualShock/DualSense prompts, a DualShock 3 diagram on the CONTROLS page, and an adaptive lightbar to distinguish which controller belongs to which player.
- **Menu changes**:
  - a **QUIT** option on the main menu
  - skippable boot intro
  - Xbox LIVE and Xbox profile pop-ups removed. **ONLINE PLAY** is greyed out for now.

## Setup

1. Download the latest release zip from the Releases tab and extract it anywhere.
2. Extract a legally obtained copy of Sega Superstars Tennis (Xbox 360) into the `assets` folder next to `sst_rematch.exe`, so that `assets/default.xex` exists.
3. Run `sst_rematch.exe`.

Requirements:

- Windows 10/11 x64.
- A GPU with Direct3D 12 support.
- The [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe), if Windows reports a missing `msvcp140.dll` or `vcruntime140.dll`.

The game's shaders come precompiled inside `sst_rematch.exe`, so there's no shader compiling on first launch. The few shaders the game generates while running are compiled the first time they're needed (on-screen ones first) and cached in `cache/shaders` for later runs.

## Controls (keyboard defaults)

| Action              | Key       |
| ------------------- | --------- |
| Move                | W A S D   |
| Shots (X A B Y)     | J K L O   |
| Superstar           | I / Space |
| Partner             | Q / E     |
| Back                | U         |
| Start               | Enter     |
| Pause / back / quit | Esc       |

All keys can be changed in **Settings › CONTROLS › KEY BINDINGS**. By default the keyboard shares player 1 with the first pad. To give it its own player slot, set that in KEY BINDINGS or launch with `--sst_keyboard_player=separate`.

## Settings files

These files are created next to the exe:

- `sst_settings.ini`: video page settings.
- `sst_keys.ini`: key bindings.

Delete either one to go back to the defaults.

## Reporting bugs

Please create an issue under the issues tab of this repo and attach the log from the `logs` folder next to the exe. If the game crashed, also attach `sst_crash.txt` from the same folder as the exe. Say what you were doing when it happened and which aspect ratio and controller you were using.

## Build

Requirements: Clang 18+, CMake 3.25+, Ninja, the Windows SDK (for `dxcompiler.dll` / `dxil.dll`), and a ReXGlue v0.10 SDK (installed or a source checkout).

1. Put the extracted game in `assets/` so `assets/default.xex` exists.
2. Configure and build:

   ```
   cmake --preset win-amd64-release -DREXSDK_DIR=path/to/rexglue-sdk
   cmake --build --preset win-amd64-release
   ```

   Codegen runs automatically as part of the build (target `sst_rematch_codegen`).
   Drop `-DREXSDK_DIR` if the SDK is installed and on `CMAKE_PREFIX_PATH`. If `dxcompiler.dll` / `dxil.dll` aren't found, point `-DSST_DXC_DIR` at the folder that contains them.

   The build also precompiles the game's shaders (target `sst_shader_pack`, tool `tools/shader_bake`). It reads the ShaderCache files from `assets/`, translates every shader with XenosRecomp, compiles each variant the renderer can use with DXC, and embeds the result in the exe, compressed (unpacked once at startup). The first build takes a minute or so for this; later builds only recompile shaders whose translation changed (`out/build/<preset>/shader_bake_work`). Options:
   - `-DSST_SHADER_BAKE=OFF`: skip it (shaders then compile on players' machines on first use).
   - `-DSST_GAME_DIR=<path>`: the extracted game, if it isn't in `assets/`.
   - Shaders the game generates while running aren't in any game file, so the first build can't include them. The exe saves each one it has to compile to `cache/containers` next to itself, and the next build bakes them in automatically. Before a release, run the exe from the build folder, play through every character, court and mode once, then rebuild.
   - `-DSST_SHADER_DUMP_DIR=<path>`: also bake another folder of saved shaders (e.g. `cache/containers` copied from another PC, or `--sst_dump_shaders=true` output in `<exe>/d3d_trace/shaders`).

3. Copy (or symlink) `assets/` next to `out/build/win-amd64-release/sst_rematch.exe` and run it.

### Current Pre-Release v0.4 Zip Contents

`sst_rematch.exe`, `rexruntime.dll`, `rexgpu-sst.dll`, `dxcompiler.dll`, `dxil.dll`, plus an empty `assets/` folder.

## Layout

| Path                           | Purpose                                                                   |
| ------------------------------ | ------------------------------------------------------------------------- |
| `sst_rematch_manifest.toml`    | Project manifest (entrypoint XEX, output dir)                             |
| `config/sst_rematch_func.toml` | Codegen hints: setjmp/longjmp, manual function starts                     |
| `src/sst_rematch_app.h`        | App hooks: default `assets/` path, GPU plugin choice, crash log           |
| `src/gpu/`                     | Native D3D12 renderer (`native/`) and its GPU plugin (`plugin/`)          |
| `src/menu/`                    | VIDEO page, pause menu and main menu patches, wide layouts, HUD anchoring |
| `src/input/`                   | Keyboard / mouse, key bindings, button prompts, lightbar                  |
| `src/movie/`                   | Movie playback hooks                                                      |
| `src/debug/`                   | Crash report, scene update-list guard                                     |
| `tools/shader_bake/`           | Build-time shader compiler: game shaders -> DXIL pack embedded in the exe |
| `thirdparty/xenosrecomp/`      | Vendored Xenos shader translator                                          |
| `generated/rexglue.cmake`      | SDK glue (regenerated by codegen)                                         |
| `generated/default/`           | Recompiled output (not committed)                                         |

Game files (`assets/`, extracted archives, the XEX) are never committed.
