
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
                ops[c].memtag = memtag;
            }
        }

        // save output tag
        blocks[b].memout = memtag;
    }

    // iterate incoming tags - converges in a few rounds
    bool progress = true;
    while(progress)
    {
        progress = false;
        for(auto b : live)
        {
            uint16_t memtag = noVal;

            for(auto cf : blocks[b].comeFrom)
            {
                // if incoming edge doesn't have a tag, skip
                if(blocks[cf].memout == noVal) continue;

                // if this matches existing tag, skip
                if(blocks[cf].memout == memtag) continue;

                // if we don't have a tag, copy
                if(memtag == noVal)
                {
                    memtag = blocks[cf].memout;
                }
                else
                {
                    // tags don't match, bail out
                    memtag = blocks[b].code.back();
                    break;
                }
            }

            // if we updated tags, then repeat
            if(blocks[b].memtag != memtag) progress = true;

            // if this block is pass-thru, keep it pass-thru
            if(blocks[b].memtag == blocks[b].memout)
            {
                blocks[b].memout = memtag;
            }

            blocks[b].memtag = memtag;
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

    // we'll do a second round of dom-rebuilding
    // if we break edges when hoisting
    bool needRebuildDOM = false;

    // need DCE when hoisting, even if we don't make real progress
    bool needDCE = false;

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
            const auto opIndex = bc;
            if(opIndex == noVal) continue;

            auto & op = ops[opIndex];

            if(op.opcode == ops::nop) { continue; }
            
            // CSE: do this after simplification
            if(!op.canCSE() || (!unsafeOpt && op.hasSideFX())) continue;

            // update memtag to that of block if we need one
            if(op.hasMemTag() && op.memtag == noVal) op.memtag = blocks[b].memtag;

            // always try to hoist first?
            // walk up the idom chain
            auto mblock = b;

            // try to find a better block, but don't if we just sunk this
            // if this is not sunken and we hoist it into a branching block
            // then next SINK pass will break a critical edge for us
            //
            // don't bother if it's a constant
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
                && blocks[blocks[mblock].idom].memout != op.memtag) break;

                // if we're not the pdom of the idom, our idom branches
                // if the current block has more than one incoming edge
                // then hoist to the edge from idom, otherwise break out
                //
                // ignore this condition for ops with no inputs (ie. constants)
                // since we can always rematerialize these cheap
                if(op.nInputs() && blocks[blocks[mblock].idom].pdom != mblock)
                {
                    if(blocks[mblock].comeFrom.size() > 1)
                    {
                        auto & jcc =ops[blocks[blocks[mblock].idom].code.back()];
                        BJIT_ASSERT(jcc.opcode < ops::jmp);
                        
                        auto e = breakEdge(blocks[mblock].idom, mblock);
                        if(jcc.label[0] == mblock) jcc.label[0] = e;
                        if(jcc.label[1] == mblock) jcc.label[1] = e;
                        mblock = e;

                        needRebuildDOM = true;
                    }
                    break;
                }

                mblock = blocks[mblock].idom;
            }

            // if mblock is the current block, then we can't move
            if(mblock != b)
            {
                if(cse_debug)
                    BJIT_LOG("\nhoisting %04x: %d -> %d", opIndex, b, mblock);

                needDCE = true;
                    
                bc = noVal;
                op.block = mblock;

                // try to move the new op backwards
                int k = blocks[mblock].code.size();
                blocks[mblock].code.push_back(opIndex);
                while(k--)
                {
                    // don't move past anything that can't move (phi, alloc)
                    // but DO move past jumps
                    if(blocks[mblock].code[k] != noVal
                    && ops[blocks[mblock].code[k]].opcode > ops::jmp
                    && !ops[blocks[mblock].code[k]].canMove()) break;
                    
                    // sanity check that we don't move loads past sideFX
                    if(op.hasMemTag() &&
                    ops[blocks[mblock].code[k]].hasSideFX()) break;
                    
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
                    BJIT_ASSERT(ops[blocks[mblock].code[k]].pos == k);
                    std::swap(blocks[mblock].code[k],
                        blocks[mblock].code[k+1]);
                    ops[blocks[mblock].code[k+1]].pos = k+1;
                    op.pos = k;
                }
            }
            else if(cse_debug)
                BJIT_LOG("\ncan't move %04x from %d", opIndex, b);
                 

            // now check if we have another op in the table?
            // 
            auto * ptr = cseTable.find(op);

            if(!ptr) { OpCSE cseOp(opIndex, op); cseTable.insert(cseOp); }
            else
            {
                // we should never find this op in the table anymore
                BJIT_ASSERT(ptr->index != opIndex);

                // add to pairs, original op goes to MSB
                pairs.push_back(opIndex + (uint32_t(ptr->index) << 16));
            }
        }
    }

    // do we need a DOM rebuild?
    if(needRebuildDOM) rebuild_dom();

    // found no pairs, we're done
    if(!pairs.size()) return needDCE;

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

        bool hasPhi = false;

        // fast path check whether there's any point trying :)
        if(op.nInputs() >= 1
        && ops[op.in[0]].opcode == ops::phi
        && ops[op.in[0]].block == op.block) hasPhi = true;
        if(op.nInputs() >= 2
        && ops[op.in[1]].opcode == ops::phi
        && ops[op.in[1]].block == op.block) hasPhi = true;
        if(op.nInputs() >= 3
        && ops[op.in[2]].opcode == ops::phi
        && ops[op.in[2]].block == op.block) hasPhi = true;
        BJIT_ASSERT(op.nInputs() < 4);

        if(!hasPhi) return;
        
        // in the interest of simplicity, we rely on hoisting to pull
        // us to the phis we're going to check.. this is not perfect
        // but it covers the most obvious cases (especially with CSE)
        auto & mb = blocks[op.block];

        preList.clear();
        preList.insert(preList.begin(), mb.comeFrom.size(), noVal);

        // only do PRE if we find a match
        bool matches = false;

        // match for each potential source
        for(int c = 0; c < mb.comeFrom.size(); ++c)
        {
            auto cf = mb.comeFrom[c];

            // forbid PRE if we're not the pdom of the source
            if(blocks[cf].pdom != op.block) return;
            
            OpCSE match(cse.index, op);
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
                auto pds = blocks[p->block].dom.size();
                if(cfdom.size() >= pds && cfdom[pds-1] == p->block)
                {
                    preList[c] = p->index;
                    matches = true;
                }
                else
                {
                    auto it = std::lower_bound(
                        pairs.begin(), pairs.end(), p->index << 16);

                    while(it != pairs.end() && (*it)>>16 == p->index)
                    {
                        auto altIndex = (*it)&noVal;
                        auto & alt = ops[altIndex];
                        auto ads = blocks[alt.block].dom.size();
                        if(cfdom.size() >= ads && cfdom[ads-1] == alt.block)
                        {
                            preList[c] = altIndex;
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
        if(cse_debug)
            BJIT_LOG("\nPRE: L%d:%04x matches with:", op.block, cse.index);
            
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
                OpCSE match(cse.index, op);
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

        rename.add(cse.index, phi);
        op.makeNOP();
    };
    cseTable.foreach(checkPre);

    // returns true if we made some progress
    auto csePair = [&](uint16_t op0index, uint16_t op1index) -> bool
    {
        auto & op0 = ops[op0index];
        auto & op1 = ops[op1index];
        
        // did we already eliminate one of these
        if(op0.opcode == ops::nop) return false;
        if(op1.opcode == ops::nop) return false;

        auto b0 = op0.block;
        auto b1 = op1.block;
        
        if(cse_debug) BJIT_LOG("\nCSE: %04x (in %d) vs. %04x (in %d): ",
            op0index, op0.block, op1index, op1.block);

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
        
        // check post-dominator conditions, unless one block is ccd
        if(b0 != ccd && b1 != ccd)
        {
            for(int i = b0 ; i; i = blocks[i].idom)
            {
                // NOTE: DCE checks phis, so don't worry
                if(ccd == blocks[i].idom) break; // this is fine
                if(blocks[blocks[i].idom].pdom != i) bad = true;
                if(bad) break;
            }
    
            if(!bad)
            for(int i = b1; i; i = blocks[i].idom)
            {
                // NOTE: DCE checks phis, so don't worry
                if(ccd == blocks[i].idom) break; // this is fine
                if(blocks[blocks[i].idom].pdom != i) bad = true;
                if(bad) break;
            }
        }

        if(bad) { if(cse_debug) BJIT_LOG("BAD"); return false; }
        else if(b0 == b1)
        {
            // same block case, figure out which one is earlier
            // sanity check that positions are actually up to date
            BJIT_ASSERT(blocks[op0.block].code[op0.pos] == op0index);
            BJIT_ASSERT(blocks[op1.block].code[op1.pos] == op1index);
            if(op0.pos < op1.pos)
            {
                if(cse_debug)
                    BJIT_LOG("GOOD: %04x first in block", op0index);
                rename.add(op1index, op0index);
                op1.makeNOP();
                op0.flags.no_opt = false;   // can optimize again
                return true;
            }
            else
            {
                BJIT_LOG("GOOD: %04x first in block", op1index);
                rename.add(op0index, op1index);
                op0.makeNOP();
                op1.flags.no_opt = false;   // can optimize again
                return true;
            }
        }
        else if(ccd == b1)
        {
            if(cse_debug) BJIT_LOG("GOOD: %04x in ccd", op1index);
            rename.add(op0index, op1index);
            op0.makeNOP();
            op1.flags.no_opt = false;   // can optimize again

            return true;
        }
        else if(ccd == b0)
        {
            if(cse_debug) BJIT_LOG("GOOD: %04x in ccd", op0index);
            rename.add(op1index, op0index);
            op1.makeNOP();
            op0.flags.no_opt = false;   // can optimize again
            
            return true;
        }
        else
        {
            if(cse_debug) BJIT_LOG("GOOD: move to CCD:%d", ccd);
            if(op0.hasMemTag())
            {
                BJIT_ASSERT(op0.memtag == blocks[ccd].memout);
            }
        
            // NOTE: We do a lazy clear of the original position
            // in the rename pass below when block doesn't match.
            op0.block = ccd;

            // try to move this op backwards
            int k = blocks[ccd].code.size();
            blocks[ccd].code.push_back(op0index);
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
                BJIT_ASSERT(ops[blocks[ccd].code[k]].pos == k);
                std::swap(blocks[ccd].code[k],
                    blocks[ccd].code[k+1]);
                ops[blocks[ccd].code[k+1]].pos = k+1;
                op0.pos = k;
            }

            rename.add(op1index, op0index);
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
            if(csePair(pairs[i]>>16, pairs[i]&noVal))
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
                        if(csePair(pairs[p]&noVal, pairs[q]&noVal))
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
                if(csePair(pairs[p]&noVal, pairs[q]&noVal))
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
            if(op.block != b) { bc = noVal; needDCE = true; continue; }

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

    return needDCE || anyProgress;
}