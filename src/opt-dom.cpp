
#include "bjit.h"

using namespace bjit;

void Proc::rebuild_cfg()
{
    // rebuild comeFrom, should delay this until iteration done
    for(int b = live.size();b--;) blocks[live[b]].comeFrom.clear();
    for(int b = live.size();b--;)
    {
        // if this fails, we're probably missing return
        BJIT_ASSERT(blocks[live[b]].code.size());
        BJIT_ASSERT(ops[blocks[b].code.back()].opcode <= ops::tcalln);
        
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
    {
        int j = 0;
        auto & alts = blocks[b].alts;
        for(int i = 0; i < alts.size(); ++i)
        {
            if(ops[alts[i].val].opcode == ops::nop) continue;
            
            bool keep = false;
            for(auto s : blocks[b].comeFrom)
            {
                if(alts[i].src != s) continue;
                keep = true;
                break;
            }
            if(!keep) continue;
            if(i != j) alts[j] = alts[i];
            ++j;
        }
        if(j != alts.size()) alts.resize(j);
    }
}

void Proc::rebuild_dom()
{
    rebuild_cfg();
    
    // find dominator algorithm
    //
    // start with every node dominating itself
    // iterate blocks n until no change:
    //   tdom(n) = blocks
    //   for p in comeFrom(n):
    //     tdom(n) = sdom(n) intersect dom(p)
    //   dom(n) = { n } union sdom(n)


    // We run the same algorithm twice, first for post-dominators
    // then for dominators. We only keep immediate post-dominators
    // but we rebuild (in order) the full-list for dominators.
    //
    // We do post-dominators first so we can use .dom as temp for both
    // which saves some useless per-block allocation.
    
    int domIters = 0;
    bool iterate = true;
    std::vector<uint16_t>   tdom;
    
    // post dominators first, so we can reuse .dom
    for(auto & b : live)
    {
        // reset postdominators
        if(ops[blocks[b].code.back()].opcode > ops::jmp)
        {
            blocks[b].dom.clear();
            blocks[b].dom.push_back(b);
        }
        else blocks[b].dom = live;
    }    
    
    while(iterate)
    {
        iterate = false;
        ++domIters;

        // backwards
        for(int bi = live.size(); bi--;)
        {
            auto & b = blocks[live[bi]];
            auto & jmp = ops[b.code.back()];
            // this is an exit block
            if(jmp.opcode > ops::jmp) continue;

            int nLabel = (jmp.opcode == ops::jmp) ? 1 : 2;

            tdom = live;
            for(int k = 0; k < nLabel; ++k)
            {
                for(int t = 0; t < tdom.size();)
                {
                    bool found = false;
                    for(auto & d : blocks[jmp.label[k]].dom)
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
            for(auto & t : tdom)
            { if(t != live[bi]) continue; foundSelf = true; break; }
    
            if(!foundSelf) tdom.push_back(live[bi]);
            if(tdom.size() != b.dom.size()) iterate = true;

            // save copy, we'll reset tdom above anyway
            std::swap(b.dom, tdom);
        }
    }

    // push theoretical exit-block (unifies multiple returns)
    for(auto & b : live) { blocks[b].dom.push_back(noVal); }

    // find immediate post-dominators: we use the fact that the immediate
    // dominator must have exactly one less dominator
    for(auto & b : live)
    {
        blocks[b].pdom = noVal;
        for(auto & d : blocks[b].dom)
        {
            if(d == noVal) continue;   // no common post-dominator
            if(blocks[d].dom.size() == blocks[b].dom.size() - 1)
            {
                blocks[b].pdom = d;
                break;
            }
        }
    }
    
    // forward pass
    for(auto & b : live)
    {
        // reset dominators
        if(!b) {
            blocks[b].dom.clear();
            blocks[b].dom.push_back(b);
        }
        else blocks[b].dom = live;
    }
    
    iterate = true;
    while(iterate)
    {
        iterate = false;
        ++domIters;

        for(auto & b : live)
        {
            // this is entry block
            if(!b) continue;
            BJIT_ASSERT(blocks[b].comeFrom.size());

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
    
    // find immediate dominators: we use the fact that the immediate
    // dominator must have exactly one less dominator
    for(auto & b : live)
    {
        blocks[b].idom = 0;
        
        for(auto & d : blocks[b].dom)
        {
            if(blocks[d].dom.size() == blocks[b].dom.size() - 1)
            {
                blocks[b].idom = d;
                break;
            }
        }
        
    }

    // order dominators; we use these for CCD in CSE
    for(auto & b : live)
    {
        for(auto & d : blocks[b].dom) d = noVal;
        for(int d = b, i = blocks[b].dom.size(); i--;)
        {
            blocks[b].dom[i] = d;
            d = blocks[d].idom;
        }
    }

    BJIT_LOG(" Dom:%d", domIters);
};