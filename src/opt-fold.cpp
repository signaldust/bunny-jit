
#include "bjit.h"

#include "hash.h"

using namespace bjit;

// This stores the data CSE needs in our hash table.
struct OpCSE
{
    // not included in hash
    uint16_t index = noVal;
    uint16_t block = noVal;

    // rest is hashed
    union
    {
        struct {
            uint32_t imm32 = 0;
            uint16_t in[2] = { noVal, noVal };
        };

        // for lci/lcf
        int64_t     i64;
    };
    uint16_t opcode = noVal;

    OpCSE() {}
    OpCSE(Op const & op) { set(op); }

    void set(Op const & op)
    {
        index = op.index;
        block = op.block;
        opcode = op.opcode;
        if(op.hasI64() || op.hasF64())
        {
            i64 = op.i64;
        }
        else
        {
            in[0] = op.nInputs() >= 1 ? op.in[0] : noVal;
            in[1] = op.nInputs() >= 2 ? op.in[1] : noVal;
            imm32 = op.hasImm32() ? op.imm32 : 0;
        }
    }

    // NOTE: we need temporary to force the "noVals"
    bool isEqual(Op const & op) const
    { OpCSE tmp(op); return isEqual(tmp); }

    // NOTE: we need temporary to force the "noVals"
    static uint64_t getHash(Op const & op)
    { OpCSE tmp(op); return getHash(tmp); }
    
    bool isEqual(OpCSE const & op) const
    { return i64 == op.i64 && opcode == op.opcode; }

    static uint64_t getHash(OpCSE const & op)
    { return hash64(op.i64 + op.opcode); }
};

// match op
#define I(x) (op.opcode == x)

// match operands
#define I0(x) (op.nInputs() >= 1 && ops[op.in[0]].opcode == x)
#define I1(x) (op.nInputs() >= 2 && ops[op.in[1]].opcode == x)

// check for constants
#define C0 (op.nInputs() >= 1 && (I0(ops::lci) || I0(ops::lcf)))
#define C1 (op.nInputs() >= 2 && (I1(ops::lci) || I1(ops::lcf)))

// fetch operands
#define N0 ops[op.in[0]]
#define N1 ops[op.in[1]]


/*

We try to avoid any folding here that isn't guaranteed to be profitable.

*/
bool Proc::opt_fold()
{

    //debug();

    assert(live.size());   // should have at least one DCE pass done

    Rename rename;
    HashTable<OpCSE> cseTable(ops.size());

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
                
                rename(op);
    
                // rename phis
                if(op.opcode <= ops::jmp)
                {
                    for(auto & a : blocks[op.label[0]].args)
                    for(auto & s : a.alts)
                    for(auto & r : rename.map)
                    {
                        if(s.val == r.src) s.val = r.dst;
                    }
                }
    
                if(op.opcode < ops::jmp)
                {
                    for(auto & a : blocks[op.label[1]].args)
                    for(auto & s : a.alts)
                    for(auto & r : rename.map)
                    {
                        if(s.val == r.src) s.val = r.dst;
                    }
                }

                // for associative operations and comparisons where neither
                // operand is constant, sort operands to improve CSE
                if(!C0 && !C1)
                switch(op.opcode)
                {
                    case ops::ieq: case ops::ine:
                    case ops::feq: case ops::fne:
                    case ops::iadd: case ops::imul:
                    case ops::fadd: case ops::fmul:
                    case ops::iand: case ops::ior: case ops::ixor:
                        if(op.in[0] < op.in[1])
                        {
                            std::swap(op.in[0], op.in[1]);
                            progress = true;
                        }
                        break;
                        
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                        // (xor 2) relative to ilt swaps operands
                        if(op.in[0] < op.in[1])
                        {
                            op.opcode = ops::ilt + (2^(op.opcode-ops::ilt));
                            std::swap(op.in[0], op.in[1]);
                            progress = true;
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
                    case ops::jfeq: case ops::jfne:
                    case ops::feq: case ops::fne:
                    case ops::iadd: case ops::imul:
                    case ops::fadd: case ops::fmul:
                    case ops::iand: case ops::ior: case ops::ixor:
                        std::swap(op.in[0], op.in[1]);
                        progress = true;
                        break;
    
                    // rewrite 0 - a = -a
                    case ops::isub:
                        if(!ops[op.in[1]].i64)
                        {
                            op.opcode = ops::ineg;
                            op.in[0] = op.in[1];
                            op.in[1] = noVal;
                            progress = true;
                        }
                        break;
    
                    case ops::jilt: case ops::jige: case ops::jigt: case ops::jile:
                    case ops::jult: case ops::juge: case ops::jugt: case ops::jule:
                    case ops::jflt: case ops::jfge: case ops::jfgt: case ops::jfle:
                        op.opcode = 2 ^ op.opcode;
                        std::swap(op.in[0], op.in[1]);
                        progress = true;
                        break;
                        
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                        op.opcode = ops::ilt + (2^(op.opcode-ops::ilt));
                        std::swap(op.in[0], op.in[1]);
                        progress = true;
                        break;
                }
    
                // simplify ieqI/ineI into inverted test when possible
                // don't bother if there are other uses
                if((I(ops::ieqI) || I(ops::ineI)) && !op.i64 && N0.nUse == 1)
                {
                    switch(N0.opcode)
                    {
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
    
                    case ops::iltI: case ops::igeI: case ops::igtI: case ops::ileI:
                    case ops::ultI: case ops::ugeI: case ops::ugtI: case ops::uleI:
                        {
                            bool negate = (op.opcode == ops::ieqI);
                            auto ii = N0;
                            op.opcode = N0.opcode;
                            if(N0.nInputs() == 2) op.in[1] = N0.in[1];
                            if(N0.hasImm32()) op.imm32 = N0.imm32;
                            op.in[0] = N0.in[0];
                            if(negate) op.opcode ^= 1;
                            progress = true;
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
                        progress = true;
                        break;
                    
                    case ops::ilt: case ops::ige:
                    case ops::igt: case ops::ile:
                    case ops::ieq: case ops::ine:
                    case ops::ult: case ops::uge:
                    case ops::ugt: case ops::ule:
                        op.opcode += ops::iltI - ops::ilt;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    
                    case ops::iadd:
                        op.opcode = ops::iaddI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::isub:
                        // FIXME: rewrite as iaddI with negated immediated?
                        op.opcode = ops::isubI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::imul:
                        op.opcode = ops::imulI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::iand:
                        op.opcode = ops::iandI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::ior:
                        op.opcode = ops::iorI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::ixor:
                        op.opcode = ops::ixorI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::ishl:
                        op.opcode = ops::ishlI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::ishr:
                        op.opcode = ops::ishrI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::ushr:
                        op.opcode = ops::ushrI;
                        op.imm32 = (int32_t) N1.i64;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                }
                
                // fold conditions into jumps
                if((I(ops::jz) || I(ops::jnz)) && N0.nUse == 1
                && (N0.opcode >= ops::ilt && N0.opcode - ops::ilt <= ops::jfne))
                {
                    int negate = (op.opcode == ops::jz) ? 1 : 0;
                    op.opcode = negate ^ (N0.opcode + ops::jilt - ops::ilt);
                    op.in[1] = N0.in[1];
                    op.in[0] = N0.in[0];
                    progress = true;
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
                    progress = true;
                }
    
                
                // simplify jieqI/jineI into jz/jnz when possible
                if((I(ops::jieqI) || I(ops::jineI)) && !op.imm32)
                {
                    op.opcode = I(ops::jieqI) ? ops::jz : ops::jnz;
                    op.in[0] = noVal;
                    progress = true;
                }
    
                // -(-a) = a
                if(I(ops::ineg) && I0(ops::ineg)) rename.add(bc, N0.in[0]);
                if(I(ops::fneg) && I0(ops::fneg)) rename.add(bc, N0.in[0]);
                
                // a + 0, a - 0, a * 1 -> a
                if((I(ops::iaddI) && !op.imm32)
                || (I(ops::isubI) && !op.imm32)
                || (I(ops::imulI) && 1 == op.imm32))
                {
                    rename.add(bc, op.in[0]);
                    progress = true;
                    op.makeNOP();
                    continue;
                }

                if((I(ops::fadd) && I1(ops::lcf) && N1.f64 == 0.)
                || (I(ops::fsub) && I1(ops::lcf) && N1.f64 == 0.)
                || (I(ops::fmul) && I1(ops::lcf) && N1.f64 == 1.))
                {
                    rename.add(bc, op.in[0]);
                    progress = true;
                    op.makeNOP();
                    continue;                    
                }
                
                // a * -1 -> -a
                if(I(ops::imulI) && -1 == op.i64)
                {
                    op.opcode = ops::ineg; progress = true;
                }
                if(I(ops::fmul) && I1(ops::lcf) && N1.f64 == -1.)
                {
                    op.opcode = ops::fneg; op.in[1] = noVal; progress = true;
                }
                
                // a + (-b) = a-b
                if(I(ops::iadd) && I1(ops::ineg))
                {
                    op.opcode = ops::isub;
                    op.in[1] = N1.in[0];
                    progress = true;
                }
                if(I(ops::fadd) && I1(ops::fneg))
                {
                    op.opcode = ops::fsub;
                    op.in[1] = N1.in[0];
                    progress = true;
                }
                
    
                // a - (-b) = a + b
                if(I(ops::isub) && I1(ops::ineg))
                {
                    op.opcode = ops::iadd;
                    op.in[1] = N1.in[0];
                    progress = true;
                }
                if(I(ops::fsub) && I1(ops::fneg))
                {
                    op.opcode = ops::fadd;
                    op.in[1] = N1.in[0];
                    progress = true;
                }
    
                // -a + b = b - a
                if(I(ops::iadd) && I0(ops::ineg))
                {
                    op.opcode = ops::isub;
                    op.in[1] = N0.in[0];
                    std::swap(op.in[0], op.in[1]);
                    progress = true;
                }
                if(I(ops::fadd) && I0(ops::fneg))
                {
                    op.opcode = ops::fsub;
                    op.in[1] = N0.in[0];
                    std::swap(op.in[0], op.in[1]);
                    progress = true;
                }

                // a+a = a<<1
                if(I(ops::iadd) && op.in[0] == op.in[1])
                {
                    op.opcode = ops::ishlI;
                    op.in[1] = noVal;
                    op.imm32 = 1;
                    progress = true;
                }
    
                // addition immediate merges
                if(I(ops::iaddI) && I0(ops::iaddI))
                {
                    int64_t v = (int64_t) op.imm32 + (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true;
                    }
                }
                if(I(ops::isubI) && I0(ops::isubI))
                {
                    int64_t v = (int64_t) op.imm32 + (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true;
                    }
                }
                if(I(ops::iaddI) && I0(ops::isubI))
                {
                    int64_t v = (int64_t) op.imm32 - (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true;
                    }
                }
                if(I(ops::isubI) && I0(ops::iaddI))
                {
                    int64_t v = (int64_t) op.imm32 - (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        op.in[0] = N0.in[0];
                        op.imm32 = v;
                        progress = true;
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
                        progress = true;
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
                        progress = true;
                    }
                }
    
                // can we replace multiply with shift?
                if(I(ops::imulI) && !(op.imm32 & (op.imm32 - 1)))
                {
                    uint32_t b = op.imm32;
                    int shift = 0; while(b >>= 1) ++shift;
                    op.opcode = ops::ishlI;
                    op.imm32 = shift;
                    progress = true;
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
                    progress = true;
                }

                // can we replace modulo with mask? max 32-bits for now
                if(I(ops::umod) && I1(ops::lci)
                && !(N1.u64 & (N1.u64 - 1)) && (N1.u64 <= (1ull<<32)))
                {
                    uint32_t b = N1.imm32;
                    int shift = 0; while(b >>= 1) ++shift;
                    op.opcode = ops::iandI;
                    op.imm32 = ((1ull<<shift)-1);
                    op.in[1] = noVal;
                    progress = true;
                }
                
                // (a<<n)<<m = (a<<(n+m)), happens from adds/muls
                if(I(ops::ishlI) && I0(ops::ishlI))
                {
                    int shift = op.imm32 + N0.imm32;
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
                    progress = true;
                }

                // shift by zero is always a NOP
                if((I(ops::ishlI) || I(ops::ishrI) || I(ops::ushrI)) && !op.imm32)
                {
                    rename.add(op.index, op.in[0]);
                    op.makeNOP();
                    continue;
                }
                
                // FIXME: add NAND and NOR to simplify
                // some constructs involving bitwise NOT?
    
                // ~(~a) = a
                if(I(ops::inot) && I0(ops::inot))
                {
                    rename.add(bc, N0.in[0]);
                    op.makeNOP();
                    progress = true;
                    continue;
                }
                
                // bit-AND merges : (a&b)&c = a&(b&c)
                if(I(ops::iandI) && I0(ops::iandI))
                {
                    op.imm32 &= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true;
                }
    
                // bit-OR merges : (a|b)|c = a|(b|c)
                if(I(ops::iorI) && I0(ops::iorI))
                {
                    op.imm32 |= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true;
                }
    
                // bit-XOR merges : (a^b)^c = a^(b^c)
                if(I(ops::ixorI) && I0(ops::ixorI))
                {
                    op.imm32 ^= N0.imm32;
                    op.in[0] = N0.in[0];
                    progress = true;
                }
    
                if(I(ops::ixorI) && !~op.imm32)
                {
                    op.opcode = ops::inot;
                    progress = true;
                }
    
                if(I(ops::ixorI) && !op.imm32)
                {
                    rename.add(bc, op.in[0]);
                    progress = true;
                    op.makeNOP();
                    continue;
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
                            progress = true;
                        }
                        break;
                    case ops::jz:
                        op.opcode = ops::jmp;
                        if(N0.i64) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                        
                    case ops::jnz:
                        op.opcode = ops::jmp;
                        if(!N0.i64) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
    
                    case ops::jiltI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 < (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::jigeI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 >= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::jigtI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 > (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::jileI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 <= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
    
                    case ops::jieqI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 == (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::jineI:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 != (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                        
                    case ops::jultI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 < (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::jugeI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 >= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::jugtI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 > (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                    case ops::juleI:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 <= (int64_t)op.imm32)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        progress = true;
                        break;
                        
                    case ops::iltI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 < op.imm32);
                        progress = true;
                        break;
                    case ops::igeI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 >= op.imm32);
                        progress = true;
                        break;
                    case ops::igtI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 > op.imm32);
                        progress = true;
                        break;
                    case ops::ileI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 <= op.imm32);
                        progress = true;
                        break;
    
                    case ops::ieqI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 == op.imm32);
                        progress = true;
                        break;
                    case ops::ineI:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 != op.imm32);
                        progress = true;
                        break;
                        
                    case ops::ultI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 < (int64_t)op.imm32);
                        progress = true;
                        break;
                    case ops::ugeI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 >= (int64_t)op.imm32);
                        progress = true;
                        break;
                    case ops::ugtI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 > (int64_t)op.imm32);
                        progress = true;
                        break;
                    case ops::uleI:
                        op.opcode = ops::lci;
                        op.u64 = (N0.u64 <= (int64_t)op.imm32);
                        progress = true;
                        break;
                        
                    // arithmetic
                    case ops::iaddI:
                        op.i64 = N0.i64 + (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::isubI:
                        op.i64 = N0.i64 - (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ineg:
                        op.i64 = -N0.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::imulI:
                        op.i64 = N0.i64 * (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                         
                    // bitwise
                    case ops::inot:
                        op.i64 = ~N0.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    
                    case ops::iandI:
                        op.i64 = N0.i64 & (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::iorI:
                        op.i64 = N0.i64 | (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ixorI:
                        op.i64 = N0.i64 ^ (int64_t)op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
    
                    // bitshifts
                    case ops::ishlI:
                        op.i64 = N0.i64 << op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                        
                    case ops::ishrI:
                        // this relies on compiler giving signed shift
                        op.i64 = N0.i64 >> op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
    
                    case ops::ushrI:
                        // this relies on compiler giving unsigned shift
                        op.u64 = N0.u64 >> op.imm32;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
    
                    case ops::fneg:
                        op.f64 = -N0.f64;
                        op.opcode = ops::lcf;
                        progress = true;
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
                        progress = true;
                        break;
                    case ops::jige:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 >= N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jigt:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 > N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jile:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 <= N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
    
                    case ops::jieq:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 == N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jine:
                        op.opcode = ops::jmp;
                        if(!(N0.i64 != N1.i64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                        
                    case ops::jult:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 < N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::juge:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 >= N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jugt:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 > N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jule:
                        op.opcode = ops::jmp;
                        if(!(N0.u64 <= N1.u64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                        
                    case ops::jfeq:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 == N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jfne:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 != N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jflt:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 < N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jfge:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 >= N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jfgt:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 > N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
                    case ops::jfle:
                        op.opcode = ops::jmp;
                        if(!(N0.f64 <= N1.f64)) op.label[0] = op.label[1];
                        op.in[0] = noVal;
                        op.in[1] = noVal;
                        progress = true;
                        break;
    
                    case ops::ilt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 < N1.i64);
                        progress = true;
                        break;
                    case ops::ige:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 >= N1.i64);
                        progress = true;
                        break;
                    case ops::igt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 > N1.i64);
                        progress = true;
                        break;
                    case ops::ile:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 <= N1.i64);
                        progress = true;
                        break;
    
                    case ops::ieq:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 == N1.i64);
                        progress = true;
                        break;
                    case ops::ine:
                        op.opcode = ops::lci;
                        op.i64 = (N0.i64 != N1.i64);
                        progress = true;
                        break;
                        
                    case ops::ult:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 < N1.u64);
                        progress = true;
                        break;
                    case ops::uge:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 >= N1.u64);
                        progress = true;
                        break;
                    case ops::ugt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 > N1.u64);
                        progress = true;
                        break;
                    case ops::ule:
                        op.opcode = ops::lci;
                        op.i64 = (N0.u64 <= N1.u64);
                        progress = true;
                        break;
    
                    // Floating point versions
                    // These never get converted to immediates
                    case ops::feq:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 == N1.f64);
                        progress = true;
                        break;
                    case ops::fne:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 != N1.f64);
                        progress = true;
                        break;
                        
                    case ops::flt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 < N1.f64);
                        progress = true;
                        break;
                    case ops::fge:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 >= N1.f64);
                        progress = true;
                        break;
                    case ops::fgt:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 > N1.f64);
                        progress = true;
                        break;
                    case ops::fle:
                        op.opcode = ops::lci;
                        op.i64 = (N0.f64 <= N1.f64);
                        progress = true;
                        break;
                    
                    // arithmetic
                    case ops::iadd:
                        op.i64 = N0.i64 + N1.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::isub:
                        op.i64 = N0.i64 - N1.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::imul:
                        op.i64 = N0.i64 * N1.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::idiv:
                        // preserve division by zero!
                        if(N1.i64)
                        {
                            op.i64 = N0.i64 / N1.i64;
                            op.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                    case ops::imod:
                        // preserve division by zero!
                        if(N1.i64)
                        {
                            op.i64 = N0.i64 % N1.i64;
                            op.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                    case ops::udiv:
                        // preserve division by zero!
                        if(N1.u64)
                        {
                            op.u64 = N0.u64 / N1.u64;
                            op.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                    case ops::umod:
                        // preserve division by zero!
                        if(N1.u64)
                        {
                            op.u64 = N0.u64 % N1.u64;
                            op.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                         
                    // bitwise
                    case ops::iand:
                        op.i64 = N0.i64 & N1.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ior:
                        op.i64 = N0.i64 | N1.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ixor:
                        op.i64 = N0.i64 ^ N1.i64;
                        op.opcode = ops::lci;
                        progress = true;
                        break;
    
                }
                
                // CSE: do this after simplification
                if(op.canCSE())
                {
                    auto * ptr = cseTable.find(op);

                    if(ptr && ptr->index == op.index) continue;
                    
                    if(ptr)
                    {
                        // closest common dominator
                        auto & other = ops[ptr->index];
                        auto pb = ptr->block;
                        int ccd = 0;
                        int iMax = std::min(
                            blocks[b].dom.size(), blocks[pb].dom.size());
                            
                        for(int i = 0; i < iMax; ++i)
                        {
                            if(blocks[b].dom[i] == blocks[pb].dom[i]) ccd = i;
                            else break;
                        }

                        // we need to sanity check phis and post-doms
                        bool bad = false;
                        bool renameOther = false;
                        bool renameThis = false;
                        
                        // check post-dominator condition
                        for(int i = b ; i; i = blocks[i].idom)
                        {
                            // if we are skipping phis, then we'll force
                            // force the other to be kept as a rename
                            for(auto & a : blocks[i].args)
                            for(auto & s : a.alts)
                            {
                                if(ptr->index == s.val) renameOther = true;
                            }
                            
                            if(ccd == blocks[i].idom) break; // this is fine
                            if(bad) break;
                            if(blocks[blocks[i].idom].pdom != i) bad = true;
                        }

                        // check post-dominator condition
                        if(!bad)
                        for(int i = pb; i; i = blocks[i].idom)
                        {
                            // if the other one skips phis, then we'll bail out
                            // and we'll try again the other way later
                            for(auto & a : blocks[i].args)
                            for(auto & s : a.alts)
                            {
                                if(op.index == s.val) renameThis = true;
                            }
                            
                            if(ccd == blocks[i].idom) break; // this is fine
                            if(bad) break;
                            if(blocks[blocks[i].idom].pdom != i) bad = true;
                        }

                        // don't try to rename both (could be fixed)
                        if(renameThis && renameOther) bad = true;

                        if(bad) { cseTable.insert(op); continue; }

                        if(ccd == ptr->block)
                        {
                            rename.add(op.index, ptr->index);
                            if(renameThis)
                            {
                                op.opcode = ops::rename;
                                op.in[0] = other.index;
                                op.in[1] = noVal;
                            } else op.makeNOP();
                            
                            progress = true;
                            continue;
                        }
                        else if(ccd == b)
                        {
                            rename.add(other.index, op.index);

                            if(renameOther)
                            {
                                other.opcode = ops::rename;
                                other.in[0] = op.index;
                                other.in[1] = noVal;

                            } else other.makeNOP();
                            
                            cseTable.insert(op);
                            
                            progress = true;
                            continue;
                        }
                        else if(!renameThis)
                        {
                            bc = noVal;
                            op.block = ccd;
    
                            // try to move this op backwards
                            int k = blocks[ccd].code.size();
                            blocks[ccd].code.push_back(op.index);
                            while(k--)
                            {
                                // don't move past anything with sideFX
                                // but DO move past jumps
                                if(blocks[ccd].code[k] != noVal
                                && ops[blocks[ccd].code[k]].opcode > ops::jmp
                                && !ops[blocks[ccd].code[k]].canMove()) break;
                                
                                // sanity check that we don't move past inputs
                                bool canMove = true;
                                for(int j = 0; j < op.nInputs(); ++j)
                                {
                                    if(blocks[ccd].code[k] != op.in[j]) continue;
                                    canMove = false;
                                    break;
                                }

                                if(!canMove) break;
    
                                // move
                                std::swap(blocks[ccd].code[k],
                                    blocks[ccd].code[k+1]);
                            }

                            if(renameOther)
                            {
                                other.opcode = ops::rename;
                                other.in[0] = op.index;
                                other.in[1] = noVal;
                            }
                            else
                            {
                                other.makeNOP();
                                rename.add(other.index, op.index);
                            }
                            cseTable.insert(op);
                            progress = true;
                        }
                        else if(!renameOther)
                        {
                            // expensive case, need to find the other op
                            int p = blocks[other.block].code.size();
                            while(p && other.index != blocks[other.block].code[--p]);

                            assert(blocks[other.block].code[p] == other.index);

                            blocks[other.block].code[p] = noVal;
                            other.block = ccd;
                            
                            int k = blocks[ccd].code.size();
                            blocks[ccd].code.push_back(other.index);
                            while(k--)
                            {
                                // don't move past anything with sideFX
                                // but DO move past jumps
                                if(blocks[ccd].code[k] != noVal
                                && ops[blocks[ccd].code[k]].opcode > ops::jmp
                                && !ops[blocks[ccd].code[k]].canMove()) break;
                                
                                // sanity check that we don't move past inputs
                                bool canMove = true;
                                for(int j = 0; j < other.nInputs(); ++j)
                                {
                                    if(blocks[ccd].code[k] != other.in[j]) continue;
                                    canMove = false;
                                    break;
                                }

                                if(!canMove) break;
    
                                // move
                                std::swap(blocks[ccd].code[k],
                                    blocks[ccd].code[k+1]);
                            }

                            if(renameThis)
                            {
                                op.opcode = ops::rename;
                                op.in[0] = other.index;
                                op.in[1] = noVal;
                            }
                            else
                            {
                                op.makeNOP();
                                rename.add(op.index, other.index);
                            }
                        }
                        else
                        {
                            // bail out if both sides need phis
                            cseTable.insert(op); continue;
                        }
                    }
                    else 
                    {
                        // walk up the idom chain
                        auto mblock = b;
                        while(mblock)
                        {
                            bool done = false;
                            for(int k = 0; k < op.nInputs(); ++k)
                            {
                                if(mblock != ops[op.in[k]].block) continue;
                                done = true;
                                break;
                            }
                            if(done) break;

                            // sanity check post-dominators
                            auto idom = blocks[mblock].idom;
                            if(blocks[idom].pdom != mblock) break;
                                
                            mblock = blocks[mblock].idom;
                        }

                        // don't hoist if the actual target branches
                        // we allow CSE in this case, but don't pull single
                        // operations outside loops into them
                        if(ops[blocks[mblock].code.back()].opcode < ops::jmp)
                        {
                            mblock = b;
                        }

                        // if mblock is the current block, then we can't move
                        if(mblock != b)
                        {
                            bc = noVal;
                            op.block = mblock;
    
                            // try to move the new op backwards
                            int k = blocks[mblock].code.size();
                            blocks[mblock].code.push_back(op.index);
                            while(k--)
                            {
                                // don't move past anything with sideFX
                                // but DO move past jumps
                                if(blocks[mblock].code[k] != noVal
                                && ops[blocks[mblock].code[k]].opcode > ops::jmp
                                && !ops[blocks[mblock].code[k]].canMove()) break;
                                
                                // sanity check that we don't move past inputs
                                bool canMove = true;
                                for(int j = 0; j < op.nInputs(); ++j)
                                {
                                    if(blocks[mblock].code[k] != op.in[j]) continue;
                                    canMove = false;
                                    break;
                                }

                                if(!canMove) break;
    
                                // move
                                std::swap(blocks[mblock].code[k],
                                    blocks[mblock].code[k+1]);
                            }
                        }
                        
                        cseTable.insert(op);
                    }
                }
            }
        }
        if(progress) anyProgress = true;
    }

    printf(" Fold:%d\n", iter);

    return anyProgress;
}