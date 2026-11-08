# Cell CPU and LV2 in RPCS3

Code locations are relative to `rpcs3/Emu/` unless noted, as of upstream `e61d5eaf0` (Nov 2026). Line numbers drift; search for the function name.

## Hardware in one paragraph

The Cell has one PPE (64-bit PowerPC, two hardware threads, VMX/AltiVec) and eight SPEs, of which six are available to games. Each SPE runs an SPU core with 256 KB local store (LS) and talks to main memory only through its MFC (DMA queue, mailboxes, signals, atomic "lock line" commands). Main memory is 256 MB XDR; the RSX has 256 MB GDDR3 and reaches main memory through IO mapping. Games run on the LV2 kernel through syscalls and on firmware libraries (SPRX modules). See the [hardware facts](#hardware-facts) below for float semantics.

## PPU (PPE)

- **Thread:** `ppu_thread` (`Cell/PPUThread.h`): GPR/FPR/VR, an unpacked FPSCR where only FPCC is real, VSCR `nj`/`sat`, reservation state (`raddr`, `rtime`, `rdata[128]`).
- **Thread loop:** `ppu_thread::cpu_task` sets round-to-nearest and host DAZ/FTZ from "Set DAZ and FTZ". This applies process-wide through MXCSR, so it also flushes scalar FPU denormals.
- **Decoders:**
  - Interpreter: templated ops in `Cell/PPUInterpreter.cpp`; flags such as `fix_nj`, `set_vest`, `use_dfma`, `set_sat` are selected in `ppu_interpreter_rt_base`.
  - LLVM: `PPUTranslator::Translate`, with runtime helpers `__lwarx`, `__stwcx`, `__vrefp` and others bound in `PPUThread.cpp`.
- **Function discovery:** `ppu_module::analyse` in `Cell/PPUAnalyser.cpp`, which is heuristic in places (comments say "hack" or "approximation").
- **Loading:** `Emulator::Load` (`System.cpp`) calls `ppu_load_exec` (`Cell/PPUModule.cpp`). In order, it:
  1. maps the segments,
  2. computes the `PPU-<sha1>` hash,
  3. **applies game patches** (`patch_engine::apply`),
  4. initializes the HLE modules,
  5. patches embedded SPU images.

  Analysis and compilation follow later in `ppu_initialize()`. Patches therefore see the raw executable before analysis, so a patch that adds a branch into a code cave is analysed correctly.
- **PRX and overlay hashes:** `PRX-<base57>-<seg>` and `OVL-...`. SPU images get `SPU-...` in `sys_spu_image::deploy`.
- **PPU cache:** `cache/<title>/ppu-<hash>-<file>/v9-kusa-<hash>-<settings>-<cpu>.obj`. The settings bitset includes DFMA, VNaN, accurate dcbz, 128-byte reservations, SAT, DAZ and vector estimates, so changing any of these recompiles. "LLVM Precompilation" also compiles the game's and the firmware's PRX files.

### PPU accuracy options

| Option | Interpreter | LLVM |
|---|---|---|
| PPU Set FPCC Bits | `ppu_set_fpcc` | **forced off** (`fixup_settings`, `System.cpp`) |
| PPU Accurate Non-Java Mode (VSCR NJ) | `ppu_flush_denormal` / `jm_mask` | **forced off**; the "Set DAZ and FTZ" path is used instead |
| PPU Accurate Vector NaN Values | `ppu_set_vnan` (VA > VB > VC precedence) | **forced off** |
| PPU Vector NaN Handling | `ppu_fix_vnan` (canonical 0x7fc00000) | `VecHandleNan` ("Partial") |
| PPU Accurate Vector Estimates | exact `vrefp`/`vrsqrtefp` tables | `VecEstimate`; off means IEEE divide |
| Use Accurate DFMA | `std::fma` vs mul+add | `llvm.fma` vs fmul+fadd |
| PPU Set Saturation Bit | `set_sat` | only if the module contains `mfvscr` |

- `fres` and `frsqrte` always use the hardware tables (`Cell/Common.h`). The `frsqrte` table is marked inaccurate for zero, denormal and infinity inputs.
- **FPSCR is barely emulated.** `mtfsf` is a no-op, `mffs` returns only FPCC, and FPRF and exception bits are stubbed in LLVM. Games that read FPSCR exception or rounding bits will misbehave.

## Atomics and reservations

- **Reservation table:** `vm::g_reservations` (`Memory/vm.cpp`) holds one u64 stamp per 128-byte line. The index is `(addr & 0xff80) / 2`, so lines alias every 64 KB. `reservation_update` adds 128 to the stamp.
- **PPU `lwarx`/`ldarx`:** `ppu_load_acquire_reservation` (8-byte). It switches to 128-byte mode (`use_full_rdata`) when a matching `stwcx` is near; the reach is set by "Accurate PPU 128-byte Reservation Op Max Length".
- **PPU `stwcx`/`stdcx`:** `ppu_store_reservation`, either an 8-byte CAS or a 128-byte compare under locks. Real hardware allows the store anywhere in the 128-byte line; RPCS3 fails such stores unless 128-byte mode is on.
- **SPU side:** GETLLAR, PUTLLC (`do_putllc`), PUTLLUC (`do_putlluc`) and `reservation_check` in `Cell/SPUThread.cpp`, with options "Accurate SPU Reservations" and "Accurate SPU DMA".
- **Plain stores do not bump the stamp.** SPUs detect them by data compare. The exception is the Insomniac `stw 0xAAAAAAAA` hack.
- **"Accurate Cache Line Stores":** `dcbz` becomes an atomic 128-byte store (`do_cell_atomic_128_store`).
- **"PPU Reservation Priority Over SPUs"** only adds a busy wait in the range lock.

## SPU

- **Thread:** `spu_thread` (`Cell/SPUThread.h`): LS (mirrored in host memory by `map_ls`), 16-entry MFC queue, mailboxes, signals, events, decrementer. Types are threaded, raw and isolated.
- **Float environment:** `spu_thread::cpu_task` sets **round toward zero and always flushes denormals** (`fesetround(FE_TOWARDZERO)`, `gv_set_zeroing_denormals()`).
- **Channels and MFC:** `get_ch_value`, `set_ch_value`, `do_dma_transfer`, `do_list_transfer`, `process_mfc_cmd`, `stop_and_signal`.
- **Decoders:**
  - Interpreter: `Cell/SPUInterpreter.cpp`. The precise variant is compiled out with `#if 0`.
  - ASMJIT: `Cell/SPUASMJITRecompiler.cpp`.
  - LLVM: `spu_llvm_recompiler::compile` in `Cell/SPULLVMRecompiler.cpp`, plus the fast `spu_fast` path and background workers.
- **Block size (Safe/Mega/Giga):** the analysis in `spu_recompiler_base::analyse` (`Cell/SPUCommonRecompiler.cpp`). Larger blocks are faster, but assume more about control flow.
- **SPU cache:** `cache/<title>/spu-<blocksize>-v1-tane.dat`.
- **SPURS:** `libsre` is LLE by default. The HLE PPU side is in `Cell/Modules/cellSpurs.cpp`; the SPU kernel HLE is disabled.

### XFloat accuracy (SPU single precision)

| Mode | What it does |
|---|---|
| Accurate | Registers that may hold floats are kept as f64x4 (`xfloat_to_double`, `double_to_xfloat`, `xfloat_in_double`). Clamps to ±Smax, flushes values below Smin to +0. Closest to hardware (extended range, no Inf/NaN), slowest. |
| Approximate (default) | f32 math with clamp helpers (`clamp_smax`), zero×NaN masking in FM, clamping in FS |
| Relaxed | Approximate handling only for FCGT, FNMS, FREST, FRSQEST |

- The recompiler states that it does not produce extended-range values the way the hardware does (`SPULLVMRecompiler.cpp`, near the FCGT `safe_int_compare` logic).
- The SPU FPSCR (`fscrrd`/`fscrwr`) is not emulated, `fi` has a TODO, and DFCGT/DFTSV are fatal.
- **Symptom pattern:** lighting, physics or animation values that blow up or vanish only on some SPU jobs → try "SPU XFloat Accuracy: Accurate" first.

## LV2 and HLE modules

- **Syscalls:** table `g_ppu_syscall_table` (`Cell/lv2/lv2.cpp`). Files: `sys_memory`, `sys_mmapper`, `sys_ppu_thread`, `sys_mutex`/`sys_cond`/`sys_lwmutex`/`sys_lwcond`, `sys_event`, `sys_timer`, `sys_fs`, `sys_rsx`.
- **Scheduler:** `lv2_obj::sleep`/`awake`/`schedule_all`. The run queue is limited by "PPU Threads" (default 2). "Thread Scheduler Mode" only sets host affinity.
- **HLE vs LLE:** `g_prx_list` (`Cell/lv2/sys_prx.cpp`, 1 = HLE) sets the default per firmware module; for example libgcm_sys is LLE and libsysutil_savedata is HLE. "Libraries Control" (`name:lle` / `name:hle`) overrides it. HLE modules live in `Cell/Modules/`, for example `cellGcmSys.cpp` (RSX setup, `cellGcmMapMainMemory`).

## Memory map (guest)

| Range | Use |
|---|---|
| `0x00010000`–`0x0FFF0000` or `0x1FFF0000` | main (executable, heap), size by SDK version |
| `0x10000000`–`0xC0000000` | user memory, mapped on demand in 256 MB slots |
| `0xC0000000` (256 MB) | RSX local memory ("video") |
| `0xD0000000` | thread stacks (guarded) |
| `0xE0000000` | raw SPU LS and problem state (`+n*0x100000`); threaded SPU LS at `0xE8000000 + id*0x40000` |

- **Host side:** `g_base_addr` is the protected 4 GB guest space, and `g_sudo_addr` an unprotected mirror (`vm::lock_sudo` pins it, which fails harmlessly with a low `RLIMIT_MEMLOCK`). `g_exec_addr` holds the PPU function cache.
- **RSX IO mapping:** `rsx_iomap_table` is filled by `sys_rsx_context_iomap` in 1 MB entries; `rsx::get_address` resolves RSX offsets.

## Savestates and debug options

- **Savestates:** `utils::serial`, version registry in `savestate_utils.cpp`. Written in `Emulator::Kill(..., savestate)` as zstd `.SAVESTAT`. A savestate needs a compatible build and settings.
- **Debug options:**
  - "PPU Debug", "SPU Debug" (writes `spu.log`/`spu-ir.log` to the cache), "MFC Debug", "Save LLVM logs".
  - GDB server at 127.0.0.1:2345.
  - "PPU Calling History" records only with the interpreter.

## Hardware facts

Sources are listed in [rsx.md](rsx.md#sources).

Tags as in [rsx.md](rsx.md#hardware-facts): HW / DOC / INF / UNCONFIRMED.

### PPE (PowerPC 2.02 + VMX)

- **Instructions:** fsqrt(s), fres, frsqrte and fsel exist. fre, frsqrtes and popcntb are not implemented and raise illegal instruction (DOC, CBE Handbook A.2).
- **fres error:** at most 1/256 relative error (DOC). RPCS3 uses the measured table.
- **Non-IEEE mode (FPSCR[NI]):** fdiv/fsqrt drop from 74/84 to 56/66 cycles, within 1 ULP in round-to-nearest and round-to-zero (DOC).
- **VSCR at process start is `0x00010000`, so NJ = 1 (HW, RPCS3 PR #19795).** With NJ = 1, denormal VMX inputs and results become signed zero (DOC).
  - The interpreter honours NJ since Oct 2026.
  - LLVM ignores accurate NJ and uses "Set DAZ and FTZ" instead (see [the accuracy table](#ppu-accuracy-options)).
- **Estimates:**
  - Architectural bounds: vrefp/vrsqrtefp ≤ 1/4096, vexptefp/vlogefp ≤ 1/16 (DOC).
  - RPCS3 has hardware-exact tables measured on a PS3 since PR #19783 (Oct 2026), behind "PPU Accurate Vector Estimates". They are needed by lockstep games (PES 2013 online).
- **VMX graphics rounding mode (HID1):** SPU-like semantics (round toward zero, no Inf/NaN). It is off under GameOS (INF from the NJ-dependent dumps).
- **Differences from the 970:** lvlx/lvrx/stvlx/stvrx exist only on Cell, the dst/dss stream instructions are no-ops, and VMX does not touch FPSCR (DOC).

### SPU

- **Single precision is not IEEE (DOC, SPU ISA v1.2 §9):**
  - Exponent 255 is an ordinary number: no NaN or Inf. Range is 2^-126 to ≈ 6.8e38 (`0x7FFFFFFF`), and overflow saturates to ±max.
  - Denormal operands count as 0, and underflow gives +0 (a zero result is always +0).
  - The only rounding mode is **round toward zero**.
  - Exception flags per slot: OVF, UNF, DBZ, DIFF.
  - RPCS3 emulates this fully only with XFloat "Accurate". Approximate and Relaxed run IEEE f32 with clamps.
- **Estimates:** frest/frsqest return a base (13-bit fraction) plus a step; `fi` interpolates, and one Newton step after `fi` reaches single-precision accuracy (DOC). RPCS3's `fi` is marked TODO.
- **Double precision:** IEEE format with four rounding modes per slot, exceptions accumulate in FPSCR without traps, and denormal operands may be treated as zero (DOC). RPCS3 does not emulate the SPU FPSCR.
- **Symptom guide:**
  - Values that become Inf/NaN on the host where hardware saturates, or tiny values the hardware flushes to zero, often show up as black or white pixels, exploding vertices, or vanishing lighting computed on the SPUs.
  - Try XFloat Accurate before suspecting the RSX.
