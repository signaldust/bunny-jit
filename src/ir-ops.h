
#pragma once

// List of operations, order is significant
//  _(name, nOutputs, nInputs)

// output flags
#define BJIT_SIDEFX  0x10    // never DCE

// input flags
#define BJIT_IMM32   0x10    // has imm32 operand
#define BJIT_I64     0x20    // has 64-bit integer constant
#define BJIT_F64     0x40    // has double constant

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
    _(ilt, 1, 2), \
    _(ige, 1, 2), \
    _(igt, 1, 2), \
    _(ile, 1, 2), \
    /* (xor 1): unsigned integer comparisons */ \
    _(ult, 1, 2), \
    _(uge, 1, 2), \
    _(ugt, 1, 2), \
    _(ule, 1, 2), \
    /* (xor 1): integer equality (equal, not equal) */ \
    _(ieq, 1, 2), \
    _(ine, 1, 2), \
    /* (xor 1): floating point comparisons */ \
    _(flt, 1, 2), \
    _(fge, 1, 2), \
    _(fgt, 1, 2), \
    _(fle, 1, 2), \
    /* (xor 1): float equality (equal, not equal) */ \
    _(feq, 1, 2), \
    _(fne, 1, 2), \
    /* */ \
    /* NOTE: THESE SHOULD MATCH THOSE STARTING FROM 'jilt' */ \
    /* SO MAKE SURE THE POSITIONS STAY RELATIVE */ \
    /* */ \
    /* (xor 1): signed integer comparisons */ \
    _(iltI, 1, 1+BJIT_IMM32), \
    _(igeI, 1, 1+BJIT_IMM32), \
    _(igtI, 1, 1+BJIT_IMM32), \
    _(ileI, 1, 1+BJIT_IMM32), \
    /* (xor 1): unsigned integer comparisons */ \
    _(ultI, 1, 1+BJIT_IMM32), \
    _(ugeI, 1, 1+BJIT_IMM32), \
    _(ugtI, 1, 1+BJIT_IMM32), \
    _(uleI, 1, 1+BJIT_IMM32), \
    /* (xor 1): integer equality (equal, not equal) */ \
    _(ieqI, 1, 1+BJIT_IMM32), \
    _(ineI, 1, 1+BJIT_IMM32), \
    /* integer arithmetic */ \
    _(iadd, 1, 2), \
    _(isub, 1, 2), \
    _(ineg, 1, 1), \
    _(imul, 1, 2), \
    _(idiv, 1, 2), \
    _(imod, 1, 2), \
    /* unsigned integer arithmetic */ \
    _(udiv, 1, 2), \
    _(umod, 1, 2), \
    /* integer bitwise */ \
    _(inot, 1, 1), \
    _(iand, 1, 2), \
    _(ior, 1, 2),  \
    _(ixor, 1, 2), \
    /* integer shifts */ \
    _(ishl, 1, 2), \
    _(ishr, 1, 2), \
    _(ushr, 1, 2), \
    /* integer arithmetic */ \
    _(iaddI, 1, 1+BJIT_IMM32), \
    _(isubI, 1, 1+BJIT_IMM32), \
    _(imulI, 1, 1+BJIT_IMM32), \
    /* integer bitwise */ \
    _(iandI, 1, 1+BJIT_IMM32), \
    _(iorI,  1, 1+BJIT_IMM32),  \
    _(ixorI, 1, 1+BJIT_IMM32), \
    /* integer shifts */ \
    _(ishlI, 1, 1+BJIT_IMM32), \
    _(ishrI, 1, 1+BJIT_IMM32), \
    _(ushrI, 1, 1+BJIT_IMM32), \
    /* floating point arithmetic */ \
    _(fadd, 1, 2), \
    _(fsub, 1, 2), \
    _(fneg, 1, 1), \
    _(fmul, 1, 2), \
    _(fdiv, 1, 2), \
    /* load constants */ \
    _(lci, 1, BJIT_I64), \
    _(lcf, 1, BJIT_F64), \
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
    /* type conversions: int -> float, uint -> float, float -> int */ \
    _(ci2f, 1, 1), \
    _(cf2i, 1, 1), \
    /* procedure parameters */ \
    _(iparam, 1, 0), \
    _(fparam, 1, 0), \
    /* CALL ARGUMENTS - left to right before call */ \
    _(iarg, 0, 1), \
    _(farg, 0, 1), \
    /* CALLS: typed for return value */ \
    _(icall, 1+BJIT_SIDEFX, 1), \
    _(fcall, 1+BJIT_SIDEFX, 1), \
    /* pseudo-ops: polymorphic and don't participate in use-def */ \
    /* NOTE: we use "phi" as marker, so it must be first        */ \
    _(phi,    1, 0), \
    _(rename, 1, 1), \
    _(reload, 1, 1), \
    _(nop,    0, 0) /* removed by DCE */
