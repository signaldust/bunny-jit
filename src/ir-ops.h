
#pragma once

// List of operations, order is significant
//  _(name, nOutputs, nInputs)

// output flags
#define BJIT_SIDEFX 0x10    // never DCE
#define BJIT_CSE    0x20    // can CSE

// input flags
#define BJIT_IMM32  0x10    // has imm32 operand
#define BJIT_I64    0x20    // has 64-bit integer constant
#define BJIT_F64    0x40    // has double constant

#define BJIT_OPS(_) \
    /* (xor 1): branch signed integer comparisons */ \
    _(jilt, 0, 2), \
    _(jige, 0, 2), \
    _(jigt, 0, 2), \
    _(jile, 0, 2), \
    /* (xor 1): branch unsigned integer comparisons */ \
    _(jult, 0, 2), \
    _(juge, 0, 2), \
    _(jugt, 0, 2), \
    _(jule, 0, 2), \
    /* (xor 1): branch integer equality (equal, not equal) */ \
    _(jieq, 0, 2), \
    _(jine, 0, 2), \
    /* (xor 1): branch float comparisons */ \
    _(jflt, 0, 2), \
    _(jfge, 0, 2), \
    _(jfgt, 0, 2), \
    _(jfle, 0, 2), \
    /* (xor 1): branch float equality (equal, not equal) */ \
    _(jfeq, 0, 2), \
    _(jfne, 0, 2), \
    /* (xor 1): integer zero, not-zero tests */ \
    _(jz,  0, 1), \
    _(jnz, 0, 1), \
    /* */ \
    /* NOTE: THESE SHOULD MATCH THOSE STARTING FROM 'jilt' */ \
    /* SO MAKE SURE THE POSITIONS STAY RELATIVE */ \
    /* */ \
    /* (xor 1): branch signed integer comparisons */ \
    _(jiltI, 0, 1+BJIT_IMM32), \
    _(jigeI, 0, 1+BJIT_IMM32), \
    _(jigtI, 0, 1+BJIT_IMM32), \
    _(jileI, 0, 1+BJIT_IMM32), \
    /* (xor 1): branch unsigned integer comparisons */ \
    _(jultI, 0, 1+BJIT_IMM32), \
    _(jugeI, 0, 1+BJIT_IMM32), \
    _(jugtI, 0, 1+BJIT_IMM32), \
    _(juleI, 0, 1+BJIT_IMM32), \
    /* (xor 1): branch integer equality comparisons */ \
    _(jieqI, 0, 1+BJIT_IMM32), \
    _(jineI, 0, 1+BJIT_IMM32), \
    /* control flow, jump must come after conditionals!  */ \
    /* make sure there are even number of these (for xor1 below)  */ \
    _(jmp, 0, 0), \
    _(fret, 0, 1), \
    _(iret, 0, 1), \
    _(iretI, 0, BJIT_IMM32), /* opt-dce needs to know which one is last */ \
    /* */ \
    /* NOTE: THESE SHOULD MATCH THOSE STARTING FROM 'jilt' */ \
    /* SO MAKE SURE THE POSITIONS STAY RELATIVE */ \
    /* */ \
    /* (xor 1): signed integer comparisons */ \
    _(ilt, BJIT_CSE+1, 2), \
    _(ige, BJIT_CSE+1, 2), \
    _(igt, BJIT_CSE+1, 2), \
    _(ile, BJIT_CSE+1, 2), \
    /* (xor 1): unsigned integer comparisons */ \
    _(ult, BJIT_CSE+1, 2), \
    _(uge, BJIT_CSE+1, 2), \
    _(ugt, BJIT_CSE+1, 2), \
    _(ule, BJIT_CSE+1, 2), \
    /* (xor 1): integer equality (equal, not equal) */ \
    _(ieq, BJIT_CSE+1, 2), \
    _(ine, BJIT_CSE+1, 2), \
    /* (xor 1): floating point comparisons */ \
    _(flt, BJIT_CSE+1, 2), \
    _(fge, BJIT_CSE+1, 2), \
    _(fgt, BJIT_CSE+1, 2), \
    _(fle, BJIT_CSE+1, 2), \
    /* (xor 1): float equality (equal, not equal) */ \
    _(feq, BJIT_CSE+1, 2), \
    _(fne, BJIT_CSE+1, 2), \
    /* */ \
    /* NOTE: THESE SHOULD MATCH THOSE STARTING FROM 'jilt' */ \
    /* SO MAKE SURE THE POSITIONS STAY RELATIVE */ \
    /* */ \
    /* (xor 1): signed integer comparisons */ \
    _(iltI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(igeI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(igtI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ileI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* (xor 1): unsigned integer comparisons */ \
    _(ultI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ugeI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ugtI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(uleI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* (xor 1): integer equality (equal, not equal) */ \
    _(ieqI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ineI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* integer arithmetic */ \
    _(iadd, BJIT_CSE+1, 2), \
    _(isub, BJIT_CSE+1, 2), \
    _(ineg, BJIT_CSE+1, 1), \
    _(imul, BJIT_CSE+1, 2), \
    _(idiv, BJIT_CSE+1, 2), \
    _(imod, BJIT_CSE+1, 2), \
    /* unsigned integer arithmetic */ \
    _(udiv, BJIT_CSE+1, 2), \
    _(umod, BJIT_CSE+1, 2), \
    /* integer bitwise */ \
    _(inot, BJIT_CSE+1, 1), \
    _(iand, BJIT_CSE+1, 2), \
    _(ior,  BJIT_CSE+1, 2),  \
    _(ixor, BJIT_CSE+1, 2), \
    /* integer shifts */ \
    _(ishl, BJIT_CSE+1, 2), \
    _(ishr, BJIT_CSE+1, 2), \
    _(ushr, BJIT_CSE+1, 2), \
    /* integer arithmetic */ \
    _(iaddI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(isubI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(imulI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* integer bitwise */ \
    _(iandI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(iorI,  BJIT_CSE+1, 1+BJIT_IMM32),  \
    _(ixorI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* integer shifts */ \
    _(ishlI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ishrI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ushrI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* floating point arithmetic */ \
    _(fadd, BJIT_CSE+1, 2), \
    _(fsub, BJIT_CSE+1, 2), \
    _(fneg, BJIT_CSE+1, 1), \
    _(fmul, BJIT_CSE+1, 2), \
    _(fdiv, BJIT_CSE+1, 2), \
    /* type conversions: int -> float, float -> int */ \
    _(ci2f, BJIT_CSE+1, 1), \
    _(cf2i, BJIT_CSE+1, 1), \
    /* reinterpret bitcasts: int -> float, float -> int */ \
    _(bci2f, BJIT_CSE+1, 1), \
    _(bcf2i, BJIT_CSE+1, 1), \
    /* load constants, NOTE: fold breaks if we CSE these */ \
    _(lci, 1, BJIT_I64), \
    _(lcf, 1, BJIT_F64), \
    /* sign-extend values (cast to smaller type) */ \
    _(i8,  BJIT_CSE+1, 1), \
    _(i16, BJIT_CSE+1, 1), \
    _(i32, BJIT_CSE+1, 1), \
    /* unsigned variants (zero-extend) */ \
    _(u8,  BJIT_CSE+1, 1), \
    _(u16, BJIT_CSE+1, 1), \
    _(u32, BJIT_CSE+1, 1), \
    /* memory loads: load out <- [in0+offset] */ \
    /* integer variants: sign-extended */ \
    _(li8,  1, 1+BJIT_IMM32), \
    _(li16, 1, 1+BJIT_IMM32), \
    _(li32, 1, 1+BJIT_IMM32), \
    _(li64, 1, 1+BJIT_IMM32), \
    /* unsigned variants (zero-extend) */ \
    _(lu8,  1, 1+BJIT_IMM32), \
    _(lu16, 1, 1+BJIT_IMM32), \
    _(lu32, 1, 1+BJIT_IMM32), \
    /* memory stores: store [in0+offset] <- in1 */ \
    _(si8,  0, 2+BJIT_IMM32), \
    _(si16, 0, 2+BJIT_IMM32), \
    _(si32, 0, 2+BJIT_IMM32), \
    _(si64, 0, 2+BJIT_IMM32), \
    /* floating point load and store */ \
    _(lf64, 1, 1+BJIT_IMM32), \
    _(sf64, 0, 2+BJIT_IMM32), \
    /* procedure arguments */ \
    _(iarg, 1, 0), \
    _(farg, 1, 0), \
    /* Call arguments - right to left before call */ \
    _(ipass, 0, 1), \
    _(fpass, 0, 1), \
    /* Indirect calls: typed for return value */ \
    _(icallp, 1+BJIT_SIDEFX, 1), \
    _(fcallp, 1+BJIT_SIDEFX, 1), \
    /* pseudo-ops: polymorphic and don't participate in use-def */ \
    /* NOTE: we use "phi" as marker, so it must be first        */ \
    _(phi,    1, 0), \
    _(rename, 1, 1), \
    _(reload, 1, 1), \
    _(nop,    0, 0) /* removed by DCE */
