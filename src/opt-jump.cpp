
#include "bjit.h"

using namespace bjit;

/*

 This implements non-trivial control-flow optimizations.
 Trivial jump-threading is not done here, we let DCE handle that.

*/

static const bool jump_debug = false;

// This optimizes simple back edges (jump, target dominates)
// by duplicating the contents of the block and building new phis.
bool Proc::opt_jump_be(uint16_t b)
{
    auto & jmp = ops[blocks[b].code.back()];

    // is this simple jump?
    if(jmp.opcode != ops::jmp) return false;

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
    if(jcc.opcode >= ops::jmp) return false;

    // does target dominate the branches?
    if(blocks[jcc.label[0]].idom != target
    || blocks[jcc.label[1]].idom != target) return false;

    // does one of the branches dominate us?
    // if not, then this is potentially complicated
    bool haveDom = (jcc.label[0] == b || jcc.label[1] == b);
    if(!haveDom && blocks[b].dom.size() > blocks[jcc.label[0]].dom.size()
    && blocks[b].dom[blocks[jcc.label[0]].dom.size()-1] == jcc.label[0])
    {
        haveDom = true;
    }
    
    if(!haveDom && blocks[b].dom.size() > blocks[jcc.label[1]].dom.size()
    && blocks[b].dom[blocks[jcc.label[1]].dom.size()-1] == jcc.label[1])
    {
        haveDom = true;
    }

    if(!haveDom)
    {
        if(jump_debug) BJIT_LOG(" LOOP:%d (%d:%d,%d) no dom",
            b, target, jcc.label[0], jcc.label[1]);
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
            copy.args[opc.phiIndex].alts = head.args[opi.phiIndex].alts;

        }

        renameCopy.add(opi.index, opc.index);
    }
    if(jump_debug) BJIT_LOG("Copied %d ops.\n", (int) copy.code.size());

    BJIT_ASSERT(copy.code.size());

    // next, we need to fix-up target blocks
    for(int k = 0; k < 2; ++k)
    {
        // simple jump, only one block to fix
        if(k && jcc.opcode == ops::jmp) break;

        renameJump.map.clear();

        auto & fixBlock = blocks[jcc.label[k]];

        // figure out how many phis do we need
        int nPhi = 0;
        for(auto & in : fixBlock.livein)
        {
            // does this come from original head?
            if(ops[in].block == target) ++nPhi;
        }

        if(!nPhi)
        {
            if(jump_debug) BJIT_LOG("Don't need any phi.\n");
            continue; // no phi, no fixup
        }
        else
        {
            if(jump_debug) BJIT_LOG("Need %d phi.\n", nPhi);
        }

        // make some space in the target
        fixBlock.code.insert(fixBlock.code.begin(), nPhi, noVal);

        // just loop live-in again
        int iPhi = 0;
        for(auto & in : fixBlock.livein)
        {
            // does this come from original head?
            if(ops[in].block != target) continue;

            fixBlock.code[iPhi]
                = newOp(ops::phi, ops[in].flags.type, jcc.label[k]);

            // target needs to rename to use the phi
            renameJump.add(in, fixBlock.code[iPhi]);

            // setup the new phi
            ops[fixBlock.code[iPhi]].phiIndex = fixBlock.args.size();
            fixBlock.args.resize(fixBlock.args.size() + 1);
            fixBlock.args.back().phiop = fixBlock.code[iPhi];

            // add alternatives, they are in our rename map
            for(auto & r : renameCopy.map)
            {
                if(r.src != in) continue;

                fixBlock.args.back().add(r.src, target);
                fixBlock.args.back().add(r.dst, nb);

                break;
            }

            ++iPhi;
        }

        BJIT_ASSERT(iPhi == nPhi);

        if(jump_debug) for(auto & r : renameJump.map)
        {
            BJIT_LOG(" %04x -> %04x\n", r.src, r.dst);
        }

        for(auto rb : live)
        {
            if(blocks[rb].dom.size() < fixBlock.dom.size()
            || blocks[rb].dom[fixBlock.dom.size()-1] != jcc.label[k])
            {
                if(jump_debug)
                    BJIT_LOG("Block L%d is not in branch L%d\n", rb, jcc.label[k]);
                continue;
            }

            if(jump_debug)
                BJIT_LOG("Renaming L%d in L%d branch\n", rb, jcc.label[k]);

            for(auto & rop : blocks[rb].code)
            {
                renameJump(ops[rop]);
            }

            auto & rjmp = ops[blocks[rb].code.back()];
            if(rjmp.opcode > ops::jmp) continue;   // return or tail-call
            
            for(int x = 0; x < 2; ++x)
            {
                if(x && rjmp.opcode == ops::jmp) break;

                for(auto & a : blocks[rjmp.label[x]].args)
                for(auto & s : a.alts)
                for(auto & r : renameJump.map)
                {
                    if(s.src == rb && s.val == r.src) s.val = r.dst;
                }
            }
        }
    }
    
    if(jump_debug) { opt_dom(); debug(); }

    live.clear();   // force rebuild by DCE
    
    return true;
}

bool Proc::opt_jump()
{
    //livescan();   // don't need this if after sink

    if(jump_debug) debug();
    
    bool progress = false;
    for(auto b : live)
    {
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
                for(auto & a : blocks[jmp.label[0]].args)
                for(auto & s : a.alts)
                {
                    if(s.src == op.label[0]) s.src = b;
                }
            }
            if(jmp.opcode < ops::jmp)
            {
                for(auto & a : blocks[jmp.label[1]].args)
                for(auto & s : a.alts)
                {
                    if(s.src == op.label[0]) s.src = b;
                }
            }

            BJIT_LOG(" JUMP");
            continue;
        }

        // if we didn't do a trivial pull, try opt_jump
        // but only once per fold, we need to update live info
        if(op.opcode == ops::jmp && opt_jump_be(b))
        {
            progress = true;
            break;  // in case we cause live to realloc
        }
    }
    
    return progress;
}