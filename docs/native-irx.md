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

`ReferThreadStatus` uses the SDK's 68-byte layout, and `SysClock2USec` takes a clock pointer plus two output pointers. ABI definitions were checked against [PS2SDK's public thbase declarations](https://github.com/ps2dev/ps2sdk/blob/master/iop/system/threadman/include/thbase.h); no implementation code was copied from that project.

## Other reusable port services

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
