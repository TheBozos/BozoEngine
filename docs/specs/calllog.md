# Spec: the call log (PS2XCLG1)

Phase 2 runs the whole game on BozoEngine: its own generated code, its own EE kernel and scheduler, its own disc
access. Every library it does not implement yet (989SND over SIF RPC, the pad, the memory card, CD streaming,
libmpeg, the GS library, IOP module loading, the C library...) is answered from a **call log** recorded on the
reference build, and the per-frame reference record (`harness.md`, PS2XREF1) must still match for the whole run.

The call log is the whole deterministic run, in execution order: every call out of guest code with what it changed,
and every change to guest-visible state made outside guest code with the exact point it was made. This document is the
format and its meaning; it is enough to read, check and replay a log. No other source is needed.
`tools/capture/calllog_read.py` lists and checks logs.

It builds on the per-function capture (`capture.md`, PS2XCAP1): the flat space (section 3 there), byte runs, the
context record (section 5 there) and the out-call record (section 7.3 there) are the same and are not repeated here.

## 1. Terms

- **Guest code**: the game's own code (the functions of its ELF), run by the recompiled code.
- **Out-call**: anything guest code does that is not guest code (`capture.md` 7.3): a call to a library or kernel
  function the runtime implements natively (a **stub**), a call to a port function bound to a guest address (an
  **override**), a `SYSCALL`, a VU0 microprogram, a load from or store to an address that is not memory, an exception.
  Out-calls made by guest code running *inside* an out-call (an override that calls back into the game) are part of
  it and are not logged on their own.
- **Kernel calls**: the out-calls BozoEngine implements natively (section 6). **Library calls**: every other stub.
  Overrides, VU0 microprograms and hardware loads and stores are answered from the log too.
- **Thread**: a guest thread of the EE kernel, by its id (1 is the main thread). A **handler** (an interrupt or DMA
  handler, an alarm, a library callback) runs as an **invocation** stacked on a thread: depth 0 is the thread's own
  context, depth n its n-th stacked handler. Handlers the dispatcher starts while no thread runs get a thread of their
  own with a negative id (-1, -2...), reused once idle.
- **Dispatcher**: the runtime's loop that picks the thread to run, starts and ends handlers, resumes waiting threads
  and processes the scheduler's events (vblank, timers, alarms). Everything it does runs between two stretches of guest
  code.
- **Game frame**: as in `harness.md`; frame 0 runs until the game first hands a picture over.

## 2. Time and points

Everything in the log happens on one host thread (the EE's), in the order of the file. Each record carries its
**point**:

- **cycle**: the EE cycle count when it happened. EE time is what the reference accounts, never host time;
- **seq**: the number of out-calls logged before it (for a CALL record, its own index from 0);
- **frame**: the game frames ended so far;
- **checkpoints**: the EE checkpoints (below) passed since the previous out-call ended;
- **thread** and **pc**: the guest thread running and its context's `pc` (the guest address being executed).

The reference's time model, which places every interrupt and every asynchronous change:

| Where | EE cycles charged |
|---|---|
| A taken backward branch to a label of the same function (the target's address <= the branch's), a **checkpoint** | 32 |
| A call or jump that leaves the function (through the runtime's dispatch of guest branches), a checkpoint | 8 |
| Each time the dispatcher starts running guest code, a checkpoint | 8 |
| A read of an EE timer register (`T0`-`T3` `COUNT`, from a polling loop) | the timer's period in EE cycles, charged at the next checkpoint |
| An out-call | its own time: `cycleAfter - cycle` of its CALL record (a disc read charges 114,400 a sector; the frame end charges the rest of the field) |
| Every thread waiting and no handler due | up to the next event's cycle |

A checkpoint is also where guest code can be stopped: when an event is due (a vblank, a timer interrupt, a posted
event), when a waiting thread was made ready by an interrupt, or at the end of a time slice (65,536 cycles, only when
another thread of the same or higher priority is ready), guest code returns to the dispatcher there and resumes at the
same `pc` later. An interrupt or a thread switch therefore happens at a checkpoint, and its point says which: the
cycle, or equivalently the number of checkpoints since the previous out-call at the given `pc`. A runtime that charges
the same cycles at the same places lands every interrupt at the same guest instruction; one that does not can use
(seq, checkpoints) to find the same place.

## 3. File

Little-endian. A header (128 bytes), then chunks until the end of the file.

### 3.1 Header

| Offset | Type | Field |
|---|---|---|
| 0 | char[8] | magic `PS2XCLG1` |
| 8 | u32 | version (1) |
| 12 | u32 | headerBytes (128) |
| 16 | u32 | contextBytes (1328: `capture.md` section 5) |
| 20 | u32 | flatBytes (`0x2005000`) |
| 24 | u32 | ramBytes (`0x2000000`) |
| 28 | u32 | scratchpadBytes (`0x4000`) |
| 32 | u32 | vu0DataBytes (`0x1000`) |
| 36 | u32 | pageBytes (4096) |
| 40 | u32 | frameLimit: the frames the run was recorded for (the reference record's length; 0 if none) |
| 44 | u32 | flags: bit 0 = recorded in strict mode (section 9) |
| 48 | u64 | elfHash: XXH64 (seed 0) of the game's ELF file |
| 56 | u32 | the EE clock (294,912,000 cycles a second) |
| 60 | u32 | recordHeaderBytes (40) |
| 64 | char[64] | run name, NUL-padded |

### 3.2 Chunks

As in `capture.md` section 6: tag (char[4]), flags (u32, bit 0 = zlib, RFC 1950), storedBytes (u32), rawBytes (u32),
payload. Unknown tags are skipped.

| Tag | Payload |
|---|---|
| `NAME` | u32 count, then count entries: u32 guest address, u32 class (1 kernel, 2 library, 3 override), u32 nameBytes, the name (ELF symbol, C++ names mangled), padded to 4. The stubs the reference knows by name; class 3 when a port function is bound there too |
| `OVRD` | u32 count, then count u32 guest addresses: the overrides (`capture.md`, `OVRD`). The latest one applies |
| `RECS` | records (section 4), back to back. A record never spans two chunks, and the records made inside a call (flag 0x80) are in the chunk of their CALL record |
| `END ` | u64 records, u64 calls, u32 the last frame, u32 reserved, u64 the last cycle. Last chunk of a complete log |

## 4. Records

### 4.1 Header (40 bytes, every record)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | type (below) |
| 1 | u8 | flags: bits 0-6 per type; bit 7 (`0x80`, any type but CALL) = made **inside** the out-call whose CALL record comes before it |
| 2 | u16 | sub: per type |
| 4 | u32 | bytes: the whole record, this header included (a multiple of 4) |
| 8 | u64 | cycle |
| 16 | u64 | seq (inside a call: that call's index) |
| 24 | u32 | frame |
| 28 | i32 | thread (0: none) |
| 32 | u32 | checkpoints since the previous out-call |
| 36 | u32 | pc of the running context (0: none) |

| Type | Name | What |
|---|---|---|
| 1 | CALL | an out-call (4.2) |
| 2 | MEM | memory changed outside guest code and outside any out-call (4.3) |
| 3 | CTX | a context changed outside guest code and outside any out-call, as the dispatcher resumes it (4.4) |
| 4 | INVOKE | a handler stacked on a thread (4.5) |
| 5 | INVOKE_END | a handler returned (4.5) |
| 6 | SWITCH | the dispatcher runs another thread or handler (4.6) |
| 7 | FRAME | a game frame ended (4.7) |
| 8 | OP | an operation on the kernel's state (4.8) |
| 9 | INIT | the memory as the run starts (4.9) |

**A call's records.** A CALL record is written when the call ends; the records made while it ran (operations on the
kernel, handlers it stacked, a frame end inside it) follow it, flagged `0x80`, with its seq. Their cycle is the time
they were made.

### 4.2 CALL

flags: bit 0 **unwound** (the call did not return to its caller: its thread blocked, was switched out, or had a handler
stacked on it; the dispatcher resumes it later), bit 1 **frame inside** (the game frame ended inside it: a FRAME record
follows), bit 2 **cut** (the run ended inside it), bit 3 **context lost** (unwound, and the context it ran on no longer
exists: no context words), bit 4 **transfer** (it returned with the dispatcher's transfer set: the guest code that
called it goes straight back to the dispatcher, at the `pc` the context holds, as after a checkpoint that fired).

| Offset | Type | Field |
|---|---|---|
| 40 | u32 | `ra` at the call (for a function call, the caller's return address) |
| 44 | u32 | `sp` at the call (the stack arguments of a call with more than eight are at `sp`) |
| 48 | u32[4] | `t0`-`t3` (low words: arguments 5-8) |
| 64 | u32 | `f12` (bits) |
| 68 | u32 | `f13` (bits) |
| 72 | u64 | cycleAfter: the EE cycle when it ended |
| 80 | u32 | timer-poll cycles pending at the call (charged at the next checkpoint) |
| 84 | u32 | timer-poll cycles pending when it ended |
| 88 | u32 | depth: the handler depth of the context it was made from |
| 92 | u32 | reserved (0) |
| 96 | out-call | the out-call record of `capture.md` 7.3: kind, size, target, value (a0-a3 for kinds 1-2; v1, a0-a2 for a syscall; the value loaded or stored), the context words it changed against its entry, and the memory runs it changed against the memory at its entry |
| ... | u32 | traceCount |
| ... | traced[traceCount] | the calls of the reference record's `calls` field made inside this call (4.10) |

The memory runs are the call's whole effect, including what the IOP or the dispatcher did while the call ran (a call
that accounts EE time runs the IOP; a frame end inside it may run vblank work). For a call with a frame inside, the
FRAME record holds the memory as of the frame end; the CALL's runs are the final state.

### 4.3 MEM

sub: the **source** (section 5).

| Offset | Type | Field |
|---|---|---|
| 40 | u32 | runCount |
| 44 | u32 | reserved |
| 48 | u64 | originCall: for sources 5 and 6, the seq of the call that made the wait or the handler being completed (all ones when unknown or none) |
| 56 | runs | the memory changed, against the memory when the change began |

### 4.4 CTX

| Offset | Type | Field |
|---|---|---|
| 40 | i32 | thread |
| 44 | u32 | depth |
| 48 | u32 | count: context words changed |
| 52 | u32 | reserved |
| 56 | u32[11] | changed-word mask (as in `capture.md` 7.3) |
| 100 | u32[count] | the new values, in word order |

Written as the dispatcher is about to run guest code on (thread, depth), when its context differs from its **last
context**: the context it had when guest code last stopped running on (thread, depth). The last context is all zeros
when guest code never ran there, and again from the INVOKE that stacks a handler at that (thread, depth). So the first
CTX of a thread holds its start context, and the first CTX of a handler its whole initial context (a library's handler
context may hold more than its arguments: what it copied from the caller). What a CTX record shows is what the kernel
or a library continuation changed meanwhile: a wait's result in `v0`, a resumed thread's `pc`.

### 4.5 INVOKE, INVOKE_END

sub: the handler's kind: 1 interrupt (INTC or DMAC handler), 2 alarm, 3 GS vsync callback, 4 RPC callback, 5 syscall
override, 6 exit handler, 7 library call (a library running guest code on a thread, libmpeg's), 8 SIF command handler.

INVOKE (104 bytes): 40 i32 thread, 44 u32 depth (its depth, the thread's depth after it), 48 u32 `pc` (the handler),
52 u32 flags (bit 0: it has a completion, run by the dispatcher when it returns: section 5 source 6), 56 u64 tag
(the SIF command id for kind 8), 64 u64 sequence (the scheduler's count of handlers queued or stacked), 72 u32[4]
`a0`-`a3`, 88 u32 `gp`, 92 u32 `sp`, 96 u32 `ra` (0: the handler returns to the dispatcher), 100 u32 reserved.
An INVOKE inside a call: the call stacked the handler on its own thread and left (the call is unwound).

INVOKE_END (64 bytes): 40 i32 thread, 44 u32 depth (the handler's), 48 u32 `v0`, 52 u32 `v1` (its result), 56 u64
sequence. A handler ends when it returns to address 0.

### 4.6 SWITCH

Written before guest code runs on another (thread, depth) than the last one that ran. sub: the **reason**, from the
state of the thread that ran last: 1 blocked (it waits), 2 preempted (it is ready: a higher-priority thread became
ready, the end of a time slice, a rotation of its ready queue), 3 ended, 4 suspended, 5 deleted, 6 the first guest code
of the run, 7 a handler starts on the same thread, 8 a handler returned to the context below it, 9 other.

| Offset | Type | Field |
|---|---|---|
| 40 | i32 | from: the thread that ran last |
| 44 | u32 | its depth |
| 48 | i32 | to |
| 52 | u32 | its depth |
| 56 | u32 | wait reason of `from` when blocked: 1 sleep, 2 semaphore, 3 event flag, 4 vblank, 5 external (a library's wait), 6 libmpeg's picture wait |
| 60 | u32 | the wait's object: the semaphore or event flag id, or the external wait's type |
| 64 | u64 | the external wait's token, or the vblank count waited past |

A thread that blocks and is resumed with no other guest code running in between gives no SWITCH.

### 4.7 FRAME

The game ended a frame: the point where the reference writes its per-frame record (the RAM and scratchpad hashes are
of the memory at this point). 40 u32 the frame that ended, 44 u32 runCount, 48 u32 traceCount, 52 u32 reserved,
56 runs, then traceCount traced calls (4.10). When the frame ends inside a call (flag `0x80`; in TMB always: the
frame end is a port override), the runs are what the call had written by then and the traced calls those it had made
by then; the frame's record sees exactly those. Otherwise both counts are 0.

### 4.8 OP

An operation on the EE kernel's state, made by whoever made it: a kernel call, a library, a port override, the
dispatcher, the IOP. sub: the operation (below).

| Offset | Type | Field |
|---|---|---|
| 40 | u32 | origin (section 5) |
| 44 | u32 | count: argument words |
| 48 | u32 | depth: 0 when made by the origin itself; n when made by the kernel while carrying out an operation of depth n-1 (a semaphore's signal waking a thread) |
| 52 | u32 | reserved |
| 56 | u64 | originCall: the call's seq when the origin is 1 (all ones otherwise) |
| 64 | u64 | wide argument (below) |
| 72 | u32[count] | arguments |

| Op | Name | Arguments; wide |
|---|---|---|
| 1 | queue | a handler queued to run on its own: kind (as INVOKE's sub), `pc`, `a0`-`a3`, `gp`, `sp`, `ra`, has completion, sequence (low word), tag (low word); wide = tag |
| 2 | block | the running thread waits: thread, wait reason (as SWITCH), object (semaphore or event flag id, external type), event flag bits, event flag mode, has completion; wide = the event flag's result address, the vblank count, or the external token. For a vblank wait the 4th argument is the fixed result (-1: the vblank parity) |
| 3 | wake | a waiting thread made ready: thread, result (its `v0`), has completion, wait reason |
| 4 | irq | an interrupt raised: 1 for DMAC (0 INTC), cause, enabled (0: kept pending while masked) |
| 5 | task | a host task posted to run at the next dispatcher pass (its operations have origin 10) |
| 6 | event | a scheduler event posted: type, id; wide = value |
| 7 | create | a thread created: id, entry, stack, stack size, priority, `gp`, attributes, option |
| 8 | start | a thread started: id, argument, interrupt-safe |
| 9 | exit | a thread ends (returned from its entry, or exited): id, deleted |
| 10 | terminate | id, 0 terminated (then interrupt-safe) or 1 deleted |
| 11 | priority | id, priority, interrupt-safe |
| 12 | rotate | priority, interrupt-safe (`RotateThreadReadyQueue`) |
| 13 / 14 | suspend / resume | id, interrupt-safe |
| 15 / 16 | wakeup / cancel wakeup | id (wakeup: interrupt-safe) |
| 17 | release wait | id, interrupt-safe |
| 18 / 19 | semaphore create / delete | initial count, max count, attributes, option / id, interrupt-safe |
| 20 / 21 / 22 | signal / poll / wait | semaphore id (signal: interrupt-safe) |
| 23 / 24 | event flag create / delete | bits, attributes, option / id, interrupt-safe |
| 25 / 26 / 27 / 28 | set / clear / poll / wait | id, bits (or mask), (mode), (interrupt-safe, result address) |
| 29 / 30 | alarm set / cancel | ticks, handler, argument, `gp`, `sp` / id |
| 31 | handler add | DMAC, cause, handler, append, argument, `gp`, `sp` |
| 32 | handler remove | DMAC, cause, id |
| 33 / 34 | handler enable / cause enable | DMAC, id or cause, on |
| 35 | vsync flag | flag address, tick address (`SetVSyncFlag`) |
| 36 | GS vsync callback | callback, `gp`, `sp` |
| 37 | field rate | numerator, denominator (fields per second: 60000/1001 NTSC) |
| 38 | thread setup | stack, stack size, `gp` (`SetupThread`) |
| 39 | transfer | interrupt-safe: a switch the kernel had requested happens now, if one is pending (a non-interrupt kernel call ends this way) |

### 4.9 INIT

The first record: the flat memory as the run starts (before any guest code), as runs over all zeros. 40 u32 runCount,
44 u32 reserved, 48 runs. The main thread's start context is the first CTX record.

### 4.10 Traced calls

The reference record's `calls` field (`harness.md`) hashes, per frame, every call guest code makes through the
dispatcher to a named stub address (the NAME chunk) and every raw `SYSCALL`, in call order. Calls made by guest code
running inside an out-call (an override that runs game code which calls `memset`) are in it too, and a runtime that
answers the out-call from the log does not make them; so each CALL record (and a FRAME inside a call) carries them.
Entry (32 bytes): u32 id (the FNV-1a 32 hash of the stub's name, top bit clear; `0x80000000 | number` for a syscall),
u32 guest address, u32[4] `a0`-`a3`, u32 `v0` (its result), u32 flags (bit 0 blocked; bit 31 it had returned or
blocked when recorded). They follow the out-call's own entry in the frame's list, in order. As for any entry, the
per-frame record itself marks one still open when its frame ends (bit 31 clear) with its flag "open at the frame end"
and no result.

## 5. Sources and origins

MEM records' source and OP records' origin:

| Code | Name | What runs |
|---|---|---|
| 0 | checkpoint | (origin only) an EE checkpoint inside guest code |
| 1 | call | (origin only) the out-call being made; its memory and context effects are in its CALL record |
| 2 | iop | an IOP slice, run as EE cycles were accounted (section 7) |
| 3 | iop-vblank | the IOP brought to the end of the vblank's field, at a vblank |
| 4 | events | the scheduler's events: a vblank (the vsync flag and counter written, `SetVSyncFlag`), timer interrupts, alarms, posted events and tasks |
| 5 | resume | a wait's completion, run as its thread resumes: a library's continuation (TMB: the CD library copying a read's sectors into the caller's buffer, libmpeg copying a decoded picture, libmpeg queueing its next call) |
| 6 | handler-end | a handler's completion, run as it returns (a library's: libmpeg waking the thread that waits for the picture) |
| 7 | handler-start | a handler stacked on a thread (its stack) |
| 8 | thread-end | a thread's end (a library's observer restarting its callback thread) |
| 9 | dispatcher | the rest of the dispatcher's work (strict mode only, section 9) |
| 10 | task | (origin only) a posted task running inside the events |

The kernel itself (BozoEngine's own) makes the events' work, `handler-start`, and every operation of depth > 0.
Libraries make sources 5, 6, 8 and 10, the operations of origin 1 made by library calls, overrides and VU0 or hardware
out-calls, and those of origin 2 and 3 (the IOP).

## 6. Kernel calls and library calls

**Implemented natively by BozoEngine** (NAME class 1, and every `SYSCALL`): the EE kernel's threads (`InitThread`,
`CreateThread`, `StartThread`, `ExitThread`, `ExitDeleteThread`, `TerminateThread`, `DeleteThread`,
`ChangeThreadPriority`, `RotateThreadReadyQueue`, `ReleaseWaitThread`, `GetThreadId`, `ReferThreadStatus`,
`SleepThread`, `WakeupThread`, `CancelWakeupThread`, `SuspendThread`, `ResumeThread` and their `i` forms),
semaphores (`CreateSema`, `DeleteSema`, `SignalSema`, `iSignalSema`, `WaitSema`, `PollSema`, `iPollSema`,
`ReferSemaStatus`), interrupt and DMA handlers (`AddIntcHandler`, `AddIntcHandler2`, `RemoveIntcHandler`,
`AddDmacHandler`, `AddDmacHandler2`, `RemoveDmacHandler`, `EnableIntc`, `DisableIntc`, `EnableDmac`, `DisableDmac` and
their `i` and `_` forms), alarms (`SetAlarm`, `ReleaseAlarm`), `SetVSyncFlag`, `FlushCache`, `EnableCache`,
`DisableCache`, `EndOfHeap`, `GetMemorySize`, `GsGetIMR`, `GsPutIMR`, `SetGsCrt`, `ResetEE`,
`Get/SetOsdConfigParam(2)`, and the raw syscalls (TMB's crt0: 60 `SetupThread`, 61 `SetupHeap`). Their CALL records
are what to check BozoEngine's results against (context words, memory, and the operations they made, nested at depth
0 under origin 1).

One kernel decision depends on hardware state the log answers: `EnableDmac` (cause enable, operation 34, on a DMAC
channel) raises an interrupt that arrived while the channel was masked only if the channel's `D_STAT` bit is still set,
and `D_STAT` is written by the game's hardware stores, which are answered from the log. A replaying kernel takes that
decision from the log: the interrupt is raised exactly when an irq operation for that channel follows (depth 1) inside
the kernel call's records.

**Answered from the log**: every other stub (the C library and libm, SIF and RPC, `sceSifSetDma` and the SIF
registers, the IOP heap and module loading, the pad, the memory card, CD and CD streaming, libmpeg and the IPU, the GS
library), every port override, every VU0 microprogram (kind 4), every hardware register load and store (kinds 5 and
6). Exceptions (kind 7) never occur in TMB.

## 7. Where the asynchronous changes happen (deterministic mode)

In the reference's deterministic mode everything that can change guest-visible state runs on the EE's host thread;
no other host thread writes guest memory or touches the scheduler (the IOP's own thread and the VIF1 worker are off;
the audio device only drains a queue; the disc's read-ahead worker fills host buffers only). The points:

- **The IOP** runs inside EE time accounting: each time 2048 EE cycles (256 IOP cycles) have accumulated, at an EE
  checkpoint inside guest code, inside an out-call that accounts time, or inside the dispatcher (an idle wait; and at
  each vblank, where it is brought to a sixteenth of a field past the end of that vblank's field). It reaches the EE
  only through the runtime's IOP interface: writes into EE memory (SIF DMA, RPC replies), SIF commands (a packet
  written into EE memory and a handler queued), RPC calls to EE servers, the sound driver's buffers. Inside an out-call this is part of the call's record; elsewhere it is a MEM
  record of source 2 or 3 and OP records of origin 2, pinned to the cycle of the accounting that ran the slice
  (checkpoints and pc say where guest code was).
- **The dispatcher** runs between two stretches of guest code: its MEM records (sources 4-8) and CTX records come
  before the guest code they precede; their point is the cycle at which the dispatcher took control (a checkpoint).
- **Out-calls** happen where guest code makes them.

TMB's boot and match1p runs: the IOP never changes EE state outside an out-call (no MEM record of source 2 or 3, no OP of origin
2): every SIF/RPC reply lands inside `sceSifCallRpc` or another call. The changes outside calls are the vblank's
(interrupts 2 and 3 raised, their handlers stacked; the vsync flag), the CD library's and libmpeg's continuations (24
MEM records, 12.9 MB: sectors and pictures copied into the game's buffers as its threads resume) and libmpeg's
library calls (`hle-call` handlers at `0x1feb98` on the decode threads, 59k).

## 8. Replaying

What a runtime does with the log (the reference build does exactly this in its own self-check, section 9):

1. Load the flat memory of INIT. Start the main thread; its first CTX gives its start context.
2. Run guest code. At each out-call, the next record must be this CALL: same kind and target, same inputs (`a0`-`a3`
   for stubs and overrides, the syscall number, the store's size and value), same point (cycle, seq, thread). A
   divergence is the first place two runs part.
3. A kernel call: make it natively; its record says what it must have done.
4. Any other call: do not make it.
   - Apply its context words to the context at the call, and (unless a frame ends inside it) its memory runs; give
     a load the value recorded.
   - Make again, in order, the records inside it: an OP of depth 0 (the operation, on your kernel: a queued handler
     with its completion if it had one, a block with a completion if it had one, a wake, an interrupt raised, a
     priority change, a rotation, a transfer... OPs of depth > 0 your kernel makes itself); an INVOKE (stack that
     handler on the current thread, then leave as the call did); a FRAME (first move time to its cycle, apply its runs
     and traced calls, then end the frame: the per-frame record is taken here).
   - Apply the context words again after those operations (the kernel's own may set the caller's return registers),
     then the call's memory runs if a frame ended inside it.
   - Move EE time to cycleAfter (the timers follow; no event is processed here, only at the next checkpoint) and the
     pending timer-poll cycles to the recorded value; if the transfer flag is set, return to the dispatcher at the
     next opportunity; if it was unwound, leave to the dispatcher (an operation made again usually did: a block, a
     transfer that switched threads, a stacked handler).
5. Library continuations: a wait created by a library call that had a completion completes with the log's operations
   of origin 5 (made again where its thread resumes), a handler with a completion ends with those of origin 6, a
   posted task with those of origin 10, a thread end with those of origin 8. Their memory is the MEM record at that
   point, their effect on contexts the next CTX of each thread.
6. Records of the IOP (source 2, 3; origin 2) are applied where EE time reaches their cycle with no call in between
   (their seq).
7. At every point the dispatcher runs guest code, the CTX record (if any) for that (thread, depth) gives the context
   exactly; SWITCH, INVOKE and INVOKE_END records say what the scheduler must have decided, and the events' MEM what
   the kernel must have written.

What the log cannot give: the scheduler's decisions (the log records them for checking, the kernel makes them); EE
time inside guest code (the runtime must charge it as in section 2); the draw lists and the sound (PS2XREF1's `draws`
and `sound` come from BozoEngine's renderer and mixer, which the log does not feed: the GS packets, VIF transfers and
the IOP's sound driver are among what it stands in for); anything the run did not exercise.

## 9. Self-checks of the reference build

The log is only published when these pass (TMB boot, below):

- **Determinism**: two recordings of the same run give the same file but for the run name.
- **Strict mode**: recorded again comparing the dispatcher's memory after every stretch of guest code, not only in its
  phases: the same records. So nothing the dispatcher writes falls outside a MEM record.
- **Substitution**: the run made again, every record compared with the log's, and every out-call's and every
  asynchronous change's effect on memory and contexts thrown away and replaced by the log's: every record the same,
  and the reference record identical to the one recorded without the log on every field.
- **Replay**: section 8, on the reference build: every out-call but the kernel's answered from the log (the call not
  made), the IOP not run at all, the libraries' continuations and operations made from the log, only guest code, the
  EE kernel and the scheduler running. Every record the replay makes equals the log's, and the reference record equals
  the one recorded without the log on `ram`, `scratchpad`, `calls`, `threads` and `eeCycle` for every frame (`draws` and
  `sound` are not fed, as above).

## 10. Checks a valid log satisfies

`calllog_read.py <log> check`: the first record is INIT; cycle and frame never go back; a CALL's seq counts the CALLs
before it and every other record's seq equals it (or, flagged `0x80`, the index of the CALL just before, whose records
those are); a CALL ends at or after it starts; FRAME records count 0, 1, 2...; each SWITCH starts from where the
previous one went; INVOKE and INVOKE_END keep each thread's depth; MEM sources and OP origins and operations are known
codes; runs stay in the flat space; context masks match their counts; the END chunk counts the records and calls.

## 11. TMB

| Run | Frames | Records | Calls | Size |
|---|---|---|---|---|
| `boot` (`ref_boot.env`) | 3000 | 4,650,558 | 2,432,936 | 194 MB (203,677,655 bytes) |
| `match1p` (`det1p.env`) | 16000 | 74,615,923 | 63,675,806 | 3.46 GB (3,714,443,336 bytes) |

Boot's calls: 743,918 to 25 overrides (708,818 of them libmpeg's `switchThread` replacement, which rotates the ready
queue), 374,583 VU0 microprograms (3), 500,969 hardware stores and 354,129 loads (DMA channel and GS registers),
310,986 library calls (72 functions), 148,349 kernel calls (18) and 2 syscalls; 130,824 thread switches, 66,677
handlers, 1,883,705 kernel operations.
