# RPCS3 – fork Cue-System90/rpcs3

Personal fork of [RPCS3/rpcs3](https://github.com/RPCS3/rpcs3), the open-source PlayStation 3 emulator and debugger
(C++23, CMake, Qt 6 GUI, LLVM-based PPU/SPU recompilers, Vulkan/OpenGL RSX backends). License: GPL-2.0.

## Working with the user

- Talk to the user in **German**. Write code, comments, commit messages and PR texts in **English** (upstream convention).
- Work on a feature branch. Never push to `master`: it mirrors upstream plus the cloud template commit.
- Do not open PRs against `RPCS3/rpcs3` unless the user explicitly asks for it.

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

## Building and testing (cloud sessions)

Cloud sessions have no GPU and no display, so games cannot be run. Verify changes by compiling and running the unit tests.

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

Note: upstream's `.gitignore` lists `CLAUDE.md`; this file was added with `git add -f` and is tracked normally.
