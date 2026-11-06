# Symbol maps: an executable without symbols

Everything that binds a port's calls goes by the ELF's function symbols: `bozo-externals` writes the stubs from them,
`bozo-recomp` names and places functions by them, the free host binds kernel calls and native libraries to them. Some
games ship stripped. A symbol map gives such an executable the names it lost, and every reader treats the map's names as
the ELF's own: there is no second path through the engine.

## 1. The format

A text file, one function per line, `#` starts a comment:

    # Symbol map of SLUS_212.16
    scePadRead 0x422210
    WaitSema 0x436fb0   # a comment

- A name is any token without blanks; an address is C notation (`0x...`), nonzero, four-byte aligned, and must be an
  instruction of the ELF's executable sections (or executable segments, without section headers).
- A name given twice with one address counts once; with two addresses the map is refused. Two names at one address are
  allowed, as in an ELF (aliases): the first name read is the one the free host binds.
- Anything else refuses the whole map before anything reads it: a malformed line, an address outside the code. A bad map
  never half-applies.
- The names of a map come after the ELF's own symbols, so a symbol the ELF has always wins at its address.

Map entries have no size, so they name functions and do not fix their extent: the recompiler finds a function's end as
it does for any other entry.

## 2. Who reads it

| Reader | How | What it does |
|---|---|---|
| `bozo-recomp` | `--symbols <file>` (repeatable) | function names in the generated code, labels |
| `bozo-externals` | `--symbols <file>` (repeatable) | a named kernel call or native library function becomes a stub |
| `host::FreeHost` | `FreeOptions::symbolMap` | binds kernel calls and native libraries, names faults and profiles |

`ElfFile::loadSymbolMap` is the one parser. A port keeps the map beside its other recompiler configuration (TMB's
`recomp/tmb_stubs.txt`, for instance) and passes the same file to all three. A map holds names and addresses and no game
code, so it can be committed like those configurations.

## 3. Making a map: `tools/symbolmap/bozo_symbolmap.py`

The tool (Python standard library, nothing else) names a stripped EE executable from *reference* executables that keep
their symbols. A misnamed function bound natively breaks a game silently, so **it never guesses**: a name goes into the
map only when the evidence picks one function and one name, anything else is reported with its candidates.

    bozo_symbolmap.py engine-names <engine checkout> want.txt
    bozo_symbolmap.py match game.elf --ref reference.elf [--ref ...] --out map.txt --report report.txt --want want.txt

What it compares is the function's code with its relocated fields masked: `jal` and `j` targets, `lui` immediates, the
low halves and offsets that use a `lui`'s register, `$gp` offsets. A kernel wrapper keeps its call number (`li v1,N`),
and a function's own constants stay.

| Tier | Evidence |
|---|---|
| `exact` | The whole masked code equals one reference function's; no other reference name has that code, no other target function matches. Short code (under 12 words for a leaf, under 24 with calls) must also call the same functions as the reference. |
| `calls` | An identified function calls it at the same place the reference's calls that name, and its code is at least 0.70 alike. |
| `string` | It refers to the same string as the reference function, its code is at least 0.60 alike, nothing else does both. |
| `layout` | It sits between two identified functions of its library in the reference's order, its code is close (0.75, or 0.90 under 16 words), and nothing in that span is as close. Identical twins (`AddIntcHandler`, `AddIntcHandler2`) are told apart by that order. |
| `rpc` | Same place, same shape (the instruction sequence without registers or fields), the same RPC function number passed to `sceSifCallRpc`. |
| `drift` | Same place, shape at least 0.85, at least 3 identified calls agree, at least 2 distinctive constant loads agree. |
| `reviewed` | A person read the code: a `--reviewed` line (`name 0xADDR # evidence`). The tool refuses a line that contradicts its own matches. |

A close function that loads another constant into an argument register is a sibling (the RPC or kernel call next to the
right one), never the same function. A rival check refuses a fuzzy match when another reference function, not placed
elsewhere, is at least as close.

The report lists how each name was found and, for each `--want` name left out, why: its candidates, or `not linked` when
its identified neighbours are adjacent in the target, which is the code's own proof that the function is not there.

### How the tiers are measured

`--check` matches a target that keeps its symbols as if it were stripped, then scores the map against them. On two
references built on one SDK release (2.8, with different library sets), matched against each other in both directions:

| Tier | Right | Wrong |
|---|---|---|
| `exact` | 1249 | 0 |
| `calls` | 108 | 0 |
| `layout` | 112 | 0 |
| `drift` | 1 | 0 |

A same-shape match with nothing else behind it was wrong one time in three in this measure, which is why it stays a
candidate in the report and never reaches the map.

This measures errors from functions one side lacks, not drift between SDK releases: no two references of different
releases were at hand. `rpc` had no case and `drift` one. For a target on another release, `exact` carries this measure;
treat the close-code tiers as evidence to read, and read what a port binds natively from them.

### Clean room

References are executables a port's developers own and that carry their symbols. Maps are derived from code alone, by
this tool and by reading: never from another project's analysis output or stub lists. Signatures are never stored or
committed (they would be Sony's code); the tool reads the references each time and only names and addresses come out.
