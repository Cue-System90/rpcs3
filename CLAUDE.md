# RPCS3 – fork Cue-System90/rpcs3

Personal fork of [RPCS3/rpcs3](https://github.com/RPCS3/rpcs3), the open-source PlayStation 3 emulator and debugger
(C++23, CMake, Qt 6 GUI, LLVM-based PPU/SPU recompilers, Vulkan/OpenGL RSX backends). License: GPL-2.0.

## Working with the user

- Talk to the user in **German**. Write code, comments, commit messages and PR texts in **English** (upstream convention).
- Work on a feature branch. Never push to `master`: it mirrors upstream plus fork-only additions
  (cloud template, `game-patches/`, README note). Changes reach `master` through PRs in this fork.
- Track work in this fork's issue tracker (labels `gran-turismo-5`, `rendering`, `game-patch`, `upstream`, `needs-test`, `fork-maintenance`).
- Do not open PRs against `RPCS3/rpcs3` unless the user explicitly asks for it. Upstream CI rejects PRs whose
  commits name an AI agent (`Co-authored-by: Claude`, `claude-session:`); such PRs need commits written by the user.

## Repository layout

- `rpcs3/Emu/` – emulator core
  - `Cell/` – PPU/SPU, LV2 syscalls (`Cell/lv2/`), HLE system modules (`Cell/Modules/`)
  - `RSX/` – GPU emulation with `VK/` and `GL/` backends
  - `CPU/` – shared CPU and LLVM translator code · `Memory/` · `Audio/` · `Io/` (pads, peripherals) · `NP/` (PSN)
- `rpcs3/rpcs3qt/` – Qt GUI · `rpcs3/Input/` – pad handlers · `rpcs3/Loader/` – ELF/ISO/PSF/PUP/TRP loaders · `rpcs3/Crypto/`
- `rpcs3/util/`, `Utilities/` – shared helpers
- `rpcs3/tests/` – GoogleTest unit tests (target `rpcs3_test`)
- `3rdparty/` – git submodules and vendored libraries: do not edit
- `.ci/`, `.github/workflows/` – upstream CI · `.claude/cloud/`, `scripts/claude-cloud/` – this cloud template

## Testing workflow

Runtime testing (games, rendering, performance) happens on the user's **CachyOS desktop with an NVIDIA RTX 4090** (Vulkan).
Cloud sessions have no GPU and no display, so the loop is:

1. Cloud session: change the code, build the affected targets, run the unit tests.
2. Commit and push the feature branch.
3. Tell the user in German what to test on CachyOS: branch name, game or scenario, relevant settings
   (renderer, SPU/PPU decoder, etc.), what to look for, and which lines of `RPCS3.log` to send back.
4. Analyse the log or result the user reports and iterate.

## Building locally (CachyOS desktop)

Local sessions can build and run games directly. Checkout: `/mnt/NVME-DATA/Logo_work_progamming/rpcs3`.

```bash
git -c submodule.3rdparty/llvm/llvm.update=none submodule update --init --depth 1 --jobs 8
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_LLVM=OFF -DUSE_LTO=OFF -DCMAKE_LINKER_TYPE=LLD -DUSE_SYSTEM_FFMPEG=ON -DUSE_NATIVE_INSTRUCTIONS=ON
nice -n 15 ninja -C build -j24        # ~8 min from scratch, binary: build/bin/rpcs3
```

- `USE_LTO` defaults to ON; with clang it needs lld, so turn it off (or set `CMAKE_LINKER_TYPE=LLD`).
- The FFmpeg prebuilt download from GitHub release assets hangs here; use the system FFmpeg.
- System LLVM (23.x) works with `BUILD_LLVM=OFF`.
- Never run test builds against the user's real config (`~/.config/rpcs3` = `console_emulation/PS3/rpcs3_data`).
  Use isolated `XDG_CONFIG_HOME`/`XDG_CACHE_HOME` outside `/tmp` (RPCS3 refuses temporary locations), for example
  `console_emulation/PS3/tools/gt5_research/xdg/`, and run windows inside `kwin_wayland --virtual`, never on the user's desktop.
- Announce GPU-heavy runs to the user and stop them when asked.

## Building and testing (cloud sessions)

All builds run in the official CI image (`rpcs3/rpcs3-ci-jammy`, same toolchain as upstream Linux CI) via `scripts/claude-cloud/build.sh`.
The script fetches the needed submodules itself (fresh clone per session).

```bash
scripts/claude-cloud/build.sh --target rpcs3_emu          # emulator core only
scripts/claude-cloud/build.sh --target rpcs3_ui           # Qt GUI library only
scripts/claude-cloud/build.sh --tests                     # build and run unit tests (target run_tests)
scripts/claude-cloud/build.sh                             # full build -> build-cloud-clang/
scripts/claude-cloud/build.sh --compiler gcc --target rpcs3_emu   # also check GCC
scripts/claude-cloud/build.sh --exec '$CXX --version'     # run a command inside the CI container
scripts/claude-cloud/build.sh --help
```

- Build the smallest target that covers the change. ccache is used, but its cache only lives for the session.
- Full builds take a long time on the 4 vCPUs: start them in the background and poll the log:
  `nohup scripts/claude-cloud/build.sh > /tmp/rpcs3-build.log 2>&1 &`, then `tail -n 30 /tmp/rpcs3-build.log`.
- LTO is off for speed; CI builds with LTO (`--lto` to match it).
- Build directories are `build-cloud-<compiler>/` and are ignored by `.gitignore`.
- If Docker or the image is unavailable, tell the user instead of installing a toolchain by hand.

## Code style

- Follow the surrounding code. Tabs, indent size 4 (`.editorconfig`), formatting per `.clang-format`.
- Format only lines you changed (`git clang-format`); never reformat untouched code.
- Commit subjects are short and prefixed with the area, as upstream does: `rsx: ...`, `SPU LLVM: ...`, `cellSaveData: ...`, `Qt: ...`.

## Keeping the fork current

Use **Sync fork** on GitHub, or locally: `git fetch upstream && git merge upstream/master`
(`git remote add upstream https://github.com/RPCS3/rpcs3.git` if the remote is missing).

- Pushing upstream commits that touch `.github/workflows/` needs the `workflow` scope on the GitHub token
  (`gh auth refresh -h github.com -s workflow`, done by the user).
- If upstream rewrites history again (as in Nov 2026), do not merge: rebase the fork-only commits onto
  `upstream/master` and force-push `master`, then rebase the feature branches.

Note: upstream's `.gitignore` lists `CLAUDE.md`; this file was added with `git add -f` and is tracked normally.
