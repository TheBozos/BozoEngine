# Explicit offline entry points

`bozo-recomp input.elf out --entries entries.txt` adds callable addresses from
an offline analysis map. Each nonblank line contains one aligned address in C
integer notation; `#` starts a comment. Addresses must be backed by executable
image bytes. Duplicate, malformed, overflowing and out-of-code entries fail
before any source emission. The option is per image; use a separate map for
each overlay.

Entries inside a sized function become extra entry wrappers and resume labels
without truncating its extent. Entries in uncovered executable spans seed the
normal function discovery. Ordinary ELF labels remain ordinary labels unless
existing control/data references or this explicit option make them callable.
Without the option the analysis input is unchanged.

This changes offline analysis only. It does not modify ELF load bytes, runtime
memory, instructions, native operation declarations or scheduling policy.
`explicit_entries` tests opt-in behavior and atomic rejection using a synthetic
ELF. Consumers should also execute generated primary/interior calls and resumes.
