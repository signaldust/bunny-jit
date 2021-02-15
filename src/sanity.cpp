
#include "bjit.h"

using namespace bjit;

// Internal sanity checker
//
// This checks (some of the) invariants that we rely on.
//
// Note that in some cases we can violate invariants temporarily,
// but they should always check out after each DCE pass.
// 
void Proc::sanity()
{
    BJIT_ASSERT(live.size());    // must have one pass DCE

    rebuild_livein();
    opt_dce();  // do another round to get use counts?
    rebuild_dom();
    
    debug();
    
    for(auto & b : live)
    {
        for(auto & c : blocks[b].code)
        {
            auto & op = ops[c];

            //debugOp(c);

            // sanity check that block/index are correct
            BJIT_ASSERT(op.index == c);
            BJIT_ASSERT(op.block == b);

            if(op.opcode == ops::phi)
            {
                int nPhiSrc = 0;
                for(auto & a : blocks[b].alts) if(a.phi == c) ++nPhiSrc;
                BJIT_ASSERT(nPhiSrc == blocks[b].comeFrom.size());
            
                int phiSourcesFound = 0;
                for(auto & s : blocks[b].alts)
                {
                    if(s.phi != c) continue;
                    
                    bool phiSourceInComeFrom = false;
                    for(auto cf : blocks[b].comeFrom)
                    {
                        if(s.src != cf) continue;
                        phiSourceInComeFrom = true;
                        break;
                    }
                    BJIT_ASSERT(phiSourceInComeFrom);
                    ++phiSourcesFound;
                }
                BJIT_ASSERT(phiSourcesFound == blocks[b].comeFrom.size());
            }
            
            // sanity check that definitions dominate uses
            // also check that non-locals are marked as livein
            for(int i = 0; i < op.nInputs(); ++i)
            {
                bool inputDominates = false;
                for(auto & d : blocks[b].dom)
                {
                    if(d == ops[op.in[i]].block)
                    {
                        inputDominates = true;
                        break;
                    }
                }
                
                BJIT_ASSERT(inputDominates);
                
                bool liveIn = (ops[op.in[i]].block == b);
                if(!liveIn)
                {
                    for(auto & in : blocks[b].livein)
                    {
                        if(in == op.in[i]) liveIn = true;
                    }
                }
                BJIT_ASSERT(liveIn);
            }
        }
    }

    BJIT_LOG(" SANE\n");
}