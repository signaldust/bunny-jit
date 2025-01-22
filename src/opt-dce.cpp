
#include "bjit.h"

using namespace bjit;

void Proc::opt_dce(bool unsafeOpt)
{
    bool progress = true;

    int iters = 0;
    while(progress)
    {
        ++iters;
        progress = false;
        for(auto & b : blocks)
        {
            if(!b.flags.live) continue;
            b.flags.live = false;
    
            for(auto & i : b.code)
            {
                if(i == noVal) continue;
                // NOTE: nUse aliases on labels
                if(ops[i].hasOutput()) ops[i].nUse = 0;
            }
        }
    
        todo.clear();
        live.clear();
        
        todo.push_back(0);
        live.push_back(todo.back());
        blocks[0].flags.live = true;
    
        while(todo.size())
        {
            auto b = todo.back(); todo.pop_back();
            bool deadTail = false;
            
            for(auto i : blocks[b].code)
            {
                if(i == noVal) continue;
            
                if(deadTail)
                {
                    ops[i].makeNOP();
                    continue;
                }
            
                switch(ops[i].nInputs())
                {
                    case 3: ++ops[ops[i].in[2]].nUse;
                    case 2: ++ops[ops[i].in[1]].nUse;
                    case 1: ++ops[ops[i].in[0]].nUse;
                    case 0: break;
                    default: BJIT_ASSERT(false);
                }
                
                // only need to look at last op
                if(ops[i].opcode <= ops::jmp)
                for(int k = 0; k < 2; ++k)
                {
                    if(k && ops[i].opcode == ops::jmp) break;

                    while(true)
                    {
                        auto bsrc = ops[i].label[k];
                        auto & kc = blocks[bsrc].code;
                        auto tjmp = noVal;

                        // threading conditional jumps through empty loop preheaders
                        // with multiple entry-blocks causes havoc with IV logic
                        // so avoid conditional jumps into targets with phis for now
                        if(ops[i].opcode < ops::jmp
                        && (kc[0] == noVal || ops[kc[0]].opcode == ops::phi)) break;

                        // skip over phis
                        for(int i = 0; i < kc.size(); ++i)
                        {
                            tjmp = kc[i];
                            if(tjmp == noVal || ops[tjmp].opcode != ops::phi)
                                break;
                        }

                        // can we thread this?
                        if(tjmp == noVal || i == tjmp
                        || ops[tjmp].opcode != ops::jmp) break;

                        auto target = ops[tjmp].label[0];

                        // do another pass
                        if(false && blocks[target].code[0] == noVal)
                        {
                            progress = true;
                            break;
                        }

                        // if the block we're jumping from has phis
                        // then validate that target block also has them
                        bool noPhi = false;
                        for(auto & p : blocks[bsrc].args)
                        {
                            // this can happen
                            if(p.phiop == noVal
                            || ops[p.phiop].opcode == ops::nop) continue;

                            bool good = false;
                            for(auto & a : blocks[target].alts)
                            {
                                if(a.src == bsrc && a.val == p.phiop)
                                {
                                    good = true;
                                    break;
                                }
                            }
                            if(!good) { noPhi = true; break; }
                        }

                        if(noPhi) break;
                        
                        // if we are jumping into a block with phis then
                        // validate that a blocks isn't there for shuffle
                        //if(ops[blocks[target].code[0]].opcode == ops::phi)
                        if(blocks[target].alts.size())
                        {
                            bool bad = false;

                            // clear temps
                            for(auto & a : blocks[target].args) a.tmp = noVal;
                            
                            // find relevant alternatives
                            auto & args = blocks[target].args;
                            for(auto & a : blocks[target].alts)
                            {
                                // is this alternative relevant?
                                if(a.src != ops[i].block
                                && a.src != ops[i].label[k]) continue;

                                auto val = a.val;

                                // resolve local phis
                                if(ops[val].opcode == ops::phi
                                && ops[val].block == a.src)
                                {
                                    bool good = false;
                                    for(auto & s : blocks[a.src].alts)
                                    {
                                        if(s.phi != val) continue;
                                        if(s.src != b) continue;
                                        val = s.val;
                                        
                                        good = true;
                                        break;
                                    }
                                    // FIXME: figure out why we might not
                                    // sometimes find a suitable alt?
                                    if(!good) { bad = true; break; }
                                }
                                
                                // check for duplicate
                                for(auto & s : blocks[target].alts)
                                {
                                    if(a.phi == s.phi && b == s.src)
                                    {
                                        if(s.val == val) continue;

                                        // seems this block exists for shuffle
                                        bad = true;
                                        break;
                                    }
                                }

                                // phi got removed this pass?
                                if(ops[a.phi].opcode == ops::nop) continue;
                                
                                // if we've not seen it, store tmp
                                if(args[ops[a.phi].phiIndex].tmp == noVal)
                                {
                                    args[ops[a.phi].phiIndex].tmp = val;
                                }
                                else if(args[ops[a.phi].phiIndex].tmp != val)
                                {
                                    bad = true;
                                    break;
                                }
                            }
                            
                            if(bad) break;
                        }

                        // patch target phis
                        for(int ai = 0, sz = blocks[target].alts.size();
                            ai < sz; ++ai)
                        {
                            auto & a = blocks[target].alts[ai];
                            if(a.src == ops[i].label[k])
                            {
                                auto val = a.val;

                                // resolve local phis
                                if(ops[val].opcode == ops::phi
                                && ops[val].block == a.src)
                                {
                                    bool good = false;
                                    for(auto & s : blocks[a.src].alts)
                                    {
                                        if(s.phi != val) continue;
                                        if(s.src != b) continue;
                                        val = s.val;
                                        good = true;
                                        break;
                                    }
                                    BJIT_ASSERT_MORE(good);
                                }

                                // check for duplicate
                                bool dedup = false;
                                for(auto & s : blocks[target].alts)
                                {
                                    if(a.phi == s.phi && b == s.src)
                                    {
                                        BJIT_ASSERT_MORE(s.val == val);
                                        dedup = true;
                                    }
                                }

                                if(!dedup) blocks[target].newAlt(a.phi, b, val);
                            }
                        }

                        ops[i].label[k] = target;
                        progress = true;    // need at least new DOMs
                    }
                    
                    if(!blocks[ops[i].label[k]].flags.live)
                    {
                        todo.push_back(ops[i].label[k]);
                        live.push_back(todo.back());
                        blocks[ops[i].label[k]].flags.live = true;
                    }
                }
                
                if(ops[i].opcode < ops::jmp)
                {
                    if(ops[i].label[0] == ops[i].label[1])
                    {
                        ops[i].opcode = ops::jmp;
                        ops[i].in[0] = noVal;
                        ops[i].in[1] = noVal;
                        progress = true;
                    }
                }
                
                if(ops[i].opcode <= ops::tcallp) deadTail = true;
            }
        }
    
        // phi-uses
        for(auto & bi : live)
        {
            auto & b = blocks[bi];
            // cleanup dead sources
            {
                int j = 0;
                for(int i = 0; i < b.alts.size(); ++i)
                {
                    if(ops[b.alts[i].phi].opcode == ops::nop) continue;
                    if(!blocks[b.alts[i].src].flags.live) continue;
                    if(j != i) b.alts[j] = b.alts[i];
                    ++j;
                }
                b.alts.resize(j);
            }
            // set tmp sources to noVal
            for(auto & a : b.args) a.tmp = noVal;

            // find which phis have actual uses
            for(auto & s : b.alts)
            {
                // ignore simple loopback
                if(s.phi == s.val) continue;

                // if we don't have a value yet, set this as value
                if(b.args[ops[s.phi].phiIndex].tmp == noVal)
                {
                    b.args[ops[s.phi].phiIndex].tmp = s.val;
                }
                else if(b.args[ops[s.phi].phiIndex].tmp != s.val)
                {
                    // had more than one value, need to keep this
                    b.args[ops[s.phi].phiIndex].tmp = s.phi;
                }
            }

            // set use-counts for phis we're going to keep
            for(auto & s : b.alts)
            {
                if(b.args[ops[s.phi].phiIndex].tmp == s.phi)
                {
                    ++ops[s.val].nUse;
                }
            }
        }
        
        for(auto & b : live)
        {
            // rename
            for(auto i : blocks[b].code)
            {
                if(i == noVal) continue;

                // rename phis we can eliminate
                for(int k = 0; k < ops[i].nInputs(); ++k)
                {
                    auto phiIndex = ops[i].in[k];
                    auto & phi = ops[phiIndex];

                    if(phi.opcode != ops::phi) continue;

                    auto src = blocks[phi.block].args[phi.phiIndex].tmp;
                    if(src != phiIndex)
                    {
                        ops[i].in[k] = src;
                        ++ops[src].nUse;
                        progress = true;
                    }
                }
            }

            for(auto & a : blocks[b].alts)
            {
                auto & phi = ops[a.val];
                if(phi.opcode != ops::phi) continue;

                auto src = blocks[phi.block].args[phi.phiIndex].tmp;
                if(src != a.val)
                {
                    a.val = src;
                    ++ops[src].nUse;
                    progress = true;
                }
            }
        }

        // count how many ops we have alive
        // this is used by CSE for intelligent hash sizing
        liveOps = 0;
        
        for(auto bi : live)
        {
            auto & b = blocks[bi];
            
            // loop backwards to figure out what's dead
            for(int i = b.code.size(); i--;)
            {
                if(b.code[i] == noVal) continue;
                
                auto & op = ops[b.code[i]];
                if(op.opcode == ops::nop) continue;
                
                // NOTE: nUse aliases on labels, check other stuff first
                if((op.hasSideFX() && (!unsafeOpt || !op.canCSE()))
                || op.nUse) continue;

                switch(op.nInputs())
                {
                case 3: --ops[op.in[2]].nUse;
                case 2: --ops[op.in[1]].nUse;
                case 1: --ops[op.in[0]].nUse;
                case 0:
                    op.makeNOP();
                    progress = true;
                    break;
                default: BJIT_ASSERT(false);
                }
            }

            // loop forward to cleanup
            int j = 0;
            for(int i = 0; i < b.code.size(); ++i)
            {
                if(b.code[i] == noVal) continue;
                if(ops[b.code[i]].opcode == ops::nop) continue;
                if(!ops[b.code[i]].hasSideFX() && !ops[b.code[i]].nUse) continue;
                
                if(j != i)
                {
                    b.code[j] = b.code[i];
                }
                ops[b.code[j]].pos = j;
                ++j;
            }

            if(b.code.size() != j) { b.code.resize(j); progress = true; }
            liveOps += j;
        }
    }
    
    BJIT_LOG("\n DCE:%d", iters);
}

void Proc::findUsesBlock(int b, bool inOnly, bool localOnly)
{
    // compute which ops are used by this block
    // this must be done in reverse
    for(int c = blocks[b].code.size(); c--;)
    {
        if(blocks[b].code[c] == noVal) continue;
        auto & op = ops[blocks[b].code[c]];

        if(!localOnly && op.opcode <= ops::jmp)
        for(int k = 0; k < 2; ++k)
        {
            if(k && op.opcode == ops::jmp) break;
            
            for(auto & s : blocks[op.label[k]].alts)
            {
                if(s.src != b) continue;
                
                if(0) BJIT_LOG("live out %d->%d : v%04x\n",
                    b, op.label[k], s.val);

                ++ops[s.val].nUse;
            }
            for(auto & a : blocks[op.label[k]].livein)
            {
                ++ops[a].nUse;
            }
        }

        switch(op.nInputs())
        {
        case 3: ++ops[op.in[2]].nUse;
        case 2: ++ops[op.in[1]].nUse;
        case 1: ++ops[op.in[0]].nUse;
        case 0: break;
        default: BJIT_ASSERT(false);
        }

        // for ops that define values, set nUse to zero
        if(inOnly && op.hasOutput()) op.nUse = 0;
    }
}

void Proc::rebuild_livein()
{
    // cleanup stale phis
    rebuild_cfg();
    
    BJIT_ASSERT(live.size());
    
    for(auto & op : ops)
    {
        // NOTE: nUse aliases on labels
        if(op.hasOutput()) op.nUse = 0;
    }

    for(auto & b : live) blocks[b].livein.clear();

    int iter = 0;
    bool progress = true;
    while(progress)
    {
        ++iter;
        progress = false;

        // reverse live almost always requires less iteration
        for(int b = live.size();b--;)
        {
            auto sz = blocks[live[b]].livein.size();

            findUsesBlock(live[b], true, false);
            blocks[live[b]].livein.clear();
    
            for(int i = 0; i < ops.size(); ++i)
            {
                // is this a variable that we need?
                if(!ops[i].hasOutput() || !ops[i].nUse) continue;

                //BJIT_LOG(" v%04x live in %d\n", i, live[b]);
                blocks[live[b]].livein.push_back(i);
                ops[i].nUse = 0;
            }
    
            if(blocks[live[b]].livein.size() != sz) progress = true;
        }
    }

    if(blocks[0].livein.size()) debug();
    BJIT_ASSERT(!blocks[0].livein.size());

    BJIT_LOG(" Live:%d", iter);
}
