
#include "bjit.h"

using namespace bjit;

/*

 This implements non-trivial control-flow optimizations.
 Trivial jump-threading is not done here, we let DCE handle that.

*/

static const bool jump_debug = false;

// This optimizes simple back edges: target block dominates and branches.
// Break critical edges if any. Copy target block into a new block.
//
// Then for each block whose immediate dominator is the target block
// find all live-in variables originating from that block and insert phis.
// Rename the variables in any block dominated by this block.
//
// This effectively gives us loop-inversion for simple loops. 
//
bool Proc::opt_jump_be(uint16_t b)
{
    auto & jmp = ops[blocks[b].code.back()];

    // is this simple jump?
    if(jmp.opcode != ops::jmp)
    {
        if(jump_debug) BJIT_LOG(" JUMP:%d not jump (%s)\n", b, jmp.strOpcode());
        return false;
    }

    auto target = jmp.label[0];

    // does the target dominate?
    if(blocks[b].dom.size() <= blocks[target].dom.size()
    || blocks[b].dom[blocks[target].dom.size()-1] != target)
    {
        if(jump_debug) BJIT_LOG(" JUMP:%d target doesn't dominate\n", b);
        return false;
    }

    // does the target end in a branch?
    auto & jcc = ops[blocks[target].code.back()];
    if(jcc.opcode >= ops::jmp)
    {
        if(jump_debug) BJIT_LOG(" JUMP:%d not branch\n", b);
        return false;
    }

    if(jump_debug)
        BJIT_LOG(" LOOP:%d (%d:%d,%d)", b, target, jcc.label[0], jcc.label[1]);
    else
        BJIT_LOG(" LOOP:%d", b);

    // break edges if target has phis (valid or not)
    if(ops[blocks[jcc.label[0]].code[0]].opcode == ops::phi)
    {
        jcc.label[0] = breakEdge(target, jcc.label[0]);
    }
    if(ops[blocks[jcc.label[1]].code[0]].opcode == ops::phi)
    {
        jcc.label[1] = breakEdge(target, jcc.label[1]);
    }
    
    // Make a carbon-copy of the target block
    uint16_t nb = blocks.size();
    blocks.resize(blocks.size() + 1);
    if(jump_debug) BJIT_LOG("\n Jump L%d -> L%d (was: L%d)\n", b, nb, target);

    auto & head = blocks[target];
    auto & copy = blocks[nb];
    copy.flags.live = true;
    copy.dom = blocks[b].dom;
    copy.dom.push_back(nb);
    copy.idom = b;
    copy.pdom = blocks[b].pdom; // shouldn't REALLY need pdoms, but fix anyway
    blocks[b].pdom = nb;
    live.push_back(nb);

    jmp.label[0] = nb;

    // we copy all the phis too
    copy.args.resize(head.args.size());

    impl::Rename    renameCopy;
    impl::Rename    renameJump;
    
    BJIT_ASSERT(head.code.size());
    for(int i = 0; i < head.code.size(); ++i)
    {
        auto & opi = ops[head.code[i]];
        auto & opc = ops[addOp(opi.opcode, opi.flags.type, nb)];

        // copy operands
        opc.i64 = opi.i64;

        renameCopy(opc);
        
        // for jumps copy labels
        if(opc.opcode <= ops::jmp)
        {
            opc.label[0] = opi.label[0];
            opc.label[1] = opi.label[1];

            // need to fix come from
            blocks[opc.label[0]].comeFrom.push_back(nb);
            blocks[opc.label[1]].comeFrom.push_back(nb);

            // stop further loop-optimization here
            // even if this folds into a simple jmp
            opc.flags.no_opt = true;

            break;  // never copy dead tails
        }

        // for phis, copy sources
        if(opc.opcode == ops::phi)
        {
            BJIT_ASSERT(opc.phiIndex == opi.phiIndex);
            
            copy.args[opc.phiIndex].phiop = opc.index;
        }

        renameCopy.add(opi.index, opc.index);
    }
    
    // copy phi alts
    copy.alts = head.alts;
    for(auto & a : copy.alts)
    {
        a.phi = blocks[nb].args[ops[a.phi].phiIndex].phiop;
    }
    
    if(jump_debug) BJIT_LOG("Copied %d ops.\n", (int) copy.code.size());

    BJIT_ASSERT(copy.code.size());

    // next we need to fix all blocks that target immediately dominates
    for(auto fb : live)
    {
        auto & fixBlock = blocks[fb];
        if(fixBlock.idom != target) continue;
        
        if(jump_debug) BJIT_LOG("Block %d needs fixup.\n", fb);

        int nPhi = 0;
        for(auto & in : fixBlock.livein)
        {
            // does this come from original head?
            if(ops[in].block == target) ++nPhi;
        }

        // insert phis
        fixBlock.code.insert(fixBlock.code.begin(), nPhi, noVal);
        
        int iPhi = 0;
        for(auto & in : fixBlock.livein)
        {
            // does this come from original head?
            if(ops[in].block != target) continue;

            fixBlock.code[iPhi] = newOp(ops::phi, ops[in].flags.type, fb);

            // target needs to rename to use the phi
            renameJump.add(in, fixBlock.code[iPhi]);

            // setup the new phi
            ops[fixBlock.code[iPhi]].phiIndex = fixBlock.args.size();
            fixBlock.args.emplace_back(impl::Phi(fixBlock.code[iPhi]));

            // add alternatives, they are in our rename map
            // we fix the real sources later
            for(auto & r : renameCopy.map)
            {
                if(r.src != in) continue;

                fixBlock.newAlt(fixBlock.code[iPhi], target, r.src);
                fixBlock.newAlt(fixBlock.code[iPhi], nb, r.dst);

                break;
            }

            ++iPhi;
        }
    }

    // put the original phis to jump rename list
    for(auto & r : renameCopy.map) renameJump.add(r.src, r.dst);

    // do a second pass to actually rename
    for(auto fb : live)
    {
        auto & fixBlock = blocks[fb];
        if(fixBlock.idom != target) continue;

        renameCopy.map.clear();
        // filter renames relevant to this block
        for(auto & r : renameJump.map)
        {
            if(ops[r.dst].block == fb) renameCopy.add(r.src, r.dst);
        }

        // find all blocks dominated by this block
        for(auto rb : live)
        {
            // we need to do this the old-fashioned way because
            // break-edge doesn't try to fix .dom globally
            bool found = false;
            for(int db = rb; db; db = blocks[db].idom)
            {
                if(db != fb) continue;
                found = true;
                break;
            }
            if(!found) continue;
            
            if(jump_debug)
                BJIT_LOG("Renaming L%d in branch %d\n", rb, fb);

            // rename livein for better debugs
            for(auto & in : blocks[rb].livein)
            for(auto & r : renameCopy.map)
            {
                if(in == r.src) in = r.dst;
            }
                
            for(auto & rop : blocks[rb].code)
            {
                renameCopy(ops[rop]);
            }

            // don't patch jumps in the copied block, we already fixed these
            if(rb == nb) continue;
            
            auto & rjmp = ops[blocks[rb].code.back()];
            if(rjmp.opcode > ops::jmp) continue;   // return or tail-call

            for(int x = 0; x < 2; ++x)
            {
                if(x && rjmp.opcode == ops::jmp) break;

                if(jump_debug) BJIT_LOG("Patching jump to %d\n", rjmp.label[x]);
                for(auto & s : blocks[rjmp.label[x]].alts)
                {
                    if(s.src == rb && ops[s.val].block != target)
                    {
                        if(jump_debug)
                            BJIT_LOG("L:%d:%04x is from %d (keep)\n",
                                s.src, s.val, ops[s.val].block);
                        continue;
                    }
                    // is this from somewhere else?
                    if(s.src != rb) continue;
                    
                    for(auto & r : renameCopy.map)
                    {
                        if(s.val == r.src)
                        {
                            if(jump_debug)
                                BJIT_LOG("L:%d:%04x needs rewrite: ",
                                    s.src, s.val);
                                    
                            if(s.src == rb)
                            {
                                if(jump_debug)
                                    BJIT_LOG("simple: L:%d:%04x\n",
                                        ops[r.dst].block, r.dst);
                                s.val = r.dst;
                            }
                            else BJIT_ASSERT(false);
                            break;
                        }
                    }
                }
            }
        }
    }

    if(jump_debug) { debug(); }

    return true;
}

bool Proc::opt_jump()
{
    //rebuild_dom();    // don't need this if after CSE
    rebuild_livein();   // don't need this if after sink

    if(jump_debug) debug();
    
    BJIT_LOG(" JUMP");
    
    bool progress = false;
    for(int li = 0, liveSz = live.size(); li < liveSz; ++li)
    {
        auto b = live[li];
        if(blocks[b].code.back() == noVal) continue;

        auto & op = ops[blocks[b].code.back()];
    
        // if this is a pointless jump then pull the contents
        if(op.opcode == ops::jmp
        && blocks[op.label[0]].comeFrom.size() == 1
        && ops[blocks[op.label[0]].code[0]].opcode != ops::phi)
        {
            blocks[b].code.pop_back();
            for(auto & tc : blocks[op.label[0]].code)
            {
                blocks[b].code.push_back(tc);
                ops[tc].block = b;
                tc = noVal;
            }

            auto & jmp = ops[blocks[b].code.back()];
            // rewrite phi-sources
            if(jmp.opcode <= ops::jmp)
            {
                for(auto & s : blocks[jmp.label[0]].alts)
                {
                    if(s.src == op.label[0]) s.src = b;
                }
            }
            if(jmp.opcode < ops::jmp)
            {
                for(auto & s : blocks[jmp.label[1]].alts)
                {
                    if(s.src == op.label[0]) s.src = b;
                }
            }

            BJIT_LOG(" MERGE");
            progress = true;
            continue;
        }

        if(op.flags.no_opt)
        {
            continue;
        }

        // handle degenerate loops too?
        if(op.opcode < ops::jmp && op.label[0] == b)
        {
            op.label[0] = breakEdge(b, b);
            op.flags.no_opt = true;
            progress = true;

            // want doms though (FIXME: move this to opt_jmp_be only?)
            rebuild_dom();

            if(jump_debug) BJIT_LOG(" TRY %d\n", op.label[0]);

            if(opt_jump_be(op.label[0]))
            {
                break;
            }
        }
        
        if(op.opcode < ops::jmp && op.label[1] == b)
        {
            op.label[1] = breakEdge(b, b);
            op.flags.no_opt = true;
            progress = true;
            
            // want doms though (FIXME: move this to opt_jmp_be only?)
            rebuild_dom();
            
            if(jump_debug) BJIT_LOG(" TRY %d\n", op.label[1]);
            
            if(opt_jump_be(op.label[1]))
            {
                break;
            }
        }

        // if we didn't do a trivial pull, try opt_jump
        // but only once per fold, we need to update live info
        if(op.opcode == ops::jmp && opt_jump_be(b))
        {
            progress = true;
            break;
        }
    }

    // we really need this here because it cleans up
    // any stale phis, so DCE doesn't get confused
    rebuild_cfg();
    
    return progress;
}