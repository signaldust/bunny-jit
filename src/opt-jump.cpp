
#include "bjit.h"

using namespace bjit;

/*

 This implements non-trivial control-flow optimizations.
 Trivial jump-threading is not done here, we let DCE handle that.

*/

bool Proc::opt_jump()
{
    bool progress = false;
    
    //livescan(); // need this to add phis to targets (opt_sink does)
    //debug();

    impl::Rename    renameCopy;
    impl::Rename    renameJump;

    for(auto b : live)
    {
        auto & jmp = ops[blocks[b].code.back()];
        
        // simple jumps only for now, each loop only once
        if(jmp.opcode != ops::jmp || jmp.flags.no_opt) continue;

        // only if target has multiple incoming edges
        BJIT_ASSERT(blocks[jmp.label[0]].comeFrom.size());
        if(blocks[jmp.label[0]].comeFrom.size() == 1)
        {
            BJIT_LOG("\nJump B%d->B%d is the only source", b, jmp.label[0]);
            continue;
        }

        // only if this block dominates the target
        if(blocks[jmp.label[0]].dom.size() <= blocks[b].dom.size()
        || blocks[jmp.label[0]].dom[blocks[b].dom.size()-1] != b)
        {
            BJIT_LOG("\nJump B%d->B%d is not from dominator", b, jmp.label[0]);
            continue;
        }

        BJIT_LOG("\nJump B%d->B%d is loop entry", b, jmp.label[0]);

        // Make a carbon-copy of the target block
        uint16_t nb = blocks.size();
        blocks.resize(blocks.size() + 1);
        BJIT_LOG("\nCopying B%d to B%d", jmp.label[0], nb);

        auto & head = blocks[jmp.label[0]];

        // sanity check, we can't handle this yet
        if((ops[head.code.back()].opcode <= ops::jmp
            && blocks[ops[head.code.back()].label[0]].comeFrom.size() > 1)
        || (ops[head.code.back()].opcode < ops::jmp
            && blocks[ops[head.code.back()].label[1]].comeFrom.size() > 1))
        {
            BJIT_LOG(" - complicated, bailing out\n");
            blocks.pop_back();
            continue;
        }

        BJIT_LOG(" - simple\n");
        
        auto & copy = blocks[nb];
        copy.flags.live = true;
            
        // we copy all the phis too
        copy.args.resize(head.args.size());
        
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

        BJIT_LOG("Copied %d ops.\n", (int) copy.code.size());

        BJIT_ASSERT(copy.code.size());

        // next, we need to fix-up target blocks
        auto & jcc = ops[copy.code.back()];
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
                if(ops[in].block == jmp.label[0]) ++nPhi;
            }

            if(!nPhi)
            {
                BJIT_LOG("Don't need any phi.\n");
                continue; // no phi, no fixup
            }
            else
            {
                BJIT_LOG("Need %d phi.\n", nPhi);
            }

            // make some space in the target
            fixBlock.code.insert(fixBlock.code.begin(), nPhi, noVal);

            // just loop live-in again
            int iPhi = 0;
            for(auto & in : fixBlock.livein)
            {
                // does this come from original head?
                if(ops[in].block != jmp.label[0]) continue;

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

                    fixBlock.args.back().add(r.src, jmp.label[0]);
                    fixBlock.args.back().add(r.dst, nb);

                    break;
                }

                ++iPhi;
            }

            BJIT_ASSERT(iPhi == nPhi);
            
            for(auto & r : renameJump.map)
            {
                BJIT_LOG(" %04x -> %04x\n", r.src, r.dst);
            }

            for(auto rb : live)
            {
                if(blocks[rb].dom.size() < fixBlock.dom.size()
                || blocks[rb].dom[fixBlock.dom.size()-1] != jcc.label[k])
                {
                    BJIT_LOG("Block B%d is not in branch B%d\n", rb, jcc.label[k]);
                    continue;
                }

                BJIT_LOG("Renaming B%d in B%d branch\n", rb, jcc.label[k]);

                for(auto & rop : blocks[rb].code)
                {
                    renameJump(ops[rop]);

                    // if this is direct target, that's all
                    if(rb == jcc.label[k]) continue;

                    // otherwise rename phis too
                    if(ops[rop].opcode == ops::phi)
                    {
                        for(auto & s : blocks[rb].args[ops[rop].phiIndex].alts)
                        for(auto & r : renameJump.map)
                        {
                            if(s.val == r.src) s.val = r.dst;
                        }
                    }
                }
            }

            // fixup looping phis
            for(auto & a : head.args)
            for(auto & s : a.alts)
            for(auto & r : renameJump.map)
            {
                if(s.val == r.src) s.val = r.dst;
            }
        }

        // set original jump to the copied blockq
        jmp.label[0] = nb;
        
        live.push_back(nb); // For debugs..
        progress = true;

        // don't rewrite more than one jump
        // we want to rebuild doms and stuff first
        break;
    }

    debug();

    // force DCE to rebuild everything
    if(progress) live.clear();

    return progress;
}