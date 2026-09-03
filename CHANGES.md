
# Round 30 — the REAL binary: parse a genuine PS5 eboot.bin (SELF), HLE
# dynamic linking, and Minecraft's CRT running inside the emulator

The user supplied a real 254,075,005-byte PS5 `eboot.bin` (debug/devkit
image, unencrypted). This round reverse-engineers its container format from
the actual bytes, builds the parser + flattener into the emulator, and —
driven by what the real image needed — implements HLE dynamic linking so
the game's own CRT startup executes on our CPU with live kernel/libc calls.

## 30a. The real container: Prospero SELF (magic 54 14 F5 EE)

Format facts established from the binary (all asserted by
tests/self_parser_test.cpp against the real file):

* Header: magic `54 14 F5 EE`, version, mode, `aux` u32 @0x0C, tail-table
  u64 @0x10, entry count u16 @0x18, then N x 32-byte chained segment
  entries at 0x20 (flags / offset / size / size-dup — the dup is an
  integrity invariant, the chain is 16-byte aligned).
* The data segments (flags & 0xFF00 == 0x2800) pair 1:1, BY EXACT SIZE,
  with the embedded ELF's PT_LOAD program headers — 5 loads + the
  PT_SCE_DYNDATA (PATHX) module path.
* The embedded ELF is a real ELF64/LE/x86-64, OSABI 9 (FreeBSD),
  **e_type 0xFE10 (ET_SCE_DYNEXEC)**, 0x4000 alignment, entry 0x80.
* The PATHX segment carries the devkit build path:
  `D:/a/_work/1/s/build/ps5_x64/Minecraft/Minecraft.Sony/Publish/
  Minecraft.Sony.elf` — the image is Minecraft PS5.
* Payload is PLAINTEXT (a code-prologue + printable-DT_NEEDED verdict),
  so no decryption is ever needed — this project still refuses ciphertext
  retail images (fail-closed, tested).

New module: `include/loader/prospero_self.hpp` + `src/loader/prospero_self.cpp`
(windowed reads only — the 192 MB text segment is never loaded to parse).
`FlattenSelfToElf` streams the paired segments into a bootable flat ELF
(e_shoff stripped — the container carries no section headers) via 1 MiB
chunks. `prospero-run eboot.bin` now parses, validates, flattens to
`eboot.bin.flat.elf`, and boots that through the REAL pipeline.

## 30b. Sony e_types and the loader gaps the real image exposed

* `GuestLauncher::Boot` accepts ET_SCE_DYNEXEC/EXEC/DYNAMIC as PIE-style
  (load bias, like ET_DYN) — Minecraft is PIE with ~877k relocations.
* Zero-permission PT_LOADs (Sony debug-data loads at unaligned vaddr) are
  skipped for VMM mapping but stay in the image for symbol resolution.
* The entry point now receives a zeroed 4K module-args page: the real
  entry starts `mov r14d,[rdi]` — a NULL rdi faults on instruction one.

## 30c. HLE dynamic linking (the big one)

The real image's first PLT call faulted at a link-time resolver address:
the runtime linker had no provider for its 877 imports, so every GOT slot
kept Sony's lazy-binding default. Three mechanisms landed:

1. **Dynsym through DT_HASH.** The real image places .dynstr BEFORE
   .dynsym, which breaks the size heuristic; nchain in DT_HASH is the
   SysV-standard symbol count (877 here).
2. **Versioned import names.** PS5 dynsym names look like `NID#D#E`; the
   '#' suffix is stripped before the SymbolDatabase lookup (bare NIDs —
   394 of the 877 resolve against our 1573 registered ones).
3. **Guest trampolines.** `cpu/hle_trampoline.*`: every resolved import
   gets an executable stub `mov eax,<magic+id>; syscall; ret` near the
   arena top. BOTH engines route the magic syscall to the host function
   with the guest's own argument registers (SysV). 306 relocations now
   resolve to 294 distinct trampolines.

Bugs found by the real binary along the way (all fixed + tested): the
W^X flip after the first stub made every later stub write fail (only ONE
import resolved); the stub's syscall was encoded 0F 34 (sysenter!) instead
of 0F 05; the magic number must not set the x32 bit (bit 30) or a leaked
stub syscall kills the host with SIGSYS; and the direct backend only arms
entry traps for DIRECT branch targets, so freshly emitted stubs are now
pre-discovered through a registered hook (native-speed HLE calls).

## 30d. Where the real game actually gets (honest walls)

`prospero-run eboot.bin --interpreter`: SELF parsed (12 entries, 6
pairings, 50 DT_NEEDED) -> flattened ELF -> 632,633 relocations applied
(306 imports via trampolines) -> 4 PT_LOADs mapped (240 MB) -> TLS ->
DT_INIT runs -> entry calls the REAL libc surface: `MtxInitWithName`,
`__cxa_atexit`, `PthreadGetthreadid`, `init_env`, `atexit` (twice) —
Minecraft's CRT startup executing on the interpreter with live syscalls.
The current walls, stated plainly: 568 import NIDs belong to libraries
without HLE coverage yet (libSceAgc — the PS5 graphics driver — and
friends), and the game's first unresolved PLT call faults at the honest
link-time address. Guest-fault telemetry now reports status/RIP/fault
address for every failed run.

## 30e. Tests

* `self_parser_test` (94 checks): synthetic container (parse + flatten +
  BOOT through GuestLauncher, exit 42), a six-way corruption matrix
  (fail-closed), and the REAL eboot.bin structural facts + flatten
  round-trip (skipped when the fixture is absent).
* `hle_plt_test` (7 checks): versioned NID -> relocation -> trampoline ->
  syscall -> host function -> exit code; unknown NIDs skipped honestly.
* Full suite: `make unit` EXIT=0, **4,433 checks** across 55 suites.

# Round 29 — the audit round: fix every finding, boot games for real, and
# expand CPU/GPU/PM4/HLE

The user audited v28 with an actual build-and-run pass (not documentation
reading) and found three classes of problems. Every one is fixed here, and
the four requested expansions are real.

## 29a. The audit findings (all fixed)

1. **"52 test binaries" was wrong.** The Makefile had 48 suites in
   UNIT_TESTS (49 test variables defined) and the `unit` recipe executed
   only 46. The count is now audited every run: **53 suites, all executed**
   (5 new this round), and the recipe lists them 1:1 with UNIT_TESTS.
2. **Two suites were built but never run.** `cpu_simd_diff_test` (passed,
   2190 checks) and `gpu_image_flat_test` (**7 real failures hiding in
   plain sight** — three test-side encoding bugs: a missing memory-bridge
   argument, `v_mov` encodings without the opcode bits, and the constant 1
   written as src0=1 which means SGPR1 instead of the inline constant
   encoding 129). Both are wired into `unit` and both are green.
3. **Seven dead test files** (GoogleTest-era remains referencing `.h`
   headers that no longer exist: cpu_extended, vmm_extended,
   pm4_indirect_draw, pm4_memory_sync, software_rasterizer_scissor,
   test_simple, and the v19-era main.cpp) — removed. The integrated
   self-test `main.cpp` is rewritten (see 29c).
4. **`prospero-run` was a 31-line stub.** It printed "CPU/ABI/GPU execution
   is not enabled" and exited after reading the ELF header. It is now the
   REAL entry point (see 29b).
5. **The seccomp denylist was untouched since round 20** — and worse, the
   round-20 code SILENTLY ignored a failed `prctl(PR_SET_SECCOMP)` (an
   invalid filter would simply never install). Rewritten deny-by-default
   (see 29e).
6. **Round 28's title overclaimed.** Retitled with an explicit correction
   note; its "52 binaries" validation claim corrected in place.

## 29b. `prospero-run` boots games through the real pipeline

New shared boot module (`include/prospero_boot.hpp` +
`src/prospero_boot.cpp`, used by BOTH `prospero-run` and the integrated
self-test): game-folder scan (or a direct ELF path) -> honest header
classification (SELF/encrypted, 32-bit, wrong-arch, non-ELF each get a
precise reason) -> `Libs::InitAll` (the full HLE NID surface, once per
process) -> Vulkan loader pre-warm on the unfiltered main thread ->
`GuestLauncher::Boot` (RuntimeLinker load, relocations, TLS, DT_INIT/
INIT_ARRAY, entry point) with LIVE guest syscalls on the CPUJitEngine.

Two production gaps found and fixed while wiring it:
- **`DirectExecutionBackend::Enable()` was never called outside its own
  test** — the native path was unreachable from any production entry
  point. `RunGame` now enables it (and records the decline reason when the
  preconditions fail, falling back to the interpreter).
- **`dlopen("libvulkan")` on a guest thread** would fail once the seccomp
  guard arms (file-backed mmap is denied there by design). The loader is
  pre-opened on the main thread; a cached handle needs no new mappings.

A real eboot.bin (write syscall + SSE + exit code 42) now reports:
`engine: native-direct, guest syscalls: 1, exit code: 42` — guest code
executing at native host speed with the syscall intercepted and served.

## 29c. The integrated self-test (`tests/main.cpp`) is real now

The old file (titled "PS5 EMULATOR ENGINE v19.0") hand-assembled
instructions and PM4 bytes in-process and never touched the load/link/boot
path — while CHANGES.md leaned on it for the "full prototype links clean"
claim. The rewrite: a real eboot.bin written into a real game directory,
booted through the exact `RunGame()` above (exit code, SSE side effects in
guest memory, syscall traffic, HLE registration asserted; 42), plus a PM4
DISPATCH ring through the real translator + executor, plus the honest
negative path (garbage file refused with a reason). 19 checks;
"ALL ENGINE MODULES FULLY VERIFIED" now means what it says.

## 29d. CPU: the complete x87 engine + BSWAP

Before this round every x87 instruction (opcodes 0xD8..0xDF) failed
closed — ~100 instruction forms absent: loads/stores (m32/m64/m80, BCD),
the full arithmetic set (register + memory + pop forms, correctly decoded
by modrm RANGE as the hardware does: C0-C7 fadd, C8-CF fmul, E0-E7 fsub,
...), compares (fcom/fcomp/fcompp/fucom*/fcomi with the C0/C2/C3
condition codes and FSTSW AX), transcendentals (fsin/fcos/fsincos/fptan/
fpatan/fyl2x/f2xm1/fsqrt/fscale/fprem/frndint/fxtract), constants, stack
ops (fld/fstp/fxch/ffree/fincstp/fdecstp), integer conversions (fild/
fist/fistp with the control-word rounding modes), and the environment
(fldenv/fnstenv/fldcw/fnstcw/fnsave/frstor with the TOP pointer carried
in FSW bits 11..13 and the register image in logical order).

Fidelity: the host IS x86-64, so `long double` is the 80-bit x87 format
and the model matches real x87 bit-for-bit by construction — the test
proves it (m80 round trips bit-identical, frndint follows the loaded
rounding mode, fcomi drives the real EFLAGS). Plus BSWAP (0F C8-CF).
Four engine bugs were found by the test and fixed along the way: the
st(i) index comes from the RM field (not reg), the D8/DC/DE register
forms decode by modrm range, FNSAVE/FRSTOR carry TOP, and the BCD sign
nibble is bit 7 of byte 9 (not a digit).

## 29e. seccomp: deny-by-default with argument-checked gates

The round-20 denylist allowed everything except 19 process-escaping
syscalls — a racing guest syscall (FreeBSD numbers collide with arbitrary
Linux numbers) could still reach open(2)/socket(2)/clone-as-process. The
policy is inverted: a documented ALLOWLIST of the emulator's own surface
(glibc thread setup, VMM/allocator memory, futex, mediated file and
socket I/O, clocks, the budget timer) and EVERYTHING ELSE returns EPERM.
`clone(56)` is allowed only with CLONE_THREAD set; `mmap(9)` only with
MAP_ANONYMOUS; `open(2)` is denied outright (mediation is openat-only).

The rewrite immediately caught a latent bug: the first version's BPF jump
offsets were off by one, the kernel's bounds checker rejected the filter,
and the OLD code's ignored `prctl` return value would have hidden it —
the new code makes an install failure LOUD. `seccomp_guard_test` PROVES
enforcement on a thread that entered native guest execution: allowed
surface still works; open/ptrace/fork/clone-without-CLONE_THREAD and an
arbitrary unlisted number all return EPERM (the ENOSYS->EPERM
discriminator proves default-deny).

Documented residual: guest fork() emulation only works on the interpreter
path (the guard denies host fork by design — fail-closed, not silent).

## 29f. GPU: VOP3P + VINTRP + wavefront ops

- **VOP3P (0xD3 prefix)**: decode with the per-source half selectors;
  `v_mad_mix_f32` executes mixed-precision FMA with correct IEEE binary16
  subnormal normalization (the test caught an off-by-one in the exponent).
- **VINTRP**: v_interp_mov/p1/p2 execute against the EXP parameter
  exports (p2 is the ISA's read-modify-write form), fed by real EXP
  packets in the same program.
- **DS wavefront ops**: ds_bpermute_b32 (cross-lane shuffle) and
  ds_swizzle_b32, plus the integer/float reductions (min_i32/max_i32/
  min_f32/max_f32/add_f32). Two deep fixes came with them: the DS opcode
  decode now covers the real 10-bit space (dual decode keeps the legacy
  8-bit encodings byte-compatible), and lane inputs are seeded for ALL
  lanes before any lane executes (wave ops read peer lanes' VGPRs; lazy
  per-lane seeding served zeros). bpermute captures the source column
  once per instruction (pc-keyed) so later lanes never observe earlier
  lanes' destination writes — simultaneous-wavefront semantics.
- 48 new checks; every existing GPU suite stayed green.

## 29g. PM4: five packets with real semantics

`SET_UCONFIG_REG` stores a real register file (queryable);
`EVENT_WRITE` advances the event counter and publishes the fence value to
guest memory; `EVENT_WRITE_EOS` publishes its immediate payload;
`DMA_DATA` performs REAL dword-granular copies through the guest bridge
(fail-closed on unmapped ranges); `SET_PREDICATION` establishes
conditional rendering — an active failing predication suppresses
subsequent draw packets (the hardware behaviour), verified end to end.

## 29h. HLE: real entropy + real fsync

`RandomGetRandomNumber` (NID PI7jIZj4pcE) was a FIXED-SEED LCG — every
run produced the same "random" stream. It is backed by `getrandom(2)`
now (validated: 251/256 byte values covered across 1024 draws, documented
error contract preserved). `KernelFsync` returns real fsync through the
host descriptor (sockets correctly EINVAL).

## Validation (audited counts)

- `make clean` + full build: EXIT=0 (`prospero-run` and the rewritten
  integrated self-test both link against the full engine).
- `make unit`: **53 suites, every one executed by the recipe** (the two
  orphaned suites included), 0 failures, EXIT=0 — audited totals: the 25
  suites that report the `N checks, M failures` format sum to **3586
  individual checks with 0 failures**, the rest print their own PASS
  lines (recipe order verified 1:1 against the UNIT_TESTS list).
- Integrated self-test: ALL ENGINE MODULES FULLY VERIFIED, EXIT=0.
- New suites this round: `cpu_x87_test` (12), `seccomp_guard_test` (16),
  `gpu_wave29_test` (48), `pm4_events_dma_test` (26), `hle_entropy_test`
  (25) — plus the 19-check integrated self-test rewrite.

## Deliberately next

- The GCN->SPIR-V compiler still fail-closes on VOP3P/VINTRP (they need
  the fragment-stage ABI; the software executor covers them for parity
  testing first).
- x87 status-word exception flags are not modelled (condition codes are).
- The bpermute column cache assumes the shuffled column is not
  divergently rewritten between lanes mid-instruction (documented).

# Round 28 — real MIMG on the GPU path, real POSIX sockets, real save-data
# persistence

RETITLED IN ROUND 29 (honesty correction): this round's original heading
claimed it "closed the four-layer gap" from the user's v26 gap analysis.
It did not. What it actually delivered is listed below and is real: MIMG
sampling/gather/atomics on both GPU executors, the full POSIX socket
backend, and save-data persistence. What it did NOT touch: `prospero-run`
(still printed "CPU/ABI/GPU execution is not enabled"), the seccomp
denylist (still the round-20 list, missing open(2)/socket(2)/process-form
clone), and the CPU opcode surface (unchanged since the SIMD rounds).
Round 29 is the round that actually closes those -- see its section above
this one.

The user's gap analysis (v26) named four layers still far from a real
emulator. This round attacks three of them, in the same fail-closed style
as every earlier round.

## 28a. GPU — full MIMG (image sample / fetch / gather / store / atomics)

The RDNA2->SPIR-V compiler had ZERO image support (the software executor had
a basic model since round 28-of-v26). Now both paths run the same programs:

- **Compiler (`rdna2_compute_compiler.cpp`)**: `LowerMimg` lowers
  `IMAGE_SAMPLE[_L/_LZ/_B/_D]`, `IMAGE_GATHER4[_LZ]`, `IMAGE_LOAD[_MIP]`,
  `IMAGE_STORE`, `IMAGE_GET_RESINFO` and all **13 `IMAGE_ATOMIC_*`** ops.
  Every image resource occupies THREE descriptor slots (sampled image
  `Sampled=1`, storage image `Sampled=2`/Rgba32ui, sampler) after the
  buffer SSBOs -- the exact layout `BuildSkeleton` decorates. All sampling
  lowers to `OpImageSampleExplicitLod` (implicit-LOD is only well-defined in
  Fragment; the single-mip model makes explicit `Lod 0` bit-identical).
  Atomics go through `OpImageTexelPointer` + `OpAtomic*` with Device scope.
  Every new opcode number was verified against KhronosGroup/SPIRV-Headers
  `spirv.h` (OpTypeImage=25, OpSampledImage=86, OpImageFetch=95,
  OpImageGather=96, OpImageWrite=99, OpImageQuerySizeLod=103,
  OpAtomicExchange=229..OpAtomicXor=242). `a16` addressing and the
  fragment-only variants (GET_LOD, SAMPLE_C*, SAMPLE_O/CD/PCK) fail closed
  with explicit messages (the software executor still runs the C/O models).
- **Software executor (`gcn_decoder.cpp`)**: the full variant set with
  hardware-matching semantics -- OOB LOAD returns zero (the old wrap-around
  is gone), SAMPLE is NEAREST+CLAMP_TO_EDGE (the exact sampler the Vulkan
  path binds), GATHER4 gathers the bilinear footprint
  (`floor(u*w-0.5)`), GET_RESINFO reports `(w, h, 1, mips)`, and the
  atomics do real read-modify-write returning the OLD value.
- **Resource-table ABI (`pm4.h` + `pm4_translator.cpp`)**: the round-19
  table grows an optional image section guarded by the `"IMGE"` magic --
  OLD tables parse byte-identically (no magic -> zero images). Image
  entries are 6 dwords (base GVA, width, height, mips, reserved) with full
  fail-closed validation.
- **Vulkan executor (`vulkan_compute_executor.cpp`)**: per image a real
  `VkImage` (R32G32B32A32_UINT, SAMPLED|STORAGE|TRANSFER usage, GENERAL
  layout), one `VkImageView` serving both descriptor slots, a
  NEAREST+CLAMP_TO_EDGE `VkSampler`, and a staging buffer -- upload
  (barrier + `vkCmdCopyBufferToImage`) and download
  (`vkCmdCopyImageToBuffer`) ride the SAME command buffer as the dispatch,
  so MIMG stores/atomics land in guest memory. `RunSpirv` grew an
  `extra_images` parameter; `RunRDNA2WithResources` stages images through
  `LoadImageContents`/`StoreImageContents` (public, unit-testable) and the
  software fallback seeds the SGPR descriptor quads `s[4*(i+1)] = {i, w, h,
  0}` -- the SAME `srsrc = 4*(i+1)` convention the compiler derives
  statically, so hardware and software agree on every program.
- **Parity (the acceptance check)**: `gpu_mimg_test` dispatches a program
  mixing LOAD + SAMPLE + ATOMIC_ADD + STORE through
  `RunRDNA2WithResources` and compares BOTH the output SSBO and the final
  guest image dword-for-dword against a direct `GcnSwExecutor` reference
  run -- on every host (hardware when a Vulkan device exists, the honest
  software interpreter otherwise). The whole executor was additionally
  compiled against the official KhronosGroup Vulkan headers with
  `-Werror` (the round-19 shim trick) -- the new image code is fully
  type-checked against the real API.

## 28b. HLE — the network layer becomes REAL on POSIX

The socket backend (`libs/network.cpp`) was Win32-only: every POSIX branch
returned `ENOSYS` -- the emulator this project builds on Linux had NO
functional sockets at all. Round 28 gives every operation a real POSIX
implementation: `socket` (with the guest non-blocking flag), `bind`,
`connect`, `listen`, `accept`, `shutdown`, `getsockname`, `getpeername`
(new), `getsockopt`/`setsockopt` (with FreeBSD->Linux option-number and
`SO_NBIO` translation), `send`/`sendto`/`recv`/`recvfrom`, `sendmsg`/
`recvmsg` (new; guest msghdr/iov conversion, control data fails closed),
`select` (correct POSIX nfds -- Win32 ignores it), `close`, `socketabort`
(new; shutdown-based), `ioctl` (new; FIONBIO/FIONREAD), the DNS resolver
(`getaddrinfo`), and `inet_ntop` (the old code passed the guest family
value 28 straight to the host, which fails on Linux where AF_INET6=10).
`SetPosixSocketError` now translates Linux errno numbers to the guest's
FreeBSD numbering (the old code passed raw errno through), and
`ConvertMessageFlags` translates the guest MSG_* values (MSG_NOSIGNAL
0x20000 -> 0x4000 on Linux).

The guest-facing surface (`libs/libNet.cpp`) registers the missing data
path with NIDs verified against the public shadPS4 `libSceNet` table:
sceNetConnect/Send/Sendto/Sendmsg/Recv/Recvfrom/Recvmsg/Getsockopt/
Getpeername/SocketAbort/Ioctl/Term/Htonll/Ntohll/EtherStrton (+ the
wrappers Getpeername/Sendmsg/Recvmsg/SocketAbort/Ioctl/EtherStrton/
NetEtherNtostrReal are new backends). `net_sockets_test` drives a FULL
loopback TCP lifecycle through the guest-facing wrappers -- real bytes over
a real kernel socket pair -- plus sockaddr conversion round-trips, select,
ioctl FIONREAD, EBADF/ECONNREFUSED errno translation in FreeBSD numbering,
and the NID registrations.

## 28c. HLE — save data persists through the host filesystem

`SetParam`/`GetParam` were logging no-ops and `SaveIcon`/`LoadIcon`
discarded/returned nothing. `GetRealFilename` was a stub that returned the
guest path UNRESOLVED. Round 28:

- `GetRealFilename` now lives in `file_system.cpp` and resolves guest
  paths through the real mount table (`ResolvePath`) -- `/savedata0/X`
  maps to the host directory `Mount` registered.
- `SaveDataSetParam` writes a documented `PARAM.bin` container (magic
  `"PLSD"`, version, size, raw struct) INSIDE the mounted save directory;
  `SaveDataGetParam` reads it back. Persistence survives umount/remount
  cycles -- verified by `savedata_persist_test`, not just in-memory state.
- `SaveDataSaveIcon` persists the raw icon bytes to `icon0.png` and
  `SaveDataLoadIcon` reads them back bounded by the guest buffer.
- `savedata_persist_test` (39 checks): mount(create) -> SetParam ->
  SaveIcon -> umount -> remount(open) -> GetParam/LoadIcon round-trip ->
  DirNameSearch -> Delete, plus the header layout on the host filesystem
  and fail-closed validation.

## Validation

- `make` (full prototype): links clean, EXIT=0.
- `make unit`: **CORRECTED IN ROUND 29**: the original text here claimed
  "52 test binaries built, every executed suite green". The real numbers
  at the time were **48 suites in UNIT_TESTS (49 test variables defined)
  and only 46 of them executed by the `unit` recipe** -- `cpu_simd_diff_test`
  and `gpu_image_flat_test` were built but never run, and
  `gpu_image_flat_test` was in fact FAILING (7 hidden failures from three
  test-side encoding bugs, fixed in round 29). Round 29 wires every suite
  into the recipe and publishes the audited counts.
- Integrated emulator: `ALL ENGINE MODULES FULLY VERIFIED`, EXIT=0.
- New suites: `gpu_mimg_test` (93), `net_sockets_test` (67),
  `savedata_persist_test` (39).
- Two latent defects fixed along the way: `NetInetNtop` passed the raw
  guest family value to the host on POSIX (AF_INET6=28 fails on Linux),
  and `SetPosixSocketError` leaked Linux errno numbers into the guest's
  FreeBSD error surface.
- `libs/libNet.cpp`/`network.cpp` are now `-Werror`-clean (the two
  non-trivial `memset`s became value-initialisations, aggregate inits
  became explicit field assignments); the one pre-existing `-Wnonnull`
  class in the (unused) Http section is suppressed for the network test
  rule only, with a comment.

## Deliberately NOT done (honest boundary)

- MIMG fragment-stage features stay closed on the hardware path (GET_LOD,
  SAMPLE_C/O/CD/PCK): the compute model has no derivatives and no
  depth-compare descriptor model yet; the software executor implements the
  documented nearest equivalents.
- The graphics-pipeline fragment shader still samples nothing (textures
  are bound on the COMPUTE path only); wiring MIMG into the FS needs the
  vertex-UV ABI first.
- `sceNetSelect`/`sceNetPoll` have no verified NIDs in the public tables,
  so they are implemented in the backend but not registered (the guest
  cannot reach them yet -- fail-closed rather than a guessed NID).



- GPU/GCN software execution: added a 64 KiB software LDS model and basic DS read/write/modify operations with bounds checks.
- SMEM: added `S_BUFFER_LOAD_DWORD`, `S_BUFFER_LOAD_DWORDX2`, and `S_BUFFER_LOAD_DWORDX4` using the existing descriptor table, with compiler and software-executor parity plus descriptor-size validation.
- MUBUF: descriptor-aware byte-range checks now prevent loads/stores beyond the guest buffer declaration; zero stride is handled conservatively.
- Kernel I/O: `dup` now returns the new descriptor, `readv` performs checked host-backed vector reads, `pipe` creates a real host pipe and copies descriptors into guest memory, and `ftruncate`/other file operations no longer report unconditional success.
- Added `gpu_memory_extended_test` and `syscall_io_extended_test`.
- Updated scope/HLE documentation to reflect the newly implemented paths.

Validation: 25/25 CMake/CTest suites pass.

# CHANGES — ProsperoLayer RDNA2 Core (fix + expansion)

This file documents the build fixes and test expansion applied to the
`prosperolayer-rdna2-core` project. Before these changes, the full prototype
target (`make`) failed to link and the integrated emulator test crashed with a
segfault. After the changes, both the dependency-free unit suites and the full
prototype build and run cleanly.

## Verified state after the fix

| Command | Result |
|---------|--------|
| `make` | ✅ links `ps5_native_vulkan_emulator` + `prospero-run` with no undefined symbols |
| `make unit` | ✅ 11 dependency-free suites, all PASS |
| `./ps5_native_vulkan_emulator` | ✅ 7/7 integrated tests, clean exit code 0 |

## Expansion round 2 — real x86-64 execution core (CPU component #1)

The original CPU was a deliberately tiny fail-closed *scanner*
(`x86_64_subset_interpreter`) that only recognised ~7 instruction forms and
never executed control flow. This round adds a genuine execution core so the
CPU actually *runs* guest basic blocks.

**New: `include/cpu/x86_64_interpreter.hpp` + `src/cpu/x86_64_interpreter.cpp`**
A real integer-ISA interpreter over an abstract `GuestMemoryBus`:

* REX prefixes (W/R/X/B), `0x66` operand-size override, segment/lock/rep
  prefixes accepted; 8/16/32/64-bit operands incl. the REX byte-register remap
  (AH/CH/DH/BH vs SPL/BPL/SIL/DIL).
* Full **ModRM + SIB** effective-address decode: `[base + index*scale + disp]`,
  RIP-relative, and register-direct forms.
* **ALU groups** add/or/adc/sbb/and/sub/xor/cmp (r/m↔r, AL/eAX-imm, and the
  0x80/0x81/0x83 group1 immediate forms), `test`, `inc/dec`, `neg/not`,
  `mul/imul/div/idiv` (group3), 2- and 3-operand `imul` (0FAF / 69 / 6B).
* **Data movement**: all `mov` encodings, `movzx`/`movsx`, `lea`, `xchg`,
  `push`/`pop` (reg + imm), `cwde/cdqe`, `cdq/cqo`.
* **Shifts/rotates**: shl/sal, shr, sar by imm8, by 1, and by CL (group2).
* **Control flow**: `jmp`/`call` rel + indirect, `ret`/`ret imm16`, `jcc`
  rel8 and rel32, `setcc`, `cmovcc`, `loop/loope/loopne`, `jrcxz`, `syscall`.
* **RFLAGS** model (CF/PF/AF/ZF/SF/OF) computed with hardware-accurate carry /
  overflow / borrow semantics; all `jcc/setcc/cmovcc` read the real flags.
* **Memory-safe**: every access goes through the bus; a denied access halts with
  `MemoryFault` instead of touching host memory.

**New: `include/cpu/vmm_memory_bus.hpp`** — bridges the interpreter to the live
16 GB `VirtualMemoryManager` arena via its protection-checked
`CopyFromGuest`/`CopyToGuest` paths.

**New: `CPUJitEngine::ExecuteGuestFull()`** — a full-fidelity execution entry
point that runs the extended interpreter directly over the guest arena,
allocates a private guest stack with a return sentinel, and dispatches guest
`syscall` instructions to `ProsperoSyscallDispatcher` using the real x86-64
syscall ABI (number in rax; args in rdi/rsi/rdx/r10/r8/r9).

**New tests (both wired into `make unit`):**

* `tests/x86_64_interpreter_test.cpp` — 26 checks over a flat memory bus:
  add/ret, a real `dec`/`jnz` loop summing 1..10, `call`/`ret`, memory
  store/load, SIB `[base+index*4]` array indexing, `cmp`+`setl`,
  `movzx`/`movsx`, `imul`+`shl`, `push`/`pop`, syscall dispatch, an iterative
  Fibonacci(10), and memory-fault safety.
* `tests/guest_execution_integration_test.cpp` — 11 checks end-to-end over the
  **live VMM arena** through `ExecuteGuestFull`: `(a+b)*2`, a loop summing
  1..100 → 5050, a guest `getpid` syscall dispatched through the real
  dispatcher, a guest-stack round-trip, and fail-closed refusal of a
  non-executable entry point.

The JIT/syscall unit targets were updated to link the new sources (the JIT
engine's full path now references the interpreter and dispatcher).

## Expansion round 3 — ELF load + execute pipeline (shadPS4-style flow)

With a real execution core in place, this round wires the front-to-back path a
shadPS4-style emulator depends on: take an **already-decrypted** x86-64 ELF,
map it, and run it. No firmware or Sony keys are involved — the game/homebrew is
assumed pre-decrypted, exactly as shadPS4 requires.

**New test: `tests/elf_execution_integration_test.cpp` (3 checks)** — synthesises
a minimal valid static ELF64 (Ehdr + one R+X PT_LOAD segment), loads it through
the existing `ElfLoader` into the live 16 GB guest arena, then starts execution
at the ELF entry point via `CPUJitEngine::ExecuteGuestFull`:

* a 7*6 loop program returns 42 from the ELF entry point,
* a program that reaches a helper through a real relative `call`/`ret` returns
  9² = 81,
* a corrupt ELF (bad magic) is rejected by the loader and never executed.

This proves ELF parsing, segment mapping + protection, entry-point validation,
and execution of freshly loaded guest code all cooperate. Wired into
`make unit` (now 12 suites).

## Expansion round 4 — GPU: real RDNA2 → SPIR-V compute kernels

The legacy `ShaderSpirvRecompiler` emits a module whose computed results are
never stored — effectively dead code a driver would optimise away. This round
adds a compiler that emits a **real, data-parallel compute shader with
observable side effects**, which is exactly what a GPU-less host can build and
validate for a real Vulkan driver to consume later.

**New: `include/gpu/rdna2_compute_compiler.hpp` + `src/gpu/rdna2_compute_compiler.cpp`**

* Emits a complete SPIR-V compute module:
  `layout(local_size_x=64)`, an input SSBO (set 0, binding 0) and an output
  SSBO (binding 1), `gl_GlobalInvocationID`-indexed. `v0` is **loaded** from
  the input buffer, the RDNA2 program runs, and the final `v0` is **stored** to
  the output buffer (`OpLoad` / `OpAccessChain` / `OpStore` — not dead code).
* Broader opcode set than the legacy path:
  * VOP2: `V_ADD/SUB/SUBREV/MUL/MIN/MAX/MAC_F32`
  * VOP1: `V_MOV/SQRT/RCP/FLOOR/FRACT/EXP/LOG/RNDNE_F32`
  * SOPP: `S_ENDPGM`
* Same fail-closed contract: uninitialised VGPR, missing `S_ENDPGM`, empty
  input, and unsupported encodings all return typed diagnostics.
* Reports `instruction_count` and `alu_op_count` for the caller.

**New test: `tests/rdna2_compute_compiler_test.cpp` (22 checks)** — compiles
`sqrt(in*in)`, an `add/min/floor/rcp` chain, and a `V_MAC_F32` accumulate;
asserts the module parses cleanly and actually contains `OpLoad`/`OpStore`/
`OpAccessChain` (proving observable I/O), and checks the fail-closed paths. Each
module is dumped to `build/tests/*.spv`.

**External validation:** `make spirv-validate` round-trips every emitted `.spv`
through `spirv-cross`. Confirmed output (decompiled GLSL) for the three kernels:

```
out = floatBitsToUint(sqrt(uintBitsToFloat(in) * uintBitsToFloat(in)))   // sqrt(in*in)
out = rcp(floor(min(in, 1.0 + in)))                                       // extended chain
out = in*in + 2.0                                                          // V_MAC_F32
```

No GPU is required to build or validate this — the emitted SPIR-V is real and a
Vulkan driver on the user's own machine can consume it. Wired into `make unit`
(now 13 suites).

The Vulkan path degrades safely to a software reference framebuffer pipeline
when no GPU ICD is present (as in headless CI / sandboxes).

## Expansion round 5 — HLE (component #3) + expanded syscalls (component #4)

> Documentation note: this round was implemented and verified but never written
> up at the time (no CHANGES.md or report entry existed for it). This section is
> the retroactive record; the code and tests it describes are what shipped in
> the round-5 archive.

### 5a. HLE graphics-driver submit path (item #3)

The modern headless-testable GPU stack (`PM4VulkanTranslator` +
`VulkanRendererBackend`) was not connected to the legacy `Graphics::Gpu`
interface that the HLE `GraphicsDriverSubmit*` entry points call — there was no
headless implementer of that interface, so the HLE submit path had nothing real
to drive.

**New: `include/graphics/host_gpu/headless_gpu_bridge.hpp` +
`src/gpu/headless_gpu_bridge.cpp`** — a headless `Graphics::Gpu` bridge that
forwards guest DCB/ACB command rings from the HLE driver into the real PM4
translator and the real Vulkan backend (which uses a physical GPU ICD when
present — llvmpipe/lavapipe in the sandbox — and degrades to a software
framebuffer otherwise).

**New test: `tests/hle_graphics_submit_test.cpp` (19 checks)** — drives the
bridge (the object the HLE driver holds as `g_renderer`) with real PM4
draw/compute rings: submit counts, draw call reached, guest viewport applied,
interrupts, compute dispatch, truncated-ring rejection, and multi-ring
accumulation. Fixed the `DRAW_INDEX_AUTO` ring builder to supply the 2-dword
payload (vertex_count + draw_initiator).

**New test: `tests/hle_libkernel_test.cpp` (25 checks)** — drives the shipped
libKernel primitives (semaphore, sync-on-address, time) directly through their
guest-facing entry points, including real cross-thread producer/consumer and
futex-style wakeups. Fixed pre-existing `-Werror` unused-parameter warnings in
`src/kernel/time.cpp` (`KernelClockGettime`/`KernelClockGetres`).

### 5b. Expanded syscalls (item #4)

Real gaps found and closed in `src/cpu/prospero_syscalls.cpp`:

* **`kevent` was a pure stub** (ignored the changelist, always reported 0
  events). Implemented real FreeBSD-9 `EVFILT_USER` semantics:
  `EV_ADD/EV_DELETE/EV_ENABLE/EV_DISABLE/EV_CLEAR/EV_ONESHOT` and
  `NOTE_TRIGGER` / `NOTE_FFOR/FFNOP` fflags control. Added the 32-byte
  FreeBSD-9 `GuestKevent` struct layout to `prospero_syscalls.hpp`.
* **`evf_cancel` was declared but unregistered** (silent ENOSYS). Added
  `EventFlag::Cancel` + `EventFlagManager::Cancel` (a cancel-epoch force-wakes
  all waiters, resets the pattern, and reports the pre-cancel pattern) in
  `event_flag.hpp` + `kernel_managers.cpp`, and registered the handler.
* **`open` was declared but unregistered.** Registered a host-fd-backed handler
  that reads the path from guest memory and maps POSIX flags/mode.
* **`evf_open`** was left honestly `ENOSYS` — its ABI conflicts with the
  existing `evf_create` argument layout locked in by tests — and documented as
  such rather than faked.

**New test: `tests/syscall_kevent_expanded_test.cpp` (8 checks)** — real
`kevent` EVFILT_USER lifecycle, `evf_cancel` wakeups, and `open`. Guest
addresses kept within the 16 GB arena; value-initialised `GuestKevent` copied
with `memcpy` to avoid `-Werror=class-memaccess`. No regressions in the
existing `syscall_dispatcher_test`.

After round 5 `make unit` runs **17 suites**, all PASS (`hle_graphics_submit`,
`hle_libkernel` and `syscall_kevent_expanded` joining the previous 14).

## Expansion round 6 — GPU: real Vulkan compute + image pipeline (item #1)

Round 4 emitted real SPIR-V but nothing executed it on a device: the legacy
`VulkanRendererBackend` created a `VkDevice`/queue/command pool but never built
any `VkPipeline`, `VkBuffer` or descriptor set, so every `DispatchCompute` /
`DrawAuto` fell back to a **CPU "simulation"** (an XOR / fill over a memory
framebuffer). That is the exact gap item #1 ("تعميق GPU — buffers/images/
pipelines فعلية") names.

**New: `include/gpu/vulkan_compute_executor.hpp` +
`src/gpu/vulkan_compute_executor.cpp`** — a genuine end-to-end GPU compute path
that performs the full real pipeline and reads results back from GPU memory:

```
RDNA2 bytecode --(RDNA2ComputeCompiler)--> SPIR-V
  -> vkCreateShaderModule
  -> vkCreateDescriptorSetLayout (2x storage buffer / SSBO)
  -> vkCreatePipelineLayout + vkCreateComputePipelines   (real VkPipeline)
  -> vkCreateBuffer x2 (input+output SSBO) + vkAllocateMemory + vkBindBufferMemory
  -> vkAllocateDescriptorSets + vkUpdateDescriptorSets
  -> vkCmdBindPipeline + vkCmdBindDescriptorSets + vkCmdDispatch
  -> vkQueueSubmit + vkQueueWaitIdle
  -> map output memory and read the results BACK from the GPU.
```

* **Buffers + pipelines**: `RunRDNA2` / `RunRDNA2Float` / `RunSpirv` compile an
  RDNA2 stream (or take ready SPIR-V), run it over a host-supplied input SSBO
  on a real Vulkan device, and return exactly what the GPU wrote to the output
  SSBO.
* **Images**: `ClearImage` exercises the real `VkImage` lifecycle — create a
  device-local 2D RGBA8 image, allocate + bind image memory, transition
  `UNDEFINED -> TRANSFER_DST` with a pipeline barrier, clear it on the GPU,
  transition `-> TRANSFER_SRC`, `vkCmdCopyImageToBuffer` into a host-visible
  staging buffer, and read the pixels back.
* **Honest degradation**: the real path is compiled only when
  `<vulkan/vulkan.h>` is present (guarded with `__has_include`). On a host with
  no Vulkan loader the executor reports `ComputeExecStatus::Unavailable` — never
  a silently-wrong CPU fake.

**New test: `tests/vulkan_compute_executor_test.cpp` (16 checks)** — on a host
with a Vulkan device (llvmpipe/lavapipe here, a physical GPU on the user's
machine) the readback values are asserted exactly:

* `out = sqrt(in)` — 128/128 lanes correct on the GPU,
* `out = in*in` (V_MUL_F32) — 128/128 lanes correct,
* identity `V_MOV_B32` — exact bitwise readback matches input,
* `out = sqrt(in*in) = |in|` multi-op chain — 128/128 lanes correct,
* compile failure is reported (not faked),
* `ClearImage(32x32)` — all 1024 RGBA8 texels match the requested colour after
  a real GPU clear + image-to-buffer copy.

When no device is present the value checks are skipped and the suite still
passes, so `make unit` stays portable across headless hosts.

**Build wiring:** the Makefile detects `/usr/include/vulkan/vulkan.h`
(`HAVE_VULKAN`) and links `-lvulkan` for this executor and the full prototype
when present; the new suite is registered in `UNIT_TESTS` and the `unit` recipe.

After round 6 `make unit` runs **17 suites**, all PASS.

### 6b. Wiring the real compute path into the HLE submit path (item #1 completion)

Round 6 left the executor as a *standalone* proven path; a real game's compute
submission still flowed to the legacy `VulkanRendererBackend` CPU fallback. This
sub-round closes that gap end-to-end:

* **New: `include/gpu/gpu_guest_memory.hpp`** — a `GpuGuestMemory` abstraction
  (read/write guest dwords) so the translator can fetch the compute shader and
  SSBOs from guest memory without depending on the VMM directly.
* **New: `include/gpu/vmm_gpu_memory.hpp`** — `VmmGpuMemory`, the real-emulator
  implementer backed by the 16 GB VMM's protection-checked copy paths.
* **`PM4VulkanTranslator::BindComputeExecutor(executor, memory)`** — when both
  are bound, `PKT3_DISPATCH_DIRECT` reads `COMPUTE_PGM_LO/HI` (shader GVA) and
  `COMPUTE_USER_DATA_0..4` (input/output SSBO GVAs + element count) from the
  recorded SH registers, reads the RDNA2 shader + input SSBO from guest memory,
  runs `VulkanComputeExecutor::RunRDNA2` on the real device, and writes the
  output SSBO back. If either is unbound, the legacy `DispatchCompute()` path is
  used unchanged (full back-compat, existing tests unaffected).
* **`HeadlessGpuBridge::EnableRealCompute(guest_memory)`** — on the next
  `InitializeGpu` the bridge brings up an executor and binds it, so the HLE
  `GraphicsDriverSubmit*` -> `SubmitCompute` path itself performs real GPU
  compute.
* **ProsperoLayer compute ABI** added to `graphics/guest_gpu/pm4.h`:
  `COMPUTE_PGM_HI` (0x64) and `COMPUTE_USER_DATA_0` (0x240).

**New tests:**
* `tests/pm4_real_compute_integration_test.cpp` (12 checks) — a PM4 DISPATCH
  ring drives the executor through the translator; `out = sqrt(in)` read back
  from guest memory, 64/64 lanes correct. Confirms the unbound translator still
  uses the legacy path.
* `tests/hle_real_compute_submit_test.cpp` (7 checks) — the full HLE-style
  `HeadlessGpuBridge::SubmitCompute` path with `EnableRealCompute`: a compute
  ACB executes `out = in*in` on the GPU, verified via guest memory (64/64).

After round 6b `make unit` runs **19 suites**, all PASS. Item #1 is complete for
the COMPUTE path (guest submit -> real GPU -> guest memory). The DRAW/graphics
pipeline path remains on the legacy backend and is the next GPU deepening.

## Expansion round 7 — audio + input (item #2: libAudio / libPad)

### 7a. Input (libPad)

The pad state machine (`libs/controller.cpp`) was complete and injectable
(`ControllerConnect/Button/Axis` feed state; `PadReadState/PadRead` return it),
but two real defects blocked using/testing it:

* the `KYTY_SUBSYSTEM_INIT(Controller)` hook is `inline` and was never
  linked/called, so `g_controller` stayed null (any `Pad*` call would
  `EXIT_IF`). Added a real, idempotent **`ControllerEnsureInitialized()`** entry
  point (declared in `libs/controller.h`) that the subsystem init now delegates
  to, giving the emulator and tests a linkable way to bring input up.
* a latent `-Werror=sign-compare` in `ReadStates` (comparing `int` to
  `uint32_t STATES_MAX`) — fixed with an explicit cast.

**New test: `tests/hle_libpad_input_test.cpp` (34 checks)** — drives the real
input pipeline through the guest-facing entry points with injected host events:
handle/arg validation, button press/release edge tracking, multiple
simultaneous buttons, analog stick + trigger mapping (incl. the trigger->L2/R2
digital latch), the queued-sample `PadRead` history path, and disconnect
stability. No SDL / no physical gamepad required.

### 7b. Audio (libAudio)

The `AudioOut` backend (`libs/audio.cpp`) is entirely SDL-gated: with no SDL the
whole library is replaced by no-op stubs, so guest audio output silently
disappears and nothing is testable. Added a real, SDL-free core:

* **New: `include/audio/headless_audio_sink.hpp` + `src/audio/headless_audio_sink.cpp`**
  — models a PS5 audio-out port (format / channels / sample rate / grain),
  accepts guest PCM, decodes every supported format (S16 mono/stereo/8ch and
  float variants) to normalized float, and accounts for frames/bytes plus
  per-channel peak, per-call RMS and stream duration. The real emulator can feed
  an SDL device from the same decoded stream on a host that has one; headless
  hosts still run and verify.

**New test: `tests/headless_audio_sink_test.cpp` (55 checks)** — format/byte
math, open validation, exact int16 and float decode, a known sine wave whose
measured RMS matches amplitude/√2, and multi-grain streaming with frame +
duration accounting.

After round 7 `make unit` runs **21 suites**, all PASS.

## Expansion round 8 — semicolon hygiene fix + headless AudioOut wiring (item #2 completion)

### 8a. KYTY_CLASS_NO_COPY extra-semicolon hygiene fix

`KYTY_CLASS_NO_COPY` (`include/common/subsystems.h`) is defined with a trailing
`;`, so every call site of the form `KYTY_CLASS_NO_COPY(X);` expands to a
doubled `;;` (an empty member declaration at class scope). Four sites shipped
with the extra semicolon:

- `libs/controller.cpp` (`GameController`) — the only one exposed so far: round
  7's `hle_libpad_input_test` was the first test to compile that file
- `libs/ajm.cpp` (`AjmDecoder`)
- `libs/audio.cpp` (`Audio`)
- `libs/network.cpp` (`Network`)

Honest compiler note: the doubled semicolon is valid C++11+ (empty member
declaration), and GCC 14.2 with the project's exact test flags
(`-std=c++20 -Wall -Wextra -Wpedantic -Werror`) compiles it clean —
`hle_libpad_input_test` has been green since round 7. It is nonetheless a
portability hazard: clang's `-Wextra-semi` (and stricter CI toolchains)
reject exactly this pattern, and the macro's own trailing `;` is the root
cause. Fix applied: removed the redundant `;` at **all four** call sites (the
macro definition is untouched, so both calling conventions keep compiling).

Warning for future rounds: `libs/ajm.cpp`, `libs/audio.cpp` and
`libs/network.cpp` are excluded from the SDL-less / FFmpeg-less build today
(the Makefile filters them out with their optional backends), so **no test or
target currently compiles them** — the fix above is verified by inspection
and the controller.cpp precedent, not by the test suite. They are documented
here so the first round that links any of them does not rediscover the
pattern as a surprise failure.

### 8b. Headless AudioOut wiring (audio sink -> AudioOutOutput entry points)

Round 7 built the SDL-free `HeadlessAudioSink` but nothing fed it: without
SDL2 the whole AudioOut surface disappeared (`libs/audio.cpp` and
`libs/libAudio.cpp` excluded by the Makefile, `InitAudio_1` a no-op), so a
headless guest could not resolve a single `sceAudioOut*` symbol.

**New: `libs/audio_headless.cpp` + `libs/audio_headless.h`** — a real SDL-free
AudioOut backend, compiled only when SDL2 is absent (the same gating pattern
as `optional_stubs_sdl.cpp`):

* Implements the guest-facing `Libs::Audio::AudioOut` entry points declared in
  `libs/audio.h` (`AudioOutInit/Open/Output/Outputs/Close/GetPortState/
  SetVolume`) over a 32-port table of `HeadlessAudioSink`s: 1-based handles,
  `OUT_PORTS_MAX = 32`, the same port-type / param-format validation as the
  SDL backend.
* `AudioOutOutput` / `AudioOutOutputs` submit full grains of interleaved guest
  PCM to the sink — real S16/float decode, frame/byte accounting, RMS/peak
  measurement, and the captured float stream a host device (or test) consumes.
  `AudioOutOutputs` validates the whole batch before any sink side effect
  (all-or-nothing, like the PM4 path).
* Registers the **same seven NIDs** `libs/libAudio.cpp` registers under SDL2
  (`JfEPXVxhFqA`, `ekNvsT22rsY`, `b+uAV89IlxE`, `w3PdaSTSwGE`,
  `QOQtbeDqsT4`, `s1--uE9mBFw`, `GrQ9s4IrNaQ`), so the guest
  `dynlib_dlsym` path resolves the identical symbol set on a headless host.
* Divergence from the SDL path, documented in-tree: conditions the SDL path
  handles with `EXIT_NOT_IMPLEMENTED` (non-zero index, unknown format) return
  typed Sony errors here instead, so every rejection path stays testable.
* `libs/optional_stubs_sdl.cpp`: `InitAudio_1` now calls the headless
  registration instead of being a no-op. The SDL configuration is untouched —
  `audio_headless.cpp` is not compiled when SDL2 is present.
* Makefile: `src/audio/*.cpp` joins `CORE_SRCS` — the sink is now a real
  dependency of the full prototype, not a test-only unit.

**New test: `tests/hle_audio_headless_test.cpp` (87 checks)** — NID resolution
through `SymbolDatabase::FindSymbol` (the actual `dynlib_dlsym` lookup),
including pointer identity (the `AudioOutOutput` NID resolves to this
backend's entry point); fail-closed open/output validation; a full-scale
750 Hz S16 stereo sine whose measured RMS matches amplitude/sqrt(2) with
exact frame/byte/duration accounting; float stereo passthrough; multi-port
`AudioOutOutputs` with all-or-nothing rejection (a closed handle rejects the
whole batch with zero side effects on the valid port); port-state reporting
(MAIN/VOICE output/channel mapping); volume; close/reopen with slot reuse;
and port-budget exhaustion at 32 (`AUDIO_OUT_ERROR_PORT_FULL`).

After round 8 `make unit` runs **22 suites**, all PASS; the full prototype
builds and links with the headless audio path (`libs/audio_headless.o` +
`src/audio/headless_audio_sink.o` in the final link) and the integrated test
stays green (7/7, clean exit).

## Expansion round 9 — DRAW path on the real GPU + runtime_linker / event_queue deepening

Both "deliberately next" items from round 8, executed together. Three real
latent bugs were found and fixed along the way — all in subsystems no test had
ever exercised. After this round `make unit` runs **25 suites**, all PASS;
the full prototype builds and the integrated test stays green (7/7, clean
exit). 157 new checks across three new dependency-free suites.

### 9a. The DRAW path on the real GPU (item: "only COMPUTE was wired")

Round 8's compute wiring left every PM4 draw packet on the legacy
CPU-sim `DrawAuto()` counter. The draw state packets
(`PKT3_INDEX_BASE` / `PKT3_INDEX_TYPE` / `PKT3_INDEX_BUFFER_SIZE` /
`PKT3_NUM_INSTANCES`) were consumed as no-ops.

* **`include/graphics/guest_gpu/pm4.h`** — the ProsperoLayer draw ABI is now
  documented in-tree: the vertex-stage program is programmed like the compute
  program through `SPI_SHADER_PGM_LO_VS` / `HI_VS` (0x2A/0x2B, same
  256-byte `lo << 8` alignment), its buffers through
  `SPI_SHADER_USER_DATA_VS_0` (0x250: input lo/hi, output lo/hi, element
  count), and the index encoding through `INDEX_TYPE_U16` / `INDEX_TYPE_U32`.
* **`PM4VulkanTranslator::TryRealDrawDispatch()`** — when the executor +
  guest-memory binding is present, `DRAW_INDEX_2` / `DRAW_INDEX_OFFSET_2` /
  `DRAW_INDEX_AUTO` now read the vertex-stage RDNA2 program from guest
  memory, build the per-lane input stream (indexed draws decode the index
  buffer at `INDEX_BASE` — u16 zero-extended or u32; non-indexed draws pass
  the attribute buffer through), execute one lane per element on the real
  device via `VulkanComputeExecutor::RunRDNA2`, and write the transformed
  vertices BACK to guest memory. Attribute fetching (real VGT fetch shaders)
  stays future work and is documented as such. The new `DrawDispatchRecord`
  (indexed / shader / input / output / index_base / index_type / element
  count / instance count / attempted / executed_on_gpu) is exposed for
  diagnostics.
* Fallback discipline is unchanged: no device / unbound translator /
  incomplete descriptors -> legacy `DrawAuto()`, backend counter advances
  exactly once per draw either way. **`PKT3_NUM_INSTANCES` is now honoured**
  by the legacy call too (it was hardcoded to 1).
* **New test: `tests/pm4_draw_realpath_test.cpp` (40 checks)** — u32 indexed
  draw, u16 indexed draw (packed-stream expansion), non-indexed draw; on a
  Vulkan device the transformed vertices are read back FROM guest memory and
  asserted (V_MUL_F32 v0, v0, v0); on a headless host the fallback path and
  record fields are asserted instead. Also covers: unbound translator
  (back-compat), missing VS descriptors, missing INDEX_BASE, NUM_INSTANCES
  recording, and the all-or-nothing guarantee (a tail-truncated stream applies
  zero backend side effects).

### 9b. runtime_linker deepening — three real bugs + a real ELF loader

`src/loader/runtime_linker.cpp` was scaffolding: `RelocateProgram` was a
no-op, `Dlsym`/`FindSymbol` returned null, `ReadFromElf` always returned 0,
and nothing parsed any ELF. Deepening it surfaced a bug CHAIN:

1. **`unique_id` was never assigned** — `LoadProgram` set only `handle`, so
   `KernelLoadModule` (libs/libKernel.cpp) returned handle **0** for every
   loaded module and every subsequent `KernelDlsym` failed with `ESRCH`
   (`FindProgramById(0)` never matches). Fixed: `handle` and `unique_id` are
   both assigned from a new monotonic allocator.
2. **Handle collisions after unload** — the old `size()+1` scheme reuses
   handles as soon as a module is erased (load A=1, B=2, unload A, load C=2
   — colliding with B). Fixed by the same monotonic allocator; a
   load/unload/reload regression test asserts handles are never reused.
3. **Two singletons** — `RuntimeLinker::Instance()` served its own static
   while libKernel used `Common::Singleton<RuntimeLinker>::Instance()`: two
   separate module lists. `Instance()` now routes to the Common::Singleton
   object; a test asserts both accessors return the same instance.

New real behaviour (all bounds-checked, fail-closed):

* **`ElfImage::LoadFromMemory()`** — the linker now OWNS the module image
  (validated ELF64/x86-64, program headers, section headers, section names).
* **`LoadProgram`** parses the export symbol table (.dynsym entries defined
  in the module, names from .dynstr) when the file is a valid ELF; missing /
  non-ELF paths keep the legacy name-only recording so every existing caller
  is unaffected.
* **`RelocateProgram`** applies real x86-64 relocations to the owned image
  in place: `R_X86_64_RELATIVE` (B + A), `R_X86_64_64` / `GLOB_DAT` /
  `JUMP_SLOT` (S + A resolved against the module's own .dynsym; cross-module
  imports are counted and skipped, never faked), through PT_DYNAMIC /
  DT_RELA / DT_JMPREL with vaddr->file-offset translation via PT_LOAD.
  Applied/skipped counters are exposed on ModuleInfo.
* **`Dlsym` / `FindSymbol`** resolve through the export table with the load
  bias; **`ReadFromElf`** reads the PATCHED image back.
* `include/loader/elf.h` gained the missing ELF definitions (Elf64_Sym /
  Elf64_Dyn / Elf64_Rela, SHT_*, DT_*, R_X86_64_*, ST_*, ELF64_R_SYM/TYPE).
* **New test: `tests/runtime_linker_test.cpp` (50 checks)** — builds a
  synthetic ELF64 shared object in memory (program headers, .dynsym/.dynstr,
  .rela.dyn with a RELATIVE entry, .rela.plt with a JUMP_SLOT entry,
  PT_DYNAMIC, section headers), writes it to a temp file and proves: image
  ownership, export parsing, Dlsym/FindSymbol resolution (with and without
  load bias), RELATIVE and JUMP_SLOT relocation values read back through
  ReadFromElf, handle uniqueness across unload/reload, double-unload failing
  closed, and the legacy name-only behaviour for non-ELF / missing paths.

### 9c. event_queue deepening — pointer truncation, use-after-free, EVFILT_HRTIMER

`src/kernel/event_queue.cpp` had never been compiled into any test (its NIDs
were registered since round 1, video_out posts events to it, but no suite
linked it). The new test found two real bugs on its first run (the second
confirmed under AddressSanitizer):

1. **Pointer truncation** — `KernelCreateEqueue` returned
   `static_cast<int32_t>(reinterpret_cast<intptr_t>(impl))`: a 64-bit heap
   pointer squeezed into the 32-bit `KernelEqueue` handle, then cast back by
   `GetImpl()` — every subsequent call dereferenced a garbage address (the
   ASAN run showed the impl resolved to address 0x70). Fixed with a
  handle-registry: small monotonic ids resolved through
   `unordered_map<KernelEqueue, unique_ptr<EqueueImpl>>`, mirroring the
   syscall layer's kqueue-fd discipline.
2. **Use-after-free on delete** — `KernelDeleteEqueue` freed the impl while
   a detached HR-timer delivery thread could still be sleeping on it (the
   `deleted` flag existed but was never set, and the thread locks the mutex
   of freed memory). Fixed: the impl tracks `pending_timers`; the deleter
   sets `deleted` under the lock and waits for every delivery thread to
   retire before the registry frees the object. `KernelDeleteHRTimerEvent`
   now actually cancels delivery, and `KernelWaitEqueue` sets `*out_num = 0`
   on ETIMEDOUT.
3. **EVFILT_HRTIMER in the raw syscall kevent** (Sony's -15 extension to the
   FreeBSD-9 ABI) — the syscall-layer kqueue modelled only EVFILT_USER.
   Timer knotes now arm from the kevent `data` microseconds; readiness is
   the monotonic deadline passing, evaluated lazily at collect time (no
   delivery thread); EV_ONESHOT erases after delivery, a timer without
   ONESHOT re-arms (periodic), EV_DELETE cancels, unsupported filters stay
   skipped fail-closed.
* **New test: `tests/kernel_event_queue_test.cpp` (67 checks)** — Part A
  drives the libkernel Equeue directly (user-event lifecycle with udata/data
  round-trip, ENOENT, ETIMEDOUT with out_num, real HR-timer delivery with
  timing assertion, timer cancellation, and the delete-with-pending-timer
  regression: the deleter is asserted to BLOCK for the delivery thread
  (~350 ms), then a fresh equeue proves the process is healthy). Part B
  drives the syscall dispatcher's kevent with EVFILT_HRTIMER (not-ready
  before the deadline, delivered after, periodic re-arm, EV_DELETE cancel,
  ONESHOT fires exactly once, unsupported filter skipped, user + timer
  knotes coexisting on one kqueue).

### Build note (for anyone hacking on this tree)

The Makefile does **not** generate header dependencies (no `-MMD`); object
files are only rebuilt when their `.cpp` changes. This round changed public
struct layouts (`ModuleInfo`, `PM4VulkanTranslator`, `Knote`), and an
incremental `make` linked stale objects against new ones — reproducing a
`pthread_mutex_lock` assertion abort in the integrated test that a clean
build does not have. **After pulling changes that touch headers, run
`make clean` first.** (Verified: clean build + full suite + integrated test
all green; the abort only appeared with mixed-generation objects.)

### Deliberately next (not in this round)

- Real VGT-style attribute fetching for the draw path (fetch shaders +
  multiple vertex attributes per lane).
- Cross-module symbol resolution in RelocateProgram (a module's imports
  resolved against other loaded modules' export tables).
- The remaining event filters (EVFILT_SIGNAL / EVFILT_GRAPHICS) in the
  syscall kevent, and wiring video-out flip/vblank events into the syscall
  kqueue as well as the libkernel Equeue.

*(The two items round 8 left open — the DRAW path on the real backend and
the runtime_linker / event_queue deepening — were both executed in this
round, above.)*

## Expansion round 10 — VGT attribute fetch + cross-module linking + remaining event filters

All three "deliberately next" items from round 9, executed together. After
this round `make unit` runs **26 suites / 621 checks**, all PASS (EXIT 0); the
full prototype builds clean from scratch and the integrated test stays green
(7/7). 149 new checks across one new suite and two extended suites.

### 10a. VGT-style attribute fetching for the draw path

Round 9 fed the vertex-stage kernel exactly ONE dword per lane (the decoded
index, or the raw attribute buffer). Real hardware hands the shader each
vertex's gathered attributes. Round 10 models the VGT gather:

* **ABI (`include/graphics/guest_gpu/pm4.h`)** — the guest programs a
  self-describing attribute-fetch descriptor table through the VS user SGPRs:
  `SPI_SHADER_USER_DATA_VS_0 + 5..6` = table GVA (lo/hi; **zero keeps the
  round-9 single-stream model — full back-compat**), `+7` = transformed-vertex
  output dwords per vertex (1..16). The table is `[count N (1..8)][N entries
  x 4 dwords]`, each entry = attribute buffer GVA (lo/hi), stride in dwords
  between consecutive vertices, attribute size in dwords (1..8). The honesty
  note in-tree: real RDNA2 runs a guest fetch SHADER (TF buffer loads);
  ProsperoLayer models the same gather declaratively and the translator
  executes it as the fixed-function VGT gather.
* **`PM4VulkanTranslator::TryRealDrawDispatch()`** — when a table is
  programmed, per lane i: vertex id = index[i] (indexed draws, after u16/u32
  decode) or i; every attribute contributes its dwords read at
  `attr_gva[a] + vertex_id * stride[a]`; the concatenated dwords are the
  kernel's lane input (lane-major), and the kernel's first m dwords per lane
  are written back at `output_gva + i*m`. Malformed tables (count 0, zero
  stride/size, > 8 entries) make NO real attempt and fall back to legacy
  DrawAuto; an out-of-range attribute buffer read also falls back with the
  record keeping `attempted = true`. `DrawDispatchRecord` gained
  fetch_enabled / fetch_table_gva / attribute_count / in_dwords_per_lane /
  out_dwords_per_lane for diagnostics.
* **`RDNA2ComputeCompiler`** — new lane geometry options
  `in_dwords_per_lane` (k, 1..64) / `out_dwords_per_lane` (m, 1..16): the
  kernel seeds `v0..v{k-1}` from `in_data[gid*k + j]` and stores
  `v0..v{m-1}` to `out_data[gid*m + j]` (OpIMul/OpIAdd lane-base arithmetic
  added). Defaults (1,1) emit the compute model unchanged. Output VGPRs the
  program never wrote store 0 — fail-closed, never stale bits.
* **`VulkanComputeExecutor::RunRDNA2Strided()`** — compiles with the strided
  lane model, dispatches one thread per lane, sizes the in/out SSBOs
  separately (threads*k / threads*m dwords). `RunSpirv` gained optional
  `threads` / `out_elements` overrides (defaults preserve the old geometry);
  both the Vulkan and the no-Vulkan builds accept them.
* **New suite: `tests/pm4_vgt_fetch_test.cpp` (42 checks)** — two attributes
  per vertex (pos x,y + weight; k=3, m=2, shader
  `v0 = w*x, v1 = w*y`): u32 indexed draw with shuffled + REPEATED indices
  (a repeated index re-fetches the same vertex — gather semantics), u16
  packed indexed draw, non-indexed sequential gather; on a Vulkan device the
  transformed vertices are read back FROM guest memory and asserted; back-compat
  (no table -> the round-9 single-stream path, `fetch_enabled = false`);
  malformed table / malformed entry / out-of-range buffer / tail-truncated
  stream all fall back or apply zero side effects. Headless-safe.

### 10b. Cross-module symbol resolution in RelocateProgram

Round 9 resolved S+A relocations only against the module's own .dynsym;
undefined (imported) symbols were counted and skipped. Round 10 resolves
them like a real dynamic linker:

* `RelocateProgram` now locates the module's .dynstr (new `LocateDynstr`
  helper, shared logic with the export parser) and, for every undefined
  symbol referenced by an `R_X86_64_64 / GLOB_DAT / JUMP_SLOT` relocation,
  searches the OTHER loaded modules' export tables **in load order**
  (POSIX RTLD_GLOBAL-style search — the stand-in for the dynlib dependency
  graph, documented in-tree). First provider wins; the value is
  `provider->base_addr + provider_export_value + addend`. Unresolved imports
  stay honestly skipped and counted.
* `ModuleInfo` gained `relocs_imported` (the cross-module subset of
  `relocs_applied`). `Dlsym` stays module-scoped (Sony's KernelDlsym
  semantics) — documented.
* **`tests/runtime_linker_test.cpp` extended (50 -> 74 checks)** — new
  synthetic PROVIDER pair (A exports `api_func` + `dup_func`, B exports
  `dup_func` at a different value) and an IMPORTER whose relocations
  reference `api_func` (resolvable), `dup_func` (double-defined — must bind
  to the FIRST loaded provider) and `ghost_func` (defined nowhere). Asserts:
  applied == 3, imported == 2, skipped == 1; the patched values read back
  through ReadFromElf; the unresolved slot keeps its original bytes; Dlsym
  module-scoping. Pointer discipline honoured (re-find after every load —
  LoadProgram's push_back can reallocate the module vector).

### 10c. EVFILT_SIGNAL / EVFILT_GRAPHICS + video-out flip/vblank wiring

* **`EVFILT_SIGNAL` (FreeBSD -6) in the syscall kevent** — the dispatcher
  models the kernel's pending-signal state: `RaiseGuestSignal(signo, count)`
  (public API for the emulator's host layers) accumulates a per-signal
  pending count; a signal knote is ready when the count for its `ident` is
  non-zero; delivery reports the count in `data` and CONSUMES it (FreeBSD
  signal knotes are inherently EV_CLEAR); ONESHOT / EV_DELETE honoured.
* **`EVFILT_GRAPHICS`** — Sony's libkernel equeue constant (0x200) adopted
  as the syscall-level filter id, with an in-tree honesty note (Sony's
  numeric syscall-level value is not publicly documented). `ident` = the
  video-out handle; `PostGraphicsEvent(handle, kind, event_id, udata)`
  (public bridge API) queues a trigger on every matching knote; delivery
  reports kind in `fflags` (1 = flip, 2 = vblank), the event id in `data`
  and the registration udata; one trigger per knote per kevent call (FIFO),
  the rest stay queued (FreeBSD knote discipline).
* **Video-out wiring (fidelity fix)** — `VideoOutAddFlipEvent` /
  `AddVblankEvent` used to POST the event immediately at registration time
  (a registration is not a flip!). They now REGISTER per handle and the
  events fire when they actually happen: `VideoOutSubmitFlip` fires every
  registered flip event, each `VideoOutGetVblankStatus` poll IS one vblank
  tick in the headless model and fires the vblank events. Delivery fans out
  to BOTH queue systems, exactly as the round-10 item required: the
  libkernel Equeue path (KernelPostEvent, the classic game path) AND the
  syscall-kqueue EVFILT_GRAPHICS path (a handle is at most one of the two,
  so exactly one path lands). `VideoOutDeleteFlipEvent` / `DeleteVblankEvent`
  are no longer no-ops — they remove the registration. `Sync::AddEqEvent`
  (the AGC path) is untouched.
* **`tests/kernel_event_queue_test.cpp` extended with Part C (67 -> 150
  checks)** — EVFILT_SIGNAL full lifecycle (raise -> deliver with count ->
  consumed; accumulation; ONESHOT; EV_DELETE); EVFILT_GRAPHICS (matching
  handle only, other handles unheard, FIFO triggers, EV_DELETE); the actual
  WIRING: `VideoOutSubmitFlip` delivers the flip event to a syscall kqueue
  (kind/id/udata asserted) AND to a libkernel Equeue (KernelWaitEqueue
  round-trip); registration silence; DeleteFlipEvent removal; vblank tick
  firing on both systems.
* `src/gpu/video_out_impl.cpp` joins the kernel_event_queue_test link (and
  its two pre-existing unused-parameter warnings are fixed so the suite's
  `-Werror` is clean).

### Deliberately next (not in this round)

- Guest fetch SHADERS on the VGT path (executing a real TF-buffer-load
  program instead of the declarative descriptor table).
- The dynlib dependency graph (DT_NEEDED): resolution currently searches
  load order, not the module's declared dependencies.
- AMPr events and EVFILT_PROCFS; libkernel Equeue built ON the syscall
  kqueue (today they are two parallel subsystems bridged by video-out).

*(All three items round 9 left open — the VGT attribute fetch, the
cross-module symbol resolution, and the remaining event filters with the
video-out wiring — were executed in this round, above.)*

## 1. Fixes (build)

1. **Missing header `kernel/kernel_managers.h`** — `libs/system_services.cpp`
   included a header that does not exist anywhere in the tree, breaking `make`.
   `EventFlagManager` / `SemaphoreManager` live in `include/kernel/event_flag.hpp`
   and `include/kernel/semaphore.hpp`; the stray include was removed.

2. **Undefined `LOG_*` macros** — `libs/system_services.cpp` uses
   `LOG_INFO/LOG_DEBUG/LOG_WARN/LOG_ERROR` which were not defined. Added the
   four macros to `include/common/logging/log.h` as aliases of the existing
   Kyty-style `PRINT_*` macros, so `{}`-style call sites compile and run safely.

3. **`const`-correctness** — `SystemServices::GetStatistics() const` called the
   non-const `GetTickCount()`. Made `GetTickCount()` const in both the header
   and the implementation.

4. **Full build did not link without optional dependencies (SDL2/FFmpeg)** —
   `libAudio.cpp` is a registration lib whose backing backend (`audio.cpp`) is
   excluded when SDL2 is absent, leaving ~140 undefined audio symbols, plus
   `InitNet_1` / `InitPlatform_1` / `VideoDec2::InitVideoDec2_1` (excluded with
   their backends) and `avpriv_vga16_font` (normally from FFmpeg). Added two
   conditional stub translation units that are compiled only when the matching
   optional dependency is missing, mirroring the existing `filter-out` logic in
   the Makefile:
   - `libs/optional_stubs_sdl.cpp` — no-op `InitAudio_1` / `InitNet_1` / `InitPlatform_1`
   - `libs/optional_stubs_ffmpeg.cpp` — external `avpriv_vga16_font[4096]` and no-op `VideoDec2::InitVideoDec2_1`
   The Makefile also excludes `libs/libAudio.cpp` when SDL2 is absent.

## 2. Fix (runtime — the segfault)

The integrated test `tests/main.cpp` allocated 64 MB at guest address
`0x1000100000` (covering `0x1000100000` .. `0x1004100000`) and then requested
`0x1000300000`, `0x1000400000` and `0x1000200000` — all inside that window. The
VMM collision check correctly rejected them, `AllocateVirtual` returned `0`,
`GvaToHva(0)` returned null, and `memcpy` faulted. The test addresses were
moved to non-overlapping locations (`0x1005000000`, `0x1005100000`,
`0x1005200000`), all within the 16 GB arena.

## 3. Expansion

Added a new dependency-free suite `tests/vmm_expanded_test.cpp` and wired it
into `make unit` (Makefile). It covers behaviour the other suites did not:
allocation + GVA<->HVA round-trip + payload copy, overlapping-allocation
rejection, safe handling of unmapped ranges, protection-gated access, and
protection/mismatch rejection. 5 new checks, all PASS.

## 3b. Expansion - additional test suites + HLE documentation (this pass)

Built on top of the fix branch, verified with `make unit` (all suites green):

1. **`tests/syscall_dispatcher_test.cpp` (13 checks)** - dependency-free
   coverage of the register-only Prospero syscall dispatcher
   (`src/cpu/prospero_syscalls.cpp`). It asserts the fail-closed guest ABI:
   unknown opcode -> `ENOSYS` (never a silent 0); `mmap`/`mprotect`/`munmap`
   route through the VMM and honour its collision + permission checks;
   `cpuset_get/setaffinity` and `clock_gettime` round-trip through mapped guest
   memory; `kqueue` hands out unique descriptors and `kevent` reports
   "0 events ready"; the `evf_*` event-flag lifecycle
   (create/set/wait-AND/clear/delete) works and a double-delete fails closed;
   `thr_new` refuses a non-mapped-executable entry point with the memory-fault
   status. Wired into `make unit`.

2. **`tests/pm4_translator_expanded_test.cpp` (8 checks)** - translator-level
   coverage beyond `pm4_decoder_test`: `SET_SH_REG`/`SET_CONTEXT_REG` register
   state tracking; guest-driven viewport programming (regs 0xA200/0xA201) with
   no host default and suppression when a dimension is zero; processed-packet
   counter persistence across multiple ring submissions; NOP consumed without
   side effects; a mixed viewport+dispatch+draw ring with exact counts; and the
   all-or-nothing guarantee that a tail-truncated stream applies no backend
   side effects. Supplies its own backend stub (no Vulkan link). Wired into
   `make unit`.

3. **`docs/HLE_COVERAGE.md`** - a detailed HLE coverage report: the export
   mechanism (`LIB_FUNC` NID registration + `SymbolDatabase` + `dynlib_dlsym`
   resolution), a per-module symbol-count table (1611 exported symbols over 30
   modules, tiered by surface size), the 35-handler syscall surface, the
   dependency-free test matrix, and an explicit honesty boundary distinguishing
   "resolvable/callable" from "behaviourally faithful".

After this pass `make unit` runs **9 suites / 58 checks**, all PASS.

## 4. Notes / honest limits

- The project remains a research prototype; it does not run commercial games.
- `vmm_extended_test.cpp` / `cpu_extended_test.cpp` shipped in the archive use
  GoogleTest and a different VMM API and are not part of the supported build;
  they were not wired in. This new suite replaces their intended coverage with
  a dependency-free implementation that actually compiles and passes.
- With SDL2/FFmpeg installed the real backends are used and the stubs are not
  compiled (mirroring the existing Makefile behaviour).
## Expansion rounds 11-14 — execution end-to-end, real GCN decode, software rasterizer, DT_NEEDED

### Round 11 — end-to-end guest execution (TLS + init arrays + SSE)

1. **SSE/SSE2 in the extended x86-64 interpreter** (`src/cpu/x86_64_interpreter.cpp`).
   The interpreter previously modelled only the integer ISA; every real PS5
   module does float math through XMM. Added: the 16-register XMM file in
   `CpuState`, the `0x66/0xF2/0xF3` SIMD prefix model, and the full scalar +
   packed set `movups/movss/movupd/movsd`, `movaps/movapd`, `add/sub/mul/div/
   min/max` (ss/sd/ps/pd), `sqrt`, `comiss/ucomiss/comisd/ucomisd` (real
   ZF/PF/CF semantics incl. unordered), `cvtss2sd/cvtsd2ss/cvtps2pd/cvtpd2ps`,
   `cvttss2si/cvttsd2si` + rounding forms, `cvtsi2ss/cvtsi2sd`, `movd/movq`
   both directions, `pxor`, `andps/andpd/orps/orpd/xorps/xorpd`. Lane-index
   bugs were caught and fixed by the packed tests. Unknown opcodes stay
   fail-closed (`UnsupportedOpcode`).

2. **FS segment + TLS** — the `0x64` prefix was silently ignored; it now
   rebases every memory operand onto `CpuState::fs_base` (the thread pointer,
   initial-exec TLS model). `ExecuteGuestFull` gained an `fs_base` parameter
   (default 0 preserves every existing caller).

3. **Init/fini tables** — `RuntimeLinker::LoadProgram` now parses `DT_INIT`,
   `DT_INIT_ARRAY`, `DT_FINI`, `DT_FINI_ARRAY` (+SZ) and `PT_TLS`; new
   `GetInitFunctions` / `GetFiniFunctions` return them in ELF execution order
   (init: DT_INIT then array forward; fini: array REVERSE then DT_FINI).

4. **`GuestLauncher`** (`include/loader/guest_launcher.hpp`,
   `src/loader/guest_launcher.cpp`) — the boot orchestrator: own + relocate
   the image through the runtime linker, map the PATCHED PT_LOADs into the
   guest VMM (relocations now actually reach guest memory), allocate the
   main-thread TLS block (`[self-ptr][PT_TLS image]`, `fs:[0]` reads back the
   block), run the init functions with TLS live, then execute the ELF entry
   point and return its exit code. `Shutdown` runs fini functions once.
   Per-thread TLS isolation is proven: two blocks, fresh template images,
   independent mutation.

5. **Latent bug fixed**: `RuntimeLinker::m_modules` was a `std::vector` —
   loading a second module reallocated it and left every outstanding
   `ModuleInfo*` dangling. Now a `std::deque` (stable pointers).

6. **Test** `tests/guest_boot_test.cpp` (42 checks): a fully synthetic ET_EXEC
   ELF (code + data + PT_TLS + PT_DYNAMIC with DT_INIT/DT_INIT_ARRAY/DT_FINI/
   DT_FINI_ARRAY) is hand-assembled with RIP-relative fixups; the boot proves
   SSE float math, packed arithmetic, FS-based TLS counters, init ordering
   (sequence number 1-2-3), the entry exit code (53), idempotent re-entry,
   per-thread TLS isolation, fini ordering (reverse array then DT_FINI) and
   the run-once guards.

### Round 12 — real GCN/GFX10 decoder + software GCN executor

1. **`GcnDecoder`** (`include/gpu/gcn_decoder.hpp`, `src/gpu/gcn_decoder.cpp`)
   decodes the ACTUAL AMD RDNA2 (GFX10) instruction encodings. Every field
   position and opcode number was extracted from LLVM's AMDGPU tablegen
   (`VOP1/VOP2/VOP3/VOPC/SOP/SM/BUF/DS Instructions.td`, GFX10 classes):
   formats SOP1/SOP2/SOPK/SOPC/SOPP/VOP1/VOP2/VOPC/VOP3/SMEM/MUBUF/MTBUF/DS,
   32-bit literal-constant consumption (`src0 == 0xFF` eats the next dword;
   VOP3 is dual-dword), inline constants, VOP3 abs/neg/omod/clamp modifiers,
   SMEM sbase/sdst/offset, MUBUF vaddr/vdata/srsrc/offen/idxen, DS
   offsets, and SOPP branch targets. Fail-closed on anything that cannot
   start an instruction.

2. **`GcnSwExecutor`** — a software GCN interpreter with real semantics:
   SGPRs/SCC/VCC/EXEC, per-lane VGPR files (heap-allocated; the first version
   put 1 MB on the stack and overflowed), the 9-bit source encoding
   (`0..101` SGPR / special / inline constants / `255` literal / `256+` VGPR),
   SOP1/SOP2/SOPK(cmpk)/SOPC/SOPP (branches, s_endpgm), VOP1 (float/math/
   convert/bit ops), VOP2 (float + integer ALU), VOP3 (mad/fma/min3/max3/
   med3/mul_lo/mul_hi/bfe/bfm/bcnt/ldexp... with modifiers), SMEM scalar
   loads and MUBUF buffer load/store through `GpuGuestMemory` + a host
   buffer-descriptor table (`s[4+4i..7+4i]` convention).

3. **Honest software fallback** — `VulkanComputeExecutor::SetSoftwareFallback`:
   when the SPIR-V path cannot run (no Vulkan device, or the RDNA2->SPIR-V
   compiler rejects the program), the raw GFX10 bytecode executes on the GCN
   software interpreter with the SAME lane model; results are flagged
   `hardware=false` with `device_name = "GCN software interpreter (GFX10)"` —
   real instruction semantics, never faked numbers.
   `PM4VulkanTranslator::BindComputeExecutor` enables it automatically, so the
   draw/compute paths now execute real GCN on GPU-less hosts.

4. **Latent bug fixed (hardware-accuracy)**: the legacy
   `ShaderSpirvRecompiler::DecodeRDNA2Instruction` decoded VOP1 with the
   `opcode` and `vdst` fields SWAPPED versus the AMD encoding (op is at
   [24:17], vdst at [16:9]). Fixed, and every test encoder updated to the
   real layout.

5. **Test** `tests/gcn_decoder_test.cpp` (70 checks): format identification +
   field extraction for all 12 formats, literal-stream consumption, VOP3
   dual-dword decode, mnemonics, and executor semantics (inline constants,
   SGPR broadcast, VOP3 mad with modifiers, literals, SMEM, branches, MUBUF,
   and the executor-level fallback end-to-end).

### Round 13 — software rasterizer + two latent-defect fixes

1. **`SoftwareRasterizer`** (`include/gpu/software_rasterizer.hpp`,
   `src/gpu/software_rasterizer.cpp`): the headless pixel stage. Consumes the
   draw path's transformed vertices (lane-major, m dwords per vertex;
   `[xyzw][rgba]`), performs the clip->NDC->viewport transform driven by the
   guest viewport, perspective-correct barycentric coverage, 1/w-weighted
   colour interpolation, a LESS depth test against a guest float depth plane,
   RGBA8 writes through `GpuGuestMemory`, guard-band rejection, near-plane
   (w<=0) rejection and degenerate culling. Bound via
   `PM4VulkanTranslator::SetRasterTarget`; every real-path draw (GPU or GCN
   software interpreter) feeds it.

2. **Latent bug fixed (round-10 defect, exposed by the rasterizer)**: the VGT
   attribute gather added the descriptor stride (documented in DWORDS)
   directly to the byte GVA — every gather past vertex 0 read at
   `attr_gva + vertex_id * stride` BYTES (misaligned/corrupted). Now
   `stride * 4`. The old VGT test never caught it because its value checks
   were gated on `available && executed_on_gpu` (never true on the headless
   host). Those checks now run whenever the real path executed
   (GPU or software fallback) — and pass.

3. **Latent bug fixed (S_ENDPGM)**: the shader trim loops only matched
   `s_endpgm` with `simm16 == 1` (a test-encoder convention), so a program
   ending in plain `s_endpgm` (simm 0) pulled in trailing guest-memory zeros
   as instructions. Both trim sites now accept any simm16 (hardware
   semantics).

4. **Test-suite hardening**: three PM4 test scenarios had their own encoding
   defects that were invisible for the same GPU-gated reason: the VOP2
   encoders passed `V(x)=256+x` into the 8-bit `src1` field (overflowing
   into `vdst`), and the single-stream scenarios wrote INTEGER index payloads
   but expected float arithmetic. All fixed; the u16 scenario now uses an
   exact `v_mul_u32_u24` integer transform.

5. **Test** `tests/software_rasterizer_test.cpp` (40 checks): coverage + flat
   colour, mid-triangle interpolation, depth write/read + LESS rejection +
   nearer overwrite, culling (off-screen / behind the eye / degenerate),
   viewport scale+offset, and the full PM4 ring -> VGT fetch -> GCN program ->
   software raster pipeline with barycentric colour verification.

### Round 14 — DT_NEEDED dependency graph

1. `LoadProgram` parses `DT_NEEDED` (declaration order) and `DT_SONAME` from
   PT_DYNAMIC into `ModuleInfo::needed_libraries` / `so_name`.

2. **Dependency-ordered import resolution**: `RelocateProgram` now resolves
   undefined symbols through `CollectResolutionOrder` — direct DT_NEEDED
   dependencies first (declared order), then their transitive dependencies
   (DFS, cycle-guarded, depth limit 16), then the remaining modules in load
   order (the round-10 behaviour as fallback). `FindModuleBySoName` matches
   by SONAME or file name.

3. **Test** (`tests/runtime_linker_test.cpp`, 74 -> 104 checks): providers
   with conflicting `dup_func` exports under SONAMEs — a direct dependency
   beats load order, and an importer that declares only `libM.so` (which
   itself needs `libB.so`) resolves `dup_func` transitively through the
   chain while the load-order winner loses. SONAME and needed-list parsing
   verified. (Test-builder defect also fixed: a `std::string` initialised
   from a `"\0..."` literal silently truncated at the first NUL, plus the
   `.dynstr` section size went stale when needed names were appended.)

### Deliberately next (not in these rounds)

- Guest threads on real host threads (thr_new currently runs the tiny
  fail-closed subset executor): per-thread VMM stacks + TLS + scheduler
  integration with the full interpreter.
- Near-plane/clipping in the rasterizer (currently guard-band reject), and
  render-target binding from CB_COLOR0_BASE-style PM4 registers instead of
  the host API.
- Extending the RDNA2->SPIR-V compiler to accept the full decoded GCN set
  (SOP1/2/K/C, VOP3, SMEM/MUBUF) so GPU hosts run the same programs the
  software interpreter already executes; then retire the legacy
  simplified-encoding path.
- ET_DYN load bias in GuestLauncher (ET_EXEC fixed addresses only today).

### Expansion rounds 15-17 (guest threads, full-ISA CPU, deep syscalls)

1. **Round 15 -- real guest threads on host threads** (`include/cpu/guest_threads.hpp`,
   `src/cpu/guest_threads.cpp`): the documented "deliberately next" step.
   `thr_new` no longer runs the tiny fail-closed subset executor: every guest
   thread now gets a VMM-allocated stack, a per-thread TLS block (fresh copy of
   the main module's PT_TLS template, self-pointer at fs:[0] -- installed by
   `GuestLauncher::Boot`), the FULL extended interpreter with a per-thread
   `CpuState`, per-thread syscall context, and join/detach with exit-code
   propagation. `thr_exit` unwinds the exact thread that called it (its exit
   code survives to the joiner); a top-level `ret` uses RAX. Test
   `tests/guest_threads_test.cpp` (61 checks): thr_exit + natural-return exit
   codes, SSE on the per-thread stack, the raw thr_new syscall end-to-end
   (guest main spins on guest memory until the worker publishes), 4-way TLS
   isolation with provable time overlap, unwind on unsupported opcode, join
   timeout / detach semantics, thr_self identity.

2. **Round 16 -- the full-ISA CPU core** (`src/cpu/x86_64_isa_ext.cpp`):
   the interpreter's modelled subset grew by whole instruction families:
   rotations (ROL/ROR/RCL/RCR), bit ops (BT/BTS/BTR/BTC reg+imm8, BSF/BSR,
   POPCNT/TZCNT/LZCNT), atomics-style (CMPXCHG, XADD), string ops with
   REP/REPE/REPNE and DF (MOVS/STOS/LODS/SCAS/CMPS, byte..qword),
   LEAVE/PUSHFQ/POPFQ/SAHF/LAHF, legacy integer SIMD (punpck*/pack*/pcmpgt*/
   pcmpeq*/pand/por/pandn/padd*/psub*/pmul*/psll*/psrl*/psra*/pslldq/psrldq/
   pshufd/pextrw/pinsrw, movdqa/movdqu), SSSE3/SSE4.1 (pshufb, phaddw/d,
   pabs*, ptest, pmulld, pcmpeqq, packusdw, palignr, roundps/pd, pextrd,
   pinsrd), and **AVX.128 VEX** (C5 2-byte + C4 3-byte, 3-operand
   non-destructive forms; the bit layout verified EMPIRICALLY against gas:
   `C5 [R~ v3~..v0~ L pp]`, `C4 [R~X~B~ mmmmm][W v3~..v0~ L pp]`, W direct).
   AVX.256 fails closed (128-bit register file). **The JIT block cache now
   uses the full decoder** (`X86Interpreter::InspectBlock`): blocks end at
   ret/branch/call/syscall/hlt like a real JIT, replacing the 5-opcode subset
   scanner. Test `tests/cpu_full_isa_test.cpp` (62 checks).

3. **Round 17 -- deep syscall layer** (`src/cpu/prospero_syscalls.cpp`,
   37 -> 80+ registered syscalls, FreeBSD-9 x86-64 numbering the PS4/PS5
   kernels inherit): **_umtx_op (436)** with REAL guest-memory semantics --
   WAIT/WAKE/LOCK/UNLOCK/MUTEX_LOCK/UNLOCK/CV_* poll the GVA through the VMM,
   so pthread mutexes/condvars built on umtx work across real threads
   (proven: a guest thread wakes a blocked umtx waiter by storing the new
   value). sigaction records real handler state; kill routes into the round-10
   pending-signal model; getrusage returns real host rusage; sysctl answers
   kern.ostype/osrelease/hostname + hw.machine/ncpu with FreeBSD identity and
   fails closed (ENOENT) on unknown nodes; stat/lstat/access/unlink/rename/
   mkdir/rmdir are host-backed like the existing open/read/write; writev
   walks real guest iovecs; execve/vfork/setsid/pipe/getdirentries/readv
   return the honest kernel error instead of faking success. Test
   `tests/syscall_depth_test.cpp` (40 checks).

**Latent defects fixed along the way**: the JIT block cache was reachable
only for the 5-opcode subset (most real guest blocks were rejected);
`thr_exit` from a managed thread used to keep running (yield only); the
`GuestLauncher` TLS allocator captured a non-copyable ModuleInfo by value.

### Deliberately next (not in these rounds)

- CB_COLOR0_BASE-style render-target binding from PM4 + near-plane clipping
  in the software rasterizer (the draw path's remaining host-API coupling).
- The GCN->SPIR-V compiler accepting the full decoded instruction set
  (SOP1/2/K/C + VOP3 + SMEM/MUBUF) so Vulkan hosts run the same programs the
  software executor already runs.
- ET_DYN load bias; AVX.256 (needs a 256-bit register file); fork/exec models.

## Expansion round 18 (render targets from PM4 registers, full GCN->SPIR-V, AVX.256, ET_DYN, fork)

The round executes the user's priority list end-to-end, starting with the two
"deliberately next" items documented at the end of round 17.

1. **18a -- render-target binding from the REAL PM4 registers + near-plane
   clipping** (`include/graphics/guest_gpu/pm4.h`, `src/gpu/pm4_translator.cpp`,
   `src/gpu/software_rasterizer.cpp`):
   * The draw path's raster target now comes from the actual CB/DB/PA context
     registers, with offsets verified against the Linux kernel AMD register
     headers (`gc_10_1_0_offset.h` / `gc_10_1_0_sh_mask.h` in torvalds/linux,
     i.e. the same register map the PS5's Oberon PM4 engine decodes):
     `CB_COLOR0_BASE`(0xA318, 256B-aligned with BASE_EXT 0xA390 for the high
     bits), `CB_COLOR0_PITCH`(0xA319)/`CB_COLOR0_SLICE`(0xA31A) linear-mode
     tile arithmetic, `CB_COLOR0_INFO`(0xA31C, FORMAT[6:2]/NUMBER_TYPE[10:8]),
     `CB_TARGET_MASK`(0xA08E), `DB_Z_INFO`(0xA010), `DB_Z_WRITE_BASE`(+HI
     0xA014/0xA01C), `DB_DEPTH_SIZE_XY`(0xA007), `DB_DEPTH_CONTROL`(0xA200,
     Z_ENABLE/Z_WRITE/ZFUNC[6:4]), and the REAL viewport pair
     `PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET`(0xA10F-0xA112).
   * When the guest programs CB_COLOR0_BASE the register-derived target
     REPLACES the round-13 host-API one (a host-API target set earlier stays
     completely untouched -- proven by the test). CB_TARGET_MASK=0 renders
     depth-only; an unsupported CB_COLOR0_INFO format or malformed PITCH/SLICE
     geometry fails the RASTER stage closed while the vertex stage still
     executes, exactly like hardware with a broken CB binding.
   * The rasterizer gained a REAL homogeneous polygon clipper
     (Sutherland-Hodgman against all six GCN planes -w<=x<=w, -w<=y<=w,
     0<=z<=w) with attributes interpolated linearly in clip space at every
     intersection -- the near plane is genuinely CLIPPED now, replacing the
     round-13 guard-band reject + w<=0 cull. The depth test's comparison
     comes from DB_DEPTH_CONTROL.ZFUNC (LESS was hard-wired), and the viewport
     transform follows the real PA_CL_VPORT semantics (screen = ndc*scale +
     offset; also fixes the round-13 latent bug where the y-scale term used
     half_w instead of half_h).
   * A latent round-13 defect was fixed along the way: the old SET_CONTEXT_REG
     handler read 0xA200/0xA201 as "viewport width/height", but 0xA200 is
     really DB_DEPTH_CONTROL -- any depth-control programming was misread.
     The legacy backend viewport now follows PA_SC_SCREEN_SCISSOR_BR.
   * Test `tests/pm4_color_target_test.cpp` (68 checks): register binding
     end-to-end (pixels land at CB_COLOR0_BASE while the host-API plane stays
     black), host-API back-compat, PM4-overrides-host, DB depth state driving
     LESS rejection, fail-closed formats, CB_TARGET_MASK gating, the 256B
     alignment convention, malformed geometry rejection.
     `software_rasterizer_test` grew 40->59 checks (near-plane clipping with
     hand-computed projections, far-plane, X-plane straddlers, ZFUNC).

2. **18b -- the full GCN -> SPIR-V compiler** (`src/gpu/rdna2_compute_compiler.cpp`,
   rewritten): the documented "deliberately next" step. The compiler now
   decodes programs with the REAL GcnDecoder (all 12 GFX10 formats) and lowers
   everything the software executor runs, so a Vulkan host executes the SAME
   programs:
   * SOP1/SOP2/SOPK/SOPC scalar ALU + SCC (incl. S_CSELECT/S_CMOV, the CMPK
     immediate comparisons, BFM/SEXT/ABS/NOT/BREV);
   * SOPP STRUCTURED control flow: forward conditional branches lower to
     OpSelectionMerge + OpBranchConditional (if / if-else with the classic
     cbranch/s_branch else shape, guard-clause early exits whose target IS
     the S_ENDPGM), backward conditional branches lower to OpLoopMerge
     (do-while). Anything else reports UnstructuredControlFlow (fail-closed,
     documented). EXECZ is never taken (EXEC is all-ones in the lane model),
     EXECNZ is an unconditional forward jump; s_barrier lowers to a real
     OpControlBarrier.
   * VOP1/VOP2 full integer+float ALU, VOPC (v_cmp_* on VCC -- added to the
     software executor in the same round so both executors agree), VOP3 with
     the real modifiers (neg/abs per source, omod, clamp), SMEM
     (S_LOAD_DWORD[X2/X4/X8] from a scalar-segment mirror SSBO with u64
     address math), MUBUF (BUFFER_LOAD/STORE_DWORD[X2/X3/X4] routed per
     descriptor to its own SSBO with offen/idxen addressing).
   * Registers are function-scope VARIABLES (VGPR/SGPR/SCC/VCC), zero-
     initialised at entry (deterministic reads, SW parity), so values survive
     control-flow merges exactly like architectural state.
   * Corrections to historical lowering: V_LOG_F32 now lowers to Log2 and
     V_EXP_F32 to Exp2 (GCN semantics -- the round-10 lowering used the
     natural-log/exp ext-insts and disagreed with the software executor).
     Every SPIR-V opcode number was verified against KhronosGroup/SPIRV-Headers
     (spirv.core.grammar.json, extinst.glsl.std.450.grammar.json); the GFX10
     VOPC numbers come from LLVM's VOPC_Real_gfx6_gfx7_gfx10 table.
   * Latent defects fixed: the round-10 test used 0x1B for V_FLOOR_F32 while
     the real GFX10 opcode is 0x24 (the old compiler matched its own private
     table and contradicted the round-12 decoder); several SOP sdst>=104
     encodings wrote out of bounds in the software executor (now fail closed).
   * Test `tests/gcn_spirv_full_test.cpp` (59 checks): every program ALSO
     runs on the software executor with its OUTPUT VALUES checked (semantics
     verified, not just structure) -- if/else, do-while, VOP3+neg, SOPK/SOPC
     early-exit, SMEM mirror, MUBUF per-descriptor routing, VOPC+cndmask,
     plus fail-closed diagnostics (MUBUF without a table, SMEM without a
     mirror, backward s_branch, branch into a literal dword). All existing
     compiler-dependent suites stay green unchanged.

3. **18c -- AVX.256** (`include/cpu/x86_64_interpreter.hpp`,
   `src/cpu/x86_64_isa_ext.cpp`): the round-16 "128-bit register file" limit
   is gone. CpuState gains the UPPER 128 bits of each YMM register
   (`ymm_hi[16]`, YMM i = xmm[i] + ymm_hi[i], the same split hardware makes).
   VEX L=1 now executes: packed float arithmetic over 8 lanes (add/sub/
   mul/div/min/max), packed double over 4, bitwise (vpand/vpor/vpxor/
   vandps/vxorps), 32-byte vmovdqu/vmovdqa loads+stores, per-128-bit-lane
   vpshufd and vpshufb (the AVX2 rule), 32-byte vptest, vsqrtps, and
   VZEROUPPER/VZEROALL. The AVX.128 register-write rule is modelled (VEX.128
   ops zero the upper half). Scalar VEX with L=1 fails closed (invalid
   encoding on real hardware). Test `tests/cpu_avx256_test.cpp` (97 checks);
   `cpu_full_isa_test` grew 62->67 checks (the AVX.256 case now verifies the
   256-bit result instead of asserting the old fail-closed).

4. **18d -- ET_DYN (PIE) load bias** (`src/loader/guest_launcher.cpp`): the
   documented "ET_DYN load bias not supported yet" is implemented. The loader
   probe-allocates a free region for the image span, records it in
   ModuleInfo::base_addr so RelocateProgram applies every relocation relative
   to the bias, maps the segments at bias + p_vaddr and starts execution at
   bias + e_entry. ET_EXEC keeps base 0. Also fixed a latent gap: ModuleInfo
   .size was never set for ELF-loaded modules, so FindProgramByAddr silently
   failed for all of them. Test `tests/et_dyn_boot_test.cpp` (15 checks): a
   genuinely position-independent guest (relocated pointer vs rip-relative
   lea of the same slot -- the exit code is correct only when the bias
   matches the mapping at whatever base the loader picks), a second PIE at a
   fresh bias, and unsupported ELF types failing closed.

5. **18e -- the real fork/wait4/pid model** (`src/cpu/fork_process.{hpp,cpp}`,
   `src/cpu/prospero_syscalls.cpp`, `src/memory/virtual_memory_manager.cpp`):
   fork(2)/vfork(66) no longer fail closed. fork takes an EAGER snapshot of
   the whole committed guest address space (new
   VirtualMemoryManager::SnapshotCommitted) plus the caller's CpuState; the
   child runs on its own host thread against the SNAPSHOT bus -- its
   loads/stores are fully isolated (a child store to an inherited page is
   invisible to the parent, proven by the test). The child resumes after the
   syscall with rax=0; exit(2)/thr_exit in the child record its code and
   unwind exactly that thread. wait4(7) really waits: blocking waits park
   until the child finishes, WNOHANG reports 0 when children exist but none
   are ready, the FreeBSD wait status (exit code << 8) is written to the
   guest status pointer, and a reaped-out set reports ECHILD (POSIX
   semantics). getpid/getppid are per-process (children report their own pid
   and the main pid as parent -- proven through the exit codes children
   compute); execve keeps refusing honestly with EPERM (one guest image per
   boot). SyscallContext gained the caller's CpuState pointer (set at both
   syscall call sites) so fork can snapshot the caller. Test
   `tests/syscall_fork_test.cpp` (11 checks, five guest-side bits + host
   assertions). Documented boundary: the child's own loads/stores are
   isolated; syscalls that touch guest memory through the kernel bridge
   operate on the parent address space (the child uses registers + exit).

6. **Documentation sync**: `docs/HLE_COVERAGE.md` still claimed "35 handlers"
   while CHANGES.md documented 80+ (the user's item 6) -- now 85 with the
   fork family, and the process table lists the real fork/wait4/pid behaviour.

**Latent defects fixed along the way** (beyond the ones above): the
`make unit` recipe never RAN `guest_threads_test`, `cpu_full_isa_test` or
`syscall_depth_test` (they were built as prerequisites but not executed --
rounds 15-17 wired the rules but missed the run lines).

### Deliberately next (not in this round)

- The PM4 draw path plumbing the MUBUF/SMEM resource tables into the Vulkan
  dispatch (the compiler + executor support them; the PM4 side still falls
  back to the software interpreter for memory ops).
- A real VkGraphicsPipeline raster path (the software rasterizer now runs the
  full register-driven pipeline headlessly; the next step is mapping the
  CB_COLOR0 binding to a Vulkan image view + render pass).
- Loops with early-exit-from-nested-if in the SPIR-V compiler (targeting a
  merge beyond the innermost construct still fails closed as unstructured).
- DS (LDS) instructions in both executors; S_BUFFER_LOAD resource
  descriptors; VOP3 DPP/SDWA forms.

## Expansion round 19 -- the resource tables reach the real dispatch + the VkGraphicsPipeline raster path

The two "deliberately next" items from round 18, executed together per the
plan: (1) plumb the guest's MUBUF/SMEM resource tables from the PM4 stream
into the real Vulkan dispatch (the compiler understood them since round 18;
the dispatch never received them), and (2) a REAL VkGraphicsPipeline raster
path on top of the round-18 register-derived CB_COLOR0 binding. All external
constants were re-verified against primary sources fetched for this round:
KhronosGroup/SPIRV-Headers spirv.h, KhronosGroup/Vulkan-Headers
vulkan_core.h, and shadPS4 (the project's reference model) for the Liverpool
CB format semantics.

1. **19a -- the resource-table ABI + parsing** (`graphics/guest_gpu/pm4.h`,
   `src/gpu/pm4_translator.cpp`): the guest programs a self-describing
   buffer-resource table through COMPUTE_USER_DATA_0 +5..6 (compute) /
   SPI_SHADER_USER_DATA_VS_0 +8..9 (draw) -- the same style as the round-10
   fetch table. The table names the MUBUF descriptors (base GVA, size,
   idxen stride) and the SMEM scalar-mirror window (base + dwords).
   ParseResourceTable validates everything (count 1..8, sizes within the
   documented caps, stride >= 1 so the SPIR-V lowering and the software
   executor can never disagree, mirror base XOR size consistency) and a
   malformed table is dropped WHOLE -- the dispatch then proceeds without
   resources and MUBUF/SMEM fail closed exactly like round 18. The records
   (ComputeDispatchRecord / DrawDispatchRecord) expose what was programmed,
   parsed, and rejected.

2. **19b -- the resource-aware dispatch** (`vulkan_compute_executor.*`):
   RunRDNA2WithResources stages every descriptor's guest contents and the
   mirror window (LoadResourceContents -- public static, unit-testable
   headless), compiles with the tables, dispatches with per-descriptor
   SSBOs bound at the bindings the compiler emits (mirror at 2, descriptors
   after it), and writes stored-to buffers back (StoreResourceContents).
   RunSpirv grew optional extra SSBOs + push constants; the honest GCN
   software fallback runs the SAME program with the SAME tables when no
   device serves the dispatch, so the guest-visible result is identical
   whichever path executes. The acceptance test compares the final guest
   state (output SSBO + the MUBUF-modified buffer) against a direct
   GcnSwExecutor reference DWORD FOR DWORD -- on a Vulkan host that is a
   hardware-vs-software value comparison, on a headless host it validates
   the full plumbing; either way the same check runs.

3. **19c -- the SMEM mirror base moved to push constants**
   (`rdna2_compute_compiler.cpp`): the round-18 lowering baked the mirror
   base into the module as OpConstants; round 19 declares a push-constant
   block { uint base_lo; uint base_hi; } (Block + member offsets, storage
   class PushConstant) and the executor pushes the actual window base at
   every dispatch -- one compiled module serves any mirror window, exactly
   as the plan asked ("passing the base address ... as a push constant").

4. **19d -- the VkGraphicsPipeline raster path (phase 2)**
   (`vulkan_compute_executor.*`, `pm4_translator.*`): the compiler gained
   emit_vertex_stage -- the same lane model as a VERTEX-stage module
   (ExecutionModel Vertex, gl_VertexIndex = BuiltIn 42 as the lane index,
   NO LocalSize, v0..v3 -> gl_Position, v4..v7 -> a Location-0 colour out,
   alongside the out-SSBO dump the round-9 draw ABI promises) -- plus a
   hand-assembled passthrough FRAGMENT module (ExecutionModel Fragment +
   OriginUpperLeft, one Location-0 in/out pair). DrawVerticesToGuest builds
   the full real pipeline: a colour VkImage with the CB-derived extent and
   the converted format, an optional D32_SFLOAT depth attachment, a render
   pass with LOAD semantics (the guest's current planes are uploaded first
   -- merge behaviour like the software rasterizer), the guest VS as the
   pipeline's vertex stage fetching attributes from the in SSBO by
   gl_VertexIndex, dynamic viewport/scissor derived from the PA_CL_VPORT
   pair (a negative guest YSCALE becomes the Vulkan 1.1 negative-height
   y-flip), depth test from DB_DEPTH_CONTROL.ZFUNC (numeric identity with
   VkCompareOp, verified), and the rendered planes + transformed vertices
   read back into guest memory. The translator opts in explicitly
   (SetGraphicsRasterEnabled -- default OFF, so every existing caller keeps
   the round-18 behaviour on any host); the production headless bridge
   enables it. ANY missing piece (no device, no graphics queue, unsupported
   format, unreadable planes, pipeline rejection) declines with a recorded
   reason and the software rasterizer runs unchanged.

5. **19e -- the CB format conversion, positively** (`pm4.h`): the round-18
   reject logic reused as a conversion: CB_COLOR0_INFO FORMAT[6:2] +
   NUMBER_TYPE[10:8] -> GuestColorFormat, whose values ARE the real
   VkFormat numbers (R8G8B8A8_UNORM = 37, verified against
   vulkan_core.h) so the Vulkan-guarded code static_casts directly.
   ZFuncToVkCompareOp is the verified numeric identity (0..7).

6. **Latent defects fixed along the way** (all found by verification
   against the fetched primary sources):
   * **CB_FORMAT_8_8_8_8 was the PC-GCN value (3), not the Liverpool
     value.** shadPS4 (the project's reference model) decodes
     CB_COLOR0_INFO.FORMAT as the SQIMG DataFormat enum
     (`DataFormat(info.format)`, regs_color.h) -- the same numbering
     Gnm::DataFormat uses -- where 8_8_8_8 = 10. The round-18 gate would
     have rejected every real guest RGBA8 target. pm4.h now documents the
     verification trail.
   * **The S_ENDPGM shader trim matched the wrong bits**: the round-13
     check `(x & 0xFF80FFFF) == 0xBF800000` masks out the OPCODE field and
     requires simm16 == 0 -- it stopped at s_nop (opcode 0) and missed
     s_endpgm with a non-zero simm (the opposite of what the round-13
     comment claims). The trim now matches opcode == S_ENDPGM with ANY
     simm. The round-18 colour-target/rasterizer suites' "passthrough"
     shader dword was actually s_nop -- they passed by accident through
     the software-fallback accident; the tests now encode real s_endpgm,
     which also means the passthrough program now COMPILES (a real
     hardware dispatch instead of a compile failure).
   * **The SPIR-V OpEntryPoint interface listed Uniform-class variables**,
     which is invalid before SPIR-V 1.4 (the modules are 1.0): real
     validators reject it (VUID-StandaloneSpirv-OpEntryPoint). The
     interface now lists only the Input/Output variables.

7. **Tests**: `tests/pm4_resource_dispatch_test.cpp` (101 checks) -- the
   exact-value acceptance comparison for compute AND draw paths, malformed
   table fail-closed (5 variants + unreadable header), round-18
   back-compat, the push-constant module structure (exactly one
   PushConstant variable; mirror at binding 2, descriptor at 3; Block
   decoration), and the staging helpers (exact contents, fail-closed
   ranges, no torn write-backs). `tests/vk_graphics_pipeline_test.cpp`
   (78 checks) -- the format/compare-op conversions with verified enum
   values, the vertex-module structure (ExecutionModel Vertex, no
   LocalSize, VertexIndex/Position/Location decorations) + software
   semantic parity, the fragment-module structure, the
   register-binding -> graphics-target conversion (including the derived
   viewport numbers and every decline case), and the end-to-end opt-in
   fail-closed contract (graphics attempted -> declined with a recorded
   reason -> the software rasterizer renders the same pixels; default-off
   is byte-identical). 39 test binaries total (was 37), zero failures,
   EXIT=0; the integration emulator still reports ALL ENGINE MODULES
   FULLY VERIFIED.

8. **The Vulkan-guarded code is compile-verified against the REAL
   KhronosGroup headers**: the host has no Vulkan dev package, so the
   `#if __has_include(<vulkan/vulkan.h>)` body had never been compiled here.
   Round 19 fetched the authentic `vulkan_core.h` + `vk_platform.h` +
   `vk_video/*` from KhronosGroup/Vulkan-Headers into a shim include dir and
   compiled `vulkan_compute_executor.cpp` against them with `-Werror` --
   which immediately caught a real defect (an over-braced `VkRect2D`
   aggregate initialisation in `DrawVerticesToGuest` that would not have
   compiled on any Vulkan host). The whole graphics path now type-checks
   against the genuine API surface.

## Round 20 — DirectExecutionBackend (guest code runs NATIVELY on the host CPU) + the three documented GPU steps + a large JIT/game-ISA expansion

The execution model the project promised since round 16's JIT: the PS5 CPU
is an x86-64 Zen 2 and so is the host, so instead of interpreting every
guest instruction in C++, the **DirectExecutionBackend** jumps INTO the
guest code and lets the hardware execute it at native speed. Only the
host-dependent / guest-divergent instructions are intercepted (rewritten to
`ud2` before first execution and replayed through the interpreter core):

1. **The identity-mapped arena (the enabling change).** The VMM now reserves
   its 16 GB window with `MAP_FIXED_NOREPLACE` AT the guest base
   (0x1000000000): host VA == guest VA for everything in the arena, so raw
   guest addresses -- absolute operands, RIP-relative data, the stack, the
   FS-based TLS -- are valid host addresses unchanged. Without this the
   trampoline (and every native guest instruction) would address the wrong
   pages: the very first store of the very first run faulted at
   `rsp-56` before this fix. `IsArenaIdentityMapped()` is the backend's hard
   precondition; a host whose layout cannot host the arena declines
   (fail-closed) exactly like the GPU paths.

2. **The backend** (`include/cpu/direct_execution.hpp` +
   `src/cpu/direct_execution.cpp`): a small assembly trampoline loads the
   guest context (GPRs, flags, FS base, the WHOLE XMM/YMM file -- YMM via a
   staging array on AVX hosts so emulated SSE4a results reach the native
   stream), switches to the guest stack and jumps to the entry point.
   SIGNALS are the only exits: SIGSEGV at the stop sentinel = clean return,
   SIGILL at a planted `ud2` = interception, SIGALRM = wall-clock budget,
   everything else = a recorded guest fault. Guest `syscall`s are serviced
   through the SAME ProsperoSyscallDispatcher lambda the interpreter uses
   (FreeBSD-9 numbering -- a guest syscall reaching the HOST kernel with
   Linux numbering was the worst-case the design had to prevent);
   cpuid/rdtsc/rdtscp are virtualised (a fixed Zen 2 model + an invariant
   3.5 GHz TSC, identical on every host); SSE4a / BMI1 / BMI2 / MOVBE /
   TZCNT/LZCNT are ALWAYS patched and replayed (bit-identical behaviour on
   every host -- the "rare unsupported instruction" rule). A per-thread
   seccomp denylist (fork/execve/ptrace/... -> EPERM) bounds the documented
   worst case of a never-executed block racing two threads. The engine
   switch (`CPUJitEngine::SetExecutionBackend`) tries direct first and
   falls back to the interpreter on ANY decline; guest threads route
   through the same switch.

3. **Block discovery is incremental and (round 20 fix) COMPLETE for
   conditional branches**: the fall-through AND the taken target of every
   jcc now get armed entry traps -- the first execution of a taken branch
   used to enter unscanned code (a `syscall` on that path would have run on
   the host kernel; `tests/.../B10` proves the syscall is intercepted on
   the FIRST taken execution). Discovery also restores armed-trap bytes in
   the fetched copy before scanning (a planted `ud2` decoded as a 2-byte
   instruction shifted every later site offset -- a latent
   mis-patch-the-wrong-byte defect).

4. **Reloaded / self-modifying code invalidates the registry**: the VMM
   fires a code-write notifier on every `CopyToGuest` into an Exec page and
   on Free/Protect of an Exec region; the backend drops its patches +
   discovery for that range (restore-if-still-ud2 semantics) so the next
   execution re-scans whatever bytes are there now (B4/B6 in the suite
   reload different programs at the same address -- the stale registry used
   to let a moved `syscall` run on the host, returning -EBADF).

5. **The block scanner's terminator length was ONE BYTE SHORT for taken
   branches** (a real latent defect): the scan state's zeroed flags take
   every jnz/jo, the decoder's `next` is then the BACKWARD branch target,
   and the old `rip+pfx+1` fallback armed the successor trap INSIDE the
   branch encoding (ud2 over the imm8 -- a corrupted stream). The
   sequential end is now derived from the ENCODING for every
   control-transfer form (direct branches/calls/jcc/loop/ret/syscall/hlt)
   plus a full ModRM/SIB decode for the indirect call/jmp forms; unknown
   encodings arm nothing.

6. **XMM capture comes from the signal frame** (`fpregs+160`, xsave header
   bit 2 -> YMM at +576), not the live registers (the C++ handler itself
   may clobber xmm0-15 before the capture) -- and the trampoline RELOADS
   the whole vector file on every re-entry so emulated instructions'
   results reach the native stream (B5/B6 parity used to fail with the
   handler's leftovers in xmm0).

### The three documented GPU steps (CHANGES.md "next" list, all landed)

7. **Vertex-stage control flow** (`tests/gpu_round20_test.cpp` part A): a
   DIVERGENT per-lane if/else (v_cmp + s_cbranch_vccnz -- each vertex
   decides by its own colour attribute) compiles under
   `emit_vertex_stage`: the structured lowering (OpSelectionMerge +
   OpBranchConditional) coexists with the vertex decorations
   (VertexIndex/Position/Location) and stays LocalSize-free; the software
   executor shows the three lanes DIVERGING exactly as the semantics
   require; scalar guard-clause branches and loops get the same treatment.

8. **CB formats beyond RGBA8 UNORM** (part B): `CbInfoToGuestColorFormat`
   grew from one pair to TEN -- 8_8_8_8 x {UNORM, SNORM, CB-6(=SRGB)} x
   {Standard, Alternate} (BGRA8!), 16_16_16_16 x {UNORM, SNORM, FLOAT},
   11_11_10/FLOAT (B10G11R11 packed float) and 2_10_10_10/UNORM
   (A2B10G10R10) -- every enum value verified against the vendored
   KhronosGroup vulkan_core.h and every field meaning against shadPS4
   (refs/regs_color.h Color0Info: FORMAT[6:2] = SQIMG DataFormat,
   NUMBER_TYPE[10:8] with the CB-specific SnormNz->Srgb reading,
   COMP_SWAP[12:11] SwapMode). The software rasterizer encodes each layout
   exactly (BGRA byte order, the IEC sRGB curve, the two packed-float
   layouts, IEEE-754 half floats with round-to-nearest-even); the plane
   stride follows the format (8 bytes/pixel for the 16-bit channels, the
   depth plane stays float32); the PM4 translator's CB gate converts the
   REAL register triple and the draw leaves half-float bits in the guest
   plane; the Vulkan graphics path's upload/readback sizes follow the same
   bpp (compile-verified against the genuine KhronosGroup headers with the
   round-19 shim).

9. **Cross-dispatch pipeline caching** (part C): `gpu/vulkan_pipeline_cache.hpp`
   keys the full driver-visible input (128-bit FNV over the SPIR-V stream +
   binding count + push-constant size + the graphics format/ZFUNC/depth
   signature) in a PROCESS-WIDE registry; the executor's Impl caches the
   VkDescriptorSetLayout / pipeline layout / shader modules / render pass /
   pipeline per key (destroyed with the device) and skips the whole
   vkCreate* chain on a hit -- a second dispatch of the same program is
   pure submission. The pure key/registry contract is headless-tested
   (determinism, per-field sensitivity, first-miss/second-hit, cross-
   executor persistence, eviction accounting); the object tables are
   type-checked against the real Vulkan headers.

### The JIT/game-ISA expansion (interpreter + direct-execution parity)

10. **SSE4a** (extrq/insertq/movntsd/movntss -- bit-level semantics verified
    against QEMU's helper_extrq/insertq), **MOVBE**, **CRC32** (Castagnoli),
    **TZCNT/LZCNT**, and the COMPLETE **BMI1/BMI2** set in the VEX decoder:
    andn/bextr/blsr/blsmsk/blsi/bzhi/mulx/pdep/pext/rorx/shlx/sarx/shrx --
    every encoding verified against gas (which caught two real defects: the
    canonical ANDN has pp=00, not pp=F2 -- the old classifier+interpreter
    would have rejected every real andn; and RORX lives in the 0F3A MAP,
    which was not even decoded). **FMA3** (vfmadd/vfmsub/vfnmadd/vfnmsub x
    132/213/231 x ps/pd/ss/sd, gas-verified) runs natively on FMA hosts and
    is served by the interpreter elsewhere. The patch-site classifier
    covers all of them so the direct backend intercepts exactly the
    host-dependent set.

### Tests

11. `tests/direct_execution_test.cpp` (126 checks): the pure registry
    pieces (FetchCode/WriteCodeBytes/DiscoverBlock/EmulatePatchedInstruction
    byte-exact vs the interpreter) AND full native runs -- plain function,
    guest memory writes, a 100-iteration native loop, syscall interception
    (getpid through the dispatcher, NEVER the host kernel), SSE4a + BMI
    replay mid-run with FULL interpreter parity (registers AND memory,
    dword-for-dword), guest faults with the faulting address, the wall-clock
    timeout, genuine ud2 fail-closed, the taken-conditional-branch
    interception, FMA3 through the interpreter, and the not-enabled decline.
    `tests/gpu_round20_test.cpp` (116 checks) covers items 7-9. 41 test
    binaries (was 39), 415 individual checks, zero failures, EXIT=0; the
    integration emulator still reports ALL ENGINE MODULES FULLY VERIFIED.

### Deliberately next (not in this round)

- DS (LDS) instructions in both executors; S_BUFFER_LOAD resource
  descriptors; VOP3 DPP/SDWA forms.
- The direct-execution FS/TLS path under real guest threads (the trampoline
  loads FS base; GuestThreadManager already routes through the backend).
- Vulkan pipeline-cache statistics surfaced through the bridge diagnostics.

## Round 23 — PM4 indirect draw execution

- Implemented `PKT3_DRAW_INDIRECT` and `PKT3_DRAW_INDEX_INDIRECT` using guest-memory indirect argument records.
- Implemented multi-draw variants with guest count-buffer, maximum-count, and stride handling.
- Added `SET_BASE` address tracking for AGC-style byte offsets into indirect argument memory.
- Added fail-closed bounds/readability checks and instance-count validation.
- Added `pm4_indirect_draw_test` covering single draw, multi draw, and unreadable argument buffers.

## Round 24 - batched command execution and draw addressing

- Added indirect compute dispatch from guest memory (`DISPATCH_INDIRECT`).
- Added bounded nested indirect-buffer execution (`INDIRECT_BUFFER`) with recursion guard.
- Added indexed/non-indexed indirect draw argument semantics: first index/vertex, signed base vertex, and first instance.
- Added `DRAW_INDEX_OFFSET_2` state handling.
- Added index-buffer range validation when `INDEX_BUFFER_SIZE` is programmed.
- Added unaligned u16 indirect index offset handling.
- Tightened multi-draw stride validation to the complete argument record size.
- Extended PM4 draw diagnostics with first-index/base-vertex/first-vertex/first-instance.
- Added regression coverage for indirect dispatch, nested IBs, and indirect draw addressing.

Validation: 22/22 CTest tests pass.

## Round 25 — GPU memory synchronization + kernel wait correctness
- Added guest-memory `WAIT_REG_MEM` PM4 support with bounded polling and fail-closed unreadable-address handling.
- Added PM4 `WRITE_DATA` and `COPY_DATA` guest-memory operations using explicit adapter ABIs and no partial source copy.
- Added monotonic PM4 EOP/release completion sequence tracking and optional 64-bit guest fence publication.
- Replaced placeholder renderer fence helpers with real atomic + condition-variable signaling/waiting.
- Fixed libkernel `Wait64` so it preserves the full 64-bit address and expected value; the old implementation truncated both through `Wait32`.
- Added `pm4_memory_sync_test` covering write/copy/EOP/wait plus renderer fence signaling.
- CMake verification: 23/23 tests passed.

## Round 26 — S_BUFFER_LOAD_* through the compute compiler (bug-fix round)

Two new tests (`gpu_memory_extended_test`, `syscall_io_extended_test`) were
added to this checkout without being wired into the Makefile, and the first
did not compile cleanly. Fixing that surfaced two real defects in
`rdna2_compute_compiler.cpp` that are unrelated to this checkout's own
build-wiring gap:

- **Fixed:** `DeclareRegisters` only allocated `em.var_buf` (the SPIR-V
  uniform-buffer variables backing descriptor-table lookups) when
  `any_mubuf` was set. `LowerSmem` indexes the same `em.var_buf` array for
  `S_BUFFER_LOAD_DWORD/X2/X4` (an SMEM opcode, not MUBUF), so a program using
  `S_BUFFER_LOAD_*` without any MUBUF instruction indexed an empty vector —
  reproducible as a segfault under AddressSanitizer. `var_buf` is now
  allocated when either resource path needs it.
- **Fixed:** the register liveness pre-scan's `SMEM` case computed
  `dwords_loaded` only for `S_LOAD_DWORD/X2/X4/X8`, leaving it `0` (and the
  destination SGPRs unmarked) for `S_BUFFER_LOAD_DWORD/X2/X4`. This made
  `BufferLoadIntoRegs` reject a valid `S_BUFFER_LOAD_DWORDX2` with "SMEM sdst
  register out of range." The pre-scan now marks destination SGPRs for both
  opcode families, and only marks `sbase`/`sbase+1` as scalar-register reads
  for the plain `S_LOAD_*` form (`S_BUFFER_LOAD_*` addresses through the
  descriptor table, not through sbase as a register pair).
- **Fixed (test-only):** `gpu_memory_extended_test.cpp` itself had three
  bugs found while making it pass: an unused `Smem()` helper
  (`-Werror=unused-function`); `Smem()` XORed its offset into a single
  return word instead of returning the two real instruction dwords, which
  is only equivalent when `off == 0`; the compiler was constructed from a
  `ComputeCompilerOptions` copied *before* `opt.buffers` was populated
  (the constructor takes the struct by value); the "out of range" assertion
  used an offset (16 bytes) that is actually in-bounds for the 8-dword/
  32-byte test descriptor when loading a DWORDX2 — changed to 28, and the
  initial-load assertion was checked against the wrong address (`base_gva`
  is a byte address, not a dword index).
- Wired both new tests into the Makefile (`GPU_MEM_EXT_TEST`,
  `SYSCALL_IO_EXT_TEST`), matching the pattern of every other unit test.

Verification: rebuilt the full unit suite from a clean tree with
`-Wall -Wextra -Wpedantic -Werror` and ran every produced binary directly
(not just via `make unit`, whose job-control can drop targets under load).
28/28 built test binaries exited 0, including both new tests and every
previously-passing test (no regressions from either compiler fix).
