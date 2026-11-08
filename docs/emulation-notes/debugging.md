# Debugging workflow

## Logs

- `RPCS3.log` is written to `$XDG_CACHE_HOME/rpcs3/` (Linux default `~/.cache/rpcs3/RPCS3.log`). A previous run is kept as `RPCS3.log.gz`.
- Any RPCS3 invocation with the default config dir overwrites the log, including `--help` and `--decrypt`. Use isolated dirs (below) for helper calls.
- Useful lines: `PPU executable hash: PPU-...` (patch/version identity), `SYS: Title:` / `Version: APP_VER=...`, lines starting with `·E` (error) and `·F` (fatal).
- `Failed to lock sudo memory` is harmless on Linux with the default 8 MiB `RLIMIT_MEMLOCK` (logged once since fork commit "vm: Report a failed sudo memory lock only once").

## Isolated test runs

Never run a test build against the user's real config. RPCS3 refuses config dirs under `/tmp` ("should never be run from a temporary location"), so use a persistent folder:

```bash
R=/mnt/NVME-DATA/console_emulation/PS3/tools/gt5_research
XDG_CONFIG_HOME=$R/xdg/config XDG_CACHE_HOME=$R/xdg/cache rpcs3 ...
```

- Point `/dev_flash/` at the real firmware with `$R/xdg/config/rpcs3/vfs.yml`:
  ```yaml
  $(EmulatorDir): ""
  /dev_flash/: /mnt/NVME-DATA/console_emulation/PS3/rpcs3_data/dev_flash/
  ```
- Run windows inside an invisible compositor, never on the user's desktop:
  ```bash
  kwin_wayland --virtual --no-lockscreen --socket wayland-rpcs3test --width 1280 --height 720 &
  WAYLAND_DISPLAY=wayland-rpcs3test QT_QPA_PLATFORM=wayland rpcs3 --no-gui <boot target>
  ```
- Smoke test target without a game: `dev_flash/vsh/module/vsh.self` (the XMB). A fresh PPU cache takes ~3–4 min before the XMB allocates memory and draws.
- Check that the log says `Found Vulkan-compatible GPU` and not `The video renderer 'Vulkan' is not supported`. A build without Vulkan headers falls back to OpenGL, and OpenGL inside the virtual compositor hung the GPU context (NVIDIA Xid 109 in the test process).
- Announce GPU-heavy runs to the user. A test RPCS3 or Steam instance can block the user's own session.

## RSX captures

- In the game window press **Alt+C** (shortcut `game_window_rsx_capture`). RPCS3 writes `captures/<TITLE_ID>_<time>_capture.rrc.gz` into the config dir.
- Replay: drop the file on the RPCS3 window, use the "Select RSX Capture" menu entry, or from the command line `rpcs3 --no-gui --rsx-capture <file.rrc.gz>` (`arg_rsx_capture`, `rpcs3/rpcs3.cpp`; `Emulator::BootRsxCapture`, `rpcs3/Emu/System.cpp`). The capture holds one frame's command stream and the memory it touches, so a frame can be replayed with a different build or settings.
- Capture versions must match (`rsx::c_fc_version`, currently `0x6` in `rpcs3/Emu/RSX/Capture/rsx_replay.h`); replay with a build close to the one that recorded it.

## Game reverse engineering

- Decrypt an EBOOT (isolated dirs, see above):
  ```bash
  rpcs3 --decrypt /path/EBOOT.BIN < /dev/null      # writes EBOOT.elf next to it
  ```
  Disc updates decrypt without a KLIC. It prompts on stdin for a KLIC when one is needed.
- PPU hash used by patches (`PPU-<sha1>`): SHA-1 over each program header's `p_type`, `p_flags` (big-endian u32), and for `PT_LOAD` with `p_memsz != 0` also `p_vaddr`, `p_memsz` (u64) and the segment bytes (`ppu_load_exec`, `rpcs3/Emu/Cell/PPUModule.cpp`). Script: `tools/gt5_research/ppuhash.py`.
- Port a patch to another version: search the target ELF's text segment for a window of instructions around the patch site; branch displacements and TOC-relative loads differ, so compare with those masked (`tools/gt5_research/findsig.py`).
- Disassemble: `llvm-objdump -d --no-show-raw-insn --start-address=0x... --stop-address=0x... EBOOT.elf` (LLVM ships the PowerPC target).
- Game patches go to `patches/<TITLE_ID>_patch.yml`; RPCS3 loads `patch.yml`, `imported_patch.yml` and the per-title file (`patch_engine::append_title_patches`, `Utilities/bin_patch.cpp`).
- Free code cave: confirm that no instruction references the area (the GT5 hood-cam patch used `0x156C000` in EU 02.11).

## Hardware claims

- A fix that changes emulated hardware semantics needs evidence from a real PS3. Upstream asks for a test binary (PSL1GHT homebrew run via HEN) and compares hardware vs RPCS3 output, see [RPCS3#18171](https://github.com/RPCS3/rpcs3/issues/18171).
- Cautionary example, [RPCS3#11912](https://github.com/RPCS3/rpcs3/issues/11912): an AI-assisted ONE×4 remap "fix" made GT5P look right on three GPU stacks, but a hardware test showed the console renders that texture state black as well. The real bug was earlier, in the game's state choice.
- Host GPU behaviour can be checked locally with small Vulkan compute tests (see [host-tests/vk_depth_compare](host-tests/vk_depth_compare/)). Record the driver version with the result.
- When a symptom depends on what the game decides (which texture to bind, which objects to draw), look at the CPU side first: occlusion reports, SPU/PPU float semantics, timing. Rendering a given state wrongly is the less likely cause.
