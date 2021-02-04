
#include "bjit.h"
#include "hash.h"

#include <vector>
#include <algorithm>    // use STL heapsort?

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

/*

 We do three passes here:
   1. collect potential pairs into the hash
   2. try to combine ops, try to hoist ops
   3. final rename pass

*/
bool Proc::opt_cse(bool unsafe)
{
    Rename rename;
    bool progress = false;

    HashTable<OpCSE> cseTable(ops.size());

    // pairs, packed into uint32_t for cheap sort
    assert(sizeof(uint32_t) == 2*sizeof(noVal));
    std::vector<uint32_t>   pairs;

    // pair collection pass
    for(auto b : live)
    {
        for(auto & bc : blocks[b].code)
        {
            if(bc == noVal) continue;

            auto & op = ops[bc];

            if(op.opcode == ops::nop) { continue; }
            
            // CSE: do this after simplification
            if(!op.canCSE() || (!unsafe && op.hasSideFX())) continue;
            
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
                
                // check post-dominator condition
                for(int i = b ; i; i = blocks[i].idom)
                {
                    // NOTE: DCE checks phis, so don't worry
                    if(ccd == blocks[i].idom) break; // this is fine
                    if(blocks[blocks[i].idom].pdom != i) bad = true;
                    if(bad) break;
                }

                // check post-dominator condition
                if(!bad)
                for(int i = pb; i; i = blocks[i].idom)
                {
                    // NOTE: DCE checks phis, so don't worry
                    if(ccd == blocks[i].idom) break; // this is fine
                    if(blocks[blocks[i].idom].pdom != i) bad = true;
                    if(bad) break;
                }

                //printf("CSE: %04x, %04x: %s\n",
                //    op.index, other.index, bad ? "BAD" : "GOOD");

                if(bad) { /* fallback to hoisting */ }
                else if(ccd == ptr->block)
                {
                    rename.add(op.index, ptr->index);
                    op.makeNOP();
                    
                    progress = true;
                }
                else if(ccd == b)
                {
                    rename.add(other.index, op.index);
                    other.makeNOP();
                    
                    cseTable.insert(op);
                    
                    progress = true;
                }
                else
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

                    rename.add(other.index, op.index);
                    other.makeNOP();
                    
                    cseTable.insert(op);
                    progress = true;
                }
            }
            
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

            // if target branches, then just give up, for now
            // we don't want to hoist stuff back into loops
            //
            // we should add loop headers elsewhere
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

    // 
    std::make_heap(pairs.begin(), pairs.end());
    std::sort_heap(pairs.begin(), pairs.end());


    // rename pass
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
        }
    }

    return progress;
}