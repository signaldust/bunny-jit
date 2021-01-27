# Bunny JIT

This is a tiny optimising SSA-based JIT backend, currently targeting x64, but
designed to be (somewhat) portable. The Makefile expects a Unix-like system,
but the code itself should (hopefully) work on Windows as well.

This is work in relatively early progress. It sort of works, but is still missing
some essentials like function calls. Please don't use it for production yet.

It provides a relatively simple interface for generating bytecode in SSA form,
then performs basic optimisations (DCE, CSE, constant folding, register allocation)
and assembles the result into native code in memory. Some additional optimisations
are highly likely in the future, but the goal is to keep things simple.

It is intended for situations where it is desirable to create some native code
on the fly (eg. for performance reasons), but including something like LLVM would
be overkill. However, we want a simple JIT, not a naive one.

BJIT currently supports integers and double-precision floats only. It will probably
support single-precision and SIMD-operations at some point in the future.

It comes with some sort of simple front-end language, but this is intended more
for testing than as a serious programming language; the focus is on the backend.

The test-driver currently parses this simple language from `stdin` and compiles
it into native code, which is written to `out.bin` for disassembly purposes with
something like:
```
gobjdump --insn-width=16 -mi386:x86-64:intel -d -D -b binary out.bin
```

## License?

I should paste this into every file, but for the time being:

```
/****************************************************************************\
* Bunny-JIT is (c) Copyright pihlaja@signaldust.com 2021                     *
*----------------------------------------------------------------------------*
* You can use and/or redistribute this for whatever purpose, free of charge, *
* provided that the above copyright notice and this permission notice appear *
* in all copies of the software or it's associated documentation.            *
*                                                                            *
* THIS SOFTWARE IS PROVIDED "AS-IS" WITHOUT ANY WARRANTY. USE AT YOUR OWN    *
* RISK. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE HELD LIABLE FOR ANYTHING.  *
\****************************************************************************/
```

## Instructions?

To generate instructions, you call the instruction methods on `bjit::Proc`.

Most instructions take their parameters as SSA values. The exceptions are
`lci`/`lcf` which take immediate constants and jump-labels which should be
the block-indexes returned by `Proc::newLabel()`. For instructions
with output values, the methods return the new SSA values and other
instructions return `void`.

`Proc` has a public `std::vector` member `env` which stores the "environment".
When a new label is create with `Proc::newLabel()` the number and types of
incoming arguments to the block are fixed to those contained in `env` and when
jumps are emitted, we check that the contents of `env` are compatible (same
number of values of same types). When `Proc::emitLabel()` is called to generate
code for the label, we replace the contents of `env` with fresh phi-values.
So even though we only handle SSA values, elements of `env` behave essentially
like regular variables (eg. "assignments" can simply store a new SSA value
into `env`).

Instructions expect their parameter types to be correct. Passing floating-point
values to instructions that expect integer values or vice versa will result
in undefined behaviour. The compiler should never fail with valid data, so we
do not provide error reporting other than `assert`. This is a conscious design
decision, as error checking should be done at higher levels.

The type system is very primitive though and mostly exists for the purpose of
tracking which registers we can use to store values. In particular, anything
stored in general purpose registers is called `_ptr` (or simply integers).

Instructions starting `i` are for integers, `u` are unsigned variants when
there is a distinction and `f` is floating point (though we might change the
double-precision variants to `d` if we add single-precision versions). Note
that floating-point comparisons return integers, even though they expect
`_f64` parameters.

Also note that the handling of `iparam` and `fparam` is not final and might be
moved to internals, with a cleaner user-facing interface.

### The compiler currently exposes the following instructions:

`lci i64` and `lcf f64` specify constants, `jmp label` is unconditional jump
and `jz a then else` will branch to `then` if `a` is zero or `else` otherwise,
`iret a` returns from the function with integer value and `fret a` returns with
a floating point value.

`ieq a b` and `ine a b` compare two integers for equality or inequality and
produce boolean values (ie. `0` or `1`).

`ilt a b`, `ile a b`, `ige a b` and `igt a b` and two signed integers
for less, less-or-equal, greater-or-equal and greater respectively

`ult a b`, `ule a b`, `uge a b` and `ugt a b` perform unsigned comparisons

`feq a b`, `fne a b`, `flt a b`, `fle a b`, `fge a b` and `fgt a b` are
floating point version of the same, but all return integers (ie `0` or `1`).

`iadd a b`, `isub a b` and `imul a b` perform (signed or unsigned) integer
addition, subtraction and multiplication, while `ineg a` negates an integer

`idiv a b` and `imod a b` perform signed division and modulo

`udiv a b` and `umod a b` perform unsigned division and modulo

`inot a`, `iand a b`, `ior a b` and `ixor a b` perform bitwise logical operations

`ishr a b` and `ushr a b` are signed and unsigned right-shift while 
left-shift (signed or unsigned) is `ishl a b`

`fadd a b`, `fsub a b`, `fmul a b`, `fdiv a b` and `fneg a` are floating point
versions of arithmetic operations

`cf2i a` converts doubles to integers while `ci2f` converts integers to doubles

Loads follow the form `lXX ptr imm32` where ptr is integer SSA value and imm32
is an immediate offset (eg. for field offsets). The variants defined are 
`li8`, `lu8`, `li16`, `lu16`, `li32`, `lu32`, `li64` and `lf64`. The integer
variants always produce 64-bit values with `i` variants sign-extending
and `u` variants zero-extending.

Stores follow the form `sXX ptr imm32 value` where ptr and imm32 are like loads
while `value` is the SSA value to store. Variants are like loads, but without
the unsigned versions.

Internal we have additional instructions that the fold-engine will use (in the
future the exact set might vary between platforms, so we rely on fold), but they
should be fairly obvious when seen in debug, eg. `jugeI`is a conditional jump on
`uge` comparison with the second operand converted to an `imm32` field.

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
This is always valid in SSA, because we have no variables, only values.
The assembler will then generate stores after any operations marked for spill.

To choose stack locations, we compute "stack congruence classes" (SCCs) to find
which values can and/or should be placed into the same slot. We then allocate
slots for those SCCs that are used by at least one operation flagged for spill.
This results in a relatively simple and elegant compiler pipeline that still
produces mostly reasonable native code.
