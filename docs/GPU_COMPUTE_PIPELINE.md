# GPU Compute & Image Pipeline (item #1 — real buffers / images / pipelines)

This document records the real Vulkan execution path added in expansion round 6
and how to verify it. It is the "buffers/images/pipelines فعلية" deepening: the
emulator now has a genuine GPU compute path that allocates real Vulkan
resources, dispatches on a device, and reads results back — not a CPU stand-in.

## The gap this closes

The legacy `VulkanRendererBackend` (`src/gpu/vulkan_backend.cpp`) creates a
`VkInstance`, `VkPhysicalDevice`, `VkDevice`, queue and command pool, and even a
`VkShaderModule` from compiled SPIR-V. But it never creates a `VkPipeline`, any
`VkBuffer`/`VkDeviceMemory`, or any descriptor set. Because `m_current_pipeline`
is always `VK_NULL_HANDLE`, every `DispatchCompute` / `DrawAuto` takes the
`else` branch and runs a **CPU simulation**:

```cpp
void VulkanRendererBackend::ComputeShaderSimulation(uint32_t gx,uint32_t gy,uint32_t gz){
    ... m_framebuffer[i] ^= 0x001F0000;   // an XOR over a memory framebuffer
}
```

So "GPU compute" produced no GPU-side buffers, no descriptor binding, and no
readable results. Item #1 asks for that to become real.

## What was added

### `VulkanComputeExecutor` (`include/gpu/vulkan_compute_executor.hpp`, `src/gpu/vulkan_compute_executor.cpp`)

A self-contained, reusable executor that owns a Vulkan instance + logical device
+ compute queue for its lifetime. It uses the official `<vulkan/vulkan.h>` ABI
(not the hand-rolled subset in `vulkan_backend.hpp`).

**Compute path** — `RunRDNA2` / `RunRDNA2Float` / `RunSpirv`:

| Stage | Vulkan calls |
|-------|--------------|
| Shader | `vkCreateShaderModule` (from `RDNA2ComputeCompiler` SPIR-V) |
| Layout | `vkCreateDescriptorSetLayout` (2× `STORAGE_BUFFER`), `vkCreatePipelineLayout` |
| Pipeline | `vkCreateComputePipelines` → a real `VkPipeline` |
| Buffers | `vkCreateBuffer` ×2 (in/out SSBO), `vkAllocateMemory` (host-visible\|coherent), `vkBindBufferMemory` |
| Descriptors | `vkCreateDescriptorPool`, `vkAllocateDescriptorSets`, `vkUpdateDescriptorSets` |
| Dispatch | `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdDispatch(ceil(n/64),1,1)` |
| Submit | `vkQueueSubmit`, `vkQueueWaitIdle` |
| Readback | `vkMapMemory` on the output buffer → results copied out |

The compute compiler's kernel seeds `v0` from `in_data[gid]` and stores final
`v0` to `out_data[gid]` (set 0, bindings 0/1, `local_size_x = 64`), so the
executor's descriptor layout matches the emitted SPIR-V exactly.

**Image path** — `ClearImage`:

`vkCreateImage` (2D RGBA8, device-local) → `vkAllocateMemory` +
`vkBindImageMemory` → `vkCmdPipelineBarrier` (`UNDEFINED→TRANSFER_DST`) →
`vkCmdClearColorImage` → `vkCmdPipelineBarrier` (`→TRANSFER_SRC`) →
`vkCmdCopyImageToBuffer` into a host-visible staging buffer → `vkMapMemory`
readback. This exercises the image + image-memory + layout-transition + transfer
machinery that buffers alone do not.

**Honest degradation:** the real body is compiled only under
`#if __has_include(<vulkan/vulkan.h>)`. Without the loader headers the executor
still links and returns `ComputeExecStatus::Unavailable` — it never fabricates
results.

## Verification

`tests/vulkan_compute_executor_test.cpp` (wired into `make unit`), 16 checks:

```
[info] Vulkan device: llvmpipe (LLVM 19.1.7, 256 bits)
  [ok] sqrt kernel: 128/128 lanes correct on GPU
  [ok] square kernel: 128/128 lanes correct on GPU
  [ok] identity kernel: exact bitwise readback matches input
  [ok] sqrt(in*in) chain: 128/128 lanes correct on GPU
  [ok] VkImage clear+readback: all 1024 texels match on GPU
16/16 checks passed
>> [PASS] Real Vulkan compute executor verified (GPU readback asserted).
```

The sandbox has a Mesa **lavapipe** (`lvp`) ICD, so the dispatch runs on a real
(software) Vulkan device and the readback values are asserted exactly. On the
user's machine the same code binds to the physical GPU's ICD unchanged.

Build detection: the Makefile sets `HAVE_VULKAN` from
`/usr/include/vulkan/vulkan.h` and links `-lvulkan` (`VK_LIBS`) for the executor
and the full prototype.

## Integration into the HLE submit path (completed)

The executor is no longer standalone: it is now wired into the exact path a real
game's compute submission takes.

```
GraphicsDriverSubmit* (HLE)
  -> Graphics::Gpu::SubmitCompute            (HeadlessGpuBridge)
  -> PM4VulkanTranslator (PKT3_DISPATCH_DIRECT)
  -> TryRealComputeDispatch:
       read COMPUTE_PGM_LO/HI  -> shader GVA
       read COMPUTE_USER_DATA  -> input/output SSBO GVAs + element count
       GpuGuestMemory.ReadDwords(shader), ReadDwords(input)
       VulkanComputeExecutor.RunRDNA2(...)   <-- REAL GPU dispatch
       GpuGuestMemory.WriteDwords(output)    <-- results back to guest memory
```

Wiring pieces:
- `include/gpu/gpu_guest_memory.hpp` - `GpuGuestMemory` abstraction (read/write
  guest dwords). Real emulator plugs in `VmmGpuMemory`
  (`include/gpu/vmm_gpu_memory.hpp`, VMM protection-checked copies); tests use a
  flat buffer.
- `PM4VulkanTranslator::BindComputeExecutor(executor, memory)` - when both are
  bound, `PKT3_DISPATCH_DIRECT` runs real compute; otherwise the legacy
  `DispatchCompute()` path is used unchanged (full back-compat).
- `HeadlessGpuBridge::EnableRealCompute(guest_memory)` - on the next
  `InitializeGpu`, the bridge brings up a `VulkanComputeExecutor` and binds it,
  so `SubmitCompute` from the HLE driver executes real GPU compute.
- ProsperoLayer compute ABI (in `graphics/guest_gpu/pm4.h`): `COMPUTE_PGM_LO`
  (0x60, shader GVA >> 8), `COMPUTE_PGM_HI` (0x64), `COMPUTE_USER_DATA_0` (0x240)
  = input SSBO lo/hi, output SSBO lo/hi, element count.

Verification:
- `tests/pm4_real_compute_integration_test.cpp` (12 checks) - a PM4 DISPATCH
  ring drives the executor through the translator; output read back **from guest
  memory** (64/64 lanes correct on the device). Also asserts the unbound
  translator still uses the legacy path.
- `tests/hle_real_compute_submit_test.cpp` (7 checks) - the full HLE-style
  `HeadlessGpuBridge::SubmitCompute` path with `EnableRealCompute`: a compute ACB
  with real descriptors executes `out = in*in` on the GPU, verified via guest
  memory (64/64 lanes).

Remaining boundary:
- The DRAW (graphics-pipeline) path still uses the legacy backend; only the
  COMPUTE path is wired to the real executor so far. Graphics pipelines are the
  next GPU deepening.
- No GPU is required to *build*; a device is required only to assert live
  readback (the tests skip value asserts and stay green when absent).

## Round 19: resource tables on the real dispatch + the graphics pipeline

### The MUBUF/SMEM resource-table ABI (phase 1)

The compiler has understood MUBUF (per-descriptor SSBOs) and SMEM (scalar
mirror) since round 18; round 19 plumbs the guest's tables into the dispatch.
The ABI (documented in `graphics/guest_gpu/pm4.h`):

- `COMPUTE_USER_DATA_0 + 5..6` (compute) / `SPI_SHADER_USER_DATA_VS_0 + 8..9`
  (draw) = the resource-table GVA (lo, hi). Zero keeps round-18 behaviour.
- The table in guest memory: dword 0 = buffer count N (1..8); dwords 1..2 =
  the SMEM mirror-window base (0 = none); dword 3 = the window size in
  dwords; then N entries x 4 dwords (buffer base lo/hi, size dwords,
  idxen stride dwords >= 1).

`PM4VulkanTranslator::ParseResourceTable` validates the table fail-closed
(any malformed entry drops the WHOLE table). `VulkanComputeExecutor::
RunRDNA2WithResources` stages the buffers + mirror from guest memory
(`LoadResourceContents`), dispatches with the per-descriptor SSBOs bound at
the compiler's bindings (mirror at 2, descriptors after it; the SMEM mirror
base is pushed as a push-constant block -- one module serves any window),
and writes stored-to buffers back (`StoreResourceContents`). When no device
serves the dispatch, the SAME program + tables run on the honest GCN
software interpreter; the guest-visible result is identical either way.

Verification: `tests/pm4_resource_dispatch_test.cpp` (101 checks). The
acceptance check compares the final guest state (output SSBO + the
MUBUF-modified buffer) against a direct `GcnSwExecutor` reference
**dword-for-dword** -- a hardware-vs-software value comparison on a Vulkan
host, a full plumbing validation on a headless one.

### The VkGraphicsPipeline raster path (phase 2)

On top of the round-18 register-derived CB_COLOR0 binding:

- `Pm4::CbInfoToGuestColorFormat` converts CB_COLOR0_INFO to
  `Pm4::GuestColorFormat`, whose values ARE the real `VkFormat` numbers
  (R8G8B8A8_UNORM = 37). NOTE: Liverpool programs the FORMAT field with the
  SQIMG DataFormat enum (8_8_8_8 = 10, per shadPS4's regs_color.h) -- not
  the PC-GCN COLOR_* enum (3) that round 18 assumed.
- `RDNA2ComputeCompiler` options `emit_vertex_stage` recompiles the guest
  VS as a Vertex-stage module (gl_VertexIndex lane model, gl_Position +
  Location-0 colour out, plus the out-SSBO dump), and
  `BuildPassthroughFragmentShader()` emits the minimal passthrough FS.
- `VulkanComputeExecutor::DrawVerticesToGuest` builds the full pipeline:
  colour VkImage (CB extent + converted format) + optional D32_SFLOAT depth
  attachment, a LOAD-semantics render pass (the guest's current planes are
  uploaded -- merge behaviour like the software rasterizer), the guest VS as
  the vertex stage, dynamic viewport/scissor from PA_CL_VPORT (a negative
  guest YSCALE becomes Vulkan 1.1's negative-height y-flip), the depth test
  from DB_DEPTH_CONTROL.ZFUNC (numeric identity with VkCompareOp), then the
  rendered planes + transformed vertices are read back into guest memory.
- Opt-in at the translator (`SetGraphicsRasterEnabled`, default OFF) so
  every existing caller keeps the round-18 behaviour on any host; the
  production `HeadlessGpuBridge` enables it. ANY missing piece declines
  with a recorded reason and the software rasterizer runs unchanged.

Verification: `tests/vk_graphics_pipeline_test.cpp` (78 checks) -- the
conversions, both module structures, the register binding -> target
conversion (viewport numbers included), and the end-to-end fail-closed
contract (attempted -> declined -> the software rasterizer renders the same
pixels; default-off is byte-identical).

Updated boundary: on a Vulkan host with a graphics queue, register-bound
draws rasterize through the real pipeline; everywhere else the software
rasterizer serves (honest, flagged). Remaining deliberately-next items are
in CHANGES.md round 19.

## Round 28: MIMG — images become first-class dispatch resources

Before round 28, "images" in this document meant the `ClearImage` smoke test.
Now guest TEXTURES ride the full dispatch path:

- **Compiler**: `LowerMimg` lowers `IMAGE_SAMPLE[_L/_LZ/_B/_D]`,
  `IMAGE_GATHER4[_LZ]`, `IMAGE_LOAD[_MIP]`, `IMAGE_STORE`,
  `IMAGE_GET_RESINFO` and all 13 `IMAGE_ATOMIC_*` opcodes. Each image
  resource is THREE descriptor slots (sampled image `Sampled=1`, storage
  image `Sampled=2`/Rgba32ui, sampler) after the buffer SSBOs. All sampling
  is `OpImageSampleExplicitLod` (bit-identical to the single-mip model and
  valid in compute); atomics go through `OpImageTexelPointer` +
  `OpAtomic*`.
- **Resource table**: the round-19 table grows an optional image section
  (magic `"IMGE"`, 6-dword entries: base GVA / width / height / mips /
  reserved). Old tables parse byte-identically.
- **Executor**: per image a real `VkImage` (R32G32B32A32_UINT,
  SAMPLED|STORAGE usage), one `VkImageView` for both descriptor slots, a
  NEAREST+CLAMP_TO_EDGE `VkSampler` and a staging buffer; upload and
  download ride the same command buffer as the dispatch, so MIMG
  stores/atomics land in guest memory.
- **Semantics contract** (hardware == software, by construction): images are
  raw-dword RGBA32UI, sampling is NEAREST+CLAMP_TO_EDGE, OOB fetch returns
  zero, and the descriptor index is `srsrc/4 - 1` on BOTH paths (the
  software fallback seeds the SGPR quads from the table). The parity test
  (`gpu_mimg_test`) proves it dword-for-dword.
