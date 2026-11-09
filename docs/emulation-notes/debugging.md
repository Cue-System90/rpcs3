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

## Measuring emulation overhead in a game

Fork-only timing probes (`rpcs3/Emu/perf_probe.*`) are compiled in but inactive. Set `RPCS3_PERF_PROBE=1` and the log gets a `PROBE:` line every 5 seconds with count, total, average and maximum time per probe, plus frame pacing (frames over 20 ms and 34 ms). Probes cover:

- texture cache write-protection faults per thread class (`av_ppu`, `av_spu`, `av_rsx`), faults that need a GPU readback (`av_flush`) and how that time splits up: waiting for the RSX thread (`flush_rsx_wait`), the transfer (`flush_all`, `flush_retransfer`) and the GPU signal (`flush_event_wait`);
- `sys_mmapper` shared memory map/unmap, `sys_rsx` io map/unmap, contended RSX IO range locks (`iolock_wait`);
- CPU texture uploads that first had to read back GPU-written memory (`cpu_upload_readback`, with a description of both sides).
- cellAudio: periods it waited ~1 ms for the game to write its ports (`audio_wait`), periods it gave up and dropped (`audio_skip`, an audible click), periods mixed with some ports unwritten (`audio_partial`), and device underruns with the missing time (`audio_underrun`).

Audio needs a backend that consumes in real time, otherwise cellAudio disables buffering and the game-side waits are never measured. `RPCS3_PROBE_AUDIO_CLOCK=1` makes the Null backend pull 10 ms of audio every 10 ms and discard it, so buffering behaves as with Cubeb while nothing is heard.

What it showed for GT5 (Nov 2026): cellAudio is clocked by host time, so a slow game does not underrun the device; it shows up as `audio_skip` instead, because cellAudio waits at most 2 (all ports) or 4 (some ports) block periods for the game before dropping the block. Skips appeared only where frames ran long (after the race, ~29 fps). In the race the readback fix took `audio_wait` from ~100 per 5 s to 0. `Disable Sampling Skip` and `Enable Time Stretching` did not reduce the skips: the former makes cellAudio stop waiting for the game at once, and stretching only acts on device-side buffer fill.

With probes on, the first flushes are also described (`PROBE flush #n`: address, faulting thread, section base/size/context/format/size/synchronized), and the first readback waits log a backtrace (resolve the offsets with `llvm-addr2line -f -C -e build/bin/rpcs3`).

Reproducible scenario: let the user create a savestate (Ctrl+S in the game window) at the problem spot, then replay it invisibly with copies of their data:

- copy `dev_hdd0/game/<ID>` (hardlink only large read-only trees such as GT5's `PDIPFS`; real copies for files the game writes), `dev_hdd0/home/<user>`, the custom config, patches, the PPU/SPU/shader cache, `games.yml` (the savestate finds the disc by serial) and `dev_hdd0/game/＄locks/<ID>_v*` (otherwise RPCS3 tries to reinstall the disc's INSDIR package);
- boot with `rpcs3 --no-gui --savestate <file>`; a savestate file passed without `--savestate` is rejected;
- set the audio renderer of the copied config to `"Null"`, or the test plays sound on the user's speakers;
- keep A/B switches in the same binary (`RPCS3_DISABLE_READBACK_FIX=1` for the readback change) so both sides share caches and build.

Script used for GT5: `tools/gt5_research/probe_run.sh <binary> <savestate> <seconds> <tag>` and `probe_summary.py` (outside the repo, in the research workspace).

## Hardware claims

- A fix that changes emulated hardware semantics needs evidence from a real PS3. Upstream asks for a test binary (PSL1GHT homebrew run via HEN) and compares hardware vs RPCS3 output, see [RPCS3#18171](https://github.com/RPCS3/rpcs3/issues/18171).
- Cautionary example, [RPCS3#11912](https://github.com/RPCS3/rpcs3/issues/11912): an AI-assisted ONE×4 remap "fix" made GT5P look right on three GPU stacks, but a hardware test showed the console renders that texture state black as well. The real bug was earlier, in the game's state choice.
- Host GPU behaviour can be checked locally with small Vulkan compute tests (see [host-tests/vk_depth_compare](host-tests/vk_depth_compare/)). Record the driver version with the result.
- When a symptom depends on what the game decides (which texture to bind, which objects to draw), look at the CPU side first: occlusion reports, SPU/PPU float semantics, timing. Rendering a given state wrongly is the less likely cause.
