
#include "bjit.h"

#include "hash.h"

using namespace bjit;

#define PRINTLN //BJIT_LOG("\n fold %d (op %04x)", __LINE__, getOpIndex(op));

// match op
#define I(x) (op.opcode == x)

// match operands
#define I0(x) (op.nInputs() >= 1 && ops[op.in[0]].opcode == x)
#define I1(x) (op.nInputs() >= 2 && ops[op.in[1]].opcode == x)
#define I2(x) (op.nInputs() >= 3 && ops[op.in[2]].opcode == x)

// check for constants
#define C0 (op.nInputs() >= 1 && (I0(ops::lci) || I0(ops::lcf) || I0(ops::lcd)))
#define C1 (op.nInputs() >= 2 && (I1(ops::lci) || I1(ops::lcf) || I1(ops::lcd)))
#define C2 (op.nInputs() >= 3 && (I2(ops::lci) || I2(ops::lcf) || I2(ops::lcd)))

// fetch operands
#define N0 ops[op.in[0]]
#define N1 ops[op.in[1]]
#define N2 ops[op.in[2]]

/*

We try to only do folding here that is either directly profitable, or
likely to enable further optimizations down the line.

We further try to avoid any folding that would be a pessimisation if
such the speculative further optimizations fail to materialize.

*/
bool Proc::opt_fold(bool unsafeOpt)
{
    //debug();
    rebuild_dom();
    
    BJIT_ASSERT(live.size());   // should have at least one DCE pass done

    impl::Rename rename;

    int iter = 0;

    // this should match the sort order in reassoc
    // otherwise we run into infinite loops
    auto shouldSwap = [&](uint16_t op1, uint16_t op2) -> bool
    {
        auto ndom1 = blocks[ops[op1].block].dom.size();
        auto ndom2 = blocks[ops[op2].block].dom.size();

        if(ndom1 < ndom2) return true;
        if(ndom1 > ndom2) return false;

        // if ndom is the same, then block should be same as well
        BJIT_ASSERT_MORE(ops[op1].block == ops[op2].block);

        return ops[op1].pos < ops[op2].pos;
    };

    bool progress = true, anyProgress = false;
    while(progress)
    {
        ++iter;
        progress = false;
        for(auto b : live)
        {
            for(auto & opIndex : blocks[b].code)
            {
                if(opIndex == noVal) continue;
                
                auto & op = ops[opIndex];

                if(op.opcode == ops::nop) { continue; }
                
                rename(op);
    
                // rename phis
                if(op.opcode <= ops::jmp)
                {
                    for(auto & s : blocks[op.label[0]].alts)
                    for(auto & r : rename.map)
                    {
                        if(s.val == r.src) s.val = r.dst;
                    }
                }
    
                if(op.opcode < ops::jmp)
                {
                    for(auto & s : blocks[op.label[1]].alts)
                    for(auto & r : rename.map)
                    {
                        if(s.val == r.src) s.val = r.dst;
                    }
                }

                // catch phi-rewriting problems early
                BJIT_ASSERT(op.nInputs() < 1
                || ops[op.in[0]].opcode != op.opcode || &N0 != &op);
                BJIT_ASSERT(op.nInputs() < 2
                || ops[op.in[1]].opcode != op.opcode || &N1 != &op);

                // for associative operations and comparisons where neither
                // operand is constant, sort operands to improve CSE
                //
                if(!C0 && !C1)
                switch(op.opcode)
                {
                    case ops::ieq: case ops::ine:
                    case ops::deq: case ops::dne:
                    case ops::iadd: case ops::imul:
                    case ops::fadd: case ops::fmul:
                    case ops::dadd: case ops::dmul:
                    case ops::iand: case ops::ior: case ops::ixor:
                        {
                            if(shouldSwap(op.in[0], op.in[1]))
                            {
                                std::swap(op.in[0], op.in[1]);
                                PRINTLN;
                            }
                        }
                        break;
                        
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                    case ops::dlt: case ops::dge: case ops::dgt: case ops::dle:
                        // (xor 2) relative to ilt swaps operands
                        {
                            if(shouldSwap(op.in[0], op.in[1]))
                            {
                                op.opcode = ops::ilt + (2^(op.opcode-ops::ilt));
                                std::swap(op.in[0], op.in[1]);
                                PRINTLN;
                            }
                        }
                        break;

                    default: break;
                }
    
                // swap constant to second operand for associative ops
                // also do this for comparisons, negating the condition
                if(C0 && !C1)
                switch(op.opcode)
                {
                    case ops::jieq: case ops::jine:
                    case ops::ieq: case ops::ine:
                    case ops::jdeq: case ops::jdne:
                    case ops::deq: case ops::dne:
                    case ops::iadd: case ops::imul:
                    case ops::fadd: case ops::fmul:
                    case ops::dadd: case ops::dmul:
                    case ops::iand: case ops::ior: case ops::ixor:
                        std::swap(op.in[0], op.in[1]); PRINTLN;
                        break;

                    // rewrite 0 - a = -a
                    case ops::isub:
                        if(!ops[op.in[1]].i64)
                        {
                            op.opcode = ops::ineg;
                            op.in[0] = op.in[1];
                            op.in[1] = noVal;
                            progress = true; PRINTLN;
                        }
                        break;
                    case ops::jilt: case ops::jige: case ops::jigt: case ops::jile:
                    case ops::jult: case ops::juge: case ops::jugt: case ops::jule:
                    case ops::jdlt: case ops::jdge: case ops::jdgt: case ops::jdle:
                        op.opcode = 2 ^ op.opcode;
                        std::swap(op.in[0], op.in[1]);
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                    case ops::dlt: case ops::dge: case ops::dgt: case ops::dle:
                        op.opcode = ops::ilt + (2^(op.opcode-ops::ilt));
                        std::swap(op.in[0], op.in[1]);
                        progress = true; PRINTLN;
                        break;
                }

                // simplify ieqI/ineI into inverted test when possible
                // don't bother if there are other uses
                if((I(ops::ieqI) || I(ops::ineI)) && !op.imm32 && N0.nUse == 1)
                {
                    switch(N0.opcode)
                    {
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                    case ops::dlt: case ops::dge: case ops::dgt: case ops::dle:
    
                    case ops::iltI: case ops::igeI: case ops::igtI: case ops::ileI:
                    case ops::ultI: case ops::ugeI: case ops::ugtI: case ops::uleI:
                        {
                            bool negate = (op.opcode == ops::ieqI);
                            op.opcode = N0.opcode;
                            if(N0.nInputs() == 2) op.in[1] = N0.in[1];
                            if(N0.hasImm32()) op.imm32 = N0.imm32;
                            op.in[0] = N0.in[0];
                            if(negate) op.opcode ^= 1;
                            progress = true; PRINTLN;
                        }
                    }
                }

                if(I1(ops::lci) && N1.i64 == (int32_t) N1.i64)
                switch(op.opcode)
                {
                    case ops::jilt: case ops::jige:
                    case ops::jigt: case ops::jile:
                    case ops::jieq: case ops::jine:
                    case ops::jult: case ops::juge:
                    case ops::jugt: case ops::jule:
                        op.opcode += ops::jiltI - ops::jilt;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    
                    case ops::ilt: case ops::ige:
                    case ops::igt: case ops::ile:
                    case ops::ieq: case ops::ine:
                    case ops::ult: case ops::uge:
                    case ops::ugt: case ops::ule:
                        op.opcode += ops::iltI - ops::ilt;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;

                    case ops::iadd:
                        op.opcode = ops::iaddI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::isub:
                        op.opcode = ops::isubI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::imul:
                        op.opcode = ops::imulI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::iand:
                        op.opcode = ops::iandI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::ior:
                        op.opcode = ops::iorI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::ixor:
                        op.opcode = ops::ixorI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::ishl:
                        op.opcode = ops::ishlI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::ishr:
                        op.opcode = ops::ishrI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::ushr:
                        op.opcode = ops::ushrI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                }
                
                // fold conditions into jumps, but with floating point
                // only when unsafeOpt, since condition negations disregard NaNs
                if((I(ops::jz) || I(ops::jnz)) && N0.nUse == 1
                && (N0.opcode >= ops::ilt
                && N0.opcode - ops::ilt <= (unsafeOpt ? ops::jine : ops::jfne)))
                {
                    int negate = (op.opcode == ops::jz) ? 1 : 0;
                    op.opcode = negate ^ (N0.opcode + ops::jilt - ops::ilt);
                    op.in[1] = N0.in[1];
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                }
                
                // fold conditions into jumps
                if((I(ops::jz) || I(ops::jnz)) && N0.nUse == 1
                && (N0.opcode >= ops::iltI && N0.opcode - ops::iltI <= ops::jine))
                {
                    int negate = (op.opcode == ops::jz) ? 1 : 0;
                    op.opcode = negate ^ (N0.opcode + ops::jiltI - ops::iltI);
                    op.imm32 = N0.imm32;
                    op.in[0] = N0.in[0];
                    op.in[1] = noVal;
                    progress = true; PRINTLN;
                }
    
                // simplify jieqI/jineI into jz/jnz when possible
                if((I(ops::jieqI) || I(ops::jineI)) && !op.imm32)
                {
                    op.opcode = I(ops::jieqI) ? ops::jz : ops::jnz;
                    op.in[0] = noVal;
                    progress = true; PRINTLN;
                }

                // -(-a) = a
                if((I(ops::ineg) && I0(ops::ineg))
                || (I(ops::fneg) && I0(ops::fneg))
                || (I(ops::dneg) && I0(ops::dneg)))
                {
                    rename.add(opIndex, N0.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;
                }
                
                // a + 0, a - 0, a * 1 -> a
                if((I(ops::iaddI) && !op.imm32)
                || (I(ops::isubI) && !op.imm32)
                || (I(ops::imulI) && 1 == op.imm32))
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;
                }

                if((I(ops::fadd) && I1(ops::lcf) && N1.f32 == 0.f)
                || (I(ops::fsub) && I1(ops::lcf) && N1.f32 == 0.f)
                || (I(ops::fmul) && I1(ops::lcf) && N1.f32 == 1.f))
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }
                
                if((I(ops::dadd) && I1(ops::lcd) && N1.f64 == 0.)
                || (I(ops::dsub) && I1(ops::lcd) && N1.f64 == 0.)
                || (I(ops::dmul) && I1(ops::lcd) && N1.f64 == 1.))
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }

                // a * 0 -> 0
                if(I(ops::imulI) && !op.imm32)
                {
                    op.opcode = ops::lci;
                    op.i64 = 0;
                    progress = true; PRINTLN;
                }

                // a * -1 -> -a
                if(I(ops::imulI) && -1 == op.imm32)
                {
                    op.opcode = ops::ineg; progress = true; PRINTLN;
                }
                if(I(ops::fmul) && I1(ops::lcf) && N1.f32 == -1.f)
                {
                    op.opcode = ops::fneg; op.in[1] = noVal; progress = true; PRINTLN;
                }
                
                if(I(ops::dmul) && I1(ops::lcd) && N1.f64 == -1.)
                {
                    op.opcode = ops::dneg; op.in[1] = noVal; progress = true; PRINTLN;
                }
                
                // a + (-b) = a-b
                if(I(ops::iadd) && I1(ops::ineg))
                {
                    op.opcode = ops::isub;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
                if(I(ops::fadd) && I1(ops::fneg))
                {
                    op.opcode = ops::fsub;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
                if(I(ops::dadd) && I1(ops::dneg))
                {
                    op.opcode = ops::dsub;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
                
    
                // a - (-b) = a + b
                if(I(ops::isub) && I1(ops::ineg))
                {
                    op.opcode = ops::iadd;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
                if(I(ops::fsub) && I1(ops::fneg))
                {
                    op.opcode = ops::fadd;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
                if(I(ops::dsub) && I1(ops::dneg))
                {
                    op.opcode = ops::dadd;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
    
                // -a + b = b - a
                if(I(ops::iadd) && I0(ops::ineg))
                {
                    op.opcode = ops::isub;
                    op.in[0] = N0.in[0];
                    std::swap(op.in[0], op.in[1]);
                    progress = true; PRINTLN;
                }
                if(I(ops::fadd) && I0(ops::fneg))
                {
                    op.opcode = ops::fsub;
                    op.in[0] = N0.in[0];
                    std::swap(op.in[0], op.in[1]);
                    progress = true; PRINTLN;
                }
                if(I(ops::dadd) && I0(ops::dneg))
                {
                    op.opcode = ops::dsub;
                    op.in[0] = N0.in[0];
                    std::swap(op.in[0], op.in[1]);
                    progress = true; PRINTLN;
                }

                // a+a = a<<1
                if(I(ops::iadd) && op.in[0] == op.in[1])
                {
                    op.opcode = ops::ishlI;
                    op.in[1] = noVal;
                    op.imm32 = 1;
                    progress = true; PRINTLN;
                }

                // a&a = a,  a|a = a
                if((I(ops::iand) || I(ops::ior))
                && op.in[0] == op.in[1])
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }

                // a&(a|b) = a
                if((I(ops::iand) && I0(ops::ior))
                && (op.in[1] == N0.in[0] || op.in[1] == N0.in[1]))
                {
                    rename.add(opIndex, op.in[1]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }
                if((I(ops::iand) && I1(ops::ior))
                && (op.in[0] == N1.in[0] || op.in[0] == N1.in[1]))
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }
                // a|(a&b) = a
                if((I(ops::ior) && I0(ops::iand))
                && (op.in[1] == N0.in[0] || op.in[1] == N0.in[1]))
                {
                    rename.add(opIndex, op.in[1]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }
                if((I(ops::ior) && I1(ops::iand))
                && (op.in[0] == N1.in[0] || op.in[0] == N1.in[1]))
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;                    
                }

                // a&~a = 0
                if((I(ops::iand) && I0(ops::inot))
                && (op.in[1] == N0.in[0]))
                {
                    op.opcode = ops::lci;
                    op.i64 = 0;
                    progress = true; PRINTLN;
                    continue;                    
                }
                if((I(ops::iand) && I1(ops::inot))
                && (op.in[0] == N1.in[0]))
                {
                    op.opcode = ops::lci;
                    op.i64 = 0;
                    progress = true; PRINTLN;
                    continue;                    
                }
                

                // a-a = 0,  a^a = 0
                if((I(ops::isub) || I(ops::ixor))
                && op.in[0] == op.in[1])
                {
                    op.opcode = ops::lci;
                    op.i64 = 0;
                    progress = true; PRINTLN;
                }
                if(I(ops::fsub) && op.in[0] == op.in[1])
                {
                    op.opcode = ops::lcf;
                    op.i64 = 0;
                    progress = true; PRINTLN;
                }
                if(I(ops::dsub) && op.in[0] == op.in[1])
                {
                    op.opcode = ops::lcd;
                    op.i64 = 0;
                    progress = true; PRINTLN;
                }
    
                // addition immediate merges
                if(I(ops::iaddI) && I0(ops::iaddI))
                {
                    int64_t v = (int64_t) op.imm32 + (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true; PRINTLN;
                    }
                }
                if(I(ops::isubI) && I0(ops::isubI))
                {
                    int64_t v = (int64_t) op.imm32 + (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true; PRINTLN;
                    }
                }
                if(I(ops::iaddI) && I0(ops::isubI))
                {
                    int64_t v = (int64_t) op.imm32 - (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true; PRINTLN;
                    }
                }
                if(I(ops::isubI) && I0(ops::iaddI))
                {
                    int64_t v = (int64_t) op.imm32 - (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true; PRINTLN;
                    }
                }

                // merge shift into multiply
                if(I(ops::imulI) && I0(ops::ishlI))
                {
                    int64_t v = (int64_t) op.imm32 * (((int64_t)1) << N0.imm32);
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true; PRINTLN;
                    }
                }
    
                // multiplication immediate merges
                if(I(ops::imulI) && I0(ops::imulI))
                {
                    int64_t v = (int64_t) op.imm32 * (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true; PRINTLN;
                    }
                }

                // can we replace multiply with shift?
                if(I(ops::imulI) && op.imm32
                && !(int64_t(op.imm32) & (int64_t(op.imm32) - 1)))
                {
                    uint32_t b = op.imm32;
                    int shift = 0; while(b >>= 1) ++shift;
                    op.opcode = ops::ishlI;
                    op.imm32 = shift;
                    progress = true; PRINTLN;
                }
                
                // can we replace division with shift?
                // unsigned only for now
                if(I(ops::udiv) && I1(ops::lci) && !(N1.u64 & (N1.u64 - 1)))
                {
                    uint32_t b = N1.imm32;
                    int shift = 0; while(b >>= 1) ++shift;
                    op.opcode = ops::ushrI;
                    op.imm32 = shift;
                    op.in[1] = noVal;
                    progress = true; PRINTLN;
                }

                // can we replace modulo with mask? max 32-bits for now
                if(I(ops::umod) && I1(ops::lci)
                && !(N1.u64 & (N1.u64 - 1)) && (N1.u64 <= (((uint64_t)1)<<32)))
                {
                    uint32_t b = N1.imm32;
                    int shift = 0; while(b >>= 1) ++shift;
                    op.opcode = ops::iandI;
                    op.imm32 = ((((uint64_t)1)<<shift)-1);
                    op.in[1] = noVal;
                    progress = true; PRINTLN;
                }

                // (a<<n)<<m = (a<<(n+m)), happens from adds/muls
                if(I(ops::ishlI) && I0(ops::ishlI))
                {
                    int shift = (op.imm32%64) + (N0.imm32%64);
                    // we need this rule, because (mod 64)
                    if(shift >= 64)
                    {
                        op.opcode = ops::lci;
                        op.i64 = 0;
                    }
                    else
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = shift;
                    }
                    progress = true; PRINTLN;
                }

                // (a>>n)>>m = (a>>(n+m)) - signed
                if(I(ops::ishrI) && I0(ops::ishrI))
                {
                    int shift = (op.imm32%64) + (N0.imm32%64);
                    
                    // we need this rule, because (mod 64)
                    // for signed we still need to keep the signbit
                    // so we'll simply cap the shift at maximum
                    if(shift >= 64) shift = 63;
                    
                    op.in[0] = N0.in[0];
                    op.imm32 = shift;
                    progress = true; PRINTLN;
                }
                
                // (a>>n)>>m = (a>>(n+m)) - unsigned
                if(I(ops::ushrI) && I0(ops::ushrI))
                {
                    int shift = (op.imm32%64) + (N0.imm32%64);
                    // we need this rule, because (mod 64)
                    if(shift >= 64)
                    {
                        op.opcode = ops::lci;
                        op.i64 = 0;
                    }
                    else
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = shift;
                    }
                    progress = true; PRINTLN;
                }
                

                // reassoc (a+b)<<c as (a<<c)+(b<<c) when b,c are constants
                // this way [n+1], [n+2] etc can CSE the shift
                // we'll rewrite in place, so only do it when (a+b) is not used
                if(I(ops::ishlI) && I0(ops::iaddI) && N0.nUse == 1)
                {
                    int shift = (op.imm32%64);
                    int64_t imm = int64_t(N0.imm32) << shift;
                    if(imm == (int32_t) imm)
                    {
                        N0.opcode = ops::ishlI;
                        N0.imm32 = shift;
                        
                        op.opcode = ops::iaddI;
                        op.imm32 = imm;
                        progress = true; PRINTLN;
                    }
                }

                // same as above, but (a-b)<<c -> (a<<c)-(b<<c)
                if(I(ops::ishlI) && I0(ops::isubI) && N0.nUse == 1)
                {
                    int shift = (op.imm32%64);
                    int64_t imm = int64_t(N0.imm32) << shift;
                    if(imm == (int32_t) imm)
                    {
                        N0.opcode = ops::ishlI;
                        N0.imm32 = shift;
                        
                        op.opcode = ops::isubI;
                        op.imm32 = imm;
                        progress = true; PRINTLN;
                    }
                }

                // convert any isubI to iaddI when possible
                if(I(ops::isubI) && op.imm32 != (int32_t)(1<<31))
                {
                    op.imm32 = -op.imm32;
                    op.opcode = ops::iaddI;
                }
                
                // shift by zero is always a NOP
                if((I(ops::ishlI) || I(ops::ishrI) || I(ops::ushrI))
                && !(op.imm32 % 64))
                {
                    rename.add(opIndex, op.in[0]);
                    op.makeNOP();
                    progress = true; PRINTLN;
                    continue;
                }
                
                // FIXME: add NAND and NOR to simplify
                // some constructs involving bitwise NOT?
    
                // ~(~a) = a
                if(I(ops::inot) && I0(ops::inot))
                {
                    rename.add(opIndex, N0.in[0]);
                    op.makeNOP();
                    progress = true; PRINTLN;
                    continue;
                }
                
                // bit-AND merges : (a&b)&c = a&(b&c)
                if(I(ops::iandI) && I0(ops::iandI))
                {
                    op.imm32 &= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                }
    
                // bit-OR merges : (a|b)|c = a|(b|c)
                if(I(ops::iorI) && I0(ops::iorI))
                {
                    op.imm32 |= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                }

                // bit-XOR merges : (a^b)^c = a^(b^c)
                if(I(ops::ixorI) && I0(ops::ixorI))
                {
                    op.imm32 ^= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                    debugOp(opIndex);
                }
    
                if(I(ops::ixorI) && !~op.imm32)
                {
                    op.opcode = ops::inot;
                    progress = true; PRINTLN;
                }
    
                if(I(ops::ixorI) && !op.imm32)
                {
                    rename.add(opIndex, op.in[0]);
                    progress = true; PRINTLN;
                    op.makeNOP();
                    continue;
                }

                // merge iaddI into loads?
                if(false && op.hasMem() && op.hasOutput()
                && I0(ops::iaddI) && N0.nUse == 1
                && (op.off16 + N0.imm32) == uint16_t(op.off16 + N0.imm32))
                {
                    op.off16 += N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                }
                if(false && op.hasMem() && op.hasOutput()
                && I0(ops::isubI) && N0.nUse == 1
                && (op.off16 - N0.imm32) == uint16_t(op.off16 - N0.imm32))
                {
                    op.off16 -= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                }

                // second operand or store?
                if(false && op.hasMem() && op.nInputs() >= 2
                && I1(ops::iaddI) && N1.nUse == 1
                && (op.off16 + N1.imm32) == uint16_t(op.off16 + N1.imm32))
                {
                    op.off16 += N1.imm32;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }
                if(false && op.hasMem() && op.nInputs() >= 2
                && I1(ops::isubI) && N1.nUse == 1
                && (op.off16 - N1.imm32) == uint16_t(op.off16 - N1.imm32))
                {
                    op.off16 -= N1.imm32;
                    op.in[1] = N1.in[0];
                    progress = true; PRINTLN;
                }

                // third operand on 2-reg store?
                if(false && op.hasMem() && op.nInputs() >= 3
                && I2(ops::iaddI) && N2.nUse == 1
                && (op.off16 + N2.imm32) == uint16_t(op.off16 + N2.imm32))
                {
                    debugOp(opIndex);
                    op.off16 += N2.imm32;
                    op.in[2] = N2.in[0];
                    debugOp(opIndex);
                    progress = true; PRINTLN;
                }
                if(false && op.hasMem() && op.nInputs() >= 3
                && I1(ops::isubI) && N2.nUse == 1
                && (op.off16 - N2.imm32) == uint16_t(op.off16 - N2.imm32))
                {
                    op.off16 -= N2.imm32;
                    op.in[1] = N2.in[0];
                    progress = true; PRINTLN;
                }

                // merge iadd into 1-reg loads?
                if(op.hasMem() && op.hasOutput() && op.nInputs() == 1
                && I0(ops::iadd) && N0.nUse == 1)
                {
                    op.opcode += ops::l2i8 - ops::li8;

                    op.in[1] = N0.in[1];
                    op.in[0] = N0.in[0];
                    progress = true; PRINTLN;
                }
                
                // merge iadd into 1-reg stores?
                if(op.hasMem() && !op.hasOutput() && op.nInputs() == 2
                && I1(ops::iadd) && N1.nUse == 1)
                {
                    debugOp(op.in[1]);
                    debugOp(opIndex);
                    op.opcode += ops::s2i8 - ops::si8;

                    op.in[2] = N1.in[1];
                    op.in[1] = N1.in[0];
                    debugOp(opIndex);
                    progress = true; PRINTLN;
                }

                if(C0)
                switch(op.opcode)
                {
                    case ops::iret:
                        if(N0.i64 == (int32_t) N0.i64)
                        {
                            op.opcode = ops::iretI;
                            op.imm32 = N0.i64;
                            op.in[0] = noVal;
                            progress = true; PRINTLN;
                        }
                        break;
                    case ops::jz:
                        op.opcode = ops::jmp;
                        if(N0.i64) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::jnz:
                        op.opcode = ops::jmp;
                        if(!N0.i64) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
    
                    case ops::jiltI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 < (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jigeI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 >= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jigtI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 > (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jileI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 <= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
    
                    case ops::jieqI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 == (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jineI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 != (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::jultI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 < (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jugeI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 >= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jugtI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 > (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::juleI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 <= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::iltI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 < op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::igeI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 >= op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::igtI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 > op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::ileI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 <= op.imm32);
                        progress = true; PRINTLN;
                        break;
    
                    case ops::ieqI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 == op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::ineI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 != op.imm32);
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::ultI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 < (int64_t)op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::ugeI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 >= (int64_t)op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::ugtI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 > (int64_t)op.imm32);
                        progress = true; PRINTLN;
                        break;
                    case ops::uleI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 <= (int64_t)op.imm32);
                        progress = true; PRINTLN;
                        break;
                        
                    // arithmetic
                    case ops::iaddI:
                        op.i64 = N0.i64 + (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::isubI:
                        op.i64 = N0.i64 - (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::ineg:
                        op.i64 = -N0.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::imulI:
                        op.i64 = N0.i64 * (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                         
                    // bitwise
                    case ops::inot:
                        op.i64 = ~N0.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    
                    case ops::iandI:
                        op.i64 = N0.i64 & (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::iorI:
                        op.i64 = N0.i64 | (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::ixorI:
                        op.i64 = N0.i64 ^ (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
    
                    // bitshifts
                    case ops::ishlI:
                        op.i64 = N0.i64 << op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::ishrI:
                        // this relies on compiler giving signed shift
                        op.i64 = N0.i64 >> op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
    
                    case ops::ushrI:
                        // this relies on compiler giving unsigned shift
                        op.u64 = N0.u64 >> op.imm32;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;

                    case ops::fneg:
                        op.f32 = -N0.f32;
                        op.opcode = ops::lcf;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::dneg:
                        op.f64 = -N0.f64;
                        op.opcode = ops::lcd;
                        progress = true; PRINTLN;
                        break;
                }
    
                // This has some redundancy with immediate versions..
                // We can still fold constants that don't fit imm32
                if(C0 && C1)
                switch(op.opcode)
                {
                    case ops::jilt:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 < N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jige:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 >= N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jigt:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 > N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jile:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 <= N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
    
                    case ops::jieq:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 == N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jine:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 != N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::jult:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 < N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::juge:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 >= N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jugt:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 > N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jule:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 <= N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::jdeq:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 == N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jdne:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 != N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jdlt:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 < N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jdge:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 >= N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jdgt:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 > N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jdle:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 <= N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::jfeq:
                        op.opcode = ops::jmp;
                        if(!(N0.f32 == N1.f32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jfne:
                        op.opcode = ops::jmp;
                        if(!(N0.f32 != N1.f32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jflt:
                        op.opcode = ops::jmp;
                        if(!(N0.f32 < N1.f32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jfge:
                        op.opcode = ops::jmp;
                        if(!(N0.f32 >= N1.f32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jfgt:
                        op.opcode = ops::jmp;
                        if(!(N0.f32 > N1.f32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
                    case ops::jfle:
                        op.opcode = ops::jmp;
                        if(!(N0.f32 <= N1.f32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true; PRINTLN;
                        break;
    
                    case ops::ilt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 < N1.i64);
                        progress = true; PRINTLN;
                        break;
                    case ops::ige:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 >= N1.i64);
                        progress = true; PRINTLN;
                        break;
                    case ops::igt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 > N1.i64);
                        progress = true; PRINTLN;
                        break;
                    case ops::ile:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 <= N1.i64);
                        progress = true; PRINTLN;
                        break;
    
                    case ops::ieq:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 == N1.i64);
                        progress = true; PRINTLN;
                        break;
                    case ops::ine:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 != N1.i64);
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::ult:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 < N1.u64);
                        progress = true; PRINTLN;
                        break;
                    case ops::uge:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 >= N1.u64);
                        progress = true; PRINTLN;
                        break;
                    case ops::ugt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 > N1.u64);
                        progress = true; PRINTLN;
                        break;
                    case ops::ule:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 <= N1.u64);
                        progress = true; PRINTLN;
                        break;
    
                    // Floating point versions
                    // These never get converted to immediates
                    case ops::deq:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 == N1.f64);
                        progress = true; PRINTLN;
                        break;
                    case ops::dne:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 != N1.f64);
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::dlt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 < N1.f64);
                        progress = true; PRINTLN;
                        break;
                    case ops::dge:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 >= N1.f64);
                        progress = true; PRINTLN;
                        break;
                    case ops::dgt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 > N1.f64);
                        progress = true; PRINTLN;
                        break;
                    case ops::dle:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 <= N1.f64);
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::feq:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f32 == N1.f32);
                        progress = true; PRINTLN;
                        break;
                    case ops::fne:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f32 != N1.f32);
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::flt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f32 < N1.f32);
                        progress = true; PRINTLN;
                        break;
                    case ops::fge:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f32 >= N1.f32);
                        progress = true; PRINTLN;
                        break;
                    case ops::fgt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f32 > N1.f32);
                        progress = true; PRINTLN;
                        break;
                    case ops::fle:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f32 <= N1.f32);
                        progress = true; PRINTLN;
                        break;
                    
                    // arithmetic
                    case ops::iadd:
                        op.i64 = N0.i64 + N1.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::isub:
                        op.i64 = N0.i64 - N1.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::imul:
                        op.i64 = N0.i64 * N1.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::idiv:
                        // preserve division by zero!
                        if(N1.i64)
                        {
                            op.i64 = N0.i64 / N1.i64;
                            op.opcode = ops::lci;
                            progress = true; PRINTLN;
                        }
                        break;
                    case ops::imod:
                        // preserve division by zero!
                        if(N1.i64)
                        {
                            op.i64 = N0.i64 % N1.i64;
                            op.opcode = ops::lci;
                            progress = true; PRINTLN;
                        }
                        break;
                    case ops::udiv:
                        // preserve division by zero!
                        if(N1.u64)
                        {
                            op.u64 = N0.u64 / N1.u64;
                            op.opcode = ops::lci;
                            progress = true; PRINTLN;
                        }
                        break;
                    case ops::umod:
                        // preserve division by zero!
                        if(N1.u64)
                        {
                            op.u64 = N0.u64 % N1.u64;
                            op.opcode = ops::lci;
                            progress = true; PRINTLN;
                        }
                        break;
                         
                    // bitwise
                    case ops::iand:
                        op.i64 = N0.i64 & N1.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::ior:
                        op.i64 = N0.i64 | N1.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;
                    case ops::ixor:
                        op.i64 = N0.i64 ^ N1.i64;
                        op.opcode = ops::lci;
                        progress = true; PRINTLN;
                        break;

                    // floating point, these should be safe
                    // do we want to do some fast-math opts though?
                    case ops::fadd:
                        op.f32 = N0.f32 + N1.f32;
                        op.opcode = ops::lcf;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::fsub:
                        op.f32 = N0.f32 - N1.f32;
                        op.opcode = ops::lcf;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::fmul:
                        op.f32 = N0.f32 * N1.f32;
                        op.opcode = ops::lcf;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::fdiv:
                        op.f32 = N0.f32 / N1.f32;
                        op.opcode = ops::lcf;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::dadd:
                        op.f64 = N0.f64 + N1.f64;
                        op.opcode = ops::lcd;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::dsub:
                        op.f64 = N0.f64 - N1.f64;
                        op.opcode = ops::lcd;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::dmul:
                        op.f64 = N0.f64 * N1.f64;
                        op.opcode = ops::lcd;
                        progress = true; PRINTLN;
                        break;
                        
                    case ops::ddiv:
                        op.f64 = N0.f64 / N1.f64;
                        op.opcode = ops::lcd;
                        progress = true; PRINTLN;
                        break;
                }                
            }
        }
        
        if(progress) anyProgress = true;
    }

    //debug();
    BJIT_LOG(" Fold:%d", iter);

    return anyProgress;
}

