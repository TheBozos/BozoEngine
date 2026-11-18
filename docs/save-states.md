# Save states

BozoEngine saves the whole running game to a file and loads it back, in the same process or a new one, so a port
can come back to the same frame later (to test a scene, to debug, or for players). A loaded state goes on exactly as
the saved run did: a headless run of Jade Cocoon 2 saved at frame 9000 and loaded in a new process has the same EE RAM
1500 frames later as the run that never stopped.

## What is saved

A state is taken at the dispatcher's **safe point**, the top of `Dispatcher::run`'s loop. No guest code is on the
host stack there: every thread's registers are in its kernel frame, and no out-call, override or native IOP code is
running. Requests (`FreeHost::requestSave` / `requestLoad`, from any thread) wait for the next safe point.

| Part | Section | Code |
| --- | --- | --- |
| EE RAM, scratchpad, VU0 data | `memory` | `Dispatcher::serialize` |
| Dispatcher clocks, counters, COP0 | `dispatcher` | `Dispatcher::serialize` |
| Threads and their frames, ready queues, semaphores, handlers, queued handlers, vblanks, deadlines | `kernel` | `Kernel::serialize` |
| Heap, pad, memory cards (in memory), CD, VU0 image, I/O, MPEG, SIF, timers, library continuations | `libraries` | `libs/libraries_state.cpp` |
| The 989SND driver: banks, handles, slots; its engine's IOP memory, clocks, mixer, libsd and 989SND's own model | `snd`, `snd989-engine` | `SndDriver::serialize`, `snd989::Engine::serialize` |
| Frame count, last frame's clock | `host` | `FreeHost::serializeAll` |
| Whatever a port adds | its own name | `state::Sections` |

The native IOP host (`native::irx::CompiledLibraries`) has `serialize` too: IOP memory, tasks and their registers,
semaphores, event flags, timers, events, frame listeners, the RPC servers IOP code registered, the mixer (voices,
reverb, sound memory) and libsd. A port that owns one adds it to its section (see below).

What a port or host sets up once is **not** saved: overrides, callbacks, paths, switches, IRX images and bindings. The
loading run must be set up the same way (same build, same command line apart from the state options). A file
records a game tag (`FreeOptions::gameTag`) and a format version; a section that reads a different number of bytes
than was written fails the load with its name.

## Continuations and deadlines are data

Lambdas cannot be written to a file. Library code that leaves work for later passes a `libs::Resume` instead, a kind
plus four words:

```cpp
l.waitVblank(token, -1, Resume{kResumeCdStreamRead, {n}});
l.queueHandler(h, Resume{kResumeTimerHandlerEnd, {slot, tag, id}});
l.scheduleDeadline(key, cycle, kDeadlineDelayDue, {thread, 0, 0});
```

The kind picks a function from the libraries' table (`libraries.h`, `ResumeKind`, `NativeDeadlineKind`). A port adds
its own kinds from `kPortResumeKinds` / `kPortDeadlineKinds` with `Libraries::addResumeKind` / `addDeadlineKind`.
The `std::function` overloads still work, but while one is pending the state cannot be saved (a save waits and says
why).

## Using it in a port

```cpp
bozo::host::FreeOptions options;
options.gameTag = "SLUS-12345";          // a state loads only into the game that wrote it
options.loadState = "...";               // load before the first frame
options.loadStateFrame = 0;              // or when this many frames have ended
options.saveState = "...";               // save when saveStateFrame frames have ended
options.saveStateFrame = 2000;
bozo::host::FreeHost host(options);

// The port's own state: what it registered at run time, what it keeps about guest data.
host.stateSections()->add({"myport",
    [&](bozo::state::Archive &a) {
        myIop->serialize(a);                       // a native IOP host, if the port has one
        bozo::state::fields(a, myCounter, myMap);  // the port's own fields
        if (a.loading())
            myTextureCache.clear();                // anything decoded from the old guest memory
    },
    [] { return myIop->unsaveable(); }});          // optional: why it cannot be saved right now

// Window builds: F5 save, F7 load, F6 / Shift+F6 pick the slot (0-9), and a "Save states" tab in F2.
game.saveState = [&](int slot) { host.requestSave(bozo::app::stateSlotPath(slot)); };
game.loadState = [&](int slot) { host.requestLoad(bozo::app::stateSlotPath(slot)); };
host.onStateMessage = [](bool, const std::string &m) { bozo::app::notify(m); };
```

A port's section walks its fields with `bozo::state::fields` (scalars, enums, strings, `std::vector`, `deque`,
`map`, `unordered_map`, `set`, `array`, C arrays, `pair`, `unique_ptr`, and any struct with an `io(Archive &, T &)`
next to it). `state::pod` writes a trivially copyable struct with no pointers as its bytes.

Things a port must think about:

- **Run-time registrations.** Anything the game made the port register (a SIF RPC server bridged to EE code, a
  callback) must be kept as data in the section and registered again on load, after dropping the ones of the run
  being replaced (a load can happen mid-run, from F7).
- **Caches of guest data.** Textures, meshes or pictures keyed by guest addresses are stale after a load: clear them.
- **Facts the game told the port once.** If the port noted something as the game loaded it (where a resource sits),
  save it: the loaded state will not tell it again.

## Not covered yet

- A playing movie (the decoder): a save waits, with the reason, until it ends.
- Recorded and pinned runs (`--out`, the reference harness): their records do not follow a load.

A failed load in the middle of a run puts the state back as it was (the runtime saves itself first); a failed load
before the first frame ends the run.

## Checking a port

Headless runs are deterministic, so a state can be checked against a run that never stopped:

```
game --headless --frames 3000 --save-state s.state --save-state-at 2000 --dump-ram a.bin
game --headless --frames 3000 --load-state s.state --dump-ram b.bin                     # a new process
game --headless --frames 3000 --load-state s.state --load-state-at 2500 --dump-ram c.bin # a load mid-run
```

`a.bin`, `b.bin` and `c.bin` must be identical. `tests/runtime/state_tests.cpp` covers the archive, the files and a
dispatcher saved and loaded.
