
#include "bjit.h"

using namespace bjit;

bool Proc::opt_sink(bool unsafe)
{
    livescan(); // need live-in information

    // livescan doesn't find phi-inputs, we need them here
    for(auto b : live)
    {
        for(auto & a : blocks[b].args)
        for(auto & s : a.alts)
        {
            blocks[b].livein.push_back(s.val);
        }
    }

    printf(" SINK");

    //debug();

    // collect moved ops into tmp (in reverse)
    // so that we can merge them all together
    std::vector<uint16_t>   tmp0, tmp1;

    // one pass should be enough 'cos DFS
    bool progress = false;
    for(auto b : live)
    {
        // find local-only uses
        findUsesBlock(b, true);

        auto & jmp = ops[blocks[b].code.back()];

        // is this a return block?
        if(jmp.opcode > ops::jmp) continue;

        // is this a straight jmp?
        if(jmp.opcode == ops::jmp)
        {
            // if we don't dominate the block, then bail out
            if(blocks[jmp.label[0]].idom != b) continue;
        }

        tmp0.clear();
        tmp1.clear();

        // loop code backwards
        for(int c = blocks[b].code.size(); c--;)
        {
            auto op = ops[blocks[b].code[c]];

            // is this something we're allowed to move?
            // does it have local uses?
            if(!op.canCSE() || op.nUse
            || (!unsafe && op.hasSideFX())) continue;

            // it must be live-out in at least one block
            bool live0 = false, live1 = false;

            for(auto l : blocks[jmp.label[0]].livein)
            {
                if(op.index != l) continue;
                live0 = true;
                continue;
            }

            if(jmp.opcode < ops::jmp)
                for(auto l : blocks[jmp.label[1]].livein)
            {
                if(op.index != l) continue;
                live1 = true;
                continue;
            }

            // don't move if live (or dead) in both branches
            if(live0 == live1) continue;

            // do not move into blocks that merge paths
            // this prevents us from sinking loop invariants
            // back into the loop, which would be silly
            if(blocks[jmp.label[live0?0:1]].comeFrom.size() > 1)
            {
                // if the edge is not critical, don't sink at all
                if(jmp.opcode == ops::jmp) continue;

                // otherwise break the edge
                jmp.label[live0?0:1] = breakEdge(b, jmp.label[live0?0:1]);
            }

            // pick the block where this is live
            (live0 ? tmp0 : tmp1).push_back(op.index);
            blocks[b].code[c] = noVal;  // dead at original site

            // see if we should be moving inputs too?
            for(int k = 0; k < op.nInputs(); ++k)
            {
                // if this was last use, for something in this block
                // then mark it as livein for the block where we moved
                if(ops[op.in[k]].block == b && !--ops[op.in[k]].nUse)
                {
                    (live0 ? blocks[jmp.label[0]] : blocks[jmp.label[1]])
                        .livein.push_back(op.in[k]);
                }
            }
        }

        // did we move anything?
        if(tmp0.size())
        {
            progress = true;
            
            // skip any ops that must be in the beginning
            // really just phis, but try to be future-proof
            int insertAt = 0;
            int tBlock = jmp.label[0];
            while(insertAt < blocks[tBlock].code.size())
            {
                if(ops[blocks[tBlock].code[insertAt]].canMove()) break;
                ++insertAt;
            }

            // make room and move original ops back
            auto & tcode = blocks[tBlock].code;
            tcode.resize(tcode.size() + tmp0.size());
            for(int i = tcode.size(); i-- > insertAt;)
            {
                tcode[i] = tcode[i-tmp0.size()];
            }
            
            //printf("Moving B%d -> B%d:\n", b, jmp.label[0]);
            
            // then work merge tmp which needs to be reversed
            for(int i = insertAt; tmp0.size(); i++)
            {
                //debugOp(tmp0.back());
                tcode[i] = tmp0.back(); tmp0.pop_back();
                ops[tcode[i]].block = tBlock;
            }
        }
        
        if(tmp1.size())
        {
            progress = true;
            
            assert(jmp.opcode < ops::jmp);
            
            // skip any ops that must be in the beginning
            // really just phis, but try to be future-proof
            int insertAt = 0;
            int tBlock = jmp.label[1];
            while(insertAt < blocks[tBlock].code.size())
            {
                if(ops[blocks[tBlock].code[insertAt]].canMove()) break;
                ++insertAt;
            }
            
            // make room and move original ops back
            auto & tcode = blocks[tBlock].code;
            tcode.resize(tcode.size() + tmp1.size());
            for(int i = tcode.size(); i-- > insertAt;)
            {
                tcode[i] = tcode[i-tmp1.size()];
            }

            //printf("Moving B%d -> B%d:\n", b, jmp.label[1]);
            
            // then work merge tmp which needs to be reversed
            for(int i = insertAt; tmp1.size(); i++)
            {
                //debugOp(tmp1.back());
                
                tcode[i] = tmp1.back(); tmp1.pop_back();
                ops[tcode[i]].block = tBlock;
            }
        }
    }

    //debug();
    
    return progress;
}
