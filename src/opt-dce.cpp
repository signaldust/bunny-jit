
#include "bjit.h"

using namespace bjit;

void Proc::opt_dce(bool unsafe)
{
    auto hadLiveSize = live.size();
    bool progress = true;
    //debug();

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
                if(ops[i].opcode > ops::jmp) ops[i].nUse = 0;
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
            
            for(auto i : blocks[b].code)
            {
                if(i == noVal) continue;
            
                switch(ops[i].nInputs())
                {
                    case 2: ++ops[ops[i].in[1]].nUse;
                    case 1: ++ops[ops[i].in[0]].nUse;
                }
                
                // only need to look at last op
                if(ops[i].opcode <= ops::jmp)
                for(int k = 0; k < 2; ++k)
                {
                    if(k && ops[i].opcode == ops::jmp) break;

                    while(blocks[ops[i].label[k]].code[0] != noVal
                    && ops[blocks[ops[i].label[k]].code[0]].opcode == ops::jmp
                    && i != blocks[ops[i].label[k]].code[0])    // check infinite
                    {
                        auto target = ops[blocks[ops[i].label[k]].code[0]].label[0];

                        // don't thread conditional jumps into blocks with phis
                        // if the target block is already our other label
                        // and the two blocks pass different values to any phi
                        //
                        // we need the extra block for shuffles and checking here
                        // saves CSE from having to add explicit renames
                        if(ops[i].opcode < ops::jmp && target == ops[i].label[k^1]
                        && ops[blocks[target].code[0]].opcode == ops::phi)
                        {
                            bool bad = false;
                            auto vs = noVal, vt = noVal;
                            
                            for(auto & a : blocks[target].args)
                            {
                                for(auto & s : a.alts)
                                {
                                    if(s.src == ops[i].block) vs = s.val;
                                    if(s.src == ops[i].label[k]) vt = s.val;
                                }
    
                                // if these don't match, then threading is not safe
                                if(vs != vt)
                                {
                                    if(0) BJIT_LOG("bad: B%d:%04x vs. B%d:%04x\n",
                                        ops[i].block, vs, ops[i].label[k], vt);
                                    bad = true; break;
                                }
                            }

                            if(bad) break;
                        }
                        
                        // patch target phis
                        for(auto & a : blocks[target].args)
                        {
                            for(auto & s : a.alts)
                            {
                                if(s.src == ops[i].label[k])
                                {
                                    a.alts.push_back(s);
                                    a.alts.back().src = b;
                                    break;
                                }
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
            }
        }
    
        // phi-uses
        for(auto & b : blocks)
        {
            if(!b.flags.live) continue;
            bool deadTail = false;
            for(auto i : b.code)
            {
                if(i == noVal) continue;
                
                if(deadTail)
                {
                    ops[i].opcode = ops::nop;
                    ops[i].in[0] = noVal;
                    ops[i].in[1] = noVal;
                    continue;
                }
            
                // don't short-circuit nUse=0 here because
                // another phi might mark us as used
                //
                // FIXME: does this find all cases with regular iteration
                // or do we need to add breath-first search to deal with
                // some esoteric special case?
                if(ops[i].opcode == ops::phi)
                {
                    auto & alts = blocks[ops[i].block].args[ops[i].phiIndex].alts;
                    // cleanup dead sources
                    int j = 0;
                    for(int i = 0; i < alts.size(); ++i)
                    {
                        if(!blocks[alts[i].src].flags.live) continue;
                        alts[j++] = alts[i];
                    }
                    alts.resize(j);
                
                    for(auto & src : alts)
                    {
                        // if this happens, then CSE messed up
                        BJIT_ASSERT(ops[src.val].opcode != ops::nop);
                        
                        while(ops[src.val].opcode == ops::phi
                        && blocks[ops[src.val].block]
                            .args[ops[src.val].phiIndex].alts.size() == 1)
                        {
                            BJIT_ASSERT(src.val != i);
                            src.val = blocks[ops[src.val].block]
                                .args[ops[src.val].phiIndex].alts[0].val;
                        }
    
                        // check if all source values are the same?
                        if(ops[src.val].opcode == ops::phi)
                        {
                            auto & ss = ops[src.val];
                            auto v = src.val;
                            for(int j = 0;
                                j < blocks[ss.block]
                                    .args[ss.phiIndex].alts.size(); ++j)
                            {
                                auto alt = blocks[ss.block].
                                    args[ss.phiIndex].alts[j].val;
                                if(v == src.val) v = alt;
                                if(v != alt && src.val != alt)
                                {
                                    v = src.val;
                                    break;
                                }
                            }
                            
                            if(src.val != v) progress = true;
                            src.val = v;
                        }
    
                        ++ops[src.val].nUse;
                    }
                }
    
                // check if all phi-values are the same
                for(int k = 0; k < ops[i].nInputs(); ++k)
                {
                    if(ops[ops[i].in[k]].opcode == ops::phi)
                    {
                        auto & src = ops[ops[i].in[k]];
                        auto v = ops[i].in[k];
                        for(int j = 0;
                            j < blocks[src.block]
                                .args[src.phiIndex].alts.size(); ++j)
                        {
                            auto alt = blocks[src.block]
                                .args[src.phiIndex].alts[j].val;
                            if(v == ops[i].in[k]) v = alt;
                            if(v != alt && ops[i].in[k] != alt)
                            {
                                v = ops[i].in[k];
                                break;
                            }
                        }
                        
                        if(ops[i].in[k] != v) progress = true;
                        ops[i].in[k] = v;
                    }
                }
    
                if(ops[i].opcode <= ops::tcallp) deadTail = true;
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
                if((op.hasSideFX() && (!unsafe || !op.canCSE()))
                || op.nUse) continue;

                switch(op.nInputs())
                {
                case 2: --ops[op.in[1]].nUse;
                case 1: --ops[op.in[0]].nUse;
                default:
                    if(op.opcode == ops::phi)
                    {
                        blocks[op.block].args[op.phiIndex].alts.clear();
                    }
                    op.opcode = ops::nop;
                    op.in[0] = noVal;
                    op.in[1] = noVal;
                    progress = true;
                }
            }

            // loop forward to cleanup
            int j = 0;
            for(int i = 0; i < b.code.size(); ++i)
            {
                if(b.code[i] == noVal) continue;
                if(ops[b.code[i]].opcode == ops::nop) continue;
                if(!ops[b.code[i]].hasSideFX() && !ops[b.code[i]].nUse) continue;
                
                if(j != i) b.code[j] = b.code[i];
                ++j;
            }

            b.code.resize(j);
            liveOps += j;
        }
    }
    
    BJIT_LOG("\n DCE:%d", iters);
    
    // if we made no progress, then don't bother rebuild other info
    if(live.size() == hadLiveSize && iters == 1) { return; }

    // rebuild comeFrom, should delay this until iteration done
    for(int b = live.size();b--;) blocks[live[b]].comeFrom.clear();
    for(int b = live.size();b--;)
    {
        // if this fails, we're probably missing return
        BJIT_ASSERT(blocks[live[b]].code.size());
        auto & op = ops[blocks[live[b]].code.back()];
        if(op.opcode < ops::jmp)
        {
            blocks[op.label[1]].comeFrom.push_back(live[b]);
        }
        if(op.opcode <= ops::jmp)
        {
            blocks[op.label[0]].comeFrom.push_back(live[b]);
        }
    }

    // cleanup dead phi alternatives
    for(auto & b : live)
    for(auto & a : blocks[b].args)
    {
        int j = 0;
        for(int i = 0; i < a.alts.size(); ++i)
        {
            bool keep = false;
            for(auto s : blocks[b].comeFrom)
            {
                if(a.alts[i].src != s) continue;
                keep = true;
                break;
            }
            if(!keep) continue;
            if(i != j) a.alts[j] = a.alts[i];
            ++j;
        }
        if(j != a.alts.size()) { a.alts.resize(j); progress = true; }
    }

    // rebuild dominators when control flow changes
    opt_dom();
}

void Proc::findUsesBlock(int b, bool inOnly, bool localOnly)
{
    // compute which ops are used by this block
    // this must be done in reverse
    for(int c = blocks[b].code.size(); c--;)
    {
        auto & op = ops[blocks[b].code[c]];

        if(!localOnly && op.opcode <= ops::jmp)
        for(int k = 0; k < 2; ++k)
        {
            if(k && op.opcode == ops::jmp) break;
            
            for(auto & a : blocks[op.label[k]].args)
            {
                for(auto & s : a.alts)
                {
                    if(s.src != b) continue;
                    
                    if(0) BJIT_LOG("live out %d->%d : v%04x\n",
                        b, op.label[k], s.val);

                    ++ops[s.val].nUse;
                    break;
                }
            }
            for(auto & a : blocks[op.label[k]].livein)
            {
                ++ops[a].nUse;
            }
        }

        switch(op.nInputs())
        {
        case 2: ++ops[op.in[1]].nUse;
        case 1: ++ops[op.in[0]].nUse;
        }

        // for ops that define values, set nUse to zero
        if(inOnly && op.hasOutput()) op.nUse = 0;
    }
}

void Proc::livescan()
{
    opt_dce(false);
    BJIT_ASSERT(live.size());   // at least one DCE required
    
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

    BJIT_LOG(" Live:%d", iter);
}
