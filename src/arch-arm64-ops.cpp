
#ifdef __aarch64__

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

RegMask Op::regsOut()
{
    // only deal with anything that isn't regs::mask_int explicit
    switch(opcode)
    {
        default: return regsMask(); // no special case -> any valid

        // special
        case ops::alloc: return R2Mask(regs::sp);
    
        case ops::icallp: case ops::icalln: return R2Mask(regs::x0);
        
        case ops::fcallp: case ops::fcalln:
        case ops::dcallp: case ops::dcalln: return R2Mask(regs::v0);

        // we have in[0] = index in type, in[1] = index total
        // which one we want to use varies by platform
        case ops::iarg:
            switch(indexType)   // AArch64 uses position by type
            {
            case 0: return R2Mask(regs::x0);
            case 1: return R2Mask(regs::x1);
            case 2: return R2Mask(regs::x2);
            case 3: return R2Mask(regs::x3);
            case 4: return R2Mask(regs::x4);
            case 5: return R2Mask(regs::x5);
            case 6: return R2Mask(regs::x6);
            case 7: return R2Mask(regs::x7);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
        case ops::farg:
        case ops::darg:
            switch(indexType)   // AArch64 uses position by type
            {
            case 0: return R2Mask(regs::v0);
            case 1: return R2Mask(regs::v1);
            case 2: return R2Mask(regs::v2);
            case 3: return R2Mask(regs::v3);
            case 4: return R2Mask(regs::v4);
            case 5: return R2Mask(regs::v5);
            case 6: return R2Mask(regs::v6);
            case 7: return R2Mask(regs::v7);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
    }

    // silence warning if assert is nop
    BJIT_ASSERT(false); return 0;
}

RegMask Op::regsIn(int i)
{
    switch(opcode)
    {
        default: return regsMask(); // no special case -> same as output

        // indirect calls can theoretically take any GP register
        // but we don't want to use x0-x7 used for passing arguments
        //
        // FIXME: should check how many integer arguments we have
        case ops::icallp: case ops::dcallp:
        case ops::fcallp: case ops::tcallp:
            return regs::mask_int &
                ~(R2Mask(regs::x0) |R2Mask(regs::x1)
                |R2Mask(regs::x2) |R2Mask(regs::x3)
                |R2Mask(regs::x4) |R2Mask(regs::x5)
                |R2Mask(regs::x6) |R2Mask(regs::x7));

        // loads and stores allow stack pointer as their first argument
        // FIXME: we do NOT want to rename to RSP though :D
        case ops::li8: case ops::li16: case ops::li32: case ops::li64:
        case ops::lu8: case ops::lu16: case ops::lu32:
        case ops::lf32: case ops::lf64:
        case ops::si8: case ops::si16: case ops::si32: case ops::si64:
            return regs::mask_int | (i ? 0 : R2Mask(regs::sp));
        case ops::sf32: case ops::sf64:
            return i ? regs::mask_float : ((regs::mask_int) | R2Mask(regs::sp));

        // jumps and float compares need explicit types
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

        // explicit with casts (duh)
        case ops::ci2f: case ops::bci2f:
        case ops::ci2d: case ops::bci2d:
            return regs::mask_int;
            
        case ops::ipass:
            switch(indexType)   // AArch64 uses position by type
            {
            case 0: return R2Mask(regs::x0);
            case 1: return R2Mask(regs::x1);
            case 2: return R2Mask(regs::x2);
            case 3: return R2Mask(regs::x3);
            case 4: return R2Mask(regs::x4);
            case 5: return R2Mask(regs::x5);
            case 6: return R2Mask(regs::x6);
            case 7: return R2Mask(regs::x7);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }
        case ops::fpass:
        case ops::dpass:
            switch(indexType)   // AArch64 uses position by type
            {
            case 0: return R2Mask(regs::v0);
            case 1: return R2Mask(regs::v1);
            case 2: return R2Mask(regs::v2);
            case 3: return R2Mask(regs::v3);
            case 4: return R2Mask(regs::v4);
            case 5: return R2Mask(regs::v5);
            case 6: return R2Mask(regs::v6);
            case 7: return R2Mask(regs::v7);

            // FIXME: We need to teach RA about stack parameters.
            default: BJIT_ASSERT(false);
            }

        // these are fixed
        case ops::iret: return R2Mask(regs::x0);
        case ops::fret: return R2Mask(regs::v0);
        case ops::dret: return R2Mask(regs::v0);

    }
}

RegMask Op::regsLost()
{
    switch(opcode)
    {
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

        case ops::icalln: case ops::fcalln: case ops::dcalln:
        case ops::icallp: case ops::fcallp: case ops::dcallp:
            return regs::caller_saved;

        default: return 0;
    }
}

#endif