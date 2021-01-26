# Bunny JIT

This is a tiny optimising SSA-based JIT backend, currently targeting x64, but
designed to be (somewhat) portable.

This is work in relatively early progress. It sort of works, but is still missing
some essentials like function calls.

It provides a relatively simple interface for generating bytecode in SSA form,
then performs basic optimisations (eg. DCE, constant folding, register allocation)
and assembles the result into native code in memory. Some additional optimisations
are highly likely in the future, but the goal is to keep things simple.

It is intended for situations where it is desirable to create some native code
on the fly (eg. for performance reasons), but including something like LLVM would
be overkill. However, we want a simple JIT, not a naive one.

BJIT currently supports integers and double-precision floats only. It will probably
support single-precision and SIMD-operations at some point in the future.

It comes with some sort of simple front-end language, but this is intended more
for testing than as a serious programming language; the focus is on the backend.

## SSA?

The backend keeps the code in SSA form from the beginning to the end. The interface
is designed to make emitting SSA directly relatively simple for block-structured
languages by tracking the "environment" that must be passed form one block to
another. When a new label is created, we create phis for all the values in the
environment. When a jump to a label is emitted, we take the current environment
and add the values to the target block phi-alternatives. When a label is emitted
we replace the values in the current environment with the phi-values.

Essentially for a block structured language, whenever a new variable is defined
one pushes the SSA value into the environment. When looking up variables, one
takes the values form the environment. On control-flow constructs, one needs to
match the environment size of the label and the jump-site, but the interface
will take care of the rest.

While there is no need to add temporaries to the environment, always adding phis
for any actual local variables still creates more phis than necessary. We choose
to let DCE clean this up, by simplifying those phis with only one real source.

We keep the SSA structure all the way. The register allocator is currently a bit
lazy with regards to rewriting phi-sources for shuffle-blocks properly, but
theoretically the code is valid SSA even after register allocation. We handle
phis by simply making sure that the phi-functions are no-ops: all jump-sites
place the correct values in either the same registers or stack-slots, depending
on what the phi expects. Two-way jumps always generate shuffle blocks, which are
then jump-threaded if the edge is not actually critical.

The register allocator itself runs locally, using "furthest next use" to choose
which values to throw out of the register file. We don't ever explicitly spill,
rather we flag the source operation with a spill-flag when we emit a reload.
The assembler will then generate stores after any operations marked for spill.

To choose stack locations, we compute "stack congruence classes" (SCCs) to find
which values can and/or should be placed into the same slot. We then allocate
slots for those SCCs that are used by at least one operation flagged for spill.
