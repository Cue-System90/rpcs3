# RSX in RPCS3

Code locations are relative to `rpcs3/Emu/RSX/`, as of upstream `e61d5eaf0` (Nov 2026). Line numbers drift; search for the function name.

## Hardware in one paragraph

The RSX is an NVIDIA G70/NV47-family GPU (NV40 3D class `0x4097`). The CPU writes commands into a FIFO in main or local memory and advances PUT; the RSX fetches from GET. Each command is a method header (register offset, count, increment flag) plus arguments, or a jump/call/return. Textures and render targets live in 256 MB local memory (guest `0xC0000000`) or in main memory mapped through the IO table. Vertex and fragment programs are NV40 microcode; fragment programs live in memory and are fetched per draw. See [Hardware facts](#hardware-facts).

## Pipeline map

### Command flow

- **FIFO:** `FIFO_control::read` (`RSXFIFO.cpp`) decodes headers: count in bits 18–28, non-increment flag, jump/call/return. `fetch_u32` does strict PUT/GET with "RSX FIFO Fetch Accuracy".
- **Main loop:** `thread::run_FIFO`.
  1. It stores the register with `rsx_state::decode` (latches the previous value).
  2. It marks draw state through `draw_state_signals`.
  3. It calls `methods[reg]` (table `s_methods_init` in `rsx_methods.cpp`; handlers in `NV47/HW/*.cpp`).
- **Draws:** `nv4097::set_begin_end` (`NV47/HW/nv4097.cpp`), then `core_renderer::begin`/`end` (`Core/RSXCoreRenderer.cpp`). In order this runs `decode_draw_state`, `resolve_programs`, `resolve_pipeline_state`, the backend `end_draw`, and `post_draw_hook`.
- **Vulkan draw:** `VKGSRender::begin_draw`, `resolve_sampler_state` (`load_texture_env`), `end_draw` (`VK/VKDraw.cpp`).
- **Flip:** `flip_command`, then `thread::on_frame_end`, which also runs RSX capture and FIFO reordering. The display engine (`Core/RSXDisplayEngine.cpp`) handles flips and vblank.

### Memory

- **Address resolution:** `rsx::get_address` (`Utils/rsx_utils.cpp`). Local memory base is `0xC0000000`; MAIN goes through `rsx_iomap_table` (`NV47/FW/RSXIOMap.hpp`), which `cellGcmMapMainMemory` and `sys_rsx_context_iomap` fill.
- **CPU writes to GPU-cached memory:** pages are protected, and a write faults into `handle_access_violation` (`Utilities/Thread.cpp`). From there `VKGSRender::on_access_violation` → `texture_cache::invalidate_address` → `invalidate_range_impl_base` discards, marks dirty, or flushes.
- **Flush to CPU memory:** `texture_cache::flush_set` / `flush_all` / `flush_if_cache_miss_likely`, `cached_texture_section::flush`.
- **Write Color/Depth Buffers:** lock framebuffer memory (`lock_memory_region`), so CPU reads trigger a flush. **Read Color/Depth Buffers:** initialise new render targets from guest memory (`render_target::initialize_memory` → `load_memory`, `VK/VKRenderTargets.cpp`).
- **ZCull/reports:** `RSXZCULL.cpp` (`ZCULL_control`). **Semaphores/labels:** `NV47/HW/nv406e.cpp`, `write_gcm_label`.
- **Tiling:** `get_tiled_memory_region` (`NV47/FW/context.cpp`), only with "Handle RSX Memory Tiling".

### Surface store (render targets)

- **Layout:** `get_framebuffer_layout` (`Core/RSXGraphEngine.cpp`) builds the framebuffer layout from the registers, including the aliasing heuristic.
- **Binding:** `surface_store::prepare_render_target` → `bind_surface_address`. A colour surface and a depth surface at the same address evict each other.
- **Inheritance:** `set_old_contents`/`inherit_surface_contents` (`Common/surface_utils.h`). The data is copied in `render_target::memory_barrier`.
- **Resolution scale:** `apply_resolution_scale` (`Utils/rsx_utils.h`). Surfaces smaller than "Minimum Scalable Dimension" are not scaled.
- **Texture read of a surface address:** `get_merged_texture_memory_region` (`Common/surface_store.h`), then `check_framebuffer_resource` / `process_framebuffer_resource_fast` (`Common/texture_cache_helpers.h`). It is either sampled directly or copied into a temporary subresource.

### Texture cache

- **Entry point:** `texture_cache::upload_texture` (`Common/texture_cache.h`). `fast_texture_search` tries, in order:
  1. compressed formats by dimension,
  2. the bound render target (cyclic reference),
  3. local cached sections,
  4. the surface cache,
  5. a clip shortcut (skipped in strict mode).

  On a miss, it uploads from CPU memory.
- **Temporary subresources:** `create_temporary_subresource`, cached in `m_temporary_subresource_cache`; ops in `deferred_request_command`. Upstream PR #19653 fixed this cache returning a view with the wrong channel remap.
- **Channel remap:** `fragment_texture::decoded_remap` (`RSXTexture.cpp`) forces float formats to the identity remap. `apply_swizzle_remap` maps ZERO/ONE to Vulkan component swizzles. **Depth formats always get an RRRR mapping; the remap is ignored** (`apply_component_mapping_flags`, `VK/VKTextureCache.cpp`).
- **Blit engine (nv3089):** `image_in` (`NV47/HW/nv3089.cpp`), then on the GPU path `upload_scaled_image`. "Disable Blit Engine Upscaling" and "Force CPU Blit" switch between these.

### Shaders

- **Fragment programs:**
  - Fetch and invalidation: `prefetch_fragment_program` / `invalidate_fragment_program` (`Core/RSXGraphEngine.cpp`).
  - Decompiler: `FragmentProgramDecompiler.cpp`.
  - Texture opcodes: `handle_tex_srb`. Shadow textures map to `TEX2D_SHADOW` and friends with an `.xxxx` swizzle; depth read as colour maps to `DEPTH_RGBA`.
- **GLSL helpers:** macros in `Program/GLSLCommon.cpp` (`getFunctionImpl`, `insert_glsl_legacy_function`) and snippets in `Program/GLSLSnippets/RSXProg/`. Per-texture parameters are in `sampler_info` (`RSXDefines2.glsl`), filled in `get_current_fragment_program` (`Core/RSXGraphEngine.cpp`).
- **Constants:** patched per draw (`write_fragment_constants_to_buffer`). NaN/Inf constants are zeroed as a host workaround, while real hardware keeps them.
- **Precision:**
  - `ClampValue` implements the RSX precision modifiers.
  - fp16 is used only at "Shader Precision: Low".
  - On NVIDIA a small floor bias works around interpolation rounding (`low_precision_rounding`).

### Vulkan sampler state (`load_texture_env`, `VK/VKDraw.cpp`)

- **Depth compare:** enabled for every `CELL_GCM_TEXTURE_DEPTH*` format, with the op from `tex.zfunc()` (reversed).
- **Wrap modes:** `vk_wrap_mode` (`VK/VKFormats.cpp`). All mirror-once variants become MIRROR_CLAMP_TO_EDGE.
- **Border colour:** set only for border-clamped textures; otherwise opaque black. Without `VK_EXT_custom_border_color` it falls back to the closest standard colour. Vulkan border colours bypass the view swizzle, so RPCS3 pre-remaps them when needed. AMD is the exception: it is assumed to remap them together with the view (`vkutils/device.cpp`).
- **Filtering:** linear only if the format supports it. The LOD bias is rounded to 0.5 when the mipmap mode is nearest (samplers are immutable).

### Format mapping (`VK/VKFormats.cpp`)

| RSX | Vulkan |
|---|---|
| surface z16 / texture DEPTH16 | D16_UNORM |
| surface z16 float / texture DEPTH16_FLOAT | D32_SFLOAT (memory format is E4M12, not fp16) |
| surface z24s8 / texture DEPTH24_D8 | D24_UNORM_S8_UINT if supported (NVIDIA, Intel), else D32_SFLOAT_S8_UINT (AMD) |
| surface z24s8 float / texture DEPTH24_D8_FLOAT | D32_SFLOAT_S8_UINT |
| "Force High Precision Z buffer" | hides D24 support, so everything is D32F |

## Depth textures and shadow compare

How a fragment-program read of a depth texture is emulated (`get_current_fragment_program`, `Core/RSXGraphEngine.cpp`):

- **Shadow lookup:** formats DEPTH16/DEPTH24_D8 (and their float variants) with zfunc strictly between NEVER and ALWAYS and no alpha kill. The shader uses `samplerXShadow` and the hardware compare.
- **Plain read:** the same formats with zfunc NEVER/ALWAYS or alpha kill. The shader uses `texture()`, but the Vulkan sampler still has compare enabled. **Tested on RTX 4090 / driver 615.78: the compare is ignored and the raw depth is returned**, which is formally undefined in Vulkan and may differ on other drivers.
- **Depth read as colour:** A8R8G8B8/D8R8G8B8 bound on a depth surface is "redirected", bitcast in the shader (`RSXFragmentTextureDepthConversion.glsl`).
- **Emulated shadow compare** (`_EMULATED_TEXSHADOW`): used only when the GPU lacks D24 (`VKFragmentProgram.cpp`: `emulate_depth_compare = !d24_unorm_s8`) or with "Emulate Extended Depth Range". It clamps the reference to ≤1 for non-float depth ("simulate UINT buffer behavior") and keeps it for float depth.
- **Native shadow compare (NVIDIA path):** `texture(sampler2DShadow, vec3(uv, z))` without clamping, so it relies on the host clamping `Dref`.

**Host behaviour, tested on RTX 4090 / driver 615.78** (test programs in [host-tests/vk_depth_compare](host-tests/vk_depth_compare/)):

| Format | Dref > 1 against texel 1.0 (LEQUAL) | Clamped? |
|---|---|---|
| D16_UNORM, D24_UNORM_S8 | pass (lit) | yes |
| D32_SFLOAT, D32_SFLOAT_S8 | fail (shadow) | no |

The border colour acts as depth 1.0 (white) or 0.0 (black) in the compare.

**Consequence:** on NVIDIA a fixed-point depth texture behaves like RSX for `z > 1`. Anything that ends up as D32F (float depth surfaces, "Force High Precision Z buffer", AMD) does not clamp. That matches RSX only if the game really uses float depth.

## Readback latency (measured with GT5)

- A CPU read of a GPU-written surface (a protected, "flushable" texture cache section) runs in the faulting thread: `VKGSRender::on_access_violation` posts a flush request, waits for the RSX thread to submit its command buffer (`on_local_task`), then copies the section's readback buffer into guest memory (`texture_cache::flush_all` → `cached_texture_section::flush` → `imp_flush`).
- The flush predictor records speculative GPU→CPU copies for sections that are read back regularly (`flush_if_cache_miss_likely`), so the copy itself is usually done. In GT5 (EU 02.11, in race) a PPU thread reads three 16x8 BGRA8 render targets in rotation, one per frame, two frames after drawing them; the copy took about 0.15 ms, but the thread waited about 9.2 ms for the RSX thread.
- Cause: the RSX thread sleeps in the frame limiter (`display_engine::handle_emu_flip`, one `lv2_obj::wait_timeout` for the rest of the frame) and only services flush requests afterwards. With a fast host GPU that is most of a frame.
- Fork change: the frame limiter sleeps in 0.5 ms slices and services pending flush requests in between (`wait_for_frame_limit`), and a flush whose readback has already completed on the GPU (`dma_fence` event set) is finished without any queue sync (`texture_cache::flush_all_if_ready`). Measured: PPU wait 58.6 % → 3.4 % of one core, 9.3 ms → 0.57 ms per readback.
- Frame limit "Auto" (`frame_limit_type::_auto`) limits flips to `vblank_rate` (60.00 Hz) and does not wait for vblank, while the vblank thread runs at 59.94 Hz with "Vblank NTSC Fixup". GT5 normally paces itself on vblank (16.683 ms frames). After one long frame (48 ms in the race savestate) its frames run at exactly 16.666 ms, i.e. the RPCS3 limiter paces them, and the PPU reads its 16x8 luminance targets ~2.5 ms after the RSX thread recorded their copy instead of ~13 ms (one flip) later. The copy is already submitted (blits with a DMA transfer flush the queue), but the GPU has not reached it yet, so every read waits ~4 ms for the GPU event (26 % of one PPU thread). With "PS3 Native" (`_ps3`, flips at vblank like the hardware) GT5 stays on its vblank pacing after the long frame and every readback is ready (~14 ms old): no waits in the race, 30 fps lock unchanged after the race. Recommended setting for GT5.
- A CPU texture upload over memory that still holds a flushable render target makes the RSX thread read the target back first, while holding the texture cache lock. GT5 after a race, and with depth of field on the starting grid: the game builds a blur chain with NV3089 blits (256x64, 128x32, 64x16, 32x8 into consecutive rows at `0xc94c8780`, pitch 2048). The blits declare a 512x512 destination, so RPCS3 creates a 1 MiB render target that also covers a 1024x1024 DXT5 texture at `0xc9507f80`. Each frame the blit re-locked the target, that invalidated the DXT5 texture, and its re-upload read the whole target back (~25 ms GPU sync, ~32 fps). The readback never changed a byte: the GPU writes only the rows of the blits.
- Fork change: surfaces record which memory the emulated RSX wrote (`gpu_written_range`/`gpu_written_all` in `render_target_descriptor`: draws, clears and inherited contents mark everything, blits only their destination range). The CPU upload skips the writeback when no overlapping flushable surface wrote the texture range. Measured after the race at 300 % scale: 31.8 → 41.7 fps, PPU fault wait 72 % → 0.1 %, dropped audio periods 27 → 0 per minute. What remains is GPU load: the 4090 runs at 99 % in this scene at 300 % (49 % in the race), 250 % gives ~53 fps, 200 % gives 60 fps. These savestates were made with the community patch "Unlock FPS", which removes the game's own 30 fps lock in this scene (`li r11, 2` at `0x185460` in 02.11 sets vsync interval 2 outside the race). With the lock kept: without the change ~27 fps (frames 37–38 ms) and dropped audio periods; with it a steady 30 fps (33.4 ms frames), GPU 82 %, no dropped audio periods. The race runs at 60 fps with and without the patch.

## Known divergences noted in the code

- **Border colour:** SNORM border colours are biased as on hardware, `(x-128)/127` (`VKDraw.cpp`, `RSXTexture.cpp`).
- **Polygon offset:** RSX depth scale is 2^24-1, so the bias is halved for D24 on NVIDIA.
- **Float textures** cannot be remapped on hardware, so they are forced to the identity remap.
- **NaN constants** are zeroed (host workaround).
- **Interpolation:** NVIDIA rounding breaks floor/equality tricks, so a bias is applied. Precision modifier behaviour comes from hardware tests.
- **Depth data in memory:**
  - depth read as colour is byteswapped BGRA,
  - D24S8 has reversed block endianness,
  - D16_FLOAT is E4M12 and saturates for `z > 1`.
- **MSAA** is not resolved by the host; the game resolves in software, as on hardware.
- **Depth bounds** are saturated to the depth format's range, "as hardware does".
- **Flip semaphore** behaviour differs from hardware (`nv406e.cpp`).

## Accuracy options (Video)

| Option | Main code |
|---|---|
| Write Color/Depth Buffers | `VKGSRender::on_framebuffer_bound`, `texture_cache::lock_memory_region` |
| Read Color/Depth Buffers | `render_target::initialize_memory`, `needs_cpu_upload` |
| Strict Rendering Mode | `force_strict_fbo_sampling`: copies instead of direct sampling, no clip shortcut |
| Framebuffer Aliasing Heuristic Bias | `surface_store.h`, `get_framebuffer_layout` |
| Disable ZCull Occlusion Queries / Relaxed ZCULL Sync / Accurate ZCULL stats | `RSXThread.cpp`, `RSXZCULL.cpp` |
| Resolution Scale / Minimum Scalable Dimension / Disable Blit Engine Upscaling | `RSXThread.cpp`, `texture_cache.h` |
| Emulate Special Depth Comparison | ROP depth test EQUAL emulation (`RSXGraphEngine.cpp`), **not** texture shadow compare |
| Emulate Extended Depth Range | float depth encoding (`VKHelpers.cpp`, `nv4097.cpp`) |
| Force High Precision Z buffer | hides D24 (`vkutils/device.cpp`) |
| Shader Precision | fp16 at Low, precise DP3 (`VKFragmentProgram.cpp`, decompiler) |
| Handle RSX Memory Tiling / Strict Texture Flushing / Force CPU Blit / Use GPU texture scaling | `NV47/FW/context.cpp`, `texture_cache.h`, `nv3089.cpp`, `texture_cache.cpp` |

## Debug tools

- **RSX capture and replay:** see [debugging.md](debugging.md#rsx-captures). Recording happens in `thread::on_frame_end`, replay in `rsx_replay_thread::cpu_task` (`Capture/rsx_replay.cpp`).
- **RSX debugger:** `rpcs3qt/rsx_debugger.cpp`, per-draw state from `thread::capture_frame`.
- **"Log shader programs":** writes GLSL (despite the `.spirv` extension) to `<cache>/shaderlog/FragmentProgramN.spirv`.
- **"Debug output":** enables the Vulkan validation layer.

## Hardware facts

Tags: **HW** = published real-PS3 test, **DOC** = vendor or reverse-engineered docs that agree, **INF** = inference, **UNCONFIRMED** = no source found. The RSX is an NV47/G70 hybrid, so NV40 documents apply by inference only.

### Depth textures and shadow compare

- **Depth formats cannot be read raw (HW, DOC).** On a PS3, DEPTH24_D8, DEPTH24_D8_FLOAT, DEPTH16 and DEPTH16_FLOAT return `[0,0,0,0]` for min/mid/max texels when no compare is set (`-1.0078` with REMAP_BIASED). kd-11: "PS3 cannot actually read depth formats directly (without comparison set)" [RPCS3#18171]. Nouveau agrees for NV30/NV40 [mesa nv30_fragtex.c].
  - **Divergence:** RPCS3 returns the raw depth value in that case (plain `texture()` path above). Upstream treats it as irrelevant, since games read depth through colour aliases instead. What zfunc ALWAYS returns: UNCONFIRMED.
- **zfunc is TEX_ADDRESS (`0x1A08 + 0x20*i`) bits 28–31 (DOC).** libgcm names mean "texel OP ref", the reverse of GL's "ref OP texel"; RPCS3 reverses them (`get_compare_func(..., true)`).
- **Compare is ignored on colour formats (HW).** A8R8G8B8 with zfunc GEQUAL and texel 0 renders black on a CECH-4021B [RPCS3#11912, 2026-08-01].
- **Filtering:** with LINEAR, the hardware does a 4-tap PCF, and the shadow result has 8-bit precision (DOC for GeForce 3–7, INF for RSX).
- **Output:** the result is replicated to all four channels (INF); RPCS3 uses `.xxxx`.
- **Reference clamp for fixed-point depth: no published PS3 test.** kd-11 (2019) says the clamping D24 host compare "matches PS3 behaviour" [RPCS3 commit 463b1b2]. Float depth references are not clamped in RPCS3's emulated path. Lower clamp and float behaviour: UNCONFIRMED.
- **Border colour:** register `0x1A1C + 0x20*i`, packed ARGB8 (DOC). TEX_FORMAT bit 3 selects border texels in the image (0) or the register (1). How an 8-bit border compares against 24-bit depth: UNCONFIRMED.
- **Coordinates outside [0,1]: no RSX data** (UNCONFIRMED).

### Depth storage (HW, RPCS3 depth_formats test, issue #19726 / PR #19758)

- **Z24S8:** depth in bits 31–8, stencil in bits 7–0.
- **D24FS8:** unsigned e8m16, bias 127 (fp32 bits 30–7). 0.5 → `0x7E0000`, 1 → `0x7F0000`, 2 → `0x800000`. Float depth has unrestricted range; Z in the tens of thousands is common [RPCS3 PR #8741].
- **D16F:** unsigned e4m12, bias 16. Max `0xFFFF` = 0.99987793, and z ≥ 1 saturates.
- **Float depth switch:** CONTROL0 (`0x03B0`) bit 12.
- **Depth bounds:** converted to the surface's native values before the test.

### Texture unit registers (DOC: PSL1GHT gcm_sys.h, mesa nv30-40_3d.xml.h)

- **TEX_FORMAT (`0x1A04`):** bits 8–15 hold the libgcm format byte, e.g. A8R8G8B8 5, DXT1/23/45 6/7/8, DEPTH24_D8 0x10, DEPTH16 0x12, X16 0x14, RGBA16F 0x1A, RGBA32F 0x1B. LN (0x20) means linear, UN (0x40) means unnormalised/rect.
- **TEX_ADDRESS (`0x1A08`):**
  - Wrap S/T/R in bits 0–3/8–11/16–19: 1 repeat, 2 mirror, 3 clamp-to-edge, 4 border, 5 clamp, 6/7/8 mirror-once edge/border/clamp.
  - Unsigned remap in bits 12–15 (NORMAL 0, BIASED 1).
  - Gamma in bits 20–23.
  - zfunc in bits 28–31.
- **CONTROL1 (`0x1A10`), the remap:**
  - Bits 0–7: four 2-bit source selects (A, R, G, B).
  - Bits 8–15: four 2-bit modes (ZERO 0, ONE 1, REMAP 2). `0x00` → all 0, `0xAA` → pass-through, `0x55` → all 1 (HW).
- **Remap with conversion flags (HW, RPCS3#18171):**
  - ZERO reads −1.0078 with BIASED and −1.0 with BX2.
  - Precedence is SIGNED > BX2 > GAMMA > REMAP_BIASED; ARGB8 SIGNED aliases REMAP_BIASED.
  - RPCS3 matches this since PRs #18170/#18192.
- **FILTER (`0x1A14`):** LOD bias, min filter in bits 16–19, mag filter in bits 24–27, per-channel SIGNED in bits 28–31. fp32 formats cannot be filtered. Anisotropy goes from 1× to 16×.

### Fragment program ISA (DOC for NV3x/NV4x, INF for RSX)

- **Encoding:** four 32-bit words per instruction, inline constants follow.
  - Word 0: end bit 0, destination bits 1–6, half output bit 7, CC update bit 8, writemask bits 9–12, input bits 13–16, texture unit bits 17–20, precision bits 22–23 (fp32/fp16/fx12), opcode bits 24–29, saturate bit 31.
  - Colour output is R0, depth is R1.z.
- **Registers:** R0–R31 are fp32, H0–H63 are fp16, fx12 covers [−2, 2).
  - fp32 denormals flush to zero; fp16 keeps them.
  - Overflow gives ±INF, and NaN becomes 0 in fx12.
- **Special functions:** RCP/RSQ/EX2/LG2/SIN/COS are accurate to ≥22 bits. RCP(±0) = ±INF; RSQ of a negative number is NaN.
- **Condition codes:** LT/EQ/GT/UN per component. KIL discards when any swizzled component passes.
- **Texture opcodes:** TEX 0x17, TXP 0x18, TXD 0x19, TXL 0x2F, TXB 0x31. NV40 adds IF/LOOP/REP/CAL/RET/BRK, NRM, DIV, DP2.
- **NaN/Inf behaviour on RSX:** UNCONFIRMED; RPCS3 zeroes NaN/Inf constants.

### FIFO and memory (DOC: envytools, psdevwiki)

- **Method header:** count in bits 18–28, subchannel in bits 13–15, method in bits 2–12, non-increment flag `0x40000000`.
- **Control commands:** old jump `0x20000000 | addr`, jump `addr | 1`, call `addr | 2`, return `0x00020000`.
- **Control registers:** PUT, GET and REF (REF starts at `0xFFFFFFFF`).
- **Subchannels:** 0 Curie `0x4097`, 1–2 M2MF, 4 swizzled surface, 6 image-from-CPU `0x3089`, 7 SCE driver methods (run by LV1).
- **Memory:** local memory is 256 MB; the top 4 MB is reserved for RAMIN/RAMHT/RAMFC/objects. Locations: 0 local, 1 main, 2 report.
- **RT_FORMAT:** colour in bits 0–4, zeta in bits 5–7 (Z16 1, Z24S8 2), type in bits 8–11 (linear 1, swizzled 2).
- **Reports:** SetReport `0x1800`, clear `0x17C8`. Types: ZPASS_PIXEL_CNT 1, ZCULL_STATS 2–5.
- **"Disable ZCull Occlusion Queries" in RPCS3** writes 0 into every report (`thread::get_zcull_stats`), so a game that reads pixel counts on the CPU sees every query as fully occluded.

## Sources

- [RPCS3#18171](https://github.com/RPCS3/rpcs3/issues/18171) – PS3 hardware tests: texel remap, conversion flags, depth reads
- [RPCS3#11912](https://github.com/RPCS3/rpcs3/issues/11912) – GT5/GT5P shadow darkening; hardware test of the 1×1 placeholder state (2026-08-01)
- [RPCS3#19726](https://github.com/RPCS3/rpcs3/issues/19726), [PR #19758](https://github.com/RPCS3/rpcs3/pull/19758) – depth storage formats on hardware
- [RPCS3 PR #8741](https://github.com/RPCS3/rpcs3/pull/8741), [commit 463b1b2](https://github.com/RPCS3/rpcs3/commit/463b1b220d1d87fd7d154a7dc386a4630abf24d7) – float depth range, D24 compare
- [PSL1GHT gcm_sys.h](https://github.com/ps3dev/PSL1GHT/blob/master/ppu/include/rsx/gcm_sys.h) – libgcm register and enum layout
- Mesa nouveau nv30: [nv30-40_3d.xml.h](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gallium/drivers/nouveau/nv30/nv30-40_3d.xml.h), [nv30_texture.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gallium/drivers/nouveau/nv30/nv30_texture.c), [nv30_fragtex.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gallium/drivers/nouveau/nv30/nv30_fragtex.c), [nvfx_shader.h](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gallium/drivers/nouveau/nv30/nvfx_shader.h)
- [envytools DMA pusher](https://envytools.readthedocs.io/en/latest/hw/fifo/dma-pusher.html) – FIFO command format
- [NV_fragment_program](https://registry.khronos.org/OpenGL/extensions/NV/NV_fragment_program.txt), [NV_fragment_program2](https://registry.khronos.org/OpenGL/extensions/NV/NV_fragment_program2.txt) – NV3x/NV4x fragment ISA semantics
- [NVIDIA GPU Programming Guide (GeForce 6/7)](https://developer.download.nvidia.com/GPU_Programming_Guide/GPU_Programming_Guide.pdf), [shadow mapping paper](https://developer.download.nvidia.com/assets/gamedev/docs/shadow_mapping.pdf) – hardware shadow maps, PCF
- psdevwiki [RSX](https://www.psdevwiki.com/ps3/RSX), [RSX FIFO Commands](https://www.psdevwiki.com/ps3/RSXFIFOCommands) (behind a challenge page; Wayback copies work)
- [Cell Broadband Engine Handbook v1.1](https://ilab.usc.edu/packages/cell-processor/docs/CBE_Handbook_v1.1_24APR2007_pub.pdf), [VMX PEM for Cell](https://web.archive.org/web/20120905080457/https://www-01.ibm.com/chips/techlib/techlib.nsf/techdocs/C40E4C6133B31EE8872570B500791108/%24file/vector_simd_pem_v_2.07c_26Oct2006_cell.pdf), [SPU ISA v1.2](https://web.archive.org/web/20070801195223/http://cell.scei.co.jp/pdf/SPU_ISA_v12.pdf)
- [RPCS3 PR #19783](https://github.com/RPCS3/rpcs3/pull/19783), [PR #19795](https://github.com/RPCS3/rpcs3/pull/19795) – measured VMX estimate tables and VSCR at process start
