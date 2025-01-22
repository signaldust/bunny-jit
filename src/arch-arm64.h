
#pragma once

namespace bjit
{
    // this is a hint for opt-ra
    static const bool arch_explicit_output_regs = true;

    // we use this for types, etc
    typedef uint64_t    RegMask;

    // convert single-register to a mask
    static RegMask R2Mask(int r) { return ((RegMask)1)<<r; }
        
    namespace regs
    {
        // List of registers for register allocator
        // These should be in preference order
        //
        //  x0 - x7 are used for calls
        //
        //  x16 - x17 are linker temporary (can we use them as callee saved?)
        //  x18 is platform register
        //
        //  x29 = frame pointer
        //  x30 = link register
        //  x31 = stack pointer or zero register
        //
#define BJIT_REGS(_) \
        /* caller saved */ \
        _(x0), _(x1), _(x2), _(x3), _(x4), _(x5), _(x6), _(x7), \
        _(x8), _(x9), _(x10), _(x11), _(x12), _(x13), _(x14), _(x15), \
        /* special */ \
        _(x16), _(x17), _(x18), \
        /* callee saved */ \
        _(x19), _(x20), _(x21), _(x22), _(x23), \
        _(x24), _(x25), _(x26), _(x27), _(x28), _(fp), _(lr), _(sp), \
        /* floating point */ \
        _(v0), _(v1), _(v2), _(v3), _(v4), _(v5), _(v6), _(v7), \
        _(v16), _(v17), _(v18), _(v19), _(v20), _(v21), _(v22), _(v23), \
        _(v24), _(v25), _(v26), _(v27), _(v28), _(v29), _(v30), _(v31), \
        /* callee-saved floats .. prefer them last */ \
        _(v8), _(v9), _(v10), _(v11), _(v12), _(v13), _(v14), _(v15), \
        /* placeholder */ \
        _(none)

#define BJIT_REGS_ENUM(x) x
        // set nregs as the first non-register value
        enum { BJIT_REGS(BJIT_REGS_ENUM), nregs = none };

        // Integer register mask (without specials)
        static const RegMask mask_int
            =R2Mask(x0)
            |R2Mask(x1)
            |R2Mask(x2)
            |R2Mask(x3)
            |R2Mask(x4)
            |R2Mask(x5)
            |R2Mask(x6)
            |R2Mask(x7)
            
            |R2Mask(x8)
            |R2Mask(x9)
            |R2Mask(10)
            |R2Mask(x11)
            |R2Mask(x12)
            |R2Mask(x13)
            |R2Mask(x14)
            |R2Mask(x15)
            
            |R2Mask(x19)
            |R2Mask(x20)
            |R2Mask(x21)
            |R2Mask(x22)
            |R2Mask(x23)
            |R2Mask(x24)
            |R2Mask(x25)
            |R2Mask(x26)
            |R2Mask(x27) //*/
            |R2Mask(x28)
            ;

        // Float register masks
        static const RegMask mask_float_volatile
            =R2Mask(v0)
            |R2Mask(v1)
            |R2Mask(v2)
            |R2Mask(v3)
            |R2Mask(v4)
            |R2Mask(v5)
            |R2Mask(v6)
            |R2Mask(v7)

            |R2Mask(v16)
            |R2Mask(v17)
            |R2Mask(v18)
            |R2Mask(v19)
            |R2Mask(v20)
            |R2Mask(v21)
            |R2Mask(v22)
            |R2Mask(v23)

            |R2Mask(v24)
            |R2Mask(v25)
            |R2Mask(v26)
            |R2Mask(v27)
            |R2Mask(v28)
            |R2Mask(v29)
            |R2Mask(v30)
            |R2Mask(v31)
            ;

        // note v8 - v15 are preserved up to 64 bits
        // so if we add vector ops, we might want to
        // treat these as scalar only?
        static const RegMask mask_float
            = mask_float_volatile
            |R2Mask(v8)
            |R2Mask(v9)
            |R2Mask(v10)
            |R2Mask(v11)
            |R2Mask(v12)
            |R2Mask(v13)
            |R2Mask(v14)
            |R2Mask(v15);
            
        // Caller saved (lost on function call)
        //
        // NOTE: We treat all xmm registers as volatile when calling functions.
        // The backend uses separate logic for callee_saved when we are callee.
        //
        // NOTE: This list MUST include any registers used for arguments.
        static const RegMask caller_saved
            =R2Mask(x0)
            |R2Mask(x1)
            |R2Mask(x2)
            |R2Mask(x3)
            |R2Mask(x4)
            |R2Mask(x5)
            |R2Mask(x6)
            |R2Mask(x7)
            |R2Mask(x8)
            |R2Mask(x9)
            |R2Mask(x10)
            |R2Mask(x11)
            |R2Mask(x12)
            |R2Mask(x13)
            |R2Mask(x14)
            |R2Mask(x15)
            |R2Mask(fp)
            |R2Mask(lr)
            | mask_float_volatile
            ;
    };
};
