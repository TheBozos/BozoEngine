# Function discovery

What `bozo-recomp` calls a function decides what it can translate well: a C++ function per guest function, gotos inside
it, calls between them. A wrong boundary never changes what the program does, a function that runs off its end
continues into the next one and a transfer into another function's body goes through an entry, but it costs: splitting
a function at every branch target hides its switches and makes every branch a call. This is how the functions are found.

## 1. Two paths

An executable goes by its symbols when its sized function symbols (STT_FUNC with a size, inside the code) cover at
least half of the code's bytes; `Analysis::run` measures it and the report's first line says `mode symbols` or
`mode code`. Symbol maps have no sizes (`symbol-map.md`), so a stripped ELF stays on the code path with or without one.

| | `mode symbols` | `mode code` |
|---|---|---|
| Boundaries | The symbols, code no symbol covers cut at every target | Section 2 |
| Jump tables | `findJumpTables`: a base built in place with a bound is a switch, any other goes through the dispatcher first | Section 3, all direct |
| Data words pointing at code | Every aligned one is an entry | Filtered, section 4 |

An ELF on `mode symbols` generates exactly what it generated before this path existed.

## 2. Boundaries from the code

Starts that are facts (*hard*): the entry point, `--entries`, every symbol or map name, and a `jal`/`bal` target that
holds an instruction and comes after the end of something (`jr`, `j`, `b`, `syscall`, `break`, `eret`, then padding).
A call target that opens a frame (`addiu sp, sp, -n`) is hard wherever it is, and so is a code pointer in data or built
by `lui`/`addiu` that opens one. Anything else a `jal` targets (data read as code, a call into a body) is an entry.

Between them a linear sweep cuts:

- after a `jr`, `j` or `b` and its delay slot, past the zero words of padding, when no branch, jump or table target seen
  since the function's start lies ahead. A transfer past the next hard start is a tail call and does not count, nor does
  a jump to the very next instruction (a tail call to the function placed next);
- at a frame opening right after the end of something, whatever lies ahead, and after a call or `syscall` or `break`
  if nothing does (a call that never returns leaves no other mark);
- a word that is no instruction clears what lies ahead: data among the code says nothing about branches.

Then pieces are joined: a direct branch, jump or bounded table target landing inside another piece's body, not at its
start, makes them one function, unless a hard start or a frame opening lies between them.

## 3. Jump tables

A `jr` through a register that is:

- loaded from `base + index`, the base from `lui` (with the load's offset), `lui` + `addiu`, or `lui` + `ori`; the number
  of entries from the nearest `sltiu` or `andi` mask in the 32 instructions before the load, else the words up to the
  first one that is not a target of the function;
- or `base + (index << k)`, `k` from 2 to 8, base from `lui` + `addiu`: no table, slots of code at a fixed stride (a
  hand-written computed goto). Every slot to the function's end is a target, a label that is never jumped to costs nothing.

Both become a `switch` of gotos. A value that is not in it leaves through `bozo_call_indirect`, so a wrong table costs
labels, not behaviour.

Any other `jr` through a register is listed as `unresolved` in the report and in `--verbose`, and goes through the
dispatcher: a tail call through a register, `longjmp`, a C++ exception landing. Its target must be an entry: a function,
a data or code pointer, or `--entries`. If not, the runtime's unknown target fault names the address.

## 4. Entries

As on the other path, a transfer from another function, a call into a body and a code pointer become entries of the
function holding them. A data word counts as a pointer only if it is aligned, in the code, on a word that decodes and is
not zero, and not in the delay slot of the word before it. A table's own words are labels, not entries.
A pointer built in the code counts under the same test: every `addiu` or `ori` off a `lui`'s register, up to 63 instructions
on while the register holds it, through conditional branches, up to the delay slot of a jump, and past a call for a saved
register only. `lui; jal f; addiu a0, a0, lo` passes a function pointer in a delay slot.

## 5. The report and measuring it

`--report file` writes one fact per line:

    mode code
    function 0xSTART 0xEND name
    entry 0xADDRESS 0xFUNCTION
    table 0xJR 0xBASE goto|dispatch 0xTARGET ...
    return 0xJR 0xFUNCTION
    unresolved 0xJR 0xFUNCTION

`--no-elf-symbols` forgets the ELF's symbols. On an ELF that has them, this measures the code path against the truth:

    bozo-recomp game.elf out --no-elf-symbols --report hidden.txt
    tools/discovery/bozo_discovery_score.py game.elf hidden.txt

Champions of Norrath (SLUS_205.65), symbols hidden. `.srs` mixes data objects with its code, which the code path can only
read as code once their symbols are hidden; a start the score calls false there is often code with no sized symbol.

| Section | Symbols | Found | Precision | Recall |
|---|---|---|---|---|
| `.text` | 5390 | 5395 | 0.9985 | 0.9994 |
| `.srs` | 8980 | 10377 | 0.8491 | 0.9812 |

Tables: 347 of the 348 the symbol path finds, all 337 inside a symbol with every target in it, 10 of them with more
targets than on the symbol path. The one missed is listed as `unresolved`.
