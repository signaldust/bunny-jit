# Bunny JIT

This is a tiny optimising SSA-based JIT backend, currently targeting x64, but
designed to be (somewhat) portable. The [build-system](#how-to-build) expects
Unix environment (for now), but the code should work on Windows as well (simply
compile everything in `src/`).

This is work in relatively early progress. It sort of works, but some things like
function calls not done robustly yet and there are likely serious bugs hiding.
Loads and stores in particular are not properly tested yet.

**Please don't use it for production yet.**

Features:
  * small and simple, yet tries to avoid being naive
  * portable C++11 without dependencies (other than STL)
  * uses low-level portable bytecode that models common architectures
  * supports integers and double-floats (other types in the future)
  * [end-to-end SSA](#ssa), with consistency checking and [simple interface](#env) to generate valid SSA
  * performs roughly<sup>1</sup> DCE, CSE, LICM, const-prop and register allocation (as of now)
  * assembles to native binary code (ready to be copied to executable memory)
  * uses `std::vector` to manage memory, keeps `valgrind` happy

<sup>1</sup><i>
I find it slightly challenging to relate exactly to traditional compiler
optimisations that insist talking about variables and such things. We don't have
variables to optimize, we simplify a value graph. We don't really have loops either,
just control-flow edges. But this is more or less what we end up with currently.
See [below](#optimizations).</i>

Bunny-JIT is intended for situations where it is desirable to create some native code
on the fly (eg. for performance reasons), but including something like LLVM would
be a total overkill. It is intended for situations where combinatorial expansion
makes template expansion of all possible alternatives at compile time infeasible
(or impossible, if the possible domain is infinite), yet one would like to avoid
interpretive overhead. I suppose you could use it for dynamic languages too, but
it is not a trace-compiler and might be a bit slow for the purpose.

It comes with some sort of simple front-end language, but this is intended more
for testing (and I guess example) than as a serious programming language.

The test-driver currently parses this simple language from `stdin` and compiles
it into native code, which is written to `out.bin` for disassembly purposes with
something like:
```
gobjdump --insn-width=16 -mi386:x86-64:intel -d -D -b binary out.bin
```

You can certainly run it too, but you'll have to copy it to executable memory.

## Why Bunny?

Bunnies are cute.

## How to build?

On Unix-like system with `clang` installed, simply run `make` (or `make -j`).

Any source files in `src/` are linked to `build/bjit.a` and for each directory
in `test/` we compile `bin/<name>` with `test/<name>/*.cpp`.

If you want to use another compiler, then changing `CC :=` near the top of the
`Makefile` should be enough (yes, I'll add an override for it eventtually).
If `BUILD_DIR` and/or `BIN_DIR` are defined, these will be used instead of `build/` and `bin/`.

Should you somehow run into issues with automatic dependencies, type `make clean`
to start fresh. Standard stuff.

The `Makefile` currently won't work on Windows (at least not without something
like Cygwin; I haven't tried), I will try to fix this eventually.
In the mean time, it should be possible to compile everything under `src/` into
a static library and link with whatever other source files you have.

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

## Contributing?

I would recommend opening an issue before working on anything too significant,
because this will greatly increase the chance that your work is still useful
by the time you're ready to send a pull-request.

The reason for this is that the project is still young enough that I might make
some drastic changes on a regular basis.

## Instructions?

The first step is to include `src/bjit.h`.

The second step is to create a `bjit::Proc` which takes a stack allocation size
and a string representing arguments (`i` for integer, `f` for double).
This will initialize `env[0]` with an SSA value for the pointer to a block
of the requested size on the stack (in practice, it represents stack
pointer) and `env[1..]` as the SSA values of the arguments (at most 4 for now).
More on `env` below. Pass `0` and `""` if you don't care about allocations or arguments.
At this time we only support single procedures, some day we might have modules.

To generate instructions, you call the instruction methods on `Proc`.
When done, `Proc::opt()` will optimize and `Proc::compile()` generate code.
Compile always does a few passes of DCE, but otherwise optimization is optional.

Most instructions take their parameters as SSA values. The exceptions are
`lci`/`lcf` which take immediate constants and jump-labels which should be
the block-indexes returned by `Proc::newLabel()`. For instructions
with output values, the methods return the new SSA values and other
instructions return `void`.

### Env?

`Proc` has a public `std::vector` member `env` which stores the "environment".
When a new label is create with `Proc::newLabel()` the number and types of
incoming arguments to the block are fixed to those contained in `env` and when
jumps are emitted, we check that the contents of `env` are compatible (same
number of values of same types). When `Proc::emitLabel()` is called to generate
code for the label, we replace the contents of `env` with fresh `phi`-values.
So even though we only handle SSA values, elements of `env` behave essentially
like regular variables (eg. "assignments" can simply store a new SSA value
into `env`). Note that you can adjust the size of `env` as you please as long
as constraints match for jump-sites, but keep in mind that `emitLabel()` will
resize `env` back to what it was at the time of `newLabel()`.

Instructions expect their parameter types to be correct. Passing floating-point
values to instructions that expect integer values or vice versa will result
in undefined behaviour (ie. invalid code or `assert`). The compiler should never
fail with valid data unless bytecode size limit is exceeded (we
<code>throw&nbsp;bjit::too_many_ops</code> if compiled with exceptions; otherwise we `assert`),
so we do not provide error reporting other than `assert` (lots of them). This
is a conscious design decision, as error checking should be done at higher levels.

The type system is very primitive though and mostly exists for the purpose of
tracking which registers we can use to store values. In particular, anything
stored in general purpose registers is called `_ptr` (or simply integers).

Instructions starting `i` are for integers, `u` are unsigned variants when
there is a distinction and `f` is floating point (though we might change the
double-precision variants to `d` if we add single-precision versions). Note
that floating-point comparisons return integers, even though they expect
`_f64` parameters.

### The compiler currently exposes the following instructions:

`lci i64` and `lcf f64` specify constants, `jmp label` is unconditional jump
and `jz a then else` will branch to `then` if `a` is zero or `else` otherwise,
`iret a` returns from the function with integer value and `fret a` returns with
a floating point value.

`ieq a b` and `ine a b` compare two integers for equality or inequality and
produce `0` or `1`.

`ilt a b`, `ile a b`, `ige a b` and `igt a b` compare signed integers
for less, less-or-equal, greater-or-equal and greater respectively

`ult a b`, `ule a b`, `uge a b` and `ugt a b` perform unsigned comparisons

`feq a b`, `fne a b`, `flt a b`, `fle a b`, `fge a b` and `fgt a b` are
floating point version of the same (still produce integer `0` or `1`).

`iadd a b`, `isub a b` and `imul a b` perform (signed or unsigned) integer
addition, subtraction and multiplication, while `ineg a` negates an integer

`idiv a b` and `imod a b` perform signed division and modulo

`udiv a b` and `umod a b` perform unsigned division and modulo

`inot a`, `iand a b`, `ior a b` and `ixor a b` perform bitwise logical operations

`ishr a b` and `ushr a b` are signed and unsigned right-shift while 
left-shift (signed or unsigned) is `ishl a b` and we specify that the number
of bits to shift is modulo the bitsize of integers (eg. 64 on x64 which does
this natively, but it's easy enough to mask on hardware that might not)

`fadd a b`, `fsub a b`, `fmul a b`, `fdiv a b` and `fneg a` are floating point
versions of arithmetic operations

`cf2i a` converts doubles to integers while `ci2f` converts integers to doubles

`bcf2i a` and `bci2f a` bit-cast (ie. reinterpret) without conversion

`i8 a`, `i16 a` and `i32 a` can be used to sign-extend the low 8/16/32 bits

`u8 a`, `u16 a` and `u32 a` can be used to zero-extend the low 8/16/32 bits

Loads follow the form `lXX ptr imm32` where `ptr` is integer SSA value and `imm32`
is an immediate offset (eg. for field offsets). The variants defined are 
`li8/16/32/64`, `lu8/16/32` and `lf64`. The integer `i` variants sign-extend
while the `u` variants zero-extend.

Stores follow the form `sXX ptr imm32 value` where `ptr` and `imm32` are like loads
while `value` is the SSA value to store. Variants are like loads, but without
the unsigned versions.

Internal we have additional instructions that the fold-engine will use (in the
future the exact set might vary between platforms, so we rely on fold), but they
should be fairly obvious when seen in debug, eg. `jugeI`is a conditional jump on
`uge` comparison with the second operand converted to an `imm32` field.

## What it does?

Here's a somewhat silly example:

```
$ echo 'x := 0; y := 0/0; while(x < 10) { if(y != 2) x = x+1; else x = x+1; } return (x+1);' | bin/bjit
```

Why am I dividing by zero? Because this results in an exception that
prevents the compiler from constant folding. Here's the front-end AST dump
and generated bytecode (at the time of writing):

```
(block 
  (def:0x7ff5a8404b60:x/1 @1:2 : i64
    i:0 @1:5 : i64)
  (def:0x7ff5a8404d00:y/2 @1:10 : i64
    (div @1:14 : i64
      i:0 @1:13 : i64
      i:0 @1:15 : i64))
  (while @1:18 : void
      (c:lt @1:26 : i64
        sym:0x7ff5a8404b60:x/1 @1:24 : i64
        i:10 @1:28 : i64)
    (block 
      (if @1:34 : void
          (c:neq @1:39 : i64
            sym:0x7ff5a8404d00:y/2 @1:37 : i64
            i:2 @1:42 : i64)
        (set @1:47 : i64
          sym:0x7ff5a8404b60:x/1 @1:45 : i64
          (add @1:50 : i64
            sym:0x7ff5a8404b60:x/1 @1:49 : i64
            i:1 @1:51 : i64))
        (set @1:61 : i64
          sym:0x7ff5a8404b60:x/1 @1:59 : i64
          (add @1:64 : i64
            sym:0x7ff5a8404b60:x/1 @1:63 : i64
            i:1 @1:65 : i64)))))
  (return @1:70 : i64
    (add @1:79 : i64
      sym:0x7ff5a8404b60:x/1 @1:78 : i64
      i:1 @1:80 : i64))

;----
L0:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  0000    ---    alloc   0  ptr  +0
 (0000)  0001    ---      lci   0  ptr  i64:0
 (0000)  0002    ---      lci   0  ptr  i64:0
 (0000)  0003    ---      lci   0  ptr  i64:0
 (0000)  0004    ---     idiv   0  ptr  ---:0002 ---:0003
         0008             jmp           L1
L1:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  0005    ---      phi   0  ptr  L0:[0000]:0000 L6:[0000]:001a
 (0000)  0006    ---      phi   0  ptr  L0:[0000]:0001 L6:[0000]:001b
 (0000)  0007    ---      phi   0  ptr  L0:[0000]:0004 L6:[0000]:001c
 (0000)  0009    ---      lci   0  ptr  i64:10
 (0000)  000a    ---      ilt   0  ptr  ---:0006 ---:0009
         0011              jz           ---:000a L3 L2
L2:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  000b    ---      phi   0  ptr  L1:[0000]:0005
 (0000)  000c    ---      phi   0  ptr  L1:[0000]:0006
 (0000)  000d    ---      phi   0  ptr  L1:[0000]:0007
 (0000)  0012    ---      lci   0  ptr  i64:2
 (0000)  0013    ---      ine   0  ptr  ---:000d ---:0012
         001d              jz           ---:0013 L5 L4
L3:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  000e    ---      phi   0  ptr  L1:[0000]:0005
 (0000)  000f    ---      phi   0  ptr  L1:[0000]:0006
 (0000)  0010    ---      phi   0  ptr  L1:[0000]:0007
 (0000)  0025    ---      lci   0  ptr  i64:1
 (0000)  0026    ---     iadd   0  ptr  ---:000f ---:0025
         0027            iret           ---:0026
 (0000)  0028    ---      lci   0  ptr  i64:0
         0029            iret           ---:0028
L4:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  0014    ---      phi   0  ptr  L2:[0000]:000b
 (0000)  0015    ---      phi   0  ptr  L2:[0000]:000c
 (0000)  0016    ---      phi   0  ptr  L2:[0000]:000d
 (0000)  001e    ---      lci   0  ptr  i64:1
 (0000)  001f    ---     iadd   0  ptr  ---:0015 ---:001e
         0020             jmp           L6
L5:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  0017    ---      phi   0  ptr  L2:[0000]:000b
 (0000)  0018    ---      phi   0  ptr  L2:[0000]:000c
 (0000)  0019    ---      phi   0  ptr  L2:[0000]:000d
 (0000)  0021    ---      lci   0  ptr  i64:1
 (0000)  0022    ---     iadd   0  ptr  ---:0018 ---:0021
         0023             jmp           L6
L6:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (0000)  001a    ---      phi   0  ptr  L4:[0000]:0014 L5:[0000]:0017
 (0000)  001b    ---      phi   0  ptr  L4:[0000]:001f L5:[0000]:0022
 (0000)  001c    ---      phi   0  ptr  L4:[0000]:0016 L5:[0000]:0019
         0024             jmp           L1
;----
```

That said, the code looks incredibly silly, doesn't it? But we have an optimizer:
```
-- Optimizing:
 DCE:4+2 Fold:3
 DCE:3+2 Fold:1
 DCE:1+2 Live:1
 RA:SCC DCE:1+2 Live:1
 RA:BB DCE:1+2 RA:JMP SANE DONE

;---- Slots: 1
L0:
; Dom: ^L0
; Regs:
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (ffff)  0000    rsp    alloc   0  ptr  +0
 (ffff)  0001    rax      lci   3  ptr  i64:0
 (ffff)  002d    rsi   rename   1  ptr  rax:0001
 (ffff)  0004    rax     idiv   0  ptr  rax:0001 rax:0001
         0008             jmp           L1
; Out: rsi:002d

L1: <L8 <L0
; Dom: ^L0 ^L1
; Regs: rsi:0006
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (ffff)  0006    rsi      phi   2  ptr  L0:[ffff]:002d L8:[ffff]:0030
 (ffff)  002b    rax    iaddI   1  ptr  rsi:0006 +1
 (ffff)  002c    rax       -    2  ptr  rax:002b
         0011           jigeI           rsi:0006 +10 L3 L8
; Out: rax:002c

L3: <L1
; Dom: ^L0 ^L1 ^L3
; Regs: rax:002c
; SLOT  VALUE    REG       OP USE TYPE  ARGS
         0027            iret           rax:002c
; Out:

L8: <L1
; Dom: ^L0 ^L1 ^L8
; Regs: rax:002c
; SLOT  VALUE    REG       OP USE TYPE  ARGS
 (ffff)  0030    rsi   rename   0  ptr  rax:002c
         002f             jmp           L1
; Out: rsi:0030

;----
 - Wrote out.bin
```

This looks better, doesn't it? We still have the silly division by zero,
but we've managed to combine all the `(x+1)` terms into a single instruction
and then get rid of the unnecessary branches (they were the same after all).
Note how the compiler realized that it can compute the final `(x+1)` inside
the loop, as long as it shuffles correctly: this is the power of SSA.

In this case we didn't need any slots (the `1` here was added by the assembler
to align the stack for the calling convention), otherwise spills would show
up in the `SLOT` field like `=[0123]=`. DCE fills in the global `USE` counts.
We also know all the incoming control
flow edges, the block dominators and the incoming and outgoing registers.

So what does `out.bin` look like?
```
$ gobjdump --insn-width=16 -mi386:x86-64:intel -d -D -b binary out.bin

out.bin:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	48 83 ec 08                                     	sub    rsp,0x8
   4:	33 c0                                           	xor    eax,eax
   6:	48 8b f0                                        	mov    rsi,rax
   9:	48 99                                           	cqo    
   b:	48 f7 f8                                        	idiv   rax
   e:	48 8d 46 01                                     	lea    rax,[rsi+0x1]
  12:	48 83 fe 0a                                     	cmp    rsi,0xa
  16:	0f 8d 08 00 00 00                               	jge    0x24
  1c:	48 8b f0                                        	mov    rsi,rax
  1f:	e9 ea ff ff ff                                  	jmp    0xe
  24:	48 83 c4 08                                     	add    rsp,0x8
  28:	c3                                              	ret    
  29:	90                                              	nop
  2a:	90                                              	nop
  2b:	90                                              	nop
  2c:	90                                              	nop
  2d:	90                                              	nop
  2e:	90                                              	nop
  2f:	90                                              	nop

```

This doesn't look too bad, does it? We still have the silly division, but after
that the loop is roughly what you would expect without unrolling, isn't it?
Our instruction selection is naive, but we're providing it with bytecode that
still allows the naive approach to do a half-decent job.

Loop unrolling is something I am divided about, because then we enter the
territory of complicated heuristics of profitability. Perhaps some day Bunny-JIT
will detect loops that can be solved analytically and fold those into constants?

Either way, hopefully this gives you an idea of what to expect.

## SSA?

The backend keeps the code in SSA form from the beginning to the end. We rely
on `env` to automatically add `phi`s for all cross-block variables initially.
While there is no need to add temporaries to the environment, always adding `phi`s
for any actual local variables still creates a lot more `phi`s than necessary. We choose
to let DCE clean this up, by simplifying those `phi`s with only one real source.

We keep the SSA structure all the way. The code is valid SSA even after register
allocation. Registers are not SSA values, but each value has one register. When we
need to rename registers or reload values from stack, we define new values. We handle
`phi`s by simply making sure that the `phi`-functions are no-ops: all jump-sites
place the correct values in either the same registers or stack-slots, depending
on what the `phi` expects. Two-way jumps always generate shuffle blocks, which are
then jump-threaded if the edge is not actually critical or the shuffle is empty.

The register allocator itself runs locally, using "furthest next use" to choose
which values to throw out of the register file, but passes information from one
block to the next to reduce pointless shuffling. We don't ever explicitly spill,
rather we flag the source operation with a spill-flag when we emit a reload.
This is always valid in SSA, because we have no variables, only values.

The assembler will then generate stores after any operations marked for spill,
because we resolve SCCs to actual slots only after register allocation is done.

## SCC?

To choose stack locations, we compute what I like to call "stack congruence
classes" (SCCs) to find which values can and/or should be placed into the same
slot. Essentially if two values are live at the same time, then they must have
different SCCs. On the other hand, if a value is argument to a `phi`, then we
would like to place it into the same class to avoid having to move spilled
values from one stack slot to another. For other values, we would like to try
and find a (nearly) minimal set of SCCs that can be used to hold them, in order
to keep the stack frames as small as possible.

As pointed out earlier, sometimes we have cycles. Eg. if two values are swapped
in a loop every iteration, then we can't allocate the same SCC to these without
potentially forcing a swap in memory. We solve SCCs (but not slots) before
register allocation, so at this point we don't know which variables live in
memory. To make sure the register allocator doesn't need worry about cycles (in
memory; it *can* deal with cycles in actual registers) the SCC computation adds
additional renames to temporary SCCs. This way the register allocator can mark
the rename (rather than the original value) for spill if necessary, otherwise
the renames typically compile to nops. The register allocator itself never adds
any additional SCCs: at that point we already have enough to allow every shuffle
to be trivial.

Only after register allocation is done, do we actually allocate stack slots. At
this point, we find all the SCCs with at least one value actually spilled and
allocate slots for these (and only these), but since we know (by definition)
that no two variables in the same SCC are live at the same time, at this point
we don't need to do anything else. We just rewrite all the SCCs to the slots
allocated (or "don't need" for classes without spills) and pass the total number
of slots to the assembler.

The beauty of this design is that it completely decouples the concerns of stack
layout and register allocation: the latter can pretend that every value has a
stack location, that any value can be reloaded at any time (as long as it's then
marked for "spill" at the time the reload is done) yet we can trivially collapse
the layout afterwards.

## Optimizations

At the beginning of this page I said above I find traditional compiler optimizations
a bit confusing to relate to, because they talk about things like versions of
variables and safety. In SSA, there is exactly one version of a value and values
are referentially transparent: as long as the value is defined in some dominator
block the value is safe.

We treat redundant `phi`s as "dead code" so the DCE pass serves a dual purpose:
it also performs "dataflow analysis" when it figures out which `phi`s it should
get rid of. A `phi` is considered "dead" if there is only one non-`phi` value
reachable from the (often cyclic) graph of source values. We resolve all the
cycles and drop the `phi` unless we find at least two real (non-`phi`) values.
We do this whenever DCE runs. We also find dominators for CSE-purposes.

The Fold pass can fold constants. It doesn't explicitly "propagate" anything,
but because the only way to know anything (is a constant?) about source operands
is to look at the original value, we get "propagation" for free. Fold can also
resolve constant jumps, which then potentially allows DCE to remove more `phi`s
which then might allow Fold to treat more values as constants.

I don't think we quite manage full "sparse conditional constant propagation"
because we don't deal with any fancy lattice stuff and we don't try to track
where we've been (other than by static liveliness). If there is a loop where a
path must be taken in order for the same path to be taken, then we can't figure
this out. I suppose this could be fixed, but it doesn't seem like a priority.
On the other hand, if the path doesn't actually change the condition (ie. any
phis are pass-thru) then we can certainly get rid of it.

Fold can also deal with identical instructions: if the operation code and the
operand values match and the instruction doesn't have side effects, then it will
compute the same value. If two instructions have the same operands, then they
must (necessarily) have at least one common dominator where both of the values are
defined. So we can use a hashtable to match and then find this common dominator,
move one of the operations there and rename the other. We pick the closest
common dominator. CSE seems simple?

Why not also move any single instruction the same way? In theory, in code without
side-effects (or loads, which we treat as side-effects) we could turn all
conditional branches into pure shuffles this way and part of me wants to explore
the possibility, with conditional moves to eliminate the branches completely.

However, in order to not increase computation on paths that never compute the
value, we work up the dominator tree only until we see a branch. We don't need to
worry about blocks other than dominators, because if they branch, they must
also merge. As it turns out, if a "natural loop" has a header (ie. the edge that
enters the loop is not critical), then this gives us loop invariant code motion
without even having to find the loops (well, at least with loop inversion, but
we'll force this in the future; it doesn't really require finding loops either).
We don't add missing headers yet, but we probably soon will (it's just a matter
of breaking critical edges).

We make one exception to the branch rule: when merging two operations, if
the closest common dominator (but not other dominators of either instruction
along the path) contains a branch, we don't care: we just found an instruction
that is computed separately on both sides of the branch, like in my silly
division-by-zero bytecode dump above. In that example case, it then makes
the `phi`s redundant, which means the two conditional block are empty, which
means the jumps can be threaded and the thing collapses into it's final form.

This is really all we currently do, but because we only worry about graph
theory rather than variables, we get a fairly powerful set of optimisations
essentially for free (well, some CPU is spent, but this isn't a stage0 JIT).

On the other hand Bunny-JIT does not perform any sort of aliasing analysis
for memory. It probably never will, because this is such a huge can of worms.
Bunny-JIT will not be an efficient compiler for high-level object-oriented
languages. I don't care, it's really not the focus.
