
#pragma once

// List of operations, order is significant
//  _(name, nOutputs, nInputs)

// output flags:
//
// if both SIDEFX and CSE are defined (eg. idiv) then we treat it as
// having SIDEFX for "safe" optimization, and CSE for "unsafe" optimizations
//

#define BJIT_SIDEFX 0x10    // never DCE, don't move loads across
#define BJIT_CSE    0x20    // can CSE
#define BJIT_NOMOVE 0x40    // must be in the beginning of a block
#define BJIT_ANYREG 0x80    // ignore 2-reg ISA (eg. can swap operands)

// input flags (lowest 2 bits are nInputs)
#define BJIT_MEM    0x08    // offset16 + memtag
#define BJIT_IMM32  0x10    // has imm32 operand
#define BJIT_I64    0x20    // has 64-bit integer constant
#define BJIT_F64    0x40    // has double constant
#define BJIT_F32    0x80    // has single constant

#define BJIT_OPS(_) \
    /* CAREFUL WITH THE ORDER HERE (see below also) */ \
    /* (xor 1): branch signed integer comparisons */ \
    /* (xor 2): operations swapped */ \
    _(jilt, 0, 2), \
    _(jige, 0, 2), \
    _(jigt, 0, 2), \
    _(jile, 0, 2), \
    /* (xor 1): branch unsigned integer comparisons */ \
    /* (xor 2): operations swapped */ \
    _(jult, 0, 2), \
    _(juge, 0, 2), \
    _(jugt, 0, 2), \
    _(jule, 0, 2), \
    /* (xor 1): branch integer equality (equal, not equal) */ \
    _(jieq, 0, 2), \
    _(jine, 0, 2), \
    /* (xor 1): branch double equality (equal, not equal) */ \
    _(jdeq, 0, 2), \
    _(jdne, 0, 2), \
    /* (xor 1): branch double comparisons */ \
    /* (xor 2): operations swapped */ \
    _(jdlt, 0, 2), \
    _(jdge, 0, 2), \
    _(jdgt, 0, 2), \
    _(jdle, 0, 2), \
    /* (xor 1): branch float comparisons */ \
    /* (xor 2): operations swapped */ \
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
    _(dret, 0, 1), \
    _(fret, 0, 1), \
    _(iret, 0, 1), \
    _(iretI, 0, BJIT_IMM32), /* opt-dce needs to know which one is last */ \
    _(tcallp, 0, 1), \
    _(tcalln, 0, BJIT_IMM32), \
    _(dummy_align, 0, 0), \
    /* */ \
    /* NOTE: THESE SHOULD MATCH THOSE STARTING FROM 'jilt' */ \
    /* SO MAKE SURE THE POSITIONS STAY RELATIVE */ \
    /* */ \
    /* (xor 1): signed integer comparisons */ \
    _(ilt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ige, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(igt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ile, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* (xor 1): unsigned integer comparisons */ \
    _(ult, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(uge, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ugt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ule, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* (xor 1): integer equality (equal, not equal) */ \
    _(ieq, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ine, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* (xor 1): double equality (equal, not equal) */ \
    _(deq, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(dne, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* (xor 1): double comparisons */ \
    _(dlt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(dge, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(dgt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(dle, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* (xor 1): floating point comparisons */ \
    _(flt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(fge, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(fgt, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(fle, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* (xor 1): float equality (equal, not equal) */ \
    _(feq, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(fne, BJIT_ANYREG+BJIT_CSE+1, 2), \
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
    _(iadd, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(isub, BJIT_CSE+1, 2), \
    _(ineg, BJIT_CSE+1, 1), \
    _(imul, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* division by zero is a "side-effect" */ \
    _(idiv, BJIT_SIDEFX+BJIT_CSE+1, 2), \
    _(imod, BJIT_SIDEFX+BJIT_CSE+1, 2), \
    /* unsigned integer arithmetic */ \
    _(udiv, BJIT_SIDEFX+BJIT_CSE+1, 2), \
    _(umod, BJIT_SIDEFX+BJIT_CSE+1, 2), \
    /* integer bitwise */ \
    _(inot, BJIT_CSE+1, 1), \
    _(iand, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ior,  BJIT_ANYREG+BJIT_CSE+1, 2),  \
    _(ixor, BJIT_ANYREG+BJIT_CSE+1, 2), \
    /* integer shifts */ \
    _(ishl, BJIT_CSE+1, 2), \
    _(ishr, BJIT_CSE+1, 2), \
    _(ushr, BJIT_CSE+1, 2), \
    /* integer arithmetic */ \
    _(iaddI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(isubI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(imulI, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_IMM32), \
    /* integer bitwise */ \
    _(iandI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(iorI,  BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ixorI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* integer shifts */ \
    _(ishlI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ishrI, BJIT_CSE+1, 1+BJIT_IMM32), \
    _(ushrI, BJIT_CSE+1, 1+BJIT_IMM32), \
    /* double arithmetic */ \
    _(dadd, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(dsub, BJIT_CSE+1, 2), \
    _(dneg, BJIT_CSE+1, 1), \
    _(dabs, BJIT_CSE+1, 1), \
    _(dmul, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(ddiv, BJIT_CSE+1, 2), \
    /* float arithmetic */ \
    _(fadd, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(fsub, BJIT_CSE+1, 2), \
    _(fneg, BJIT_CSE+1, 1), \
    _(fabs, BJIT_CSE+1, 1), \
    _(fmul, BJIT_ANYREG+BJIT_CSE+1, 2), \
    _(fdiv, BJIT_CSE+1, 2), \
    /* type conversions */ \
    _(ci2d, BJIT_CSE+1, 1), \
    _(cd2i, BJIT_CSE+1, 1), \
    _(ci2f, BJIT_CSE+1, 1), \
    _(cf2i, BJIT_CSE+1, 1), \
    _(cf2d, BJIT_CSE+1, 1), \
    _(cd2f, BJIT_CSE+1, 1), \
    /* reinterpret bitcasts */ \
    _(bci2d, BJIT_CSE+1, 1), \
    _(bcd2i, BJIT_CSE+1, 1), \
    _(bci2f, BJIT_CSE+1, 1), \
    _(bcf2i, BJIT_CSE+1, 1), \
    /* load constants */ \
    _(lci, BJIT_CSE+1, BJIT_I64), \
    _(lcf, BJIT_CSE+1, BJIT_F32), \
    _(lcd, BJIT_CSE+1, BJIT_F64), \
    /* load near proc address */ \
    _(lnp, BJIT_CSE+1, BJIT_IMM32), \
    /* sign-extend values (cast to smaller type) */ \
    _(i8,  BJIT_CSE+1, 1), \
    _(i16, BJIT_CSE+1, 1), \
    _(i32, BJIT_CSE+1, 1), \
    /* unsigned variants (zero-extend) */ \
    _(u8,  BJIT_CSE+1, 1), \
    _(u16, BJIT_CSE+1, 1), \
    _(u32, BJIT_CSE+1, 1), \
    /* memory loads: load out <- [in0+offset] */ \
    /* ANYREG 'cos typically explicit output reg */ \
    /* integer variants: sign-extended */ \
    /* treat as potentially causing side-effects */ \
    _(li8,  BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    _(li16, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    _(li32, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    _(li64, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    /* unsigned variants (zero-extend) */ \
    _(lu8,  BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    _(lu16, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    _(lu32, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    /* float */ \
    _(lf32, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    _(lf64, BJIT_ANYREG+BJIT_CSE+1, 1+BJIT_MEM), \
    /* two reg versions - NOTE: must be in same order! */ \
    _(l2i8,  BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    _(l2i16, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    _(l2i32, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    _(l2i64, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    /* unsigned variants (zero-extend) */ \
    _(l2u8,  BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    _(l2u16, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    _(l2u32, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    /* float */ \
    _(l2f32, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    _(l2f64, BJIT_ANYREG+BJIT_CSE+1, 2+BJIT_MEM), \
    /* memory stores: store [in0+offset] <- in1 */ \
    _(si8,  0, 2+BJIT_MEM), \
    _(si16, 0, 2+BJIT_MEM), \
    _(si32, 0, 2+BJIT_MEM), \
    _(si64, 0, 2+BJIT_MEM), \
    /* floating point */ \
    _(sf32, 0, 2+BJIT_MEM), \
    _(sf64, 0, 2+BJIT_MEM), \
    /* two reg versions - NOTE: must be in same order!  */ \
    _(s2i8,  0, 3+BJIT_MEM), \
    _(s2i16, 0, 3+BJIT_MEM), \
    _(s2i32, 0, 3+BJIT_MEM), \
    _(s2i64, 0, 3+BJIT_MEM), \
    /* floating point */ \
    _(s2f32, 0, 3+BJIT_MEM), \
    _(s2f64, 0, 3+BJIT_MEM), \
    /* procedure arguments */ \
    _(iarg, 1+BJIT_NOMOVE, 0), \
    _(farg, 1+BJIT_NOMOVE, 0), \
    _(darg, 1+BJIT_NOMOVE, 0), \
    /* Call arguments - right to left before call */ \
    _(ipass, 0, 1), \
    _(fpass, 0, 1), \
    _(dpass, 0, 1), \
    /* Indirect calls: typed for return value */ \
    _(icallp, 1+BJIT_SIDEFX, 1), \
    _(fcallp, 1+BJIT_SIDEFX, 1), \
    _(dcallp, 1+BJIT_SIDEFX, 1), \
    /* Module local "near" calls, relocated */ \
    _(icalln, 1+BJIT_SIDEFX, BJIT_IMM32), \
    _(fcalln, 1+BJIT_SIDEFX, BJIT_IMM32), \
    _(dcalln, 1+BJIT_SIDEFX, BJIT_IMM32), \
    /* this is user-requested allocation with reg = stack pointer */ \
    _(alloc,  1+BJIT_SIDEFX+BJIT_NOMOVE, BJIT_IMM32), \
    /* this keeps the compiler from moving loads across */ \
    _(fence, BJIT_SIDEFX, 0), \
    /* pseudo-ops: polymorphic */ \
    _(phi,    1+BJIT_NOMOVE, 0), \
    _(rename, 1, 1), \
    _(reload, 1, 1), \
    _(nop,    0, 0) /* removed by DCE */
