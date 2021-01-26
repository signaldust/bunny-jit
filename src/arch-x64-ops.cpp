
#include "ir.h"

using namespace bjit;

RegMask Op::regsMask()
{
    switch(flags.type)
    {
        case _ptr: return regs::mask_int;
        case _f64: return regs::mask_float;

        default: assert(false);
    }
}

RegMask Op::regsOut()
{
    // only deal with anything that isn't regs::mask_int explicit
    switch(opcode)
    {
        // pseudo-ops allow any valid register of the correct type
        case ops::phi: case ops::rename: case ops::reload:
            return regsMask();
        
        // divisions are fixed registers
        case ops::idiv: case ops::udiv: return (1ull<<regs::rax);
        case ops::imod: case ops::umod: return (1ull<<regs::rdx);

        // floating point stuff
        case ops::fadd: case ops::fsub: case ops::fneg:
        case ops::fmul: case ops::fdiv: case ops::lcf:
        case ops::lf64: case ops::ci2f:
            return regs::mask_float;

        case ops::icall: return (1ull<<regs::rax);
        case ops::fcall: return (1ull<<regs::xmm0);

        // we have in[0] = index in type, in[1] = index total
        // which one we want to use varies by platform
        case ops::iparam:
#ifdef _WIN32
            switch(in[1])   // Win64 wants the total position
            {
            case 0: return (1ull<<regs::rcx);
            case 1: return (1ull<<regs::rdx);
            case 2: return (1ull<<regs::r8);
            case 3: return (1ull<<regs::r9);

            default: assert(false); // FIXME: RA can't handle
            }
#else
            switch(in[0])   // SysV uses position by type
            {
            case 0: return (1ull<<regs::rdi);
            case 1: return (1ull<<regs::rsi);
            case 2: return (1ull<<regs::rdx);
            case 3: return (1ull<<regs::rcx);
            case 4: return (1ull<<regs::r8);
            case 5: return (1ull<<regs::r9);

            default: assert(false); // FIXME: RA can't handle
            }
#endif
        case ops::fparam:
#ifdef _WIN32
            switch(in[1])   // Win64 wants the total index
            {
            case 0: return (1ull<<regs::xmm0);
            case 1: return (1ull<<regs::xmm1);
            case 2: return (1ull<<regs::xmm2);
            case 3: return (1ull<<regs::xmm3);

            default: assert(false); // FIXME: RA can't handle
            }
#else
            switch(in[0])   // SysV uses position by type
            {
            case 0: return (1ull<<regs::xmm0);
            case 1: return (1ull<<regs::xmm1);
            case 2: return (1ull<<regs::xmm2);
            case 3: return (1ull<<regs::xmm3);
            case 4: return (1ull<<regs::xmm4);
            case 5: return (1ull<<regs::xmm5);
            case 6: return (1ull<<regs::xmm6);
            case 7: return (1ull<<regs::xmm7);

            default: assert(false); // FIXME: RA can't handle
            }
#endif

        default: return regs::mask_int;
    }
}

RegMask Op::regsIn(int i)
{
    switch(opcode)
    {
        // pseudo-ops allow any valid register of the correct type
        case ops::phi: case ops::rename: case ops::reload:
            return regsMask();
        
        // integer division takes RDX:RAX as 128-bit first operand
        // we only do 64-bit, but force RAX on 1st and forbid RDX on 2nd
        case ops::idiv: case ops::udiv:
        case ops::imod: case ops::umod:
            return (!i) ? (1ull<<regs::rax)
            : (regs::mask_int & ~(1ull<<regs::rdx));

        case ops::jflt: case ops::jfge:
        case ops::jfgt: case ops::jfle:
        case ops::jfeq: case ops::jfne:

        case ops::flt: case ops::fge:
        case ops::fgt: case ops::fle:
        case ops::feq: case ops::fne:
        
        case ops::fadd: case ops::fsub: case ops::fneg:
        case ops::fmul: case ops::fdiv: case ops::lcf:
        case ops::lf64: case ops::ci2f:
            return regs::mask_float;

        // these are fixed so might just as well
        case ops::iret: return (1ull<<regs::rax);
        case ops::fret: return (1ull<<regs::xmm0);

        default: return regs::mask_int;
    }
}

RegMask Op::regsLost()
{
    switch(opcode)
    {
        case ops::idiv: case ops::udiv: return (1ull<<regs::rdx);
        case ops::imod: case ops::umod: return (1ull<<regs::rax);

        case ops::icall:  case ops::fcall: return regs::caller_saved;

        default: return 0;
    }
}