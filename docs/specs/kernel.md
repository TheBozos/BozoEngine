# Spec: the EE kernel, the dispatcher and the cycle model (phase 2)

BozoEngine's own EE kernel and scheduler, as implemented in `runtime/ee/` (`bozo/ee/kernel.h`,
`bozo/ee/dispatcher.h`), and the EE time model the generated code and the dispatcher charge. Everything here was
written from the call logs' records (`calllog.md`), which say what the reference build decided and when; the record
that settled each rule is named next to it. The kernel and the dispatcher are the real runtime's: the call log only
stands in, behind the `ee::Host` interface, for the libraries BozoEngine does not implement yet (`tools/run/`,
`bozo-run`, section 7).

The dispatcher runs in one of two modes (section 4, "Free mode and pinned mode"). In **free mode**, the real
runtime's, BozoEngine's own cycle model decides when the vblank's events happen. In **pinned mode**, a verification
layer on top of it, they happen where the reference took them, as its log records them; every other decision is
still the native kernel's and dispatcher's.

## 1. Layout

| Piece | Where | What |
|---|---|---|
| Kernel | `runtime/ee/kernel.cpp` | threads, ready queues, semaphores, INTC/DMAC handlers and their queue, the vblank, the operations of `calllog.md` 4.8 |
| Dispatcher | `runtime/ee/dispatcher.cpp` | a `RuntimeServices`: runs guest code on the kernel's contexts, stops and resumes threads, charges EE time, processes events at checkpoints, answers the kernel calls and syscalls natively |
| Host | `ee::Host` | everything else: library calls and port overrides, VU0 microprograms, I/O, CPCOND0, the libraries' continuations |
| Generated code | `recompiler/src/emitter.cpp` | the checkpoints and resume entries the dispatcher relies on (section 3) |

The kernel never runs guest code. Every operation it makes is emitted (an `OpEvent`, `calllog.md` 4.8) to an
observer before its consequences, which are one depth deeper; `bozo-run` checks each against the log.

## 2. The kernel

**Threads.** Thread 1 is the main thread, running at boot with priority 0, its context: pc the ELF entry, sp
0x1FFFFF0, vf0.w, Q and R 1.0, COP0 Status 0x10001 (the first CTX). `CreateThread` reads an `ee_thread_t` (func at
+4, stack +8, stack size +12, gp +16, initial priority +20, attr +28, option +32); ids count up from 2 and are never
reused. `StartThread` gives the thread a fresh context: a0 the argument, gp, sp = stack + size, pc the entry, ra 0,
vf0.w and Q 1.0 (R 0), Status 0x10001. A thread whose entry returns (to pc 0) ends: the dispatcher makes `exit`
(origin dispatcher). `TerminateThread` returns 0, or -413 (KE_DORMANT) for a thread that has ended (boot #1394773);
`DeleteThread` returns 0.

**Ready queues.** One FIFO per priority (0 highest) holds the ready threads; the running thread is outside them.
The scheduler runs the running thread unless a ready thread has a strictly higher priority, else the head of the
highest non-empty queue. A preempted thread goes back to the head of its queue; a thread that gives way (a
`transfer` that switches, the end of its time slice) to the tail (boot #729731: thread 1 had given way behind the
preempted thread 2). A woken or started thread goes to the tail. `ChangeThreadPriority` of the running thread only
changes its priority; of a ready thread it moves it to the tail of the new queue.

**RotateThreadReadyQueue(p).** When the running thread has priority p it asks for a switch: the next `transfer` gives
way even if it is alone at that priority (the dispatcher then picks it again: boot #228443, its transfer recorded
with thread 0 and no SWITCH after). Otherwise the head of queue p goes to its tail.

**transfer.** A non-interrupt kernel call ends with it (`StartThread`, `SignalSema`, `DeleteSema`), and the TMB
port's `switchThread` override makes one: the running thread gives way when a switch was asked for or a ready thread
outranks it. The operation is emitted after the decision, so it carries thread 0 when it gives way.

**Waits.** `block` takes the running thread out (reason, object, token, a vblank wait's fixed result, whether the
library that made it completes it). `wake` sets the thread's v0 to the result and readies it; if the wait has a
completion, the library's continuation runs when the dispatcher next picks the thread (section 4).

**Semaphores.** `CreateSema` reads an `ee_sema_t` (max count +4, initial count +8, attr +16, option +20); ids from 1,
never reused. `WaitSema` takes a unit or blocks (FIFO); `SignalSema` wakes the first waiter with result id, else
counts up.

**Interrupts.** INTC and DMAC each keep a mask and a pending set; at boot INTC causes 2 and 3 (the vblank's start and
end) and every DMAC channel are enabled (the first irq records). `irq` emits whether the cause was enabled; if it
was, every enabled handler registered for it is queued (kind interrupt, a0 the cause, a1 its argument, gp, sp 0),
else the cause is kept pending until `EnableIntc`/`EnableDmac` (for a DMAC channel only if its D_STAT bit is still
set: the host answers, `calllog.md` section 6). Handler ids count from 1, separately for INTC and DMAC, never
reused; `AddIntcHandler(cause, pc, next, arg)` inserts at the head for next 0, else at the tail. Enable/Disable
Intc/Dmac return 0 in every TMB record.

**The handler queue.** Each queued handler gets the next sequence number (from 1). The dispatcher stacks it on the
thread it is about to run (section 4), or on a thread of its own (negative id) when none is ready. Its context is
fresh with a0-a3, gp, ra and pc set; sp, when the queue gave none, is that thread's handler stack: 0x1FFFFF0 - 0x4000
x slot, slots given out in the order threads first get a handler (thread 1 slot 0; boot's decode threads 2, 4, 6:
slots 1-3).

**The vblank.** The field lasts `294,912,000 x den / num` EE cycles, rounded down (4,920,115 for 60000/1001, also the
default before the game sets its rate); vblank n starts at n x field and ends 147,456 cycles later. At a start the
vblank count goes up, the threads waiting for a count below it are woken in thread id order (match1p and boot
#1315977: not in the order they blocked), with their fixed result or, for -1, the inverted parity of the count; then
INTC 2 is raised. The end raises INTC 3. Events due by the same cycle are processed in due order.

**Syscalls.** 60 (SetupThread): returns stack + size. 61 (SetupHeap): returns the start rounded up to 16. A syscall
is known by its return address (the log's target).

**Results.** A kernel call's result is written to v0 as a whole 128-bit register, its upper half cleared (boot frame
1615, FlushCache).

## 3. The cycle model

EE time is charged only at checkpoints. What the generated code charges (`recompiler/src/emitter.cpp`) and the
dispatcher adds, all from the logs:

| Where | Cycles | Can stop there |
|---|---|---|
| A taken branch or `j` to a label of the same function at or before it | 32 | yes, resuming at the target |
| A call (`jal`, `jalr`, `bal`...) | 8, before the callee | yes, resuming at the target (the callee's entry or the stub) |
| An inner call: a call to a label inside the same function (texmActivateTextureAsm's calls to texmUpdateLastUsedAsm and texmFindVramSpaceAsm), or to the function's own start when the function before it is a two-instruction thunk whose `j` goes there (bspVecTest into bspVecTestIn: one function for the reference) | 8, counted; a goto | events taken in place (below); a stop they call for waits for the call's return (match1p #14757679) |
| The return to an inner call's return address, by the invocation that made it | 0; a goto back there | events taken in place; a stop they call for happens there (boot frame 2088, match1p #14757679: pc 0x1fba84, after the callee ran) |
| A recursive call to the function's own start (no thunk) | 8, an ordinary call | yes (match1p #17531044) |
| A return (`jr ra`) to anything but those calls' return addresses, by an invocation that made a recursive call or a call to one of its own labels | 0; it goes through the dispatcher (the frames below are gone) | yes |
| A `jr` through any register but ra (a copy of ra included), and a jump table the reference does not resolve | 8, then the jump goes through the dispatcher (the jumping function's frame is gone) | yes |
| A jump table the reference resolves: its base built in place (`lui r; addiu r, r, lo`) and an `sltiu` range check before it | 0, a switch | no |
| A direct `j` or branch to another function, falling into the next function | 0 | no |
| `jr ra` | 0 | no |
| The dispatcher starting guest code | 8 | events due by then are processed first (boot #1315977) |
| An out-call | its record's `cycleAfter - cycle` | the log's flags say whether it left to the dispatcher |
| An EE timer read | the log's pending cycles, charged at the next checkpoint | |

**Frames.** Guest code stops by unwinding to the dispatcher (a C++ exception, `ee::StopGuest`) with the context's pc
at the point it goes on from; the dispatcher later runs it on through the generated resume entries (every function
can be entered at any label and at the return address after each call). So, as in the reference, a function whose
C++ frame is gone returns to the dispatcher, and running its caller on costs a dispatcher start (boot #191: 16
cycles from sceCdGetError to the override 0x1c2ba8). After every call the generated code compares ctx->pc with the
call's return address and returns too when they differ: an override that left guest code elsewhere (0x1d2ea0 returns
with pc 0x1fba84, boot #228128) is run on from there.

**Inner calls.** The invocation that makes an inner call keeps its return address (a return stack of its own); its
return there is a goto. Until all of them have returned, guest code does not leave to the dispatcher: an event due at
the inner call, at any checkpoint between (a call the callee makes) or at the return is taken in place, the frames
kept, and costs one dispatcher start (8 cycles, one checkpoint), as if the dispatcher had stopped guest code and
resumed it where it is with nothing lost. Only a handler to run or a thread to switch to stops it: at once at a
checkpoint or an inner return, at the call's return when the event was taken at the inner call itself. Records:
- boot frame 2088, match1p #14757679: texmActivateTextureAsm's inner call ends at 23149141076 with the vblank (due
  23149141075) and its handler queued: the irq is recorded there, the handler starts once the callee ran, and guest
  code resumes at the return address 0x1fba84 (no lost frame: one dispatcher start, then the jump through t9 and its
  own start).
- match1p #18297743 (K1, frame 5773): a vblank start (disabled, no handler) due at 30445671620 during bspVecTestIn's
  11-deep recursion is taken at its call to bspPlaneTest2Vec (30445671627): one dispatcher start; the levels return
  to each other without the dispatcher, and the recursion's exit goes through it as every such exit does. Stopping
  there instead (every level lost) costs 12 dispatcher starts too many by #18297744.
- match1p #24702159, #31440533: a vblank end taken at bspVecTestIn's inner call itself (36133472019, 41540678403):
  one dispatcher start.
- An invocation the dispatcher resumed in the middle has no return stack: its returns to those addresses are plain
  returns, each a dispatcher start (match1p #17538427: bspVecTestIn resumed at 0x201418 three levels deep after the
  override 0x1d2ea0 stopped there: three dispatcher starts at 0x201418). A recursive call to the function's own start
  without a thunk is an ordinary call whose frames a stop loses (match1p #17531044, _M_erase).

**What has a hardware reason and what is the reference's.** Free mode needs a clock that advances with guest work
so the vblank (every field, 60/1.001 Hz) and the time slice land at sensible points; any monotonic charge would do.
These rules have no reason on real hardware and are there because the reference's runtime has them. They keep
BozoEngine's checkpoint count equal to the reference's between out-calls, which is what pinned mode places events
by, so both modes keep them:
- the exact charges: 8 per call, 32 per backward branch, 8 per dispatcher start, 16 for a jump through a register;
- the frames a stop loses, and the dispatcher start each lost frame's return costs;
- the `recursed` exit through the dispatcher;
- which jump tables are a switch;
- inner calls: events taken in place, a stop deferred to the call's return, the invocation's own return stack.
A free mode that never needs to be compared with the reference (phase 3 onward) can charge a single cost per
checkpoint and treat inner calls as ordinary calls.

**Which jump tables the reference resolves.** dbsTraverse's table (`lui a0; addiu v1, a0, lo`) is not resolved: each
jump costs 16 and loses the frames (boot, 3,359 jumps: 6,719 checkpoints); uiFadeText's (`lui v0; addiu v0, v0, lo`,
`sltiu`) is resolved (boot record 228027); the table at 0x17f304, built in place but with no `sltiu`, is not
(match1p #17536212).

## 4. The dispatcher

The loop: process the events due by now (vblank, origin events); choose; charge the dispatcher start (8); if an
event is now due, go back to the start (the choice is made again); report a SWITCH when the (thread, depth) differs
from the last one that ran; check and apply the CTX; run guest code until it stops or the bottom function returns.

**Choosing.** A handler interrupted on a thread of its own goes on first. Otherwise the scheduler's thread is made
the running one; if a library's completion of its wait is due, it runs first (origin resume) and may block it again,
and the choice is made again (boot #228467). Then a queued handler is stacked on that thread (an INVOKE), or on a
thread of its own if no thread is ready (boot #729696: the completion's MEM comes before the INVOKE).

**Stopping.** A checkpoint stops guest code when an event is due, or, on a thread's own context (depth 0), when a
handler is queued, a switch is pending, or its time slice is over. A handler at depth 1 is not stopped for a queued
handler (the next one starts when it returns: boot #729748) nor for a switch, but it is for an event (boot #729698:
the decode thread's library handler preempted by the vblank).

**The time slice.** 65,536 cycles, counted from the dispatcher's decision that picked the thread out of the ready
queue (before its 8-cycle start; boot #729788 and #729793: preempted exactly 65,536 cycles later), not restarted by
a handler stacked on it (match1p #14760095). When it ends and a ready thread of the same or a higher priority exists,
the thread goes to the tail of its queue (no operation is recorded) and the SWITCH says preempted.

**Free mode and pinned mode.** In free mode (`Dispatcher::pinned` false), an event is due when the EE clock reaches
it (`Kernel::nextEvent`). In pinned mode the host says when: the vblank's next event (its start or end, in the
kernel's own order: `Kernel::processNextEvent`) is due at the point its first record has in the log. That point is
the out-calls made before it, the checkpoints passed since the last one ended, and the pc. Nothing else changes:
- every checkpoint counts as in free mode;
- the event stops guest code, is taken in place inside inner calls, and wakes, queues and switches through the
  native kernel;
- the kernel's records (OP, INVOKE, SWITCH, CTX) are checked in lockstep.

The other asynchronous decisions need no pin, because they follow from these events and from out-calls that are
answered at their own records:
- handler invocations, wait completions, the MEM records of library continuations (origin resume, handler-end,
  thread-end);
- preemption at the end of a time slice: the clock is resynced to the log's at every record, and the native slice
  matched every SWITCH of boot and match1p.

When an event is near (the next record is one, before the next out-call), every checkpoint asks the dispatcher
(`Host::eventPinnedSoon`); otherwise checkpoints run at full speed. Two tolerances, both settled by records:
- **The record's pc inside inner calls.** A record carries the reference's context pc when it was made. Outside
  inner calls that is the checkpoint's pc: every event of boot (6,144) and match1p (32,808) outside inner calls
  matches it exactly. Inside inner calls the
  reference's context pc lags: it is the pc of a checkpoint passed lately in the same inner calls.
  - match1p #18297743, at bspPlaneTest2Vec's call: 0x201418, the return address of the inner call before.
  - #18359662, at the inner call itself: 0x201660, the call before.
  - #31601175, at bspPlaneTest2Vec's call after an inner call that returns to 0x201490: 0x201418.

  So inside inner calls the logged pc is also accepted when it is one of the last 64 checkpoints' pcs. An inner
  call's own pc is its return address (#14757679 at texmActivateTextureAsm: 0x1fba84; #24702159).
- **One checkpoint late.** When BozoEngine's count is one ahead of the reference's in a stretch, the logged count
  is reached one checkpoint early, at another pc. The event is then taken at the next checkpoint, if that one goes
  on at the logged pc, and reported. This happened twice in match1p (#50893616, #50900546, frame 11358).
  - #50893616 (frame 11358): animation code after timerGetCurTics' io-load. findCurrentKeyframe's loop reaches
    count 203 at 0x1bea20 at cycle 57900060779; the reference is at 0x1bea80, the next checkpoint, at cycle
    57900060803.
  - So BozoEngine counts one 8-cycle checkpoint before the loop that the reference does not. Which one is open
    (section 9, K5).

A count that passes the logged one by more, or a pc that matches neither way, is a divergence. EE time is reported,
not checked:
- the records at another cycle, and those at another checkpoint count (the K4 kind: for a CALL, a difference ends
  at the next out-call);
- eeCycle, BozoEngine's clock at the frame's end. Out-calls set the clock to the log's, so this is the clock at the
  frame's out-call plus how far it was from the log's there.

**SWITCH reasons.** Start for the first; handler-end after a handler returned; handler-start for a handler stacked on
the thread that ran; else from the state of the thread that ran last: blocked (with its wait), preempted (ready),
ended (dormant), suspended, deleted.

**Ends.** A handler returns to address 0: INVOKE_END with its v0 and v1, then its completion (origin handler-end)
when it has one, then its frame is dropped. A thread's entry returns to 0: `exit` by the dispatcher, then the
library's thread-end continuation if any.

## 5. The reference record's `calls`

The calls hashed per frame are the calls made by `jal`/`jalr` to a named stub (NAME) and raw syscalls, each followed
by the traced calls the log's CALL carries. Not in the list: a named stub reached by a jump (a tail call: boot frame
1, 9 calls), or run by the dispatcher at the bottom of a thread (resumed at a stub's address: boot frame 0, 9 calls).
Entries are 7 words {id, a0-a3, v0, flags}: flags 1 for a call that blocked (v0 0), 2 for the call the frame ended
inside (v0 0); the traced calls keep only their blocked bit.

## 6. What the dispatcher asks of a Host

`libraryCall` (a stub or override at ctx->pc), `vu0Microprogram`, `ioRead`/`ioWrite`, `cop0Condition`, the kernel
call and syscall brackets (for checking), and the notifications of its own decisions: INVOKE, INVOKE_END, SWITCH,
the context about to run, the libraries' continuations, faults. A Host stops guest code with `Dispatcher::stop`
(a call that blocked, gave way, or stacked a handler). The real runtime's Host will be the native libraries.

## 7. bozo-run's Host (the call log)

`tools/run/log_host.cpp` answers every non-native out-call from the log and checks, in lockstep, every record: each
CALL (kind, target, a0-a3 or the store's value, ra, sp, cycle, seq, thread, depth, timer cycles pending), each
operation the kernel makes (op, origin, depth, call, arguments, wide, thread, cycle), INVOKE, INVOKE_END, SWITCH, and
each CTX (the context guest code resumes with: the words the reference changed must be the ones BozoEngine's kernel
changed, except where only a library knows them: a library handler's first context, a wait completion's changes).
A kernel call's record must equal what the native call did (every context word but pc, no memory, its flags). The
operations a library made (depth 0 in a CALL, origin resume or handler-end) are made again on the kernel; their
consequences must come out as the log's records of depth 1. The FRAME record ends the frame: its time, its memory
and traced calls, then the PS2XREF1 record.

## 8. Results (TMB)

| Run | Mode | Frames | Matching (ram, scratchpad, calls, threads, eeCycle) | Records matched | Time |
|---|---|---|---|---|---|
| boot | pinned | 3000 | all 3000 on every field | 4,650,558 of 4,650,558 | 17 s |
| match1p | pinned | 16000 | all 16000 on every field (eeCycle reported, not checked) | 74,615,923 of 74,615,923 | 164 s |
| boot | free | 3000 | all 3000 on every field | 4,650,558 of 4,650,558 | 14 s |
| match1p | free | 16000 | 5,786 (frames 0-5785) on every field | 18,359,663 of 74,615,923 | 41 s |

Pinned mode is the check (section 4). Free mode stops at match1p frame 5786 on K4 (section 9), a difference of the
reference's EE time and not of the game's state.

## 9. Open questions

**K1.** (settled: section 3, inner calls) A vblank inside bspVecTestIn's recursion (match1p frame 5773, #18297744).

**K4. One more dispatcher start after some events taken at an inner call (match1p frame 5786, #18359663).**
Settled for pinned mode, which places these events at their records' points (section 4). It remains a free-mode
difference of EE time only, kept here as history; it is not worked on further (the fork's own cycle accounting). Seven
events of TMB's match1p land on bspVecTestIn's inner call at 0x201410 (a vblank due at or up to 6 cycles before its
charge, no handler). Each record's cycle and checkpoint count equal BozoEngine's right after that inner call. Two of
them (#24702159, #31440533, depths 12 and 5) then cost one dispatcher start, as section 3 says; five (#18359662,
#31497573, #31540689, #31581810, #37796924: depths 3, 1, 3, 1, 7) cost exactly one more before the next record (+8
cycles and +1 checkpoint, wherever it is placed: no record lies between). The only field that tells them apart is
the record's pc: 0x201418 (the inner call's return address) for the first kind, 0x201660 (bspPlaneTest2Vec, the call
the new level makes next) for the second. Neither the depth, nor how late the event is, nor the vblank's kind (start
or end) separates them. Charging the extra start for exactly those five carries match1p to frame 9177 (#37860383, a
separate +48 on an OP, 4 checkpoints against 1, not looked at), but that is a fit, not a rule, and is not in the code.
Open: what state of the reference's the pc 0x201660 reflects.

**K5. A checkpoint BozoEngine counts and the reference does not (match1p #50893616, frame 11358).** It sits between
timerGetCurTics' io-load and findCurrentKeyframe's loop: animationResume twice, 0x1bf108, animationUpdateFromControlNode,
animationGetCurrentTime, timerGetFieldCount, each an 8-cycle call here. Pinned mode takes the event one checkpoint
later, at the logged pc. Which call the reference does not count is open.

**K2.** (settled) texmActivateTextureAsm's return after its calls to texmFindVramSpaceAsm (#17538483) and
bspVecTestIn's return after it recursed (#17537437, #17538428, #17538329) go through the dispatcher (section 3).

**K3.** Not reached by TMB, implemented from the EE kernel's documented behaviour only: alarms, event flags,
suspend/resume, sleep/wakeup, `ReferThreadStatus`, `SetVSyncFlag` and the GS vsync callback (the kernel records the
operation and has no effect yet for those `calllog.md` lists but TMB never makes).
