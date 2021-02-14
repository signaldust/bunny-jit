
#include "bjit.h"

#include <vector>
#include <algorithm>    // use STL heapsort

using namespace bjit;

static const bool cse_debug = false;    // print decisions

void Proc::rebuild_memtags(bool unsafeOpt)
{
    for(auto b : live)
    {
        // reset block tags to noVal, we'll solve these in 2nd pass
        blocks[b].memtag = noVal;
        
        // we start each block with noVal and then let CSE
        // lookup the actual block-tag if load's tag is noVal
        uint16_t memtag = noVal;
        
        for(auto c : blocks[b].code)
        {
            if(c == noVal) continue;
            
            if(ops[c].opcode > ops::jmp
            && ops[c].hasSideFX() && (!unsafeOpt || !ops[c].canCSE()))
            {
                memtag = c;
            }

            if(ops[c].hasMemTag())
            {
                ops[c].in[1] = memtag;
            }
        }

        // save output tag
        blocks[b].memout = memtag;
    }

    // iterate incoming tags, should usually converge in 2-3 rounds
    bool progress = true;
    while(progress)
    {
        progress = false;
        for(auto b : live)
        {
            // did we already pick unique tag for this block?
            if(blocks[b].memtag != noVal
            && ops[blocks[b].memtag].block == b) continue;
            
            for(auto cf : blocks[b].comeFrom)
            {
                // does the incoming edge have a tag?
                if(blocks[cf].memout == noVal) continue;
                
                // do tags already match?
                if(blocks[cf].memout == blocks[b].memtag) continue;

                // does this block have a tag?
                if(blocks[b].memtag == noVal)
                {
                    blocks[b].memtag = blocks[cf].memout;
                    // pass to output unless we have something better
                    if(blocks[b].memout == noVal)
                    {
                        blocks[b].memout = blocks[b].memtag;
                        progress = true;
                    }
                }
                else
                {
                    // incoming tags don't match, pick a unique one
                    // we'll pick the last op in the block (jump, return)
                    // as this isn't going to be moved or removed
                    blocks[b].memtag = blocks[b].code.back();
                    
                    // if we have an output tag that's from another block
                    // then we need to update that as well; otherwise we have
                    // local sideFX and should keep the existing out tag
                    if(ops[blocks[b].memout].block != b)
                    {
                        blocks[b].memout = blocks[b].memtag;
                        progress = true;
                    }
                }
            }
        }
    }
}

/*

 We do three passes here:
   1. hoist and collect potential pairs by matching with hash
   2. expand all the potential pairs, then do CSE if possible
   3. finally do a rename+cleanup pass to fix the code

*/
bool Proc::opt_cse(bool unsafeOpt)
{
    rebuild_dom();
    rebuild_memtags(unsafeOpt);
    
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
            if(!op.canCSE() || (!unsafeOpt && op.hasSideFX())) continue;

            // update memtag to that of block if we need one
            if(op.hasMemTag() && op.in[1] == noVal) op.in[1] = blocks[b].memtag;

            // always try to hoist first?
            // walk up the idom chain
            auto mblock = b;

            // try to find a better block, but don't if we just sunk this
            // if this is not sunken and we hoist it into a branching block
            // then next SINK pass will break a critical edge for us
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

                // if this is a load, then don't hoist into a block
                // with a different memory tag
                if(op.hasMemTag()
                && blocks[blocks[mblock].idom].memout != op.in[1]) break;

                // if this is a load, don't hoist from a non-post-dominator
                // NOTE/FIXME: we currently need this to preserve null-pointer
                // checks, but really we should check if there's a conditional
                // branch of something the load address depends on
                if(op.hasMemTag()
                && blocks[blocks[mblock].idom].pdom != mblock) break;

                mblock = blocks[mblock].idom;
            }

            // if mblock is the current block, then we can't move
            if(mblock != b)
            {
                if(cse_debug)
                    BJIT_LOG("\nhoisting %04x: %d -> %d", op.index, b, mblock);
                    
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

                    // sanity check that we don't move loads past sideFX
                    if(op.hasMemTag() &&
                    ops[blocks[mblock].code[k]].hasSideFX()) break;

                    // move
                    std::swap(blocks[mblock].code[k],
                        blocks[mblock].code[k+1]);
                }
            } else if(cse_debug)
                BJIT_LOG("\ncan't move %04x from %d", op.index, b);
                 

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

    std::vector<uint16_t>   preList;

    // check for PRE
    auto checkPre = [&](OpCSE & cse)
    {
        auto & op = ops[cse.index];

        // no PRE on loads
        if(op.hasMemTag()) return;

        bool hasPhi = false;

        // fast path check whether there's any point trying :)
        if(op.nInputs() >= 1
        && ops[op.in[0]].opcode == ops::phi
        && ops[op.in[0]].block == op.block) hasPhi = true;
        if(op.nInputs() >= 2
        && ops[op.in[1]].opcode == ops::phi
        && ops[op.in[1]].block == op.block) hasPhi = true;

        if(!hasPhi) return;
        
        // in the interest of simplicity, we rely on hoisting to pull
        // us to the phis we're going to check.. this is not perfect
        // but it covers the most obvious cases (especially with CSE)
        auto & mb = blocks[op.block];

        preList.clear();
        preList.insert(preList.begin(), mb.comeFrom.size(), noVal);

        int matches = false;

        // match for each potential source
        for(int c = 0; c < mb.comeFrom.size(); ++c)
        {
            auto cf = mb.comeFrom[c];
            
            OpCSE match(op);
            for(auto & a : mb.alts)
            {
                if(a.src != cf) continue;
                if(a.phi == match.in[0]) match.in[0] = a.val;
                if(a.phi == match.in[1]) match.in[1] = a.val;
            }

            auto & cfdom = blocks[cf].dom;
            
            // match
            auto * p = cseTable.find(match);
            if(p)
            {
                // check dominance
                if(cfdom[blocks[p->block].dom.size()-1] == p->block)
                {
                    preList[c] = p->index;
                    matches = true;
                }
                else
                {
                    auto it = std::lower_bound(
                        pairs.begin(), pairs.end(), p->index << 16);

                    while((*it)>>16 == p->index)
                    {
                        auto & alt = ops[(*it)&noVal];
                        if(cfdom[blocks[alt.block].dom.size()-1] == alt.block)
                        {
                            preList[c] = alt.index;
                            matches = true;
                            break;
                        }
                        ++it;
                    }
                }
            }
        }

        if(!matches) return;

        // found either a partial or full redundancy:
        if(cse_debug) BJIT_LOG("\nPRE: L%d:%04x matches with:", op.block, op.index);
            
        // insert a new phi into the matched block
        auto phi = newOp(ops::phi, op.flags.type, op.block);
        mb.code.insert(mb.code.begin(), phi);
            
        for(int c = 0; c < mb.comeFrom.size(); ++c)
        {

            // partial redundancy?
            //
            // FIXME: this duplicates work form above
            if(preList[c] == noVal)
            {
                OpCSE match(op);
                for(auto & a : mb.alts)
                {
                    if(a.src != mb.comeFrom[c]) continue;
                    if(a.phi == match.in[0]) match.in[0] = a.val;
                    if(a.phi == match.in[1]) match.in[1] = a.val;
                }

                preList[c] = newOp(op.opcode, op.flags.type, mb.comeFrom[c]);
                ops[preList[c]].in[0] = match.in[0];
                ops[preList[c]].in[1] = match.in[1];
                ops[preList[c]].imm32 = match.imm32;
                blocks[mb.comeFrom[c]].code.insert(
                    blocks[mb.comeFrom[c]].code.end()-1, preList[c]);

                if(cse_debug) BJIT_LOG("\n - L%d: %04x (added)",
                    mb.comeFrom[c], preList[c]);
            }
            else
            {
                if(cse_debug) BJIT_LOG("\n - L%d: %04x",
                    mb.comeFrom[c], preList[c]);
            }

            mb.newAlt(phi, mb.comeFrom[c], preList[c]);
        }

        rename.add(op.index, phi);
        op.makeNOP();
    };
    cseTable.foreach(checkPre);

    // returns true if we made some progress
    auto csePair = [&](Op & op0, Op & op1) -> bool
    {
        // did we already eliminate one of these
        if(op0.opcode == ops::nop) return false;
        if(op1.opcode == ops::nop) return false;

        auto b0 = op0.block;
        auto b1 = op1.block;
        
        if(cse_debug) BJIT_LOG("\nCSE: %04x (in %d) vs. %04x (in %d): ",
            op0.index, op0.block, op1.index, op1.block);

        // closest common dominator
        int ccd = 0;
        int iMax = std::min(
            blocks[b0].dom.size(), blocks[b1].dom.size());
            
        for(int i = 0; i < iMax; ++i)
        {
            if(blocks[b0].dom[i] == blocks[b1].dom[i]) ccd = blocks[b0].dom[i];
            else break;
        }

        if(cse_debug) BJIT_LOG(" CCD:%d ", ccd);

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
            if(op0.hasMemTag())
            {
                BJIT_ASSERT(op0.in[1] == blocks[ccd].memout);
            }
        
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
                
                // sanity check that we don't move loads past sideFX
                if(op0.hasMemTag() &&
                ops[blocks[ccd].code[k]].hasSideFX()) break;

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
        }
    }

    return anyProgress;
}