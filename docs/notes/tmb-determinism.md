# TMB on the PS2Recomp fork: what makes two runs differ

Inventory for phase 0 (the reference recorder runs on the fork), from a read of TMB_Recomp's `runtime/` and the
fork's `native/renderer` (2026-11-02). Each item is a time or host-state input that reaches guest-visible state in
the default native mode, with what the deterministic mode does about it. BozoEngine's runtime must control the same
inputs by design (spec: `docs/specs/harness.md`).

| # | Source | Reaches the game through | Deterministic mode |
|---|---|---|---|
| 1 | The frame step is host time (`FramePacer::frameEnd`) | EE cycles charged for the frame (timers, vblanks, alarms, the IOP), TMB's field count, its substeps | Exactly 1/60 s per frame |
| 2 | `sceCdReadClock` returns the host's date | `TheGame::Init` seeds the game's RNG with `min*60+sec` (pickups, weapons, deaths); save timestamps | A fixed date |
| 3 | The IOP on the host clock (`PS2X_IOP_WALLCLOCK`) | 989snd ticks, IOP vblanks, CD callbacks, SIF replies landing at host-dependent EE points | IOP advanced by EE cycles only; no IOP thread |
| 4 | The mixer runs on the audio device thread | Voice end flags, envelopes, stream positions and block-transfer interrupts the sound driver reads | Mixed on the IOP's clock (768 IOP cycles per 48 kHz sample); the device only plays a queue |
| 5 | Pad scripts timed in seconds; live host input; pad presence from plugged devices | `scePadRead` results, which players/multitap slots exist | Scripts keyed on game frames, pad latched once per frame, no host input or presence |
| 6 | EE wall-clock catch-up (skips cycles to host deadlines) | Event timing | Off |
| 7 | Movies: picture clock from host time, host-deadline waits, decode time charged to the EE | Pictures served, guest preemption points | Movies skipped (ended at once) or frame-clocked |
| 8 | Lens flare depth read back from the GPU into guest RAM, an `inMatch` window in host seconds | Flare visibility tables, native routing | Depth reported far; `inMatch` from the last frame end's kind |
| 9 | Memory card contents and timestamps | Options, unlocks, progress, save dates | A pristine card per run, fixed timestamps |
| 10 | Settings window actions (cheats, debug pad) | Applied at a frame end chosen by the user's timing | Disabled |

Checked and deterministic: guest memory starts zeroed; no host pointers or hash-map order reach guest state; idle
waits charge exact cycles; read-ahead threads fill host buffers the guest waits on synchronously.
