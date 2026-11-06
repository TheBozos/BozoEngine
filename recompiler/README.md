# bozo-recomp

BozoEngine's static recompiler: a PlayStation 2 executable's EE (R5900) code to C++, one C++ function per guest
function. What the instructions do: [`docs/specs/r5900.md`](../docs/specs/r5900.md). What the generated code looks
like to the runtime and to a port: [`docs/specs/abi.md`](../docs/specs/abi.md).

## Build

Windows, MSYS2 UCRT64 (GCC and Ninja from `C:\msys64\ucrt64\bin`), from the repository root:

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build          # the instruction tests
```

Any C++20 compiler with `<format>` works (GCC 13+, Clang 17+). The executables are linked statically on MinGW.

## Run

```sh
bozo-recomp <elf> <out dir> [--prefix bozo] [--max-file-bytes 1048576 | --files n] [--externals file]
            [--no-comments] [--verbose]
bozo-recomp <elf> --census
```

- `<out dir>` receives `<prefix>_functions.h` (every function declared, and the table), `<prefix>_table.cpp` (the
  function table: every function and extra entry by address, sorted) and `<prefix>_000.cpp`, `_001.cpp`, ... (the
  functions, split at about `--max-file-bytes` each so they compile in parallel; with `--files n`, exactly n files of
  about the same size, so a build system can list them in advance). A file is rewritten only when its contents change
  (unchanged files keep their date and are not recompiled); other `<prefix>_*` files in the directory are removed.
- `--externals file`: guest addresses the runtime answers instead of the guest code there, one per line,
  `stub 0xADDR` (a native library or kernel function: every transfer to it goes through `bozo_call_indirect`) or
  `override 0xADDR` (a port function: calls go through `bozo_call_indirect`, a direct jump runs the guest code).
  See [`docs/specs/replay.md`](../docs/specs/replay.md) section 2.
    `native 0xADDR` selects an instruction to be replaced by the port's inline native handler, including in delay
    slots. A handled BC0 test means native submission is complete. An unhandled instruction executes its original
    operation, preserving the historical checker path. Replacements cannot block or change PC/RA.
- `--census` only decodes the code and prints how many times each instruction appears, and any word it cannot
  decode (exit code 1 if there is one).
- `--verbose` lists the extra entry points and the `jr` instructions the analysis could not resolve.
- `--no-comments` leaves out the per-instruction comment (address and disassembly): smaller files, faster builds.

The generated code includes `bozo/recomp.h` (`runtime/include`) and calls the functions of
`bozo/runtime_api.h`, which the runtime provides. To check that a whole program's output compiles and links (with
the tests' stub runtime):

```sh
bozo-recomp game.elf gen
cmake -S tests/compile_check -B build-cc -G Ninja -DBOZO_GENERATED_DIR=$PWD/gen
cmake --build build-cc && build-cc/compile_check
```

## How it works

| File | What |
|---|---|
| `src/elf.*` | ELF32 loading: program headers into one memory image, sections, symbols |
| `src/program.*` | What the rest works on: the image, the code ranges (executable sections except VU microcode), data ranges, symbols |
| `src/ops.def`, `src/decoder.*` | Every R5900 instruction we know (MIPS III/IV subset, MMI, COP0, the FPU, VU0 macro mode) and the decoder |
| `src/analysis.*` | Function discovery: ELF function symbols; code no symbol covers carved at call/branch targets and code pointers; jump tables; extra entries for jumps into another function's body |
| `src/emitter.*` | The C++: labels and gotos inside a function, calls between functions, delay slots, likely branches, scheduler checkpoints; the header and the function table |
| `src/main.cpp` | The command line |

The game's executable is input data: never commit it, or generated code made from it, to this repository.
