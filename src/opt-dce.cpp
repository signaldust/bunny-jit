
#include "ir.h"

using namespace bjit;

void Proc::opt_dce()
{
    assert(!raDone);    // DCE destroys register allocation
    
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
                    
                    while(ops[blocks[ops[i].label[k]].code[0]].opcode == ops::jmp
                    && i != blocks[ops[i].label[k]].code[0])    // check infinite
                    {
                        // patch target phis
                        auto target = ops[blocks[ops[i].label[k]].code[0]].label[0];
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
                        progress = true;
                    }
                }
            }
        }
    
        // phi-uses - FIXME: get the breath-first from NNC?
        for(auto & b : blocks)
        {
            if(!b.flags.live) continue;
            bool deadTail = false;
            for(auto i : b.code)
            {
                if(deadTail) { ops[i].opcode = ops::nop; continue; }
            
                // don't short-circuit nUse=0 here because
                // another phi might mark us as used
                if(ops[i].opcode == ops::phi)
                {
                    auto & alts = blocks[ops[i].in[0]].args[ops[i].in[1]].alts;
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
                        while(ops[src.val].opcode == ops::phi
                        && blocks[ops[src.val].in[0]]
                            .args[ops[src.val].in[1]].alts.size() == 1)
                        {
                            assert(src.val != i);
                            src.val = blocks[ops[src.val].in[0]]
                                .args[ops[src.val].in[1]].alts[0].val;
                        }
    
                        // check if all source values are the same?
                        if(ops[src.val].opcode == ops::phi)
                        {
                            auto & ss = ops[src.val];
                            auto v = src.val;
                            for(int j = 0;
                                j < blocks[ss.in[0]].args[ss.in[1]].alts.size(); ++j)
                            {
                                auto alt = blocks[ss.in[0]].args[ss.in[1]].alts[j].val;
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
                            j < blocks[src.in[0]].args[src.in[1]].alts.size(); ++j)
                        {
                            auto alt = blocks[src.in[0]].args[src.in[1]].alts[j].val;
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
    
                if(ops[i].opcode <= ops::iretI) deadTail = true;
            }
        }
    
        // cleanup using a stack
        std::vector<unsigned>   deadOps;
        for(auto & b : blocks)
        {
            if(!b.flags.live) continue;
            int j = 0;
            for(int i = 0; i < b.code.size(); ++i)
            {
                if(ops[b.code[i]].opcode == ops::nop) continue;
                if(!ops[b.code[i]].hasSideFX() && !ops[b.code[i]].nUse)
                {
                    deadOps.push_back(b.code[i]);
                }
    
                if(j != i) b.code[j] = b.code[i];
                ++j;
            }
            b.code.resize(j);
        }
        
        while(deadOps.size())
        {
            auto i = deadOps.back(); deadOps.pop_back();
    
            //printf("dead: "); debugOp(i);
            
            switch(ops[i].nInputs())
            {
            case 2:
                if(!--ops[ops[i].in[1]].nUse) deadOps.push_back(ops[i].in[1]);
            case 1:
                if(!--ops[ops[i].in[0]].nUse) deadOps.push_back(ops[i].in[0]);
            default:
                if(ops[i].opcode == ops::phi)
                {
                    blocks[ops[i].in[0]].args[ops[i].in[1]].alts.clear();
                }
                ops[i].opcode = ops::nop;
                progress = true;
            }
        }
    }

    // rebuild comeFrom, should delay this until iteration done
    for(int b = live.size();b--;) blocks[live[b]].comeFrom.clear();
    for(int b = live.size();b--;)
    {
        // if this fails, we're probably missing return
        assert(blocks[live[b]].code.size());
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

    for(auto & b : live)
    {
        // reset dominators
        blocks[b].dom.clear();
        if(!blocks[b].comeFrom.size())
            blocks[b].dom.push_back(b);
        else blocks[b].dom = live;
    }    

    // find dominator
    //
    // start with every node dominating itself
    // iterate blocks n until no change:
    //   tdom(n) = blocks
    //   for p in comeFrom(n):
    //     tdom(n) = sdom(n) intersect dom(p)
    //   dom(n) = { n } union sdom(n)
    int domIters = 0;
    bool iterate = true;
    std::vector<uint16_t>   tdom;
    while(iterate)
    {
        iterate = false;
        ++domIters;

        for(auto & b : live)
        {
            // this is entry block
            if(!blocks[b].comeFrom.size()) continue;

            tdom = live;
            for(auto & f : blocks[b].comeFrom)
            {
                for(int t = 0; t < tdom.size();)
                {
                    bool found = false;
                    for(auto & d : blocks[f].dom)
                    {
                        if(d == tdom[t]) found = true;
                    }
                    
                    if(found) { ++t; }
                    else
                    {
                        std::swap(tdom[t], tdom.back());
                        tdom.pop_back();
                    }
                }
            }
            
            bool foundSelf = false;
            for(auto & t : tdom) { if(t != b) continue; foundSelf = true; break; }
    
            if(!foundSelf) tdom.push_back(b);
            if(tdom.size() != blocks[b].dom.size()) iterate = true;

            // save copy, we'll reset tdom above anyway
            std::swap(blocks[b].dom, tdom);
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

    printf(" DCE:%d+%d", iters, domIters);
}

void Proc::findUsesBlock(int b, bool inOnly)
{
    // compute which ops are used by this block
    // this must be done in reverse
    for(int c = blocks[b].code.size(); c--;)
    {
        auto & op = ops[blocks[b].code[c]];

        if(op.opcode <= ops::jmp)
        for(int k = 0; k < 2; ++k)
        {
            if(k && op.opcode == ops::jmp) break;
            
            for(auto & a : blocks[op.label[k]].args)
            {
                for(auto & s : a.alts)
                {
                    if(s.src != b) continue;
                    
                    if(0) printf("live out %d->%d : v%04x\n",
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
    opt_dce();
    assert(live.size());   // at least one DCE required
    
    for(auto & op : ops)
    {
        // NOTE: nUse aliases on labels
        if(op.opcode > ops::jmp) op.nUse = 0;
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
            auto sz = blocks[b].livein.size();

            findUsesBlock(b, true);
            blocks[b].livein.clear();
    
            for(int i = 0; i < ops.size(); ++i)
            {
                // is this a variable that we need?
                if(!ops[i].hasOutput() || !ops[i].nUse) continue;

                if(0) printf(" v%04x live in %d\n", b, i);
                blocks[b].livein.push_back(i);
                ops[i].nUse = 0;
            }
    
            if(blocks[b].livein.size() != sz) progress = true;
        }
    }

    printf(" Live:%d\n", iter);
}