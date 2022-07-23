

#include "bjit.h"

#include "hash.h"

using namespace bjit;

#define PRINTLN //BJIT_LOG("\n reassoc %d (op %04x)", __LINE__, op.index);

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

// shortcut for NDOM
#define NDOM(x) blocks[ops[x].block].dom.size()

#define SORT_GT(a,b) (NDOM(a) > NDOM(b) || (NDOM(a) == NDOM(b) && a > b))
#define SORT_LT(a,b) (NDOM(a) < NDOM(b) || (NDOM(a) == NDOM(b) && a < b))

bool Proc::opt_reassoc(bool unsafeOpt)
{
    //debug();

    rebuild_dom();  // need this for intelligent reassoc

    BJIT_ASSERT(live.size());   // should have at least one DCE pass done

    int iter = 0;

    bool progress = true, anyProgress = false;
    while(progress)
    {
        //debug();
        
        ++iter;
        progress = false;
        for(auto b : live)
        {
            for(auto & bc : blocks[b].code)
            {
                if(bc == noVal) continue;
                
                auto & op = ops[bc];

                if(op.opcode == ops::nop) { continue; }
                
                // for associative operations and comparisons where neither
                // operand is constant, sort operands to improve CSE
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
                            int ndom0 = NDOM(op.in[0]);
                            int ndom1 = NDOM(op.in[1]);
                            if(ndom0 > ndom1
                            || (ndom0 == ndom1 && op.in[0] > op.in[1]))
                            {
                                std::swap(op.in[0], op.in[1]);
                                progress = true; PRINTLN;
                            }
                        }
                        break;
                        
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                    case ops::dlt: case ops::dge: case ops::dgt: case ops::dle:
                        // (xor 2) relative to ilt swaps operands
                        {
                            int ndom0 = NDOM(op.in[0]);
                            int ndom1 = NDOM(op.in[1]);
                            if(ndom0 > ndom1
                            || (ndom0 == ndom1 && op.in[0] > op.in[1]))
                            {
                                op.opcode = ops::ilt + (2^(op.opcode-ops::ilt));
                                std::swap(op.in[0], op.in[1]);
                                progress = true; PRINTLN;
                            }
                        }
                        break;

                    default: break;
                }
    
                auto doReassoc1 = [&](int opcode)
                {
                    if(I(opcode)
                    && I0(opcode) && N0.nUse == 1
                    && I1(opcode) && N1.nUse == 1)
                    {
                        // a,b and c,d are already sorted
                        if(SORT_GT(N0.in[0],N1.in[0]))
                        {
                            std::swap(N0.in[0], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                        if(SORT_GT(N0.in[1],N1.in[1]))
                        {
                            std::swap(N0.in[1], N1.in[1]);
                            progress = true; PRINTLN;
                        }
                        if(SORT_GT(N0.in[1],N1.in[0]))
                        {
                            std::swap(N0.in[1], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                    }
                    // reassoc (a+b)+c as (a+c)+b where c.ndom < b.ndom
                    //
                    if(I(opcode) && I0(opcode) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[1]))
                    {
                        std::swap(op.in[1], N0.in[1]);
                        progress = true; PRINTLN;
                    }
                    
                    // reassoc a+(b+c) as c+(a+b) where a.ndom < c.ndom
                    //
                    if(I(opcode) && I1(opcode) && N1.nUse == 1
                    && SORT_LT(op.in[0],N1.in[1]))
                    {
                        std::swap(op.in[0], N1.in[1]);
                        progress = true; PRINTLN;
                    }
                    // reassoc (a+b)+c as (c+b)+a where c.ndom < a.ndom
                    //
                    if(I(opcode) && I0(opcode) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[0]))
                    {
                        std::swap(op.in[1], N0.in[0]);
                        progress = true; PRINTLN;
                    }
                    // reassoc a+(b+c) as b+(a+c) where a.ndom < b.ndom
                    //
                    if(I(opcode) && I1(opcode) && N1.nUse == 1
                    && SORT_LT(op.in[0],N1.in[0]))
                    {
                        std::swap(op.in[0], N1.in[0]);
                        progress = true; PRINTLN;
                    }
                };
                
                auto doReassoc2 = [&](int opcode, int opcodeI)
                {
                    doReassoc1(opcode);

                    if(I(opcode)
                    && I0(opcode) && N0.nUse == 1
                    && I1(opcodeI) && N1.nUse == 1)
                    {
                        if(SORT_GT(N0.in[0],N1.in[0]))
                        {
                            std::swap(N0.in[0], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                        if(SORT_GT(N0.in[1],N1.in[0]))
                        {
                            std::swap(N0.in[1], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                    }
                    
                    if(I(opcode)
                    && I0(opcodeI) && N0.nUse == 1
                    && I1(opcode) && N1.nUse == 1)
                    {
                        if(SORT_GT(N0.in[0],N1.in[0]))
                        {
                            std::swap(N0.in[0], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                        if(SORT_GT(N0.in[0],N1.in[1]))
                        {
                            std::swap(N0.in[0], N1.in[1]);
                            progress = true; PRINTLN;
                        }
                    }

                    if(I(opcode) && I0(opcodeI) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[0]))
                    {
                        std::swap(op.in[1], N0.in[0]);
                        progress = true; PRINTLN;
                    }
                    if(I(opcode) && I1(opcodeI) && N1.nUse == 1
                    && SORT_LT(op.in[0],N1.in[0]))
                    {
                        std::swap(op.in[0], N1.in[0]);
                        progress = true; PRINTLN;
                    }

                };

                // "preferAdd" is really for mul/div where div is more expensive
                auto doReassocSub1 = [&](int add, int sub, bool preferAdd)
                {
                    // we don't have very general handling for subs
                    // but handle a few easy cases..

                    // reassoc (a-b)+(c-d) into (a+c)-(b+d)
                    if(I(add)
                    && I0(sub) && N0.nUse == 1
                    && I1(sub) && N1.nUse == 1
                    && (preferAdd || SORT_GT(N0.in[1],N1.in[0])))
                    {
                        std::swap(N0.in[1], N1.in[0]);
                        N0.opcode = add;
                        N1.opcode = add;
                        op.opcode = sub;
                        progress = true; PRINTLN;
                    }

                    // reassoc (a-b)+(c-d) by sorting a,c and b,d
                    if(I(add)
                    && I0(sub) && N0.nUse == 1
                    && I1(sub) && N1.nUse == 1)
                    {
                        if(SORT_GT(N0.in[0],N1.in[0]))
                        {
                            std::swap(N0.in[0], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                        if(SORT_GT(N0.in[1],N1.in[1]))
                        {
                            std::swap(N0.in[1], N1.in[1]);
                            progress = true; PRINTLN;
                        }
                    }

                    // reassoc (a-b)-(c-d) as (a+d)-(c+b)
                    if(I(sub)
                    && I0(sub) && N0.nUse == 1
                    && I1(sub) && N1.nUse == 1
                    && (preferAdd || SORT_GT(N0.in[1],N1.in[1])))
                    {
                        std::swap(N0.in[1], N1.in[1]);
                        N0.opcode = add;
                        N1.opcode = add;
                        progress = true; PRINTLN;
                    }
                                        
                    // reassoc (a-b)-(c-d) by sorting a,d and b,c
                    if(I(sub)
                    && I0(sub) && N0.nUse == 1
                    && I1(sub) && N1.nUse == 1)
                    {
                        if(SORT_GT(N0.in[0],N1.in[1]))
                        {
                            std::swap(N0.in[0], N1.in[1]);
                            progress = true; PRINTLN;
                        }
                        if(SORT_GT(N0.in[1],N1.in[0]))
                        {
                            std::swap(N0.in[1], N1.in[0]);
                            progress = true; PRINTLN;
                        }
                    }

                    // reassoc (a+b)-c as (a-c)+b where c.ndom < b.ndom
                    //
                    if(I(sub) && I0(add) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[1]))
                    {
                        std::swap(op.opcode, N0.opcode);
                        std::swap(op.in[1], N0.in[1]);
                        progress = true; PRINTLN;
                    }
                    // reassoc (a+b)-c as (b-c)+a where c.ndom < a.ndom
                    if(I(sub) && I0(add) && N0.nUse == 1
                    && SORT_GT(op.in[1],N0.in[0]))
                    {
                        std::swap(op.opcode, N0.opcode);
                        std::swap(op.in[1], N0.in[0]);
                        std::swap(N0.in[0], N0.in[1]);
                        progress = true; PRINTLN;
                    }
                    // reassoc (a+b)-c as (a-c)+b where c.ndom < b.ndom
                    if(I(sub) && I0(add) && N0.nUse == 1
                    && SORT_GT(op.in[1],N0.in[1]))
                    {
                        std::swap(op.opcode, N0.opcode);
                        std::swap(op.in[1], N0.in[1]);
                        progress = true; PRINTLN;
                    }
                    
                    // reassoc (a-b)+c as (a+c)-b where c.ndom < b.ndom
                    //
                    if(I(add) && I0(sub) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[1]))
                    {
                        std::swap(op.opcode, N0.opcode);
                        std::swap(op.in[1], N0.in[1]);
                        progress = true; PRINTLN;
                    }
                    // reassoc (a-b)+c as (c-b)+a where c.ndom < a.ndom
                    //
                    if(I(add) && I0(sub) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[0]))
                    {
                        std::swap(op.in[1], N0.in[0]);
                        progress = true; PRINTLN;
                    }
                    
                    // reassoc (a-b)-c as (a-c)-b where c.ndom < b.ndom
                    //
                    if(I(sub) && I0(sub) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[1]))
                    {
                        std::swap(op.in[1], N0.in[1]);
                        progress = true; PRINTLN;
                    }
                    // reassoc (a-b)-c as a-(c+b) where c.ndom < a.ndom
                    //
                    if(I(sub) && I0(sub) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[0]))
                    {
                        std::swap(op.in[1], N0.in[0]);  // (c-b)-a  = invalid
                        std::swap(op.in[0], op.in[1]);  // a-(c-b)  = invalid
                        N0.opcode = add;          // a-(c+b)  = valid
                        progress = true; PRINTLN;
                    }
                };

                auto doReassocSub2 = [&](int add, int sub, int addI, int subI)
                {
                    doReassocSub1(add, sub, false);
                    
                    
                    // reassoc (a-b)-c as a-(c+b) where c.ndom < a.ndom
                    if(I(sub) && I0(subI) && N0.nUse == 1
                    && SORT_LT(op.in[1],N0.in[0]))
                    {
                        std::swap(op.in[1], N0.in[0]);  // (c-b)-a  = invalid
                        std::swap(op.in[0], op.in[1]);  // a-(c-b)  = invalid
                        N0.opcode = addI;               // a-(c+b)  = valid
                        progress = true; PRINTLN;
                    }                

                };

                auto doReassocC = [&](int opcode, int opcodeI) {
                    // reassoc (a+b)+c as (a+c)+b where c is constant
                    //
                    if(I(opcodeI) && I0(opcode)
                    && (op.in[1] != op.in[0]) && N0.nUse == 1)
                    {
                        BJIT_LOG("\n REASSOC \n");
                        debugOp(op.in[0]);
                        debugOp(op.index);
                        
                        auto imm32 = op.imm32;
                        op.imm32 = 0;
                        op.in[1] = N0.in[1];
                        op.opcode = opcode;
                        
                        N0.in[1] = noVal;
                        N0.imm32 = imm32;
                        N0.opcode = opcodeI;
                        progress = true; PRINTLN;

                        debugOp(op.in[0]);
                        debugOp(op.index);
                    }
                };

                // reassoc (a+b)-c as (a-c)+b where c is constant
                if(I(ops::isubI) && I0(ops::iadd) && N0.nUse == 1)
                {
                    auto imm32 = op.imm32;
                    op.imm32 = 0;
                    op.in[1] = N0.in[1];
                    op.opcode = ops::iadd;
                    
                    N0.in[1] = noVal;
                    N0.imm32 = imm32;
                    N0.opcode = ops::isubI;
                    progress = true; PRINTLN;
                }

                // reassoc (a-b)+c as (a+c)-b where c is constant
                if(I(ops::iaddI) && I0(ops::isub) && N0.nUse == 1)
                {
                    auto imm32 = op.imm32;
                    op.imm32 = 0;
                    op.in[1] = N0.in[1];
                    op.opcode = ops::isub;
                    
                    N0.in[1] = noVal;
                    N0.imm32 = imm32;
                    N0.opcode = ops::iaddI;
                    progress = true; PRINTLN;
                }
                
                // reassoc (a-b)-c as (a-c)-b where c is constant
                if(I(ops::isubI) && I0(ops::isub) && N0.nUse == 1)
                {
                    auto imm32 = op.imm32;
                    op.imm32 = 0;
                    op.in[1] = N0.in[1];
                    op.opcode = ops::isub;
                    
                    N0.in[1] = noVal;
                    N0.imm32 = imm32;
                    N0.opcode = ops::isubI;
                    progress = true; PRINTLN;
                }
                
                doReassocC(ops::iadd, ops::iaddI);
                doReassocC(ops::imul, ops::imulI);
                doReassocC(ops::iand, ops::iandI);
                doReassocC(ops::ixor, ops::ixorI);
                doReassocC(ops::ior, ops::iorI);

                if(0)   // NOTE: These are not SAFE yet :)
                {
                    doReassoc2(ops::iadd, ops::iaddI);
                    doReassocSub2(ops::iadd, ops::isub, ops::iaddI, ops::isubI);
                    doReassoc2(ops::imul, ops::imulI);
                    doReassoc2(ops::iand, ops::iandI);
                    doReassoc2(ops::ixor, ops::ixorI);
                    doReassoc2(ops::ior, ops::iorI);
    
                    if(unsafeOpt)
                    {
                        doReassoc1(ops::fadd);
                        doReassocSub1(ops::fadd, ops::fsub, false);
                        doReassoc1(ops::fmul);
                        doReassocSub1(ops::fmul, ops::fdiv, true);
                        doReassoc1(ops::dadd);
                        doReassocSub1(ops::dadd, ops::dsub, false);
                        doReassoc1(ops::dmul);
                        doReassocSub1(ops::dmul, ops::ddiv, true);
                    }
                }
            }
        }
        
        if(progress) anyProgress = true;
    }

    //debug();
    BJIT_LOG(" Fold:%d", iter);

    return anyProgress;
}