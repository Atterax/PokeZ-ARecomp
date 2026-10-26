# ZA Recomp: notes for Claude

A static-recompilation PC port of Pokémon Legends: Z-A, forked from LAS Recomp (Zelda: Link's Awakening, by
Peiikai). The game's ARM64 code is turned into C ahead of time; the runtime re-implements the Switch's OS and its GPU
(NVN -> Vulkan). Supported game versions: 1.0.0, 2.0.0, 2.0.2. Development branch: `pokemon-za`.

Machine-specific paths (game files, install folder) are in `CLAUDE.local.md` when present.

## Where things are

- `tools/gen.py`, `lifter.py`, `analyze.py`, `passes.py`, `patches.py`, `simd_c.py`: the generator, which turns ARM64
  into C. Players' machines run it from the start screen (`runtime/gamebuild.c`).
- `runtime/kernel.c`: threads, mutexes and condvars, the address arbiter, the memory map, SVCs.
- `runtime/services/fs.c`: the RomFS, the mod layers (`mods/<name>/romfs`, with `arc/data.trpfd` unlisting) and the
  save files (`save/save_0/`).
- `runtime/nvn/vulkan/exec.c`: command-list translation, per draw: pipeline key, descriptor sets, uniforms. This is
  the hot path, and Z-A runs it on the game's presenting thread. Also has GMEM_FAST (compute-buffer copies with a
  merge copy-back, `gfast_merge.comp`), the vertex cache and page-write tracking.
- `runtime/nvn/vulkan/textures.c`: texture upload and change checks.
- `runtime/nvn/vulkan/device.c`: present, `[mem]` and `[scale]` log lines, screenshots.
- `runtime/nvn/maxwell/translate.c`: Maxwell shader -> GLSL/MSL.
- `runtime/ui/launcher.mm`: the start screen (game-file setup, save import, update check).
- `runtime/ui/menu.mm`: the F1 settings menu.
- `runtime/hle/*`: host-side function hooks. The Zelda-only ones (warp, souls, digs, rando, cheats...) are gated off
  for Z-A by `g_game_is_la`. Keep them while upstream LAS Recomp is still merged.
- `tools/za/`: Z-A scripts (perf runs, log watcher, stack symbolizer, texture and font tools).

## Building (Windows, Git Bash)

One-time setup: `tools/setup_windows.ps1` (MSYS2 UCRT64 toolchain). Fast exe rebuild:

    (export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make -j"$(nproc)" B=build/za-dist \
      DEPS="$PWD/build/deps-win" RELEASE=1 GENARCH=-march=x86-64-v3 GENOPT="-O1 -DGUEST_VOLATILE" \
      build/za-dist/lasrecomp.exe)

- Keep the PATH change in a subshell: an exported MSYS PATH breaks `cmd` in later commands.
- `build/za-dist/lasrecomp.exe` has no DLLs next to it, so it can't start from there. Copy `lasrecomp.exe` and
  `lasrecomp.dll.a` into `build/dist-za/ZA Recomp/` (the test copy) and run that.
- **`GAMELIB_ID`** (Makefile) hashes the generator sources, `cpu.h`, `simd*.h`, `gamelib.h`, `hle_symbols.txt` and the
  flags. Changing any of them makes every player recompile the game (10-30 min). Runtime, Vulkan and UI changes
  don't, so prefer them.
- LF line endings everywhere (`core.autocrlf=false`, set by the setup script).

## Testing and debugging

- Test windows ignore keyboard and controller. Warn the user before starting one, and never kill or replace their
  running game.
- `tools/za/perf-win.sh`: a scripted run on a scratch copy (never touches the player's save, settings or caches).
  Takes `GAME=`, `EXE=`, `IN=@<recorded input>`, `SAVE=`, `INI=`, `T=`. Settings and data paths must be absolute
  Windows paths, or the run silently uses defaults.
- Record input: `LARECOMP_RECORD_INPUT=<file>`. Replay: `LARECOMP_INPUT=@<file>`.
- `GAME=<install> tools/za/watch-log.sh`: follows `logs/play.log` live (hitches, errors, crashes, `[shot]`).
- Profiling:
  - `LARECOMP_VK_PROF=1`: ms per second by category in the fps line.
  - `LARECOMP_HITCH_MS=18`: per-frame breakdown of slow frames.
  - `LARECOMP_SAMPLE=<delay>,<dur>` with `LARECOMP_SAMPLE_EXEC=1`: stack sampling of the command-list thread into
    `build/stacks.txt`; symbolize it with `tools/za/stacks.py`.
- In game: F1 opens the menu, F9 dumps the next frame's passes, F10 saves a screenshot to `screenshots/`.
- Kill switches for recent changes, to bisect visual bugs:
  - `LARECOMP_VK_GMEM_FAST=0`, `LARECOMP_VK_GFAST_MERGE=0`, `LARECOMP_IB_GUARD=0`, `LARECOMP_VK_CLAMP_COLOR=1`,
    `LARECOMP_WARP_EMU=0`
  - `LARECOMP_VK_SUBMIT_EACH=1`, `LARECOMP_VK_BATCH_DRAWS=<n>`
  - `LARECOMP_VK_PAGES_OUTSIDE=0`, `LARECOMP_VK_NO_SLOT_MEMO=1`
- Unit and fuzz tests exist (`tools/test_translate.sh`, `test_vk_pipelines.sh`, `test_lift.py`, `test_simd.py`) but
  there is no CI. Most checks are real game runs.

## Rules

- Never commit or attach Nintendo files: game files, `extracted/`, `generated/`, fonts, textures, saves.
- Mods from GameBanana etc. are not ours to bundle. They go in the player's `mods/` folder only.
- Comments follow the existing style: dense, explaining *why*, with parenthetical asides and the env var that
  changes the behaviour. Match it.
- Guest threads are real host threads, around 150 in Z-A. Any shared kernel state needs a lock (`g_klock`, or
  `g_mlock` for the memory map).
