# Vulkan depth compare tests

Small compute tests for host GPU behaviour that RPCS3 relies on when it samples RSX depth textures.

- `dref`: does the driver clamp the reference value (`Dref`) to [0,1] per depth format? The image is cleared to depth 1.0 and compared with LESS_OR_EQUAL, inside the texture and on white/black borders.
- `plain`: a non-Dref read (`sampler2D`) through a sampler with `compareEnable = VK_TRUE`. RPCS3 does this for depth textures with zfunc NEVER/ALWAYS or alpha kill.

```bash
glslc --target-env=vulkan1.1 dref.comp -o dref.spv
glslc --target-env=vulkan1.1 plain.comp -o plain.spv
gcc -O1 -o dref dref.c -I<Vulkan-Headers>/include -lvulkan
gcc -O1 -o plain plain.c -I<Vulkan-Headers>/include -lvulkan
./dref dref.spv 1.0
./plain plain.spv 0.5 0      # compare op: 0 NEVER, 3 LESS_OR_EQUAL, 7 ALWAYS
```

## Results

| GPU / driver | dref | plain |
|---|---|---|
| RTX 4090, NVIDIA 615.78 (2026-11-08) | D16/D24 UNORM clamp, D32F (S8) do not | compare ignored, raw depth returned |

Add a row when you run it on another GPU or driver.
