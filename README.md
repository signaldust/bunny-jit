# Bunny-JIT

This is a tiny optimising SSA-based JIT backend, currently targeting x64 and arm64.
The [`Makefile`](#how-to-build) expects either Unix environment (and `libtool`) or
Windows with `clang` (and `llvm-lib`) in path, but there is no real build magic
(just compile everything in `src/` really).

The arm64 backend is very new, so it might still have some issue (well, more issues
than the x64 backend), but at least on macOS/M1 it seems to be passing the tests.

*The project status right now is that I'm reasonably happy with the current set of
optimizations (for the time being, at least) and the focus right now is mostly on
simplifying existing functionality and extending test-coverage to expose more issues.
Feel free to experiment, but please expect to still find some serious bugs.*

**Please don't use it for production yet.**

Features:
  * small and simple (by virtue of elegant design), yet tries to avoid being naive
  * portable<sup>1</sup> C++11 without dependencies (other than STL)
  * uses low-level portable instruction set that models common architectures
  * supports integers, single- and double-floats (probably SIMD at some point)
  * [end-to-end SSA](#ssa), with consistency checking and [simple interface](#instructions) to generate valid SSA
  * performs roughly<sup>2</sup> DCE, GCSE/LICM/PRE, CF/CP (SCCP?) and register allocation (as of now)
  * assembles to native x64 binary code with simple module system that supports [hot-patching](#patching-calls)
  * uses `std::vector` to manage memory, keeps `valgrind` happy, tries to be cache efficient

<sup>1</sup><i>Obviously loading code on the fly is not entirely portable (we are
fully [W^X](https://en.wikipedia.org/wiki/W%5EX) compliant), but we support generic
`mmap`/`mprotect` (eg. Linux, macOS, etc) and Windows (now fixed and tested as well).</i>

<sup>2</sup><i>
This is a bit hand-wavy, because traditional optimizations are formulated in terms
of variables, yet we optimize purely on SSA values, but this is roughly what we get.
There are some limitations with PRE/SCCP in the name of simplicity, but we should
get most of the high-value situations; see [below](#optimizations) for details.</i>

I suggest looking at the tests (eg. [`test_fib.cpp`](tests/test_fib.cpp) or
[`test_sieve.cpp`](tests/test_sieve.cpp)) for examples of how to
use the programming API, which is the primary focus of this library.
Bunny-JIT comes with some sort of simple front-end language, but this is intended
more for testing (and I guess example) than as a serious programming language and
there is a pretty good chance that it'll eventually be retired/replaced.
The test-driver `bin/bjit` parses this simple language from `stdin` and compiles
it into native code, which is written to `out.bin` for disassembly purposes
(eg. with `./dump-bin.sh` if you have `gobjdump` in path).

You can certainly run it too, but you'll have to copy it to executable memory
(which must also be readable, but not writable; we place constants directly after
code in order to avoid having to relocate a separate `.rodata`). 

There is now `bjit::Module` that can do this for you, but I haven't got around
to updating the front-end yet.

## Why Bunny?

Bunnies are small and cute and will take over the world in the near future.

Bunny-JIT is intended for situations where it is desirable to create some native code
on the fly (eg. for performance reasons), but including something like LLVM would
be a total overkill. Why add a gigabyte of dependencies, if you can get most of the
high-value stuff with less than 10k lines of sparse C++?

Our goal is not necessarily to produce the best possible code (you should probably 
use LLVM for that), but rather to produce something that is good enough to make dynamic
code-generation worth the trouble, while simple to use (both in terms of API and in
terms of including into a project; I'm even considering an STB-style "single-file"
version once the whole thing can be considered somewhat stable).

It is primarily intended for generating run-time specialized code, especially where
this can give significant performance advantages, so we're willing to spend some
time on optimization, but because we're still aiming at interactive uses we try not
to go crazy heuristics. Instead we aim to find a set of general optimizations that are
reasonably efficient and always lead to a fixed-point.
This rules out optimizations such as loop-unrolling where profitability is not clear.

It can also be used as a backend for custom languages. It might not be great for
dynamic languages that rely heavily on memory optimizations (we don't optimize loads
and stores, except simple CSE on loads), but even then it might serve as a decent
prototype backend.

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
The `Makefile` is not covered by this license and can be considered public domain.

## How to build?

On Unix-like system or Windows with `clang` (and `libtool` on Unix) installed,
simply run `make` (or `make -j`). Ideally that's all.

In case you need local overrides, the `Makefile` includes `local.make` if it exists.

We override `clang` for `CC` but if `BJIT_USE_CC` is defined, then this is used;
use this if you don't want to use `clang` for some weird reason. If `BJIT_BUILDDIR`
and/or `BJIT_BINDIR` are defined, these will be used instead of `build/` and `bin/`.
If it fails to build because of `-Werror` then <ins>please report it</ins> because I'd
really like this to compile clean with all the popular compilers.

By default the `Makefile` is set to compile with `-fno-exceptions` (and `BJIT_ASSERT`
will simply call `assert`) but if you enable exceptions then `BJIT_ASSERT` will
<code>throw&nbsp;bjit::internal_error</code> on failures instead.

There is also `make test` that will build everything and then run `run-tests.sh`
to do some basic sanity checking (very limited for now). This won't quite work on
Windows though (it should build tests, but we don't have a `run-tests.bat` yet).

Any source files in `src/` are compiled and collected to `build/bjit.a` and each
`tests/<name>.cpp` is compiled into a separate `bin/<name>` for testing purposes.
Any source files in `front/` are compiled into `bin/bjit` which you can use after
building to test the library with the interactive front-end parser, but note that
this doesn't actually run the code (yet), it just compiles it into `out.bin`.

There should be no need to touch the `Makefile` at all just to add additional files
unless you also need a new build-target (other than for a new test in `tests/` as
those get their build-targets automatically).

Should you somehow run into issues with automatic dependencies, type `make clean`
to start fresh. Should not happen, but just in case. I hereby place the `Makefile`
into public domain so please adapt it for your own projects if you don't know how
to write one properly.

## Contributing?

I would recommend opening an issue before working on anything too significant,
because this will greatly increase the chance that your work is still useful
by the time you're ready to send a pull-request.

The reason for this is that the project is still young enough that I might make
some drastic changes on a regular basis. I also recommend opening an issue if
you want to start building something on top of it, so we can discuss stability
of interfaces (which for the time being are subject to change without notice).

The `omena` branch is my laptop's remote and generally contains the latest and
greatest and most broken code.

## Code of conduct?

Github thinks having "code of conduct" is important, so here's one:

This project is a safe-space for minorities of any kind.

This project is **not** a safe-space for SJWs. Go away.

## Instructions?

The first step is to include `src/bjit.h`.

The second step is to create a `bjit::Proc` which takes a stack allocation size
and a string representing arguments
(`i` for integer, `f` for single floats, `d` for double). The allocated block will
always be the SSA value `Value{0}` (in practice this is the stack pointer) and
the arguments will be placed in `env[0..n]` (left to right, at most 4 for now).
More on [`env`](#env) below. Pass `0` and `""` if you don't care about allocations or arguments.
Note that we don't have an LLVM-like *mem2reg* optimization, so you should not place
variables in memory unless you need to index an array or pass a pointer somewhere.
Put them into [`env`](#env) instead.

To generate instructions, you can then call [instruction methods](#instruction-set)
on `Proc`. The last instruction of every block must be either a jump (conditional
or unconditional), a return instruction or a tail-call (DCE can cleanup dead tails
though, so a front-end can safely generate extra jumps or returns when in doubt).
Any instructions are placed into the last emitted label or the entry-block if no
labels have been emitted yet.

To generate new labels call `Proc::newLabel()` and to start emitting code to a
previous created label call `Proc:emitLabel()` (see [env](#env) below). Any labels
must always be created (but not necessarily emitted) before emitting jumps to them.
Any given label can only be emitted once (ie. calling `emitLabel()` makes the
contents of the previous block final; this is done as sanity checking only,
so let me know if you can demonstrate a use-case where this should be relaxed).

Most instructions take their parameters as SSA values. The exceptions are
`lci`/`lcf`/`lcd` which take immediate constants and jump-labels which must be
labels returned by `Proc::newLabel()`. For instructions with output values,
the methods return the new SSA values and other instructions return `void`.

Next you can either call `Proc::compile()` to obtain native code, or you can
create a `bjit::Module` and pass the `Proc` to `Module::compile()` which will
return an index. Either of these can take an "optimization level" (default: `1`)
that can be `0` for DCE-only, `1` for "safe" or `2` to allow "unsafe" optimizations
(eg. "fast-math" floating-point, ignore possibility of divide-by-zero, etc).

Multiple procedures can be compiled into the same module. See [below](#calling-functions)
on how procedures can call each other or external functions. `Module::load()` 
will load the module into executable memory and `Module::unload()` will unload it.
A module can be loaded and unloaded multiple times. Additional procedures can be
compiled at any time, but they will not be loaded until the `Module` is either
reloaded or module is hot-patched with `Module::patch()` (see `bjit.h` for details).

When a `Module` is loaded call `Module::getPointer<T>()` with an index
returned by `Module::compile()` to get a pointer to your function (with `T`
being the type of the function; we don't check this, we just typecast for you).
See one of the tests (eg. `tests/test_add_ii.cpp`) for an example.

Instructions expect their parameter types to be correct. Passing floating-point
values to instructions that expect integer values or vice versa will result
in undefined behaviour (ie. invalid code or `BJIT_ASSERT`; the latter will either
call `assert` or <code>throw&nbsp;bjit::internal_error</code> depending on whether
compiled with exceptions). Exceptions should not leak memory, but if you catch an
exception, then you should assume that the throwing `Proc` (or `Module`) is no
longer in consistent state.

The compiler should never fail with valid data unless the IR size limit is exceeded.
The limit is 64k IR instructions; we <code>throw&nbsp;bjit::too_many_ops</code>
if compiled with exceptions, otherwise we `assert` as usual; note that instructions
removed by DCE and renames/reloads generated during register allocation count towards
this limit, so it can also happen during `compile()`, but if you're seriously worried
about this limit, then Bunny-JIT is probably not the best choice for your use-case.

As we do not expect to fail when used correctly (ie. if you're writing a front-end
language, your front-end should do the error-checking), we do not provide other error
reporting beyond generic `BJIT_ASSERT` (if you trap these in debugger though,
the failure conditions should typically give a reasonable hint as to what went wrong).
In practice, at this time it probably *will* `BJIT_ASSERT` on valid code in some cases;
I'm working on test coverage, but please report a bug if you come across such cases.

The type system is very primitive though and mostly exists for the purpose of
tracking which registers we can use to store values. In particular, anything
stored in general purpose registers is called `_ptr` (or simply integers).

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

I should emphasize that you don't necessarily need to put everything into `env`
if your front-end already knows that it doesn't need any `phi`s because it's
never assigned to (locally or globally). If you understand SSA, then you can
certainly be more intelligent and only keep stuff in `env` when `phi`s are
potentially required, but this is not a requirement: the very first pass of
DCE will get rid of any excess `phi`s just fine.

To clarify `env` is *only* used by the compiler:

 - at entry to new procedure it receives the arguments
 - when `newLabel()` is called: the types are copied
 - when `emitLabel()` is called: phis are generated for the stored types
 - when a jump to a label is emitted: `env` is added to target `phi` alternatives
 - when a [call](#calling-functions) is emitted: the arguments are taken from `env`

### Instruction set?

Instructions starting `i` are for integers, `u` are unsigned variants when
there is a distinction, `f` is single-precision float and `d` is double-precision
float. Note that floating-point comparisons return integers, even though they expect
`_f32` or `_f64` parameters.

The compiler currently exposes the following instructions:

`lci i64`, `lcf f32` and `lcd f64` specify constants, `jmp label` is unconditional jump
and `jz a then else` will branch to `then` if `a` is zero or `else` otherwise and
`jnz a then else` will branch to `then` if `a` is non-zero and `else` otherwise,
`iret a` returns from the function with integer value, `fret a` with single-precision
float value and `dret a` returns with a double-precision float value.

`ieq a b` and `ine a b` compare two integers for equality or inequality and
produce `0` or `1`.

`ilt a b`, `ile a b`, `ige a b` and `igt a b` compare signed integers
for less, less-or-equal, greater-or-equal and greater respectively

`ult a b`, `ule a b`, `uge a b` and `ugt a b` perform unsigned comparisons

`feq a b`, `fne a b`, `flt a b`, `fle a b`, `fge a b` and `fgt a b` are
single-float version of the same (still produce integer `0` or `1`).

`deq a b`, `dne a b`, `dlt a b`, `dle a b`, `dge a b` and `dgt a b` are
double-float version of the same (still produce integer `0` or `1`).

`iadd a b`, `isub a b` and `imul a b` perform (signed or unsigned) integer
addition, subtraction and multiplication, while `ineg a` negates an integer

`idiv a b` and `imod a b` perform signed division and modulo, note that
divide-by-zero is *undefined behaviour* for `levelOpt=2` (ie. hardware
exceptions might not happen where expected)

`udiv a b` and `umod a b` perform unsigned division and modulo, note that
divide-by-zero is *undefined behaviour* for `levelOpt=2` (ie. hardware
exceptions might not happen where expected)

`inot a`, `iand a b`, `ior a b` and `ixor a b` perform bitwise logical operations

`ishr a b` and `ushr a b` are signed and unsigned right-shift while 
left-shift (signed or unsigned) is `ishl a b` and we currently specify that the
number of bits to shift is modulo the bitsize of integers (eg. x64 does this
natively, but I don't think ARM does; it can still be implemented efficiently
by masking, but note that this is subject to change and we might rule it as
*undefined behaviour* at least for the "unsafe" `levelOpt=2`)

`fadd a b`, `fsub a b`, `fmul a b`, `fdiv a b` and `fneg a` are single-float
versions of arithmetic operations

`dadd a b`, `dsub a b`, `dmul a b`, `ddiv a b` and `dneg a` are double-float
versions of arithmetic operations

`cf2i a` converts singles to integers while `ci2f a` converts integers to singles

`cf2d a` converts singles to doubles while `cd2f a` converts doubles to singles

`cd2i a` converts doubles to integers while `ci2d a` converts integers to doubles

`bcf2i a` and `bci2f a` bit-cast (ie. reinterpret) float-to-int and int-to-float without conversion

`bcd2i a` and `bci2d a` bit-cast (ie. reinterpret) double-to-int and int-to-double without conversion

`i8 a`, `i16 a` and `i32 a` can be used to sign-extend the low 8/16/32 bits

`u8 a`, `u16 a` and `u32 a` can be used to zero-extend the low 8/16/32 bits

Loads follow the form `lXX ptr imm32` where `ptr` is integer SSA value and `imm32`
is an immediate offset (eg. for field offsets). The variants defined are 
`li8/16/32/64`, `lu8/16/32` and `lf32/64`. The integer `i` variants sign-extend
while the `u` variants zero-extend.

Stores follow the form `sXX ptr imm32 value` where `ptr` and `imm32` are like loads
while `value` is the SSA value to store. Variants are like loads, but without
the unsigned versions.

While the compiler doesn't move loads across stores (or other side-effects) it can
move them out of loops. If you need to prevent this (eg. for multi-threading reasons)
then you can use `fence` to force a memory barrier. On x64 this is a pure compiler
fence (no code generated), but on Arm64 we do issue a full memory barrier.

Internally we have additional instructions that the fold-engine will use (in the
future the exact set might vary between platforms, so we rely on fold), but they
should be fairly obvious when seen in debug, eg. `jugeI`is a conditional jump on
`uge` comparison with the second operand converted to an `imm32` field.

## Calling functions?

Function call support is still somewhat limited as we only support up to 4 parameters
and the support is currently not particularly robust as it relies on register
allocator not accidentally overwriting parameters. This "should not happen"(tm), but
there is no real sanity-checking done for this, yet.

<i>Why up to 4? Because this is as many as we can pass in registers on Windows
 and although this could be relaxed on Unix, I want to keep the feature-set identical
 across platforms. If your code works on one platform, it shouldn't suddenly fail
 on another. The limitation will probably always stay for tail-calls, but we'll
 hopefully will be supporting more parameters for regular calls in the future.</i>

There are essentially two types of calls: near-calls and indirect calls.

Indirect ("pointer") calls can be done with `icallp`, `fcallp` and `dcallp` which take
a pointer to a function (as SSA value) and the number of arguments. The arguments are
taken from the end of `env` (ie. `push_back()` them left-to-right; calls don't pop the
arguments, you'll have to clean them up yourself). `icallp` returns an integer value,
`fcallp` returns single-float and `dcallp` returns a double-float value.

There is also `tcallp` which performs a tail-call, effectively doubling as a return
with the return value of the call. As it does not return to the procedure, it can
(and generally should) be the last thing in a given block. Tail-calls *always* clean
up the stack *before* the call, so infinite chains of tail-calls are fine, but if
you allocated a stack block then this is already invalid at the time of the call
(ie. don't pass pointers to stack with tail-calls, it won't work).

There is also "near" versions `icalln`, `fcalln`, `dcalln` and `tcalln` which can
be used to call other procedures in the same module. These take the (compile-time)
index of the procedure as their first parameter. `Module::compile()` is guaranteed to
give procedures sequential indexes starting from `0` so the target procedure need not be
compiled first as long as the index is valid by the time `Module::load()` is called.

In order to call functions outside the module without having to use `lci` to load a
constant address every time, `Module` also supports stubs. To compile a stub, call
`Module::compileStub()` with the memory address of the target procedure. Stubs count
as procedures in terms of indexes and can be called with near calls.

### Patching calls

You can also change far call stub targets later by calling `Module::patchStub()` with
the index of a previously compiled stub and a new target address. The stub target will
be updated (in executable code) next time either `Module::patch()` or `Module::load()`
is called. Note that "bad things will happen"(tm) if you try to patch a procedure
that is not actually a stub (we don't check this in any way).

Near-calls can also be patched, either globally with `Module::patchCalls(oldT,newT)`
or locally in one procedure with `Module::patchCallsIn(inProc,oldT,newT)` where 
`oldT` and `newT` are the old and new targets respectively. There is currently no
support for patching a single call though (you'll need to compile some stubs for
that).

Note that `Module::patch()` does not apply *any* patches if it can't also load all
newly compiled code, but they remain pending and will be applied on module reload.

## What it does?

The [`test_sieve.cpp`](tests/test_sieve.cpp) contains a C++ variation of
the classic sieve algorithm, with "flags" array and size as parameters:
```
int sieve(char * flags, int size)
{
    int count = 0;

    for (int i = 0; i < size; ++i) flags[i] = true;
    for (int i = 2; i < size; ++i)
    { 
        if (flags[i])
        {
            int prime = i + 1; 
            int k = i + prime; 

            while (k < size)
            { 
                flags[k] = false; 
                k += prime;
            }
            
            ++count;
        }
    }

    return count;
}
```

It also contains the same thing manually translated to the Bunny-JIT API:

```
    bjit::Module module;
    {
        bjit::Proc  pr(0, "ii");

        int _flags = 0;
        int _size = 1;

        // i = 0
        int _i      = pr.env.size(); pr.env.push_back(pr.lci(0));
        // count = 0
        int _count  = pr.env.size(); pr.env.push_back(pr.lci(0));

        auto ls0 = pr.newLabel();
        auto lb0 = pr.newLabel();
        auto le0 = pr.newLabel();

        pr.jmp(ls0);
        
        pr.emitLabel(ls0);
        // while i < size
        pr.jz(pr.ilt(pr.env[_i], pr.env[_size]), le0, lb0);
        pr.emitLabel(lb0);
        
            // *(flags + i) = 1
            pr.si8(pr.iadd(pr.env[_flags], pr.env[_i]), 0, pr.lci(1));
            // ++i
            pr.env[_i] = pr.iadd(pr.env[_i], pr.lci(1));
            pr.jmp(ls0);
            
        pr.emitLabel(le0);

        // i = 2
        pr.env[_i] = pr.lci(2);
        auto ls1 = pr.newLabel();
        auto lb1 = pr.newLabel();
        auto le1 = pr.newLabel();

        pr.jmp(ls1);
        pr.emitLabel(ls1);
        
        // while i < size
        pr.jz(pr.ilt(pr.env[_i], pr.env[_size]), le1, lb1);
        pr.emitLabel(lb1);
            
            auto bt = pr.newLabel();
            auto be = pr.newLabel();
    
            // if flags[i] != 0
            pr.jnz(pr.li8(pr.iadd(pr.env[_flags], pr.env[_i]), 0), bt, be);
            pr.emitLabel(bt);
        
                // prime = i + 1
                int _prime  = pr.env.size();
                pr.env.push_back(pr.iadd(pr.env[_i], pr.lci(1)));
        
                // k = i + prim
                int _k = pr.env.size();
                pr.env.push_back(pr.iadd(pr.env[_i], pr.env[_prime]));
        
                auto ls2 = pr.newLabel();
                auto lb2 = pr.newLabel();
                auto le2 = pr.newLabel();
        
                pr.jmp(ls2);
                pr.emitLabel(ls2);
        
                // while k < size
                pr.jnz(pr.ilt(pr.env[_k], pr.env[_size]), lb2, le2);
                pr.emitLabel(lb2);
        
                    // flags[k] = false;
                    pr.si8(pr.iadd(pr.env[_flags], pr.env[_k]), 0, pr.lci(0));

                    // k = k + prime
                    pr.env[_k] = pr.iadd(pr.env[_k], pr.env[_prime]);
            
                    pr.jmp(ls2);
                    
                pr.emitLabel(le2);
        
                pr.env.pop_back();  // k
                pr.env.pop_back();  // prime
        
                pr.env[_count] = pr.iadd(pr.env[_count], pr.lci(1));
        
                pr.jmp(be);
            
            pr.emitLabel(be);
            
            // ++i
            pr.env[_i] = pr.iadd(pr.env[_i], pr.lci(1));
    
            pr.jmp(ls1);

        pr.emitLabel(le1);

        pr.iret(pr.env[_count]);

        pr.debug();

        module.compile(pr);
    }
```

Bunny-JIT currently compiles it into this native code:

```
   0:	48 83 ec 08                                     	sub    rsp,0x8
   4:	b8 02 00 00 00                                  	mov    eax,0x2
   9:	33 c9                                           	xor    ecx,ecx
   b:	48 83 fe 00                                     	cmp    rsi,0x0
   f:	0f 8e 21 00 00 00                               	jle    0x36
  15:	ba 01 00 00 00                                  	mov    edx,0x1
  1a:	4c 8b c1                                        	mov    r8,rcx
  1d:	4d 8d 0c 38                                     	lea    r9,[r8+rdi*1]
  21:	41 88 11                                        	mov    BYTE PTR [r9],dl
  24:	49 83 c0 01                                     	add    r8,0x1
  28:	4c 3b c6                                        	cmp    r8,rsi
  2b:	0f 8d 05 00 00 00                               	jge    0x36
  31:	e9 e7 ff ff ff                                  	jmp    0x1d
  36:	48 83 fe 02                                     	cmp    rsi,0x2
  3a:	0f 8e 5d 00 00 00                               	jle    0x9d
  40:	48 8d 50 01                                     	lea    rdx,[rax+0x1]
  44:	4c 8d 04 38                                     	lea    r8,[rax+rdi*1]
  48:	4d 0f be 00                                     	movsx  r8,BYTE PTR [r8]
  4c:	4d 85 c0                                        	test   r8,r8
  4f:	0f 84 2f 00 00 00                               	je     0x84
  55:	48 83 c1 01                                     	add    rcx,0x1
  59:	4c 8d 40 01                                     	lea    r8,[rax+0x1]
  5d:	49 03 c0                                        	add    rax,r8
  60:	48 3b c6                                        	cmp    rax,rsi
  63:	0f 8d 1b 00 00 00                               	jge    0x84
  69:	45 33 c9                                        	xor    r9d,r9d
  6c:	4c 8d 14 38                                     	lea    r10,[rax+rdi*1]
  70:	45 88 0a                                        	mov    BYTE PTR [r10],r9b
  73:	49 03 c0                                        	add    rax,r8
  76:	48 3b c6                                        	cmp    rax,rsi
  79:	0f 8d 05 00 00 00                               	jge    0x84
  7f:	e9 e8 ff ff ff                                  	jmp    0x6c
  84:	48 3b d6                                        	cmp    rdx,rsi
  87:	0f 8d 08 00 00 00                               	jge    0x95
  8d:	48 8b c2                                        	mov    rax,rdx
  90:	e9 ab ff ff ff                                  	jmp    0x40
  95:	48 8b c1                                        	mov    rax,rcx
  98:	e9 03 00 00 00                                  	jmp    0xa0
  9d:	48 8b c1                                        	mov    rax,rcx
  a0:	48 83 c4 08                                     	add    rsp,0x8
  a4:	c3                                              	ret
```

Comparing with `clang -Ofast` (somewhat old version) on my laptop, running both
of these on an array of 819000 bytes, we get something like this:

```
C-sieve: 65333 primes
BJIT-sieve: 65333 primes
Iterating 100 times...
C time: 442ms
BJIT time: 478ms
```

The exact times (obviously) vary from run to run (and the test might not be entirely
accurate or fai), and you should expect Bunny-JIT to do significantly worse on code
that relies heavily on optimizing memory access or code that can be effectively
vectorized, but the basic idea is that it can typically output something reasonable.

## SSA?

The backend keeps the code in SSA form from the beginning to the end. We rely
on `env` to automatically add `phi`s for all cross-block variables initially.
While there is no need to add temporaries to the environment, always adding `phi`s
for any actual local variables still creates a lot more `phi`s than necessary. We choose
to let DCE clean this up, by simplifying those `phi`s with only one real source.
This cleanup is fundamental to the design as it acts as a dataflow analysis pass
and is repeated several times through the optimization process.

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
stack location, that any value can be thrown away and reloaded at any time (as
long as it's then marked for "spill" at the time the reload is done) yet we can
trivially collapse the layout afterwards. The assembler will then generate the
actual stores after the operations that need them (valid because of SSA).

## Optimizations

At the beginning of this page I said above I find traditional compiler optimizations
a bit confusing to relate to, because they talk about things like versions of
variables and safety. In SSA, there is exactly one version of a value and values
are referentially transparent: as long as the value is defined in some dominator
block the value is safe. But here's what we do:

The DCE pass (`opt_dce`) serves a couple of purposes: first, it finds all the
actual live-blocks by doing a depth-first search from the entry. Any block that
isn't reached will be ignored. It also counts the number of uses for each value
and throw away anything (without side-effects) with a use-count of zero (and then
all values that now have a use-count of zero and so on). It also performs simple
jump threading and replaces `phi` uses with actual values if all alternatives are
either the same or simple loop-back to the `phi` itself. This essentially gives us
a simple form of data-flow analysis. 

Invariants: DCE builds the list of `live` blocks, so it must run as the very first
thing even for non-optimizing builds. It does not rely on any other analysis.
DCE is used a cleanup pass, so all other passes (including register allocation)
must leave the IR in a state where DCE is safe (ie. we even do a final DCE pass
after register allocation, just before native assembly; if this breaks the code,
it's almost certainly a bug in `opt_ra`).

DCE (and other optimizations) also calls `rebuild_cfg` which rebuilds "come from"
information and cleans up `phi` alternatives where the incoming edge no longer
exists. Invariants: `rebuild_cfg` assumes `live` contains all live blocks and
that all jumps can be found by looking at the last ops of each block (ie. all
dead tails have been eliminated).

The `opt_fold` pass only does simple constant-folding/strength-reduction and
algebraic simplications and only relies on `live` containing all live blocks.
Operations simplified to `nop` are left in-place for DCE to clean up.

Because `opt_fold` also simplifies conditional jumps into non-conditional jumps
and because `opt_dce` simplies unnecessary `phi` loops this gives us much of
SCCP although we can't handle the cases where a branch must be taken for the
branch condition to ever choose that branch. I'm not entirely convinced this
would really be worth the trouble.

Invariants: Folding does not use any properties of CFG and does not rewrite CFG.
It only relies on the SSA invariant that definitions always dominate uses.

The `opt_cse` pass does two things: it first tries to hoist operations up the
dominator chain to the earliest block where all inputs are available, unless
the operation is marked with `flags.no_opt` (eg. we already sunk the op where
it's needed; FIXME: we might want to make hoisting a bit more intelligent and
allow it to break critical edges directly).

Then `opt_cse` tries to globally combine all pairs of identical ops and
place the combined op at the closest common dominator (by SSA property there
always exists a CCD where all the inputs are defined), but only if it can reach
the CCD by following a two-way dominator/post-dominator chain (ie. don't cross
branches that don't also merge, but diamonds are fine; this is ignored for the
CCD itself, because if we come from two different branches, then both branches
compute the same value and combining just results in smaller code).

CSE also tries to match against the alternatives of a `phi` operands. If we find
matches this way, we insert similar ops on other paths (assuming they don't have
one already) and then collect results into a new `phi` which gives us PRE for the
simple cases, although we don't currently attempt to recursively search through
multiple `phi`s (maybe some day; this can get a bit expensive though).

Invariants: CSE moves/renames operations, can insert additional `phi` and break
critical edges, but rebuilds dominators if it changes the CFG.

Because `opt_cse` needs dominator information (for both hosting and actual CSE)
to avoid moving operations in the wrong places, it calls `rebuild_dom` which
finds the immediate dominators `.idom` and immediate post-dominators `.pdom`
for each block and then builds a per-block sorted list of all dominators `.dom`
to allow for easier dominance checks and CCD searches.

Invariants: `rebuild_dom` calls `rebuild_cfg` so it rebuilds both.

The `opt_sink` pass does the opposite of hoisting and tries to move ops down
branches where they are actually needed. For this it needs live-in information
from `rebuild_livein` and because it can break critical edges if necessary it will
invalidate the CFG and dominator information.

The `opt_jump` pass optimizes jumps. Currently it only optimizes jumps back to
dominators (loop back edges; `opt_jump_be`) by making a copy of the block and
adding new `phi`s for all live-in values (globally) that originated from the
(supposedly) loop header. This effectively results in loop inversion and allows
`opt_sink` to create pre-headers by breaking critical edges. Because `opt_jump`
rewrites CFG, it needs both `livein` and dominators and it will invalidate both.
It rebuilds CFG, but not dominators.

The `opt_scc` pass computes [SCCs](#scc) and the `opt_ra` pass performs
register allocation. There are only done once at the end of the compilation
and they are always done, even for non-optimized builds. After RA the code
is ready to be assembled. Note that code is still valid SSA.

Currently Bunny-JIT does very limited memory optimization: we allow DCE, CSE
and hoisting on loads, but assume any side-effect (of any kind) can alias.
It probably never try to do sophisticated analyse aliasing, because this is
such a huge can of worms, but I'm looking to maybe optimize some of the simple
cases (eg. identical loads with no stores between them) in the future.

Bunny-JIT is currently very inefficient compiler for high-level object-oriented
languages that rely heavily on following the same pointer-chains over and over
again and it will probably always be very inefficient for languages that rely
heavily on mutating objects in memory. I don't really care, it's not my focus.
