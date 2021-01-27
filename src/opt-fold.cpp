
#include "ir.h"

#include "hash.h"

using namespace bjit;

// This stores the data CSE needs in our hash table.
// Keep this packed POD so we can use memcpy and stringHash
struct OpCSE
{
    uint16_t index = noVal;
    uint16_t opcode = noVal;
    uint16_t in[2] = { noVal, noVal };
    uint32_t imm32 = 0;

    OpCSE() {}
    OpCSE(uint16_t i, Op const & op) { set(i, op); }

    void set(uint16_t i, Op const & op)
    {
        index = i;
        opcode = op.opcode;
        in[0] = op.nInputs() >= 1 ? op.in[0] : noVal;
        in[1] = op.nInputs() >= 2 ? op.in[1] : noVal;
        imm32 = op.hasImm32() ? op.imm32 : 0;
    }

    bool isEqual(Op const & op) const
    { OpCSE tmp(noVal, op); return isEqual(tmp); }
    bool isEqual(OpCSE const & op) const
    { return !memcmp(&opcode, &op.opcode, 2+4+4); }

    static uint64_t getHash(Op const & op)
    { OpCSE tmp(noVal, op); return getHash(tmp); }
    static uint64_t getHash(OpCSE const & op)
    { return stringHash64((uint8_t*)&op.opcode, 2+4+4); }
};

// match op
#define I(x) (i.opcode == x)

// match operands
#define I0(x) (i.nInputs() >= 1 && ops[i.in[0]].opcode == x)
#define I1(x) (i.nInputs() >= 2 && ops[i.in[1]].opcode == x)

// check for constants
#define C0 (i.nInputs() >= 1 && (I0(ops::lci) || I0(ops::lcf)))
#define C1 (i.nInputs() >= 2 && (I1(ops::lci) || I1(ops::lcf)))

// fetch operands
#define N0 ops[i.in[0]]
#define N1 ops[i.in[1]]


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
        ++iter;
        progress = false;
        for(auto b : live)
        {
            cseTable.clear();
            
            for(auto bc : blocks[b].code)
            {
                auto & i = ops[bc];
                rename(i);

                // CSE
                if(i.canCSE())
                {
                    auto * ptr = cseTable.find(i);
                    if(ptr)
                    {
                        assert(ptr->index != noVal);
                        rename.add(bc, ptr->index);

                        if(0)
                        {
                            printf("CSE:\n");
                            debugOp(ptr->index);
                            debugOp(bc);
                        }
                        
                        i.opcode = ops::nop;
                        progress = true;
                        continue;
                    }
                }
    
                // rename phis
                if(i.opcode <= ops::jmp)
                {
                    for(auto & a : blocks[i.label[0]].args)
                    for(auto & s : a.alts)
                    for(auto & r : rename.map)
                    {
                        if(s.val == r.src) s.val = r.dst;
                    }
                }
    
                if(i.opcode < ops::jmp)
                {
                    for(auto & a : blocks[i.label[1]].args)
                    for(auto & s : a.alts)
                    for(auto & r : rename.map)
                    {
                        if(s.val == r.src) s.val = r.dst;
                    }
                }
    
                // relive constant to second operand for associative ops
                // also do this for comparisons, negating the condition
                if(C0 && !C1)
                switch(i.opcode)
                {
                    case ops::jieq: case ops::jine:
                    case ops::ieq: case ops::ine:
                    case ops::jfeq: case ops::jfne:
                    case ops::feq: case ops::fne:
                    case ops::iadd: case ops::imul:
                    case ops::fadd: case ops::fmul:
                    case ops::iand: case ops::ior: case ops::ixor:
                        std::swap(i.in[0], i.in[1]);
                        progress = true;
                        break;
    
                    // we can replace with negate if constant is zero
                    case ops::isub:
                        if(!ops[i.in[1]].i64)
                        {
                            i.opcode = ops::ineg;
                            i.in[0] = i.in[1];
                        }
                        break;
    
                    case ops::jilt: case ops::jige: case ops::jigt: case ops::jile:
                    case ops::jult: case ops::juge: case ops::jugt: case ops::jule:
                    case ops::jflt: case ops::jfge: case ops::jfgt: case ops::jfle:
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
                        // we don't have an invariant for swapping operands
                        // for comparisons, so we have to do this manually
                        switch(i.opcode)
                        {
                        case ops::jilt: i.opcode = ops::jigt; break;
                        case ops::jige: i.opcode = ops::jile; break;
                        case ops::jigt: i.opcode = ops::jilt; break;
                        case ops::jile: i.opcode = ops::jige; break;
                        
                        case ops::jult: i.opcode = ops::jugt; break;
                        case ops::juge: i.opcode = ops::jule; break;
                        case ops::jugt: i.opcode = ops::jult; break;
                        case ops::jule: i.opcode = ops::juge; break;
                        
                        case ops::jflt: i.opcode = ops::jfgt; break;
                        case ops::jfge: i.opcode = ops::jfle; break;
                        case ops::jfgt: i.opcode = ops::jflt; break;
                        case ops::jfle: i.opcode = ops::jfge; break;
    
                        case ops::ilt: i.opcode = ops::igt; break;
                        case ops::ige: i.opcode = ops::ile; break;
                        case ops::igt: i.opcode = ops::ilt; break;
                        case ops::ile: i.opcode = ops::ige; break;
                        
                        case ops::ult: i.opcode = ops::ugt; break;
                        case ops::uge: i.opcode = ops::ule; break;
                        case ops::ugt: i.opcode = ops::ult; break;
                        case ops::ule: i.opcode = ops::uge; break;
                        
                        case ops::flt: i.opcode = ops::fgt; break;
                        case ops::fge: i.opcode = ops::fle; break;
                        case ops::fgt: i.opcode = ops::flt; break;
                        case ops::fle: i.opcode = ops::fge; break;
                        }
                        std::swap(i.in[0], i.in[1]);
    
                        progress = true;
                        break;
                }
    
                // simplify ieqI/ineI into inverted test when possible
                // don't bother if there are other uses
                if((I(ops::ieqI) || I(ops::ineI)) && !i.i64 && N0.nUse == 1)
                {
                    switch(N0.opcode)
                    {
                    case ops::ilt: case ops::ige: case ops::igt: case ops::ile:
                    case ops::ult: case ops::uge: case ops::ugt: case ops::ule:
                    case ops::flt: case ops::fge: case ops::fgt: case ops::fle:
    
                    case ops::iltI: case ops::igeI: case ops::igtI: case ops::ileI:
                    case ops::ultI: case ops::ugeI: case ops::ugtI: case ops::uleI:
                        {
                            bool negate = (i.opcode == ops::ieqI);
                            auto ii = N0;
                            i.opcode = N0.opcode;
                            if(N0.nInputs() == 2) i.in[1] = N0.in[1];
                            if(N0.hasImm32()) i.imm32 = N0.imm32;
                            i.in[0] = N0.in[0];
                            if(negate) i.opcode ^= 1;
                            progress = true;
                        }
                    }
                }
    
                if(I1(ops::lci) && N1.i64 == (int32_t) N1.i64)
                switch(i.opcode)
                {
                    case ops::jilt: case ops::jige:
                    case ops::jigt: case ops::jile:
                    case ops::jieq: case ops::jine:
                    case ops::jult: case ops::juge:
                    case ops::jugt: case ops::jule:
                        i.opcode += ops::jiltI - ops::jilt;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    
                    case ops::ilt: case ops::ige:
                    case ops::igt: case ops::ile:
                    case ops::ieq: case ops::ine:
                    case ops::ult: case ops::uge:
                    case ops::ugt: case ops::ule:
                        i.opcode += ops::iltI - ops::ilt;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    
                    case ops::iadd:
                        i.opcode = ops::iaddI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::isub:
                        // FIXME: rewrite as iaddI with negated immediated?
                        i.opcode = ops::isubI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::imul:
                        i.opcode = ops::imulI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::iand:
                        i.opcode = ops::iandI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::ior:
                        i.opcode = ops::iorI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::ixor:
                        i.opcode = ops::ixorI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::ishl:
                        i.opcode = ops::ishlI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::ishr:
                        i.opcode = ops::ishrI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                    case ops::ushr:
                        i.opcode = ops::ushrI;
                        i.imm32 = (int32_t) N1.i64;
                        progress = true;
                        break;
                }
                
                // fold conditions into jumps
                if((I(ops::jz) || I(ops::jnz)) && N0.nUse == 1
                && (N0.opcode >= ops::ilt && N0.opcode - ops::ilt <= ops::jfne))
                {
                    int negate = (i.opcode == ops::jz) ? 1 : 0;
                    i.opcode = negate ^ (N0.opcode + ops::jilt - ops::ilt);
                    i.in[1] = N0.in[1];
                    i.in[0] = N0.in[0];
                    progress = true;
                }
                
                // fold conditions into jumps
                if((I(ops::jz) || I(ops::jnz)) && N0.nUse == 1
                && (N0.opcode >= ops::iltI && N0.opcode - ops::iltI <= ops::jine))
                {
                    int negate = (i.opcode == ops::jz) ? 1 : 0;
                    i.opcode = negate ^ (N0.opcode + ops::jiltI - ops::iltI);
                    i.imm32 = N0.imm32;
                    i.in[0] = N0.in[0];
                    progress = true;
                }
    
                
                // simplify jieqI/jineI into jz/jnz when possible
                if((I(ops::jieqI) || I(ops::jineI)) && !i.imm32)
                {
                    i.opcode = I(ops::jieqI) ? ops::jz : ops::jnz;
                    progress = true;
                }
    
                // -(-a) = a
                if(I(ops::ineg) && I0(ops::ineg)) rename.add(bc, N0.in[0]);
                if(I(ops::fneg) && I0(ops::fneg)) rename.add(bc, N0.in[0]);
                
                // a + 0, a - 0, a * 1 -> a
                if((I(ops::iaddI) && !i.imm32)
                || (I(ops::isubI) && !i.imm32)
                || (I(ops::imulI) && 1 == i.imm32))
                {
                    rename.add(bc, i.in[0]);
                    progress = true;
                    i.opcode = ops::nop;
                    continue;
                }

                if((I(ops::fadd) && I1(ops::lcf) && N1.f64 == 0.)
                || (I(ops::fsub) && I1(ops::lcf) && N1.f64 == 0.)
                || (I(ops::fmul) && I1(ops::lcf) && N1.f64 == 1.))
                {
                    rename.add(bc, i.in[0]);
                    progress = true;
                    i.opcode = ops::nop;
                    continue;                    
                }
                
                // a * -1 -> -a
                if(I(ops::imulI) && -1 == i.i64)
                {
                    i.opcode = ops::ineg; progress = true;
                }
                if(I(ops::fmul) && I1(ops::lcf) && N1.f64 == -1.)
                {
                    i.opcode = ops::fneg; progress = true;
                }
                
                // a + (-b) = a-b
                if(I(ops::iadd) && I1(ops::ineg))
                {
                    i.opcode = ops::isub;
                    i.in[1] = N1.in[0];
                    progress = true;
                }
                if(I(ops::fadd) && I1(ops::fneg))
                {
                    i.opcode = ops::fsub;
                    i.in[1] = N1.in[0];
                    progress = true;
                }
                
    
                // a - (-b) = a + b
                if(I(ops::isub) && I1(ops::ineg))
                {
                    i.opcode = ops::iadd;
                    i.in[1] = N1.in[0];
                    progress = true;
                }
                if(I(ops::fsub) && I1(ops::fneg))
                {
                    i.opcode = ops::fadd;
                    i.in[1] = N1.in[0];
                    progress = true;
                }
    
                // -a + b = b - a
                if(I(ops::iadd) && I0(ops::ineg))
                {
                    i.opcode = ops::isub;
                    i.in[1] = N0.in[0];
                    std::swap(i.in[0], i.in[1]);
                    progress = true;
                }
                if(I(ops::fadd) && I0(ops::fneg))
                {
                    i.opcode = ops::fsub;
                    i.in[1] = N0.in[0];
                    std::swap(i.in[0], i.in[1]);
                    progress = true;
                }
    
                // addition immediate merges
                if(I(ops::iaddI) && I0(ops::iaddI))
                {
                    int64_t v = (int64_t) i.imm32 + (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        i.in[0] = N0.in[0];
                        i.imm32 = v;
                        progress = true;
                    }
                }
                if(I(ops::isubI) && I0(ops::isubI))
                {
                    int64_t v = (int64_t) i.imm32 + (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        i.in[0] = N0.in[0];
                        i.imm32 = v;
                        progress = true;
                    }
                }
                if(I(ops::iaddI) && I0(ops::isubI))
                {
                    int64_t v = (int64_t) i.imm32 - (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        i.in[0] = N0.in[0];
                        i.imm32 = v;
                        progress = true;
                    }
                }
                if(I(ops::isubI) && I0(ops::iaddI))
                {
                    int64_t v = (int64_t) i.imm32 - (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        i.in[0] = N0.in[0];
                        i.imm32 = v;
                        progress = true;
                    }
                }
    
                // merge shift into multiply
                if(I(ops::imulI) && I0(ops::ishlI))
                {
                    int64_t v = (int64_t) i.imm32 * (((int64_t)1) << N0.imm32);
                    if(v == (int32_t) v)
                    {
                        i.in[0] = N0.in[0];
                        i.imm32 = v;
                        progress = true;
                    }
                }
    
                // multiplication immediate merges
                if(I(ops::imulI) && I0(ops::imulI))
                {
                    int64_t v = (int64_t) i.imm32 * (int64_t) N0.imm32;
                    if(v == (int32_t) v)
                    {
                        i.in[0] = N0.in[0];
                        i.imm32 = v;
                        progress = true;
                    }
                }
    
                // can we replace multiply with shift?
                if(I(ops::imulI) && !(i.imm32 & (i.imm32 - 1)))
                {
                    uint32_t b = i.imm32;
                    int shift = 0; while(b >>= 1) ++shift;
                    i.opcode = ops::ishlI;
                    i.imm32 = shift;
                    progress = true;
                }
    
                // FIXME: add NAND and NOR to simplify
                // some constructs involving bitwise NOT?
    
                // ~(~a) = a
                if(I(ops::inot) && I0(ops::inot))
                {
                    rename.add(bc, N0.in[0]);
                    progress = true;
                    i.opcode = ops::nop;
                    continue;
                }
                
                // bit-AND merges : (a&b)&c = a&(b&c)
                if(I(ops::iandI) && I0(ops::iandI))
                {
                    i.imm32 &= N0.imm32;
                    i.in[0] = N0.in[0];
                    progress = true;
                }
    
                // bit-OR merges : (a|b)|c = a|(b|c)
                if(I(ops::iorI) && I0(ops::iorI))
                {
                    i.imm32 |= N0.imm32;
                    i.in[0] = N0.in[0];
                    progress = true;
                }
    
                // bit-XOR merges : (a^b)^c = a^(b^c)
                if(I(ops::ixorI) && I0(ops::ixorI))
                {
                    i.imm32 ^= N0.imm32;
                    i.in[0] = N0.in[0];
                    progress = true;
                }
    
                if(I(ops::ixorI) && !~i.imm32)
                {
                    i.opcode = ops::inot;
                    progress = true;
                }
    
                if(I(ops::ixorI) && !i.imm32)
                {
                    rename.add(bc, i.in[0]);
                    progress = true;
                    i.opcode = ops::nop;
                    continue;
                }
    
                if(C0)
                switch(i.opcode)
                {
                    case ops::iret:
                        if(N0.i64 == (int32_t) N0.i64)
                        {
                            i.opcode = ops::iretI;
                            i.imm32 = N0.i64;
                            progress = true;
                        }
                        break;
                    case ops::jz:
                        i.opcode = ops::jmp;
                        if(N0.i64) i.label[0] = i.label[1];
                        progress = true;
                        break;
                        
                    case ops::jnz:
                        i.opcode = ops::jmp;
                        if(!N0.i64) i.label[0] = i.label[1];
                        progress = true;
                        break;
    
                    case ops::jiltI:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 < (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jigeI:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 >= (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jigtI:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 > (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jileI:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 <= (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
    
                    case ops::jieqI:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 == (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jineI:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 != (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                        
                    case ops::jultI:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 < (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jugeI:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 >= (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jugtI:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 > (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::juleI:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 <= (int64_t)i.imm32)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                        
                    case ops::iltI:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 < i.imm32);
                        progress = true;
                        break;
                    case ops::igeI:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 >= i.imm32);
                        progress = true;
                        break;
                    case ops::igtI:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 > i.imm32);
                        progress = true;
                        break;
                    case ops::ileI:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 <= i.imm32);
                        progress = true;
                        break;
    
                    case ops::ieqI:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 == i.imm32);
                        progress = true;
                        break;
                    case ops::ineI:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 != i.imm32);
                        progress = true;
                        break;
                        
                    case ops::ultI:
                        i.opcode = ops::lci;
                        i.u64 = (N0.u64 < (int64_t)i.imm32);
                        progress = true;
                        break;
                    case ops::ugeI:
                        i.opcode = ops::lci;
                        i.u64 = (N0.u64 >= (int64_t)i.imm32);
                        progress = true;
                        break;
                    case ops::ugtI:
                        i.opcode = ops::lci;
                        i.u64 = (N0.u64 > (int64_t)i.imm32);
                        progress = true;
                        break;
                    case ops::uleI:
                        i.opcode = ops::lci;
                        i.u64 = (N0.u64 <= (int64_t)i.imm32);
                        progress = true;
                        break;
                        
                    // arithmetic
                    case ops::iaddI:
                        i.i64 = N0.i64 + (int64_t)i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::isubI:
                        i.i64 = N0.i64 - (int64_t)i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ineg:
                        i.i64 = -N0.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::imulI:
                        i.i64 = N0.i64 * (int64_t)i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                         
                    // bitwise
                    case ops::inot:
                        i.i64 = ~N0.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    
                    case ops::iandI:
                        i.i64 = N0.i64 & (int64_t)i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::iorI:
                        i.i64 = N0.i64 | (int64_t)i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ixorI:
                        i.i64 = N0.i64 ^ (int64_t)i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
    
                    // bitshifts
                    case ops::ishlI:
                        i.i64 = N0.i64 << i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                        
                    case ops::ishrI:
                        // this relies on compiler giving signed shift
                        i.i64 = N0.i64 >> i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
    
                    case ops::ushrI:
                        // this relies on compiler giving unsigned shift
                        i.u64 = N0.u64 >> i.imm32;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
    
                    case ops::fneg:
                        i.f64 = -N0.f64;
                        i.opcode = ops::lcf;
                        progress = true;
                        break;
                }
    
                // This has some redundancy with immediate versions..
                // Get rid of those once we've got rid of IMM32
                if(C0 && C1)
                switch(i.opcode)
                {
                    case ops::jilt:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 < N1.i64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jige:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 >= N1.i64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jigt:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 > N1.i64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jile:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 <= N1.i64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
    
                    case ops::jieq:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 == N1.i64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jine:
                        i.opcode = ops::jmp;
                        if(!(N0.i64 != N1.i64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                        
                    case ops::jult:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 < N1.u64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::juge:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 >= N1.u64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jugt:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 > N1.u64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jule:
                        i.opcode = ops::jmp;
                        if(!(N0.u64 <= N1.u64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                        
                    case ops::jfeq:
                        i.opcode = ops::jmp;
                        if(!(N0.f64 == N1.f64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jfne:
                        i.opcode = ops::jmp;
                        if(!(N0.f64 != N1.f64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jflt:
                        i.opcode = ops::jmp;
                        if(!(N0.f64 < N1.f64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jfge:
                        i.opcode = ops::jmp;
                        if(!(N0.f64 >= N1.f64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jfgt:
                        i.opcode = ops::jmp;
                        if(!(N0.f64 > N1.f64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
                    case ops::jfle:
                        i.opcode = ops::jmp;
                        if(!(N0.f64 <= N1.f64)) i.label[0] = i.label[1];
                        progress = true;
                        break;
    
                    case ops::ilt:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 < N1.i64);
                        progress = true;
                        break;
                    case ops::ige:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 >= N1.i64);
                        progress = true;
                        break;
                    case ops::igt:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 > N1.i64);
                        progress = true;
                        break;
                    case ops::ile:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 <= N1.i64);
                        progress = true;
                        break;
    
                    case ops::ieq:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 == N1.i64);
                        progress = true;
                        break;
                    case ops::ine:
                        i.opcode = ops::lci;
                        i.i64 = (N0.i64 != N1.i64);
                        progress = true;
                        break;
                        
                    case ops::ult:
                        i.opcode = ops::lci;
                        i.i64 = (N0.u64 < N1.u64);
                        progress = true;
                        break;
                    case ops::uge:
                        i.opcode = ops::lci;
                        i.i64 = (N0.u64 >= N1.u64);
                        progress = true;
                        break;
                    case ops::ugt:
                        i.opcode = ops::lci;
                        i.i64 = (N0.u64 > N1.u64);
                        progress = true;
                        break;
                    case ops::ule:
                        i.opcode = ops::lci;
                        i.i64 = (N0.u64 <= N1.u64);
                        progress = true;
                        break;
    
                    // Floating point versions
                    // FIXME: These never get converted to immediates
                    case ops::feq:
                        i.opcode = ops::lci;
                        i.i64 = (N0.f64 == N1.f64);
                        progress = true;
                        break;
                    case ops::fne:
                        i.opcode = ops::lci;
                        i.i64 = (N0.f64 != N1.f64);
                        progress = true;
                        break;
                        
                    case ops::flt:
                        i.opcode = ops::lci;
                        i.i64 = (N0.f64 < N1.f64);
                        progress = true;
                        break;
                    case ops::fge:
                        i.opcode = ops::lci;
                        i.i64 = (N0.f64 >= N1.f64);
                        progress = true;
                        break;
                    case ops::fgt:
                        i.opcode = ops::lci;
                        i.i64 = (N0.f64 > N1.f64);
                        progress = true;
                        break;
                    case ops::fle:
                        i.opcode = ops::lci;
                        i.i64 = (N0.f64 <= N1.f64);
                        progress = true;
                        break;
                    
                    // arithmetic
                    case ops::iadd:
                        i.i64 = N0.i64 + N1.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::isub:
                        i.i64 = N0.i64 - N1.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::imul:
                        i.i64 = N0.i64 * N1.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::idiv:
                        // preserve division by zero!
                        if(N1.i64)
                        {
                            i.i64 = N0.i64 / N1.i64;
                            i.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                    case ops::imod:
                        // preserve division by zero!
                        if(N1.i64)
                        {
                            i.i64 = N0.i64 % N1.i64;
                            i.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                    case ops::udiv:
                        if(N1.u64)
                        {
                            i.u64 = N0.u64 / N1.u64;
                            i.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                    case ops::umod:
                        if(N1.u64)
                        {
                            i.u64 = N0.u64 % N1.u64;
                            i.opcode = ops::lci;
                            progress = true;
                        }
                        break;
                         
                    // bitwise
                    case ops::iand:
                        i.i64 = N0.i64 & N1.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ior:
                        i.i64 = N0.i64 | N1.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
                    case ops::ixor:
                        i.i64 = N0.i64 ^ N1.i64;
                        i.opcode = ops::lci;
                        progress = true;
                        break;
    
                }

                // if we're here, insert to CSE table
                OpCSE cse(bc, i);
                cseTable.insert(cse);
            }
        }
        if(progress) anyProgress = true;
    }

    printf(" Fold:%d\n", iter);

    return anyProgress;
}