

#include "bjit.h"

#include "hash.h"

using namespace bjit;

#define PRINTLN //BJIT_LOG("\n reassoc %d (op %04x)", __LINE__, getOpIndex(op));

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

    // returns the op that's more dominant
    auto firstOpDominates = [&](uint16_t op1, uint16_t op2) -> bool
    {
        // we want strict dominance only
        if(op1 == op2) return false;
        
        auto ndom1 = blocks[ops[op1].block].dom.size();
        auto ndom2 = blocks[ops[op2].block].dom.size();

        if(ndom1 < ndom2) return true;
        if(ndom1 > ndom2) return false;

        // if ndom is the same, then block should be same as well
        BJIT_ASSERT_MORE(ops[op1].block == ops[op2].block);

        return ops[op1].pos < ops[op2].pos ? true : false;
    };

    bool progress = true, anyProgress = false;
    while(progress)
    {
        //debug();
        
        ++iter;
        progress = false;
        for(auto b : live)
        {
            for(const auto & bc : blocks[b].code)
            {
                if(bc == noVal) continue;
                
                auto & op = ops[bc];

                if(op.opcode == ops::nop) { continue; }

            #if 1
                auto debugReassoc =
                    [&](Op & op, int opcode, int opcodeI, int opSub) {};
            #else
                // This is rather inefficient, but it's for debugging only
                std::function<void(Op&,int,int,int)> debugReassoc
                = [&](Op & op, int opcode, int opcodeI, int opSub)
                {
                    BJIT_LOG("%04x", getOpIndex(op));
                    if(op.nUse > 1) { BJIT_LOG("!"); return; }
                    if(op.opcode == opcode || op.opcode == opSub)
                    {
                        BJIT_LOG(":( ");
                        debugReassoc(ops[op.in[0]], opcode, opcodeI, opSub);
                        BJIT_LOG(" %c ", op.opcode == opcode ? '+' : '-');
                        debugReassoc(ops[op.in[1]], opcode, opcodeI, opSub);
                        BJIT_LOG(" )");
                    }
                    else if(op.opcode == opcodeI)
                    {
                        BJIT_LOG(":( ");
                        debugReassoc(ops[op.in[0]], opcode, opcodeI, opSub);
                        BJIT_LOG(" + ");
                        BJIT_LOG("imm:%d", op.imm32);
                        BJIT_LOG(" )");
                    }
                    else return;
                };
            #endif

                //BJIT_LOG("\n");
                debugReassoc(op, ops::iadd, ops::iaddI, ops::isub);

                // This tries to reassociate into a "canonical form" where:
                //
                //  - loop invariants and constants go early -> CF + LICM
                //  - order of operands is always the same -> CSE
                //  - try to form (a+a)+b when possible -> special fold rules
                //
                // Sadly this results in flat dependency chains, so ideally
                // we should probably add a second reassoc pass just before
                // RA that builds binary trees, but only within basic blocks.
                auto doReassoc = [&](int opcode, int opcodeI)
                {
                    if(0)if(I(opcode) || I(opcodeI))
                    {
                        BJIT_LOG("\n");
                        debugReassoc(op, opcode, opcodeI,
                            opcode == ops::iadd ? ops::isub : noVal);
                    }
                
                    // reorder:       (b+a) -> (a+b)        where a < b
                    if(I(opcode) && firstOpDominates(op.in[0],op.in[1]))
                    {
                        std::swap(op.in[0], op.in[1]);// PRINTLN
                    }

                    // reassoc: (a+b)+(c+d) -> (a+c)+(b+d) where c < b
                    if(I(opcode) && N0.nUse == 1 && N1.nUse == 1
                    && I0(opcode) && I1(opcode)
                    && firstOpDominates(N0.in[1], N1.in[0]))
                    {
                        std::swap(N0.in[1], N1.in[0]);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a+b)+(c+d) -> ((c+d)+b)+a
                    //      where (a+b) < (c+d) and a < (c+d)
                    if(I(opcode) && N0.nUse == 1 && N1.nUse == 1
                    && I0(opcode) && I1(opcode)
                    && N0.in[0] != N0.in[1]   // <- gets special cased
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        auto tmp = N0.in[0];
                        N0.in[0] = op.in[1];
                        op.in[1] = tmp;
                        progress = true; PRINTLN
                    }

                    // reassoc: (a+b)+X -> (a+X)+b
                    //      where b < X
                    if(I(opcode) && N0.nUse == 1
                    && I0(opcode) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[1]))
                    {
                        std::swap(N0.in[1], op.in[1]);
                        progress = true; PRINTLN
                    }

                    // reassoc: (a+b)+X -> (X+b)+a
                    //      where a < X
                    if(I(opcode) && N0.nUse == 1
                    && I0(opcode) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        std::swap(N0.in[0], op.in[1]);
                        progress = true; PRINTLN
                    }

                    // reassoc: (a+b)+b -> (b+b)+a, see previous
                    if(I(opcode) && I0(opcode) && N0.nUse == 1
                    && N0.in[0] != N0.in[1]
                    && N0.in[1] == op.in[1])
                    {
                        std::swap(N0.in[0], op.in[1]);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a+b)+a -> (a+a)+b, see previous
                    if(I(opcode) && I0(opcode) && N0.nUse == 1
                    && N0.in[0] != N0.in[1]
                    && N0.in[0] == op.in[1])
                    {
                        std::swap(N0.in[1], op.in[1]);
                        progress = true; PRINTLN
                    }

                    // reassoc: (a+C1) + (b+C2) -> ((b+C2)+C1)+a
                    if(I(opcode) && N0.nUse == 1 && N1.nUse == 1
                    && I0(opcodeI) && I1(opcodeI))
                    {
                        auto tmp = N0.in[0];
                        N0.in[0] = op.in[1];
                        op.in[1] = tmp;
                        progress = true; PRINTLN
                    }
                    
                    // immediates always "dominate" everything
                    // rewrite (a+b)+C as (b+C)+a
                    // but special case (a+a)+C for better regalloc
                    if(I(opcodeI) && I0(opcode) && N0.nUse == 1
                    && N0.in[0] != N0.in[1])
                    {
                        std::swap(op.opcode, N0.opcode);
                        std::swap(N0.in[1], N0.in[0]);
                        std::swap(op.in[1], N0.in[1]);  // one is noval
                        std::swap(op.imm32, N0.imm32);
                        progress = true; PRINTLN
                    }

                    // see above (a+C)+a is better written the other way
                    if(I(opcode) && I0(opcodeI) && N0.nUse == 1
                    && op.in[1] == N0.in[0])
                    {
                        std::swap(op.opcode, N0.opcode);
                        std::swap(op.in[1], N0.in[1]);  // one is noval
                        std::swap(op.imm32, N0.imm32);
                        progress = true; PRINTLN
                    }

                    if(0)if(I(opcode) || I(opcodeI))
                    {
                        BJIT_LOG("\n");
                        debugReassoc(op, opcode, opcodeI,
                            opcode == ops::iadd ? ops::isub : noVal);
                    }
                };

                // These should prefer + over - because we want to also
                // apply them to * and / where division is more expensive..
                auto reassocSub = [&](int opAdd, int opSub)
                {
                    // reorder: (b+a) -> (a+b) where a < b
                    if(I(opAdd) && firstOpDominates(op.in[0],op.in[1]))
                    {
                        std::swap(op.in[0], op.in[1]);// PRINTLN
                    }
                    
                    // reassoc: (a-b)+(c-d) -> (a+c)-(b+d) where c < b
                    if(I(opAdd) && N0.nUse == 1 && N1.nUse == 1
                    && I0(opSub) && I1(opSub)
                    && firstOpDominates(N0.in[1], N1.in[0]))
                    {
                        std::swap(N0.in[1], N1.in[0]);
                        N0.opcode = opAdd;
                        N1.opcode = opAdd;
                        op.opcode = opSub;
                        progress = true; PRINTLN
                    }

                    // reassoc: (a+b)+(c-d) -> ((c-d)+b)+a
                    //      where (a+b) < (c-d) and a < (c+d)
                    if(I(opAdd) && N0.nUse == 1 && N1.nUse == 1
                    && I0(opAdd) && I1(opSub)
                    && N0.in[0] != N0.in[1]   // <- gets special cased
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        auto tmp = N0.in[0];
                        N0.in[0] = op.in[1];
                        op.in[1] = tmp;
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a-b)+(c+d) -> ((c+d)-b)+a
                    //      where (a-b) < (c+d) and a < (c+d)
                    if(I(opAdd) && N0.nUse == 1 && N1.nUse == 1
                    && I0(opSub) //&& I1(opAdd)
                    && N0.in[0] != N0.in[1]   // <- gets special cased
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        auto tmp = N0.in[0];
                        N0.in[0] = op.in[1];
                        op.in[1] = tmp;
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a+b)-X -> (a-X)+b
                    //      where b < X
                    if(I(opSub) && N0.nUse == 1
                    && I0(opAdd) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[1]))
                    {
                        std::swap(N0.in[1], op.in[1]);
                        std::swap(op.opcode, N0.opcode);
                        progress = true; PRINTLN
                    }

                    // reassoc: (a-b)+X -> (a+X)-b
                    //      where b < X
                    if(I(opAdd) && N0.nUse == 1
                    && I0(opSub) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[1]))
                    {
                        std::swap(N0.in[1], op.in[1]);
                        std::swap(op.opcode, N0.opcode);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a-b)+X -> (X-b)+a
                    //      where a < X
                    if(I(opAdd) && N0.nUse == 1
                    && I0(opSub) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        std::swap(N0.in[0], op.in[1]);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a+b)-X -> (b-X)+a
                    //      where a < X
                    if(I(opSub) && N0.nUse == 1
                    && I0(opAdd) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        std::swap(N0.in[0], op.in[1]);
                        std::swap(N0.in[0], N0.in[1]);
                        std::swap(op.opcode, N0.opcode);
                        progress = true; PRINTLN
                    }

                    // reassoc: (a-b)-X -> (a-X)-b
                    //      where b < X
                    if(I(opSub) && N0.nUse == 1
                    && I0(opSub) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[1]))
                    {
                        std::swap(N0.in[1], op.in[1]);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a-b)-X -> a-(X+b)
                    //      where a < X
                    if(I(opSub) && N0.nUse == 1
                    && I0(opSub) && N0.in[0] != N0.in[1]
                    && firstOpDominates(op.in[1], N0.in[0]))
                    {
                        N0.opcode = opAdd;
                        std::swap(N0.in[0], op.in[1]);
                        std::swap(op.in[0], op.in[1]);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a-b)+b -> (b-b)+a, see previous
                    if(I(opAdd) && I0(opSub) && N0.nUse == 1
                    && N0.in[0] != N0.in[1]
                    && N0.in[1] == op.in[1])
                    {
                        std::swap(N0.in[0], op.in[1]);
                        progress = true; PRINTLN
                    }
                     
                    // reassoc: (a-b)-b -> a-(b+b), see previous
                    if(I(opSub) && I0(opSub) && N0.nUse == 1
                    && N0.in[0] != N0.in[1]
                    && N0.in[1] == op.in[1])
                    {
                        N0.opcode = opAdd;
                        std::swap(N0.in[0], op.in[1]);
                        std::swap(op.in[0], op.in[1]);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a-b)+a -> (a+a)-b, see previous
                    if(I(opAdd) && I0(opSub) && N0.nUse == 1
                    && N0.in[0] != N0.in[1]
                    && N0.in[0] == op.in[1])
                    {
                        std::swap(N0.in[1], op.in[1]);
                        std::swap(N0.opcode, op.opcode);
                        progress = true; PRINTLN
                    }
                    
                    // reassoc: (a-b)-a -> (a-a)-b, see previous
                    if(I(opSub) && I0(opSub) && N0.nUse == 1
                    && N0.in[0] != N0.in[1]
                    && N0.in[0] == op.in[1])
                    {
                        std::swap(N0.in[1], op.in[1]);
                        std::swap(N0.opcode, op.opcode);
                        progress = true; PRINTLN
                    }

                    // reassoc a-(a-b) -> b-(a-a)
                    if(I(opSub) && I1(opSub) && N1.nUse == 1
                    && N1.in[0] != N1.in[1]
                    && N1.in[0] == op.in[0])
                    {
                        std::swap(N1.in[1], op.in[0]);
                        progress = true; PRINTLN
                    }

                    // simplify (a-b)+(b+c) -> (a + c)
                    if(I(opAdd) && I0(opSub) && I1(opAdd)
                    && N0.in[1] == N1.in[0])
                    {
                        op.in[0] = N0.in[0];
                        op.in[1] = N1.in[1];
                        progress = true; PRINTLN
                    }
                    
                    // simplify (a-b)+(c+b) -> (a + c)
                    if(I(opAdd) && I0(opSub) && I1(opAdd)
                    && N0.in[1] == N1.in[1])
                    {
                        op.in[0] = N0.in[0];
                        op.in[1] = N1.in[0];
                        progress = true; PRINTLN
                    }
                    
                    // simplify (a+b)+(c-b) -> (a + c)
                    if(I(opAdd) && I0(opAdd) && I1(opSub)
                    && N0.in[1] == N1.in[1])
                    {
                        op.in[0] = N0.in[0];
                        op.in[1] = N1.in[0];
                        progress = true; PRINTLN
                    }
                    
                    // simplify (a+b)+(c-a) -> (b + c)
                    if(I(opAdd) && I0(opAdd) && I1(opSub)
                    && N0.in[0] == N1.in[1])
                    {
                        op.in[0] = N0.in[1];
                        op.in[1] = N1.in[0];
                        progress = true; PRINTLN
                    }

                    // simplify (a+b)-(a+c) -> (b - c)
                    if(I(opSub) && I0(opAdd) && I1(opAdd)
                    && N0.in[0] == N1.in[0])
                    {
                        op.in[0] = N0.in[1];
                        op.in[1] = N1.in[1];
                        progress = true; PRINTLN
                    }
                    // simplify (a+b)-(b+c) -> (a - c)
                    if(I(opSub) && I0(opAdd) && I1(opAdd)
                    && N0.in[1] == N1.in[0])
                    {
                        op.in[0] = N0.in[0];
                        op.in[1] = N1.in[1];
                        progress = true; PRINTLN
                    }
                    // simplify (a+b)-(b+c) -> (a - c)
                    if(I(opSub) && I0(opAdd) && I1(opAdd)
                    && N0.in[1] == N1.in[0])
                    {
                        op.in[0] = N0.in[0];
                        op.in[1] = N1.in[1];
                        progress = true; PRINTLN
                    }
                    // simplify (a+b)-(c+b) -> (a - c)
                    if(I(opSub) && I0(opAdd) && I1(opAdd)
                    && N0.in[1] == N1.in[1])
                    {
                        op.in[0] = N0.in[0];
                        op.in[1] = N1.in[0];
                        progress = true; PRINTLN
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

                // This preservers idiv as-is, so it's safe
                //
                // reassoc: (a*b)*(c/d) -> ((c/d)*b)*a
                if(I(ops::imul) && N0.nUse == 1 && N1.nUse == 1
                && I0(ops::imul) && I1(ops::idiv)
                && N0.in[0] != N0.in[1]
                && firstOpDominates(op.in[1], N0.in[0]))
                {
                    auto tmp = N0.in[0];
                    N0.in[0] = op.in[1];
                    op.in[1] = tmp;
                    progress = true; PRINTLN
                }
                
                reassocSub(ops::iadd, ops::isub);
                doReassoc(ops::iadd, ops::iaddI);
                doReassoc(ops::imul, ops::imulI);
                doReassoc(ops::iand, ops::iandI);
                doReassoc(ops::ixor, ops::ixorI);
                doReassoc(ops::ior, ops::iorI);
                
                //BJIT_LOG("\n");
                debugReassoc(op, ops::iadd, ops::iaddI, ops::isub);

                if(unsafeOpt)
                {
                    reassocSub(ops::fadd, ops::fsub);
                    doReassoc(ops::fadd, noVal);
                    reassocSub(ops::fmul, ops::fdiv);
                    doReassoc(ops::fmul, noVal);
                    
                    reassocSub(ops::dadd, ops::dsub);
                    doReassoc(ops::dadd, noVal);
                    reassocSub(ops::dmul, ops::ddiv);
                    doReassoc(ops::dmul, noVal);
                }

            }
        }
        
        if(progress) anyProgress = true;
    }

    //debug();
    BJIT_LOG(" Fold:%d", iter);

    return anyProgress;
}
