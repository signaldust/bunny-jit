
#include "bjit.h"

using namespace bjit;

static const bool sink_debug = false;

bool Proc::opt_sink(bool unsafeOpt)
{
    rebuild_livein();

    // livescan doesn't find phi-inputs, we need them here
    for(auto b : live)
    {
        for(auto & s : blocks[b].alts)
        {
            blocks[b].livein.push_back(s.val);
        }
    }

    BJIT_LOG(" SINK");

    // collect moved ops into tmp (in reverse)
    // so that we can merge them all together
    std::vector<uint16_t>   tmp0, tmp1;

    // one pass should be enough 'cos DFS
    bool progress = false;
    for(int li = 0, liveSz = live.size(); li < liveSz; ++li)
    {
        auto b = live[li];
        // find local uses
        findUsesBlock(b, false, true);

        auto & jmp = ops[blocks[b].code.back()];

        if(sink_debug) BJIT_LOG("\nSink in L%d?", b);

        // is this a return block?
        if(jmp.opcode > ops::jmp)
        {
            if(sink_debug) BJIT_LOG("\nL%d is exit block", b);
            continue;
        }

        // is this a straight jmp?
        if(jmp.opcode == ops::jmp)
        {
            // if we don't dominate the block, then bail out
            if(blocks[jmp.label[0]].idom != b)
            {
                if(sink_debug)
                    BJIT_LOG("\nL%d doesn't dominate L%d", b, jmp.label[0]);
                continue;
            }
        }

        tmp0.clear();
        tmp1.clear();

        // loop code backwards
        for(int c = blocks[b].code.size(); c--;)
        {
            auto opIndex = blocks[b].code[c];
            auto op = ops[opIndex];

            // if this has no local uses, is it something we can sink?
            if(op.nUse || !op.canCSE() || (!unsafeOpt && op.hasSideFX()))
            {
                if(sink_debug)
                    BJIT_LOG("\n %04x not eligible in L%d", opIndex, b);
                continue;
            }

            // it must be live-out in at least one block
            bool live0 = false, live1 = false;

            for(auto l : blocks[jmp.label[0]].livein)
            {
                if(opIndex != l) continue;
                live0 = true;
                continue;
            }

            if(jmp.opcode < ops::jmp)
                for(auto l : blocks[jmp.label[1]].livein)
            {
                if(opIndex != l) continue;
                live1 = true;
                continue;
            }
            
            if(sink_debug) BJIT_LOG("\nLive (%s, %s)...",
                live0 ? "yes" : "no", live1 ? "yes" : "no");

            // don't move if live (or dead) in both branches
            if(live0 == live1) continue;
            
            if(sink_debug) BJIT_LOG("\nTry to sink...");
            
            // do not move into blocks that merge paths
            // this prevents us from sinking loop invariants
            // back into the loop, which would be silly
            if(blocks[jmp.label[live0?0:1]].comeFrom.size() > 1)
            {
                // if the edge is not critical, don't sink at all
                // NOTE: we check this here (not at the top) because jump-opt
                // which we want to try only after finding sinkable op
                if(jmp.opcode == ops::jmp)
                {
                    if(sink_debug) BJIT_LOG("\nMerging path not critical...");
                    break;  // no point scanning the rest
                }

                // otherwise break the edge
                jmp.label[live0?0:1] = breakEdge(b, jmp.label[live0?0:1]);
                if(sink_debug) BJIT_LOG(" L%d", jmp.label[live0?0:1]);
            }
            
            if(sink_debug) BJIT_LOG("\nSinking...");

            // pick the block where this is live
            (live0 ? tmp0 : tmp1).push_back(opIndex);
            blocks[b].code[c] = noVal;  // dead at original site
            
            // see if we should be moving inputs too?
            for(int k = 0; k < op.nInputs(); ++k)
            {
                // if this was last use, for something in this block
                // then mark it as livein for the block where we moved to
                if(ops[op.in[k]].block == b && !--ops[op.in[k]].nUse)
                {
                    if(sink_debug) BJIT_LOG("\nLast use for %04x", op.in[k]);
                    blocks[jmp.label[live0?0:1]].livein.push_back(op.in[k]);
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
            for(int i = tcode.size(); --i > insertAt;)
            {
                tcode[i] = tcode[i-tmp0.size()];
            }
            
            //BJIT_LOG("Moving B%d -> B%d:\n", b, jmp.label[0]);
            
            // then work merge tmp which needs to be reversed
            for(int i = insertAt; tmp0.size(); i++)
            {
                //debugOp(tmp0.back());
                tcode[i] = tmp0.back(); tmp0.pop_back();
                ops[tcode[i]].block = tBlock;
                ops[tcode[i]].flags.no_opt = true;  // don't hoist further
            }
        }
        
        if(tmp1.size())
        {
            progress = true;
            
            BJIT_ASSERT_MORE(jmp.opcode < ops::jmp);
            
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
            for(int i = tcode.size(); --i > insertAt;)
            {
                tcode[i] = tcode[i-tmp1.size()];
            }

            //BJIT_LOG("Moving B%d -> B%d:\n", b, jmp.label[1]);
            
            // then work merge tmp which needs to be reversed
            for(int i = insertAt; tmp1.size(); i++)
            {
                //debugOp(tmp1.back());
                
                tcode[i] = tmp1.back(); tmp1.pop_back();
                ops[tcode[i]].block = tBlock;
                ops[tcode[i]].flags.no_opt = true;  // don't hoist further
            }
        }
    }

    //debug();
    
    return progress;
}
