
#include "bjit.h"

#include <vector>
#include <algorithm>    // use STL heapsort

using namespace bjit;

static const bool cse_debug = false;    // print decisions

/*

 We do three passes here:
   1. hoist and collect potential pairs by matching with hash
   2. expand all the potential pairs, then do CSE if possible
   3. finally do a rename+cleanup pass to fix the code

*/
bool Proc::opt_cse(bool unsafe)
{
    impl::Rename rename;

    BJIT_LOG(" CSE");

    // pairs, packed into uint32_t for cheap sort
    BJIT_ASSERT(sizeof(uint32_t) == 2*sizeof(noVal));
    std::vector<uint32_t>   pairs;

    // clear hash
    cseTable.clear();
    if(cseTable.capacity() < liveOps) cseTable.reserve(liveOps);

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

            // always try to hoist first?
            // walk up the idom chain
            auto mblock = b;

            // try to find a better block, but don't if we just sunk this
            if(!op.flags.no_opt) while(mblock)
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

            // now check if we have another op in the table?
            // 
            auto * ptr = cseTable.find(op);

            if(!ptr)  cseTable.insert(op);
            else
            {
                // we should never find this op in the table anymore
                BJIT_ASSERT(ptr->index != op.index);

                // add to pairs, original op goes to MSB
                pairs.push_back(op.index + (uint32_t(ptr->index) << 16));
            }
        }
    }

    // found no pairs, we're done
    if(!pairs.size()) return false;

    // sort collected pairs
    std::make_heap(pairs.begin(), pairs.end());
    std::sort_heap(pairs.begin(), pairs.end());

    if(cse_debug)
    for(auto & p : pairs)
    {
        BJIT_LOG("\nCSE pairs: %04x vs. %04x: ", p>>16, p&noVal);
    }

    // returns true if we made some progress
    auto csePair = [&](Op & op0, Op & op1) -> bool
    {
        // did we already eliminate one of these
        if(op0.opcode == ops::nop) return false;
        if(op1.opcode == ops::nop) return false;

        auto b0 = op0.block;
        auto b1 = op1.block;
        
        if(cse_debug) BJIT_LOG("\nCSE: %04x vs. %04x: ", op0.index, op1.index);

        // closest common dominator
        int ccd = 0;
        int iMax = std::min(
            blocks[b0].dom.size(), blocks[b1].dom.size());
            
        for(int i = 0; i < iMax; ++i)
        {
            if(blocks[b0].dom[i] == blocks[b1].dom[i]) ccd = i;
            else break;
        }

        // we need to sanity check phis and post-doms
        bool bad = false;
        
        // check post-dominator condition
        for(int i = b0 ; i; i = blocks[i].idom)
        {
            // NOTE: DCE checks phis, so don't worry
            if(ccd == blocks[i].idom) break; // this is fine
            if(blocks[blocks[i].idom].pdom != i) bad = true;
            if(bad) break;
        }

        // check post-dominator condition
        if(!bad)
        for(int i = b1; i; i = blocks[i].idom)
        {
            // NOTE: DCE checks phis, so don't worry
            if(ccd == blocks[i].idom) break; // this is fine
            if(blocks[blocks[i].idom].pdom != i) bad = true;
            if(bad) break;
        }

        if(bad) { if(cse_debug) BJIT_LOG("BAD"); return false; }
        else if(b0 == b1)
        {
            // same block case, figure out which one is earlier
            BJIT_ASSERT(b0 == ccd);

            bool found = false;
            for(auto & c : blocks[ccd].code)
            {
                if(c == op0.index)
                {
                    if(cse_debug)
                        BJIT_LOG("GOOD: %04x first in block", op0.index);
                    rename.add(op1.index, op0.index);
                    op1.makeNOP();
                    op0.flags.no_opt = false;   // can optimize again
                    found = true;
                    break;
                }

                if(c == op1.index)
                {
                    if(cse_debug) 
                        BJIT_LOG("GOOD: %04x first in block", op1.index);
                    rename.add(op0.index, op1.index);
                    op0.makeNOP();
                    op1.flags.no_opt = false;   // can optimize again
                    found = true;
                    break;
                }
            }
            BJIT_ASSERT(found);
            return true;
        }
        else if(ccd == b1)
        {
            if(cse_debug) BJIT_LOG("GOOD: %04x in ccd", op1.index);
            rename.add(op0.index, op1.index);
            op0.makeNOP();
            op1.flags.no_opt = false;   // can optimize again

            return true;
        }
        else if(ccd == b0)
        {
            if(cse_debug) BJIT_LOG("GOOD: %04x in ccd", op0.index);
            rename.add(op1.index, op0.index);
            op1.makeNOP();
            op0.flags.no_opt = false;   // can optimize again
            
            return true;
        }
        else
        {
            if(cse_debug) BJIT_LOG("GOOD: move to CCD:%d", ccd);
        
            // NOTE: We do a lazy clear of the original position
            // in the rename pass below when block doesn't match.
            op0.block = ccd;

            // try to move this op backwards
            int k = blocks[ccd].code.size();
            blocks[ccd].code.push_back(op0.index);
            while(k--)
            {
                // don't move past anything with sideFX
                // but DO move past jumps
                if(blocks[ccd].code[k] != noVal
                && ops[blocks[ccd].code[k]].opcode > ops::jmp
                && !ops[blocks[ccd].code[k]].canMove()) break;
                
                // sanity check that we don't move past inputs
                bool canMove = true;
                for(int j = 0; j < op0.nInputs(); ++j)
                {
                    if(blocks[ccd].code[k] != op0.in[j]) continue;
                    canMove = false;
                    break;
                }

                if(!canMove) break;

                // move
                std::swap(blocks[ccd].code[k],
                    blocks[ccd].code[k+1]);
            }

            rename.add(op1.index, op0.index);
            op1.makeNOP();
            op0.flags.no_opt = false;   // can optimize again
            
            return true;
        }
        
        BJIT_ASSERT(false); // not reached
    };

    
    bool progress = true, anyProgress = false;
    while(progress)
    {
        progress = false;
        // try all possible candidate pairs
        int j = 0, limit = pairs.size();
        for(int i = 0; i < limit; ++i)
        {
            if(csePair(ops[pairs[i]>>16], ops[pairs[i]&noVal]))
            {
                progress = true;
            }
            
            // if representative op is different, flush
            if((pairs[j]&~noVal) != (pairs[i]&~noVal))
            {
                for(int p = j; p < i; ++p)
                {
                    for(int q = p+1; q < i; ++q)
                    {
                        if(csePair(ops[pairs[p]&noVal], ops[pairs[q]&noVal]))
                        {
                            progress = true;
                        }
                    }
                }
    
                j = i;
            }
        }
        // last set from j to limit
        for(int p = j; p < limit; ++p)
        {
            for(int q = p+1; q < limit; ++q)
            {
                if(csePair(ops[pairs[p]&noVal], ops[pairs[q]&noVal]))
                {
                    progress = true;
                }
            }
        }
    
        // don't need renames if we found nothing
        if(!progress) break;
        anyProgress = true;
    }

    // rename pass
    for(auto b : live)
    {
        for(auto & bc : blocks[b].code)
        {
            if(bc == noVal) continue;

            auto & op = ops[bc];

            // Check if we've moved the op to another block
            // and mark it as removed here if we did.
            if(op.block != b) { bc = noVal; continue; }

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

    return anyProgress;
}