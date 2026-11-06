# Statically compiled native libraries

`tools/native_irx` and `bozo/native/compiled_libraries.h` let a port retain a disc library's game-independent arithmetic and sequencing as compiled C++ while replacing its SDK calls with native services. No firmware, interpreter, instruction dispatcher, dynamic compiler or device backend is supplied.

## Offline pipeline

1. `python tools/native_irx/relocate.py library.irx <base> <output-stem>` relocates a little-endian ELF32 IRX into a `.bin` plus `.json` metadata. Supported relocations are NONE, 16, 32, REL32, 26, HI16 and LO16. Unknown formats/relocations fail. Bases are chosen by the port and must not overlap.
2. `python tools/native_irx/recompile.py <output-stem>.json --out <generated-directory>` emits `<library>_iop.cpp`. Link that C++ into the port, with `bozo_runtime_headers`. The translator's only mode is strict: unsupported instructions/targets produce explicit native failures, and unsafe load-delay cases are rejected during generation.
3. Build `Image` records from the relocated data and import/export metadata, and register them with a per-port `CompiledLibraries` object. Its name, hash, load base and byte length must match a linked generated registration. A native-libsd image can route its sound exports directly to `audio::Libsd`; pure note/pitch conversions remain compiled arithmetic.

Generated images and code belong in ignored build directories. They are derived from the player's disc, never shipped in the engine repository. The host only copies image bytes as data; execution always calls a statically linked C++ function or a native SDK binding. Pointers outside the bounded buffer fail, including device addresses.

## Port wiring

The port owns the host's lifetime, image choices, heap start and EE-facing policy. `Libraries` exposes provider hooks:

- `audioProvider(eeCycle)` advances the native library callbacks and returns their mixer.
- `sif.moduleLoader(path)` selects a native implementation for a requested library.
- `sif.transfer`, `allocateBuffer` and `freeBuffer` connect native bulk transfers to the same allocator used by compiled SDK code.
- `sif.resetProvider` resets library data and native services.
- `sif.servers` maps service IDs to native RPC callbacks, independent of any particular sound driver.

The host implements a strict subset of loadcore, stdio, sysclib, sysmem, thbase, intrman, timrman, sifman/sifcmd and ioman. Unimplemented imports fail explicitly; extending this set must implement a reusable SDK contract, not recognize a particular game address. Native libsd and the mixer supply sample playback and reverb. These are compatibility services, not replicas of the chips below the APIs.

Native tasks are cooperative and callbacks execute at safe points. The SDK's logical time uses 36.864 MHz units; EE-to-library time conversion is explicit. Periodic callbacks support clock/divisor rates, including the SDK's H-line source. Device-register reads/writes are not available. Task stacks and root callback stacks use separate bounded arenas so repeated RPC/timer callbacks cannot overwrite suspended task frames. Root nesting is limited to four 16-KiB stacks; task stacks honor the requested aligned size and fail if the arena is exhausted. The heap ends at `0x180000`; ports choose a heap start above their images.

Ports may opt into `useReadyAudioCompletions()` before execution. Actual completed libsd byte copies
and consumed mixer halves then queue ready callbacks for delivery when a linked native function
returns. This allows compiled game code to poll its own completion flag inside a root call. Delivery
honors callback exclusion, dequeues before invocation and prevents recursive redelivery. It does not
advance logical time or change work budgets. The default scheduling policy is unchanged; reset clears
pending events and retains the selected policy. The ready-audio suite reproduces default starvation
and checks actual copied bytes, exactly-once delivery, reentrancy, exclusion, reset and host isolation.

Newer timer exports 20/22/23/24 register a linked native periodic callback, configure its clock and
divisor, and start/stop its deadline. The callback receives its argument and captured GP; its return
value selects the next period, with zero cancelling. Self-stop/free is safe and wins over its return
value. Only ungated scheduling is supported; external gate modes reject. Existing legacy periodic
callbacks retain their behavior. `library_timer_handlers` checks deadlines, rearming, cancellation,
self-stop/free, clock/divisor conversion, reset and isolation. ABI declarations are from
[PS2SDK xtimrman](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/timrman/include/xtimrman.h);
the implementation schedules native jobs and contains no timer-register backend.

`useNativeFrameCallbacks()` opts into mapping start-frame listener registration/release (vblank 8/9)
to actual native game frames. The port calls `notifyFrameCompleted()` after completing a game frame.
Clock advancement alone never invokes these listeners. Priorities and FIFO ties, captured GP/argument,
zero-return removal, release of queued listeners, callback exclusion and reset are retained. Deferred
frames are bounded, and listeners registered later do not consume old frame events. Display-end phases
and legacy VBlank waits remain unsupported: this API creates no display timing or interrupt source.
`library_frame_listeners` checks actual-frame-only delivery, ordering, cancellation, exclusion, reset
and separate hosts. Declarations/error values were checked against PS2SDK's vblank/kerr headers.

User command exports `sifcmd:8/10/11` own a checked native handler table and linked callbacks. They
retain the application's function/argument records and GP; invalid IDs, system IDs, unlinked functions
and out-of-range tables reject. `submitCommand(id,data)` copies 16..112 bytes into owned native storage,
returns an owned monotonic job ID and queues actual callback delivery with exclusion. Storage is
released after delivery, including failure; reset drops pending jobs and registrations. No SIF system
command or transport/interrupt processor is present. `library_user_commands` verifies records,
registration/removal, copied bytes, GP, once-only delivery, bounds, exclusion, reset and independent
hosts. ABI checked against PS2SDK's sifcmd/common declarations. This API does not yet connect EE
`sceSifSendCmd` or extra-buffer copies; ports must provide a native producer before those paths work.

`allocatedBuffer(address,bytes)` exposes a transient span into one live native heap allocation,
including checked interior subranges. Public allocations and compiled sysmem allocations use the
same ownership check. Spans cannot cross adjacent blocks, alias opaque module handles or address
images, stacks, freed data or devices. Reset invalidates all views. `library_allocation_views` checks
real writes, interior ranges, adjacent boundaries, invalid/overflow spans, free, reset and two hosts.

Compiled `sifcmd:12/13` user replies call the explicit `sif.receiveCommand` native application provider.
The reply and optional extra data are snapshotted and bounds-checked before delivery. The provider
copies them and queues its linked application callback, returning a real job ID. The source header is
updated only after accepted submission. System commands and absent providers reject. No transport
descriptor or interrupt processor is run. `library_command_replies` verifies message/header/extra
bytes, job IDs, provider rejection, reset and independent hosts. Ports must validate the destination
and own the queued application's data until its callback completes.

`ReferThreadStatus` uses the SDK's 68-byte layout, and `SysClock2USec` takes a clock pointer plus two output pointers. ABI definitions were checked against [PS2SDK's public thbase declarations](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/threadman/include/thbase.h); no implementation code was copied from that project.

## Other reusable port services

Compiled-library `sifman:7/32` are native bulk-copy jobs from bounded library data into attached EE RAM.
Every descriptor/source/destination is checked and the full batch snapshotted before any output write.
Only plain data copies (attribute zero, 1..32 descriptors) are admitted; packet/tag/device/VU targets
reject. Jobs return an owned monotonic ID after real writes; `sifman:8` reports the terminal/error result
(-1), since no copy remains in flight. Export 32 queues one linked native completion with its argument
and GP after the bytes are copied. No DMA channel, protocol/packet executor or device status is modeled.
`library_bulk_copy` covers actual bytes and ordered overlaps, invalid-batch atomicity, attributes/bounds,
one completion, reset and two hosts. ABI checked against PS2SDK's sifman declarations; no implementation
copied. This currently requires `Libraries::attach` and an EE RAM destination.

`intrman:17/18` protects native callback scheduling with balanced per-host exclusion scopes. The SDK
receives a restore token; no processor status/register state is stored or executed. Nested exclusion
reports KE_CPUDI (-102), preserves its restore token, and must release in order. Native timer/event
delivery and cooperative task dispatch defer until release and the next safe advance; pending logical
time is retained. Blocking, budget yields, unbalanced returns/releases and legacy unscoped enable inside
a scope reject. Reset clears the scopes and pending advance. `library_exclusion` checks real deferred
native timer delivery, nesting, bad releases/returns, reset and independent owners. Declarations/error
constants were checked against PS2SDK's intrman/kerr headers; no implementation was copied.

Native RPC servers retain the task that registered them. Their compiled callbacks use separate bounded
callback stacks while thread identity, current priority (`thbase:42`) and status refer to that logical
owner. Changing its priority updates the same native task record. The parked RPC loop is not resumed
or released by dispatch; callback context restores on return or exception. A bare root priority query
returns illegal-context (-100). The `library_rpc_context` suite checks priority save/change/restore,
identity/status, parked-worker preservation, failure cleanup, reset and independent servers. The ordinal
is confirmed by [PS2SDK's export declarations](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/threadman/src/exports.tab);
no kernel implementation was copied. Blocking inside a root RPC callback remains unsupported.

The compiled-library `sysclib:14` binding implements bounded native `memset`, returning the original
destination and truncating its fill value to one byte. The complete span is checked before writing.
`library_memory` covers empty/small/large/unaligned spans, surrounding bytes, rejected overflow and
reset/independent hosts. The ordinal and ABI were checked against
[PS2SDK's declarations](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/sysclib/include/sysclib.h);
no SDK implementation was copied.

`CompiledLibraries::bindNative(address, callback)` replaces an admitted linked game entry with a
per-host C++ algorithm. Install bindings before starting any library. Closures can capture typed port
state, such as a native input source, without process-global dispatch state. Unadmitted images, unknown
interior addresses, SDK import stubs and empty callbacks reject. Bindings survive reset; captured state
must outlive the host and the port resets that state itself. This does not run the replaced instruction
body or supply any device responses. `library_bindings` tests two independent sources, a retained compiled
consumer, reset, repeat start and invalid bindings.

The native library host supports FIFO/priority `thsemap` creation/deletion, signal/iSignal, wait and poll using per-host
counts and cooperative task wait queues. SDK descriptor fields are attr/option/initial/max; attributes beyond 0/1
and invalid counts reject explicitly. Unknown/poll-empty/overflow/delete-wakeup results use SDK errors
-408/-419/-420/-425. An empty wait from a root call returns illegal-context (-100); native tasks block/resume.
WakeupThread credits do not release semaphore or RPC waits. The synthetic `library_semaphores` suite covers
real native task stop/resume, mixed sleep credits, deletion wakeup, reset and two-host isolation. ABI declarations
were checked against https://ps2dev.github.io/ps2sdk/thsemap_8h_source.html; no SDK implementation was copied.

Priority mode grants to the waiting native task with the lowest current priority number, preserving FIFO
ties. Selection reads the current task record when signalled, so reprioritizing a waiter takes effect.
`library_priority_semaphores` checks actual blocking/grants, equal-priority ties, a changed waiting
priority, unsupported attributes, reset and two owners. It qualifies native semaphore grant ordering,
not a full console scheduler/timing model. Work-limit errors include the actual stop PC and accounted work.

- Recompiler `--symbol-prefix` isolates independently compiled images; `FunctionTable::addModule` guards overlapping image tables by resident identity, including resume entries. Unsupported or unloaded slots cannot dispatch another image's code.
- `McState::slotMask` controls accepted card slots. Native memory-card operations support current directories, relative paths, parent traversal within the card root, paginated listings and operation IDs returned through Sync.
- Native ISO9660 lookup handles directory paths, filename versions and sector-crossing reads. Reentrant libc allocation shares the normal heap.
- `FreeOptions::framePeriodCycles` lets a native frame boundary advance logical SDK time without running a console frame loop; its zero default preserves existing ports.
- `LiveScriptInput` hot-reloads a port-owned text file. Each changed valid script starts at its observation frame; incomplete invalid writes keep the previous valid script.
- `BOZO_REQUIRE_FFMPEG=ON` makes absence of native movie decoding a configuration error. It does not change replay tools' default configuration.

## Native render targets

`Material::target` selects main output (0) or auxiliary images (1..7). A nonzero `fromFrame` samples `source`: the main frame so far (0), an auxiliary image (1..7), or `Material::kPreviousFrame`. Same-target feedback snapshots the source before the draw. Main-frame sampling can use mipmap LOD bias through `fromFrame`; auxiliary/history images use unbiased sampling. `nearest` selects point filtering. `frameWidth/Height` retain the port's logical frame extent in draw records.

Use `Renderer::setTargetSize` for independent target sizes and `setTargetSharesDepth` to reuse the main target's depth (then its size follows the main target). Targets are cleared on first draw per published frame; newly sampled images are initialized independently of the calling draw's clip/write masks. Previous-frame history advances only on a newly published frame. `AlphaOnly` preserves RGB, while `DestinationBySource` computes source plus destination times source alpha. Existing enum values and default draw hashes are unchanged.

`bozo-render-target-tests` creates a hidden SDL/OpenGL context and checks pixels for orientation, independent target size, current-frame copy, history stability and blend arithmetic. It is included in CTest only with `BOZO_APP=ON` and requires a working GL 3.3 driver. Other tests are headless and use synthetic fixtures, with no game data.

## Provenance

The static translator and register-value ABI helpers were moved from the team's independently authored work after a per-line authorship audit (Thomas Martin / boubou666 and Arzaroth). The engine host services and offline relocator were written for BozoEngine. No PS2Recomp CPU, kernel, device or runtime backend was imported. `tools/bench_lock.sh` is also team-authored and preserves the shared local benchmark-lock protocol for ports using either toolchain.
