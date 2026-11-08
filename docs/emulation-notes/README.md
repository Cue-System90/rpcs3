# Emulation notes

Working notes for fixing bugs in this fork: how the PS3 hardware works, how RPCS3 emulates it, and where the two are known to differ.
Facts carry a source (hardware docs) or a code location (RPCS3). Anything not verified is marked **unverified**.

- [rsx.md](rsx.md) – the GPU (RSX): hardware, RPCS3 pipeline, texture sampling, known divergences
- [cell.md](cell.md) – the CPU (Cell PPE/SPE) and the LV2 OS layer: hardware, RPCS3 implementation, accuracy options
- [debugging.md](debugging.md) – workflow: logs, RSX captures, isolated test runs, game reverse engineering
