
#include "bjit.h"

using namespace bjit;
using namespace bjit::impl;

RegMask Op::regsMask()
{
    switch(flags.type)
    {
        case _ptr: return regs::mask_int;
        case _f32: return regs::mask_float;
        case _f64: return regs::mask_float;

        default: BJIT_LOG("%s\n", strOpcode());
    }
    // silence warning if assert is nop
    BJIT_ASSERT(false); return 0;
}

RegMask Op::regsLostSoon()
{
    switch(opcode)
    {
        case ops::ipass:
        case ops::fpass:
        case ops::dpass:
            return regs::caller_saved;
        default:
            return 0;
    }
}

RegMask Op::regsOut()
{
    // only deal with anything that isn't regs::mask_int explicit
    switch(opcode)
    {
        default: return regsMask(); // no special case -> any valid

        // special
        case ops::alloc: return R2Mask(regs::rsp);
    
        // divisions are fixed registers
        case ops::idiv: case ops::udiv: return R2Mask(regs::rax);
        case ops::imod: case ops::umod: return R2Mask(regs::rdx);

        case ops::icallp: case ops::icalln: return R2Mask(regs::rax);
        
        case ops::fcallp: case ops::fcalln:
        case ops::dcallp: case ops::dcalln: return R2Mask(regs::xmm0);

        // we have in[0] = index in type, in[1] = index total
        // which one we want to use varies by platform
        case ops::iarg:
#ifdef _WIN32
            switch(indexTotal)  // Win64 wants the total position
            {
            case 0: return R2Mask(regs::rcx);
            case 1: return R2Mask(regs::rdx);
            case 2: return R2Mask(regs::r8);
            case 3: return R2Mask(regs::r9);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
#else
            switch(indexType)   // SysV uses position by type
            {
            case 0: return R2Mask(regs::rdi);
            case 1: return R2Mask(regs::rsi);
            case 2: return R2Mask(regs::rdx);
            case 3: return R2Mask(regs::rcx);
            case 4: return R2Mask(regs::r8);
            case 5: return R2Mask(regs::r9);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
#endif
        case ops::farg:
        case ops::darg:
#ifdef _WIN32
            switch(indexTotal)  // Win64 wants the total position
            {
            case 0: return R2Mask(regs::xmm0);
            case 1: return R2Mask(regs::xmm1);
            case 2: return R2Mask(regs::xmm2);
            case 3: return R2Mask(regs::xmm3);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
#else
            switch(indexType)   // SysV uses position by type
            {
            case 0: return R2Mask(regs::xmm0);
            case 1: return R2Mask(regs::xmm1);
            case 2: return R2Mask(regs::xmm2);
            case 3: return R2Mask(regs::xmm3);
            case 4: return R2Mask(regs::xmm4);
            case 5: return R2Mask(regs::xmm5);
            case 6: return R2Mask(regs::xmm6);
            case 7: return R2Mask(regs::xmm7);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
#endif
    }

    // silence warning if assert is nop
    BJIT_ASSERT(false); return 0;
}

RegMask Op::regsIn(int i)
{
    switch(opcode)
    {
        default: return regsMask(); // no special case -> same as input

        // indirect calls can theoretically take any GP register
        // but force RAX so we hopefully don't globber stuff
        case ops::icallp: case ops::dcallp:
        case ops::fcallp: case ops::tcallp:
            return R2Mask(regs::rax);
        
        // loads and stores allow stack pointer as their first argument
        // FIXME: we do NOT want to rename to RSP though :D
        case ops::li8: case ops::li16: case ops::li32: case ops::li64:
        case ops::lu8: case ops::lu16: case ops::lu32:
        case ops::lf32: case ops::lf64:
        case ops::si8: case ops::si16: case ops::si32: case ops::si64:
            return regs::mask_int | (i ? 0 : R2Mask(regs::rsp));
        case ops::sf32: case ops::sf64:
            return i ? regs::mask_float : ((regs::mask_int) | R2Mask(regs::rsp));

        // allow iadd and iaddI to take RSP too, saves moves if we use LEA
        case ops::iadd: case ops::iaddI:
            return regs::mask_int | R2Mask(regs::rsp);
        
        // integer division takes RDX:RAX as 128-bit first operand
        // we only do 64-bit, but force RAX on 1st and forbid RDX on 2nd
        case ops::idiv: case ops::udiv:
        case ops::imod: case ops::umod:
            return (!i) ? R2Mask(regs::rax)
            : (regs::mask_int & ~R2Mask(regs::rdx));

        case ops::jilt: case ops::jige:
        case ops::jigt: case ops::jile:
        case ops::jieq: case ops::jine:
        case ops::jiltI: case ops::jigeI:
        case ops::jigtI: case ops::jileI:
        case ops::jieqI: case ops::jineI:
        case ops::jz: case ops::jnz:
            return regs::mask_int;

        case ops::jdlt: case ops::jdge:
        case ops::jdgt: case ops::jdle:
        case ops::jdeq: case ops::jdne:

        case ops::flt: case ops::fge:
        case ops::fgt: case ops::fle:
        case ops::feq: case ops::fne:
        
        case ops::lcf: case ops::cf2i:

        case ops::dlt: case ops::dge:
        case ops::dgt: case ops::dle:
        case ops::deq: case ops::dne:
        
        case ops::lcd: case ops::cd2i:
        case ops::bcd2i: case ops::bcf2i:
            return regs::mask_float;

        case ops::ci2f: case ops::bci2f:
        case ops::ci2d: case ops::bci2d:
            return regs::mask_int;
            
        // shifts want their second operand in CL
        case ops::ishl: case ops::ishr: case ops::ushr:
            return i ? R2Mask(regs::rcx) :
                (regs::mask_int &~ R2Mask(regs::rcx));

        case ops::ipass:
#ifdef _WIN32
            switch(indexTotal)  // Win64 wants the total position
            {
            case 0: return R2Mask(regs::rcx);
            case 1: return R2Mask(regs::rdx);
            case 2: return R2Mask(regs::r8);
            case 3: return R2Mask(regs::r9);

            default: BJIT_ASSERT(false); // FIXME: RA can't handle
            }
#else
            switch(indexType)   // SysV uses position by type
            {
            case 0: return R2Mask(regs::rdi);
            case 1: return R2Mask(regs::rsi);
            case 2: return R2Mask(regs::rdx);
            case 3: return R2Mask(regs::rcx);
            case 4: return R2Mask(regs::r8);
            case 5: return R2Mask(regs::r9);

            default: BJIT_ASSERT(false); // FIXME: RA can't handle
            }
#endif
        case ops::fpass:
        case ops::dpass:
#ifdef _WIN32
            switch(indexTotal)  // Win64 wants the total index
            {
            case 0: return R2Mask(regs::xmm0);
            case 1: return R2Mask(regs::xmm1);
            case 2: return R2Mask(regs::xmm2);
            case 3: return R2Mask(regs::xmm3);

            default: BJIT_ASSERT(false); // FIXME: RA can't handle
            }
#else
            switch(indexType)   // SysV uses position by type
            {
            case 0: return R2Mask(regs::xmm0);
            case 1: return R2Mask(regs::xmm1);
            case 2: return R2Mask(regs::xmm2);
            case 3: return R2Mask(regs::xmm3);
            case 4: return R2Mask(regs::xmm4);
            case 5: return R2Mask(regs::xmm5);
            case 6: return R2Mask(regs::xmm6);
            case 7: return R2Mask(regs::xmm7);

            default: BJIT_ASSERT(false); // FIXME: RA can't handle
            }
#endif

        // these are fixed
        case ops::iret: return R2Mask(regs::rax);
        case ops::fret: return R2Mask(regs::xmm0);
        case ops::dret: return R2Mask(regs::xmm0);

    }
}

RegMask Op::regsLost()
{
    switch(opcode)
    {
        case ops::idiv: case ops::udiv:
        case ops::imod: case ops::umod:
            // mark the output as lost as well, so RA tries to save
            // if we still need the value after the division
            return R2Mask(regs::rax)|R2Mask(regs::rdx);

        // for now, collect registers used by previous args
        // this should help convince RA to do the right thing
        case ops::ipass:
        case ops::dpass:
            {
                RegMask used = 0;
                for(int i = 0; i < in[1]; ++i)
                {
                    used |= regsIn(i);
                }
                return used;
            }

        case ops::icalln: case ops::fcalln:  case ops::dcalln:
        case ops::icallp: case ops::fcallp:  case ops::dcallp:
            return regs::caller_saved;

        default: return 0;
    }
}