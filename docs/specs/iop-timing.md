# Spec: the reference's IOP time (deterministic mode)

How the reference (TMB_Recomp on the PS2Recomp fork, `PS2X_DETERMINISTIC=1`, the build that recorded the call logs of
`calllog.md`) advances the IOP against the EE: when the IOP runs, how much IOP time each step gives, when its hardware
timers fire (989SND's 1/240 s tick), when the sound mixer steps, and how an RPC call interleaves with all of it. Written
from the fork's behaviour by someone who knows it (PLAN.md section 6 allows that for specs): behaviour only, no code.
The rules are checked against the boot log in section 8. Who reads it: the native 989SND driver (`libraries.md`
section 15) and whatever replaces the IOP's clock natively.

## 1. Clocks and units

| Quantity | EE cycles | IOP cycles |
|---|---|---|
| A second | 294,912,000 | 36,864,000 (the EE's / 8) |
| An NTSC field (60000/1001 fields a second) | 4,920,115 | 615,014 |
| 989SND's tick (1/240 s) | 1,228,800 | 153,600 |
| A mixer sample (1/48,000 s) | 6,144 | 768 |

The IOP clock counts IOP instructions: every IOP instruction run (interpreted, or recompiled IOP code counting as the
interpreter would) advances it by one; when no IOP thread can run, it jumps to the next event (section 3). An IOP reset
(`sceSifRebootIop`) sets it to 0.

Nothing runs the IOP on its own thread or on the host's clock in deterministic mode: the IOP runs on the EE's host
thread, at the points below, and its time comes only from EE cycles.

## 2. EE time handed to the IOP

Let **H** be the EE cycles handed to the IOP so far.

- **Every EE cycle accounted is handed**, at the moment it is accounted (`calllog.md` section 2's time model: the
  checkpoints' 32 and 8, timer-poll cycles, an out-call's own time, an idle wait's cycles up to the next event): H grows
  with the EE cycle count, by the same amounts at the same points.
- **Payment in batches.** Handed cycles wait in a pending count. When pending reaches 2,048 EE cycles or more, the IOP
  is paid floor(pending / 8) IOP cycles and runs them at once (section 3); pending keeps pending mod 8. So the IOP's
  paid total is **P = (H - pending) / 8, with 0 <= pending < 2,048** (the IOP is paid in steps of at least 256 IOP
  cycles, and runs at most 255 IOP cycles plus a remainder behind EE / 8).
- **The vblank catch-up.** At the EE's k-th vblank (the scheduler's VBlankStart event, k = 1, 2, ..., due every
  4,920,115 EE cycles from the start), H is raised to V_k = 8 x (615,014 k + 38,439) if it is below (the end of the
  IOP's k-th field plus a sixteenth of a field, plus one). Since 8 x 615,014 = 4,920,112 is 3 EE cycles less than the
  EE's field, H gains 3 EE cycles a field on V_k: **only the first vblank catches up**, by D = V_1 - H then
  (5,227,624 - 4,920,115 = 307,509 EE cycles when nothing else ran ahead: 38,439 IOP cycles, 1.04 ms), and from then
  on H = EE cycle + D. So the IOP runs about 1.04 ms ahead of the EE **permanently** from the first vblank; it does not
  "run ahead after each vblank" (later vblanks hand nothing extra).
- **An IOP reset** sets the IOP clock, P and pending to 0; H goes on. In TMB the reset (boot #27, EE cycle 4,927,984)
  comes after the first vblank (4,920,115), so D cancels out: from the reset on, P = (EE cycle - 4,927,984 - pending) / 8.

## 3. Running the IOP

Each payment runs the IOP up to the target P:

1. If the IOP clock is ahead of P by more than **1,474,560 IOP cycles (40 ms)**, P is raised to clock - 1,474,560.
   That excess is never paid back: the IOP clock keeps it against EE time for good. Call **G** the sum of these
   excesses since the reset.
2. While the clock is below P: unless a thread that ended its slice with interrupts disabled must go on, the services
   run first, at the current clock: pending DMA completions, SIF commands, queued interrupt calls, **timers due**, the
   IOP's own vblank handlers; then the highest-priority ready thread runs a slice of min(256, P - clock) instructions.
   When no thread is ready, the clock jumps to the earliest of P, a thread's delay end, a DMA completion, a queued
   interrupt call, **a timer's due cycle**, the IOP vblank.
3. At the end the clocked mixer is brought up to the clock (section 6).

When the clock is already at or past P (it ran ahead inside a call, section 4), a payment runs nothing; the IOP waits
until P catches up. So, at any EE point, **the IOP clock is max(P + G, where inline work left it)**, and it is exactly
P + G whenever the IOP has had a payment since its last inline work and is idle.

## 4. Inside an RPC call (`sceSifCallRpc` to an IOP server)

- The call is an EE out-call that charges **0 EE cycles** (boot #156, the first 989SND call: "+0 cycles", its reply in
  the call's memory runs; every one of both logs' replies lands inside its call: `calllog.md` section 7).
- The IOP is **not** brought up to the EE first: the server runs at the IOP clock as the last payment left it (up to
  255 IOP cycles plus the pending remainder behind EE / 8).
- **Ahead of the server**: the ready IOP threads at the server thread's priority or higher (and first a thread that
  ended its slice with interrupts disabled) run their slices until none is ready (at most 4,096 slices). No services
  run meanwhile: no timer fires.
- **The server runs inline**, as the thread that serves its queue, the request copied into its buffer: up to 2,000,000
  instructions, each advancing the IOP clock. Its libsd calls see that clock (the mixer brought up to it). **No timer,
  IOP vblank or other thread runs during it**, unless the server blocks (a semaphore, sleeping, waiting for a loader
  thread: then the scheduler runs the others with their services, as in section 3 step 2, the clock jumping to events,
  until the server can go on) or uses up its 2,000,000 instructions (the others get a slice with services, then it goes
  on).
- When it returns, the reply is copied to the EE, still at the call's EE cycle. The IOP clock is now ahead of P by the
  server's instructions (plus any blocked time). If that exceeds 40 ms (a bank load waiting on the disc, a module's
  start), the excess becomes part of G at the next payment (section 3 step 1).
- So **a tick that falls due during the server's run fires after it returns**, at the first scheduling point: the next
  payment that runs the IOP, which is later still if the clock is ahead of P. Ticks never run inside a call whose
  server does not block.

`sceSifLoadModule` and the other IOP calls made from the EE work the same way: inline, 0 EE cycles, the IOP clock
advancing by what they run.

## 5. Timers and 989SND's tick

989SND's start routine (`libraries.md` 15.12, `989SND.IRX` 0x4a04) allocates a 32-bit timer counting the IOP clock
(source 1, prescale 1), gives it a handler (0x4c78) with the compare 153,600 (`USec2SysClock(1 s)` = 36,864,000, / 240),
and starts it at the IOP clock **S0** (its start routine's own point in IOP time).

- The timer is **due** when the clock reaches its base + 153,600 (base: S0 at the start). It fires at the first
  **service point at or after** that cycle (section 3 step 2): exactly at the due cycle when the IOP is idle there (the
  clock jumps to it); at the end of the running slice (up to 255 cycles late) when a thread runs; after the call when
  it falls inside an inline call (section 4).
- Firing runs the handler inline (its instructions advance the clock); it wakes 989SND's tick thread (0x4cd4) and
  returns 153,600, the next compare, **counted from the service cycle**: due(n+1) = service(n) + 153,600. Lateness is
  never made up: every late service shifts all later ticks.
- The tick thread then runs 989SND's tick (0x54b8) when the scheduler gives it the CPU, normally in the same pass (the
  highest ready thread). Its libsd calls (the ENVX reads, the KON and KOFF batch at its end) happen at the clock
  service(n) + the handler's instructions + the tick's own instructions up to the call. A payment's run stops at P, so a
  long tick can be split over two payments: it goes on at the next one (2,048 EE cycles later), in EE terms after the
  EE code that ran in between.

## 6. The mixer

- Sample n is made when the IOP clock reaches 768 n (`libraries.md` 15.14). The mixer is brought up to floor(t / 768)
  samples at the start of every libsd call (t: the clock at the call) and at the end of every payment's run; nothing
  else steps it. In EE terms: one sample every 6,144 EE cycles.
- 989SND's KON at clock t_k (end of tick n) and ENVX read at clock t_r (tick n + 1) see
  floor(t_r / 768) - floor(t_k / 768) samples of attack: 200 when the two ticks are 153,600 apart and the two calls lie
  at the same offset in their ticks, 199 to 201 otherwise, more when tick n + 1 was late.
- libsd's transfer ends (an upload's callback, each half of the sound data input) are interrupt calls queued with a
  2,000-cycle delay counted from the point the IOP takes them: the next instruction run with interrupts enabled, or the
  next service point.

## 7. Formulas for a native implementation

With c an EE cycle after the IOP reset at c_reset (TMB: 4,927,984) and no vblank catch-up after it (section 2):

- **IOP clock** (idle, after a payment): t(c) = floor((c - c_reset - pending(c)) / 8) + G, pending < 2,048. A native
  runtime that pays the IOP every EE cycle (no batching) differs by at most 255 IOP cycles.
- **Mixer samples made** by EE cycle c: floor(t(c) / 768).
- **Ticks**, ideal (never late): service(n) = S0 + 153,600 (n + 1); in EE terms every 1,228,800 EE cycles,
  c(n) = c_reset + 8 (S0 + 153,600 (n + 1) - G). Each lateness L (the rest of an inline call or slice covering the due
  cycle) adds L to every later tick: service(n) = max(due(n), end of the IOP work covering due(n)),
  due(n + 1) = service(n) + 153,600.
- **An RPC call** at EE cycle c runs at IOP clock t(c) (or later, if earlier inline work left it ahead), does not fire
  ticks unless its server blocks, and delays a tick due inside it to its end.

What a native runtime cannot know without the IOP's instruction counts: S0, G, the tick lateness, and the offsets of
989SND's libsd calls within a tick. A native driver can take S0 and G from a log (G mod 768 from the movie's positions,
section 8) and treat lateness as 0; the error on a tick is then at most the length of the call or slice it fell in
(a few thousand IOP cycles: a few samples), which moves a sound's end by at most one tick.

## 8. Checks against the logs

- **RPC inline, 0 EE cycles**: boot #156 (`sceSifCallRpc`, function 0, EE cycle 36,061,964, "+0 cycles"), #159 and
  #162 (the first two batches, +0 cycles, their replies in their own runs). In both logs every 989SND reply is in its
  call.
- **The IOP clock runs at exactly EE / 8, and the mixer's samples lie on its 768-cycle grid.** 989SND's GetTransStatus
  (0x40, inside the movie override) answers sceSdBlockTransStatus: the movie sound's buffer address + the byte played
  next (6,144-sample ring, 256 left then 256 right samples per KB). boot has 932 of them, one per frame (frames 619-1077
  and 1117-1589, the two movies):
  - #2745326 (frame 619, EE cycle 3,414,533,092, reply 0x000862fa) to #2745444 (frame 620, 3,419,448,204, 0x00086f3a):
    4,915,112 EE cycles = 614,389 IOP cycles = 799.986 samples; the reply moves 0xC40 bytes = 800 samples.
  - #2749535 (frame 652, 3,576,738,700, 0x00087f3a) to #2749672 (frame 653, 3,581,654,436, 0x01088b7c): 4,915,736 EE
    cycles = 800.087 samples; the reply moves 801 samples (the extra 0.087 crossed a sample boundary). Likewise
    #4025149 to #4025265 and #4045505 to #4045631 (800.08 samples, 801 moved).
  - Over each movie one offset K fits every reply: position = floor((c / 8 + K) / 768) holds for all 459 replies of the
    first (#2745326-#2808498) with K in a window 64 IOP cycles wide, and all 473 of the second (#4024154-#4080149) in
    one 53 wide. The IOP clock is EE / 8 plus a constant over 8 s at a time, payment batching included.
- **G is not 0, and grows.** The two movies' offsets differ: K mod 768 lies in [334.5, 398.5] for the first and
  [671.5, 724.5] for the second. With no unpaid IOP time (G = 0) the reset at #27 predicts K mod 768 in (450, 706]
  (t = (c - 4,927,984) / 8 less up to 256 of payment remainder and the server's own instructions before its libsd
  call): the second movie fits it, the first does not. So the IOP clock gained unpaid time after the reset (the module
  loads and bank loads run inline, section 4) and again between the movies; the log does not show where.
- **The tick**: the natural ends of `libraries.md` 15.12 (sound 3 of bank 0xe8310 at EE cycles 39,259,868,387 and
  46,869,503,443) fit KeyOn at the next tick, 1.849 s of sample, then the 256th silent tick, ticks 1,228,800 EE cycles
  apart (2.912-2.920 s after the play, against the log's 2.9002-2.9169 s).
