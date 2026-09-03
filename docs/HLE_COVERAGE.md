# HLE Coverage Report — ProsperoLayer RDNA2 Core

This document maps the High-Level Emulation (HLE) surface that the project
ships: which guest system modules are stubbed/implemented, how many symbols each
exports, how they are registered and resolved, and which of them are backed by a
real backend versus a fail-closed no-op. It complements `docs/SCOPE.md` (which
states the *narrow, audited* core) by describing the *breadth* of the HLE glue.

> Honesty boundary: an exported HLE symbol means the emulator can *resolve and
> call* that guest function without crashing. It does **not** mean the function
> reproduces PS4/PS5 behaviour. Most entries are logging stubs that return
> success (`OK`) or a documented Sony error code. This is expected for a
> research prototype and is not a compatibility claim.

## 1. How HLE is wired

- **Export macro.** Each module registers its functions with
  `LIB_FUNC("<NID>", <host_function>)` inside an `Init<Module>_1` routine.
  The NID is the Sony 11/12-char symbol hash; the host function uses the
  `KYTY_SYSV_ABI` calling convention.
- **Symbol database.** `Loader::SymbolDatabase` (header-only,
  `include/loader/symbolDatabase.h`) stores NID→host-pointer and
  name→record maps behind a mutex. Lookups: `FindSymbol`, `FindByNid`,
  `FindByName`.
- **Guest resolution path.** The guest reaches HLE through the
  `SC_SYS_dynlib_dlsym` (591) syscall, which calls
  `SymbolDatabase::FindSymbol` and returns a GVA via `HvaToGva`, or a Sony
  error (`ENOENT` / `EINVAL`) when the symbol is unknown — i.e. resolution is
  fail-closed.
- **Optional-dependency stubs.** When SDL2 / FFmpeg are absent, the real
  backends (`audio.cpp`, `libNet.cpp`, `libVideoDec2.cpp`, …) are excluded and
  replaced by `libs/optional_stubs_sdl.cpp` / `libs/optional_stubs_ffmpeg.cpp`
  so the full build still links (see `CHANGES.md` §1.4).

## 2. Module coverage (exported guest symbols)

Counts are exact `LIB_FUNC` registrations per translation unit. Total across
`libs/`: **1611 exported symbols** over **30 HLE modules**.

### Tier A — large surface (core OS / graphics / audio / net)

| Module | Symbols | Role | Backing |
|--------|:------:|------|---------|
| `libKernel` | 373 | Threads, sync, VM, fs, timers, dynlib | Mix: real (pthread/VMM/time via `src/kernel/*`) + host-backed file/vector I/O, pipe/dup/ftruncate + stubs |
| `libNet` | 271 | BSD sockets, resolver, epoll | **Round 28: REAL POSIX sockets** -- full loopback TCP lifecycle, FreeBSD errno translation, DNS resolver; HTTP/NP/SSL sections still stubs |
| `libAudio` | 170 | Audio-out ports, volume, mixing | SDL2 backend; no-op stub without SDL2 |
| `libGraphicsDriver` | 148 | GNM command submission, GDS/CE | Stubs; PM4 path handled in `src/gpu` |
| `libAmpr` | 106 | Async media processing ring | Stubs |

### Tier B — medium surface (UI / content / media)

| Module | Symbols | Role | Backing |
|--------|:------:|------|---------|
| `libFont` | 70 | Font rendering front-end | stb_truetype-backed where used |
| `libJson2` | 61 | JSON parse/serialize | nlohmann-json gated; excluded without headers |
| `libC` | 60 | libc shims (malloc/str/math) | Real host libc pass-through |
| `libDialog` | 55 | Common/message dialogs | Stubs (`dialog_kernel_stub.cpp` fallback) |
| `libSaveData` | 51 | Save-data mount/read/write | **Round 28: real persistence** -- PARAM.bin + icon0.png round-trip through the mounted host directory (survives umount/remount) |
| `libSystemService` | 34 | System params, events | Stubs + `system_services.cpp` |
| `libVideoOut` | 31 | Flip/vblank/buffer registration | `video_out_impl.cpp` reference path |
| `libPad` | 31 | DualSense pad/mouse/keyboard | `controller.cpp`; stub vibration/trigger |
| `libRtc` | 26 | Real-time clock | Real host time |
| `libUlt` | 23 | User-level threading | Stubs |

### Tier C — small surface (services / codecs / misc)

| Module | Symbols | Role | Backing |
|--------|:------:|------|---------|
| `libPsml` | 19 | PlayStation ML runtime | Stubs |
| `libPlayGo` | 18 | Chunk/streaming install | Stubs |
| `libUserService` | 17 | User/account IDs | Stubs (fixed user id) |
| `libShare` | 15 | Share/broadcast | Stubs |
| `libVideoDec2` | 11 | H.264/HEVC decode | FFmpeg-gated; no-op stub without FFmpeg |
| `libAppContent` | 8 | Add-on content / entitlements | Stubs |
| `libDbgAsan` | 7 | Guest ASan hooks | Stubs |
| `libSysmodule` | 5 | Module load/unload | Stubs (report loaded) |
| `libPngDec` | 5 | PNG decode | stb_image-backed |
| `libs` | 3 | Aggregate registrar | Registers the above |
| `libRudp` | 3 | Reliable UDP | Stubs |
| `libTextToSpeech2` | 2 | TTS | Stubs |
| `libFontFt` | 2 | FreeType font path | Stubs |
| `libDebug` | 1 | Debug output | Real (host stdout) |

## 3. Syscall surface (guest → host kernel)

The register-only Prospero syscall dispatcher
(`src/cpu/prospero_syscalls.cpp`) registers **85 handlers** (FreeBSD-9 x86-64
numbering the PS4/PS5 kernels inherit; grown from 37 in the original core to
80+ in round 17 and the fork family in round 18). They are the runtime
contract exercised by the `syscall_dispatcher_test`, `syscall_depth_test`
and `syscall_fork_test` suites.

| Group | Syscalls | Behaviour |
|-------|----------|-----------|
| Process/User | `fork`(2), `vfork`(66), `wait4`(7), `getpid`(20), `getppid`(39), `getuid`(24), `exit`(1) | **Real fork model (round 18)**: eager address-space snapshot + isolated child thread; `wait4` blocks/WNOHANG and writes the FreeBSD wait status; per-process pids. `execve`(59) refuses with EPERM (one guest image per boot -- documented honesty) |
| Memory | `mmap`(477), `munmap`(73), `mprotect`(74), `madvise`(75) | Route to VMM; honour collision + permission checks; `madvise` no-op |
| Threads | `thr_new`(432), `thr_self`(433), `thr_exit`(431), `thr_kill`(434), `thr_set_name`(464), `rtprio_thread`(466) | `thr_new` fail-closed on non-exec entry; others via `ThreadScheduler` |
| Time | `clock_gettime`(232), `gettimeofday`(116), `nanosleep`(240) | Write guest timespec/timeval via VMM pointer |
| Affinity | `cpuset_getaffinity`(487), `cpuset_setaffinity`(488) | Round-trip mask through guest memory |
| Events/kqueue | `kqueue`(362), `kevent`(363), `evf_*`(538–544), `namedobj_*`(532–533) | Unique descriptors; `evf` via `EventFlagManager`; `kevent` reports 0 ready |
| File I/O | `read`(3), `write`(4), `close`(6), `lseek`(478), `readv`(120), `writev`(121), `dup`(41), `pipe`(42), `ftruncate`(480) | Host fd pass-through with checked VMM translation |
| Dynlib/budget | `dynlib_dlsym`(591), `dynlib_get_list`(592), `dmem_container`(585), `budget_get_ptype`(610) | Symbol resolution + documented constants |
| Fallback | any unknown opcode | Returns `SCE_KERNEL_ERROR_ENOSYS` (fail-closed) |

## 4. Test coverage of the HLE / runtime surface

Dependency-free suites run by `make unit` (all green):

| Suite | Checks | Area |
|-------|:-----:|------|
| `cpu_interpreter_test` | 4 | x86-64 subset interpreter |
| `jit_executor_test` | 2 | JIT preflight / no host codegen |
| `vmm_elf_loader_test` | 4 | ELF load + checked copies |
| `vmm_expanded_test` | 5 | VMM alloc/map/protect (added in the fix branch) |
| **`syscall_dispatcher_test`** | **13** | **Syscall ABI: memory, affinity, clocks, kqueue, event-flags, thr_new fail-closed, ENOSYS fallback (new)** |
| `pm4_decoder_test` | 16 | PM4 Type-3 decode + translate |
| **`pm4_translator_expanded_test`** | **8** | **Register-file state, guest-driven viewport, cross-submission persistence, all-or-nothing execution (new)** |
| `rdna2_spirv_recompiler_test` | 6 groups | RDNA2→SPIR-V subset |
| `game_folder_test` | 1 | Game-folder layout |

New coverage added by this pass: **21 additional checks** across two suites.

Gaps deliberately left (research-prototype scope, documented in `SCOPE.md`):
per-symbol behavioural fidelity of the Tier A/B stubs, `libNet`/`libAudio`
backend behaviour without SDL2, and real GPU resource submission.
