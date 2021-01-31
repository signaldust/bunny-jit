
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
    assert(live.size());    // must have one pass DCE

    livescan();
    opt_dce();  // do another round to get use counts?
    debug();
    
    for(auto & b : live)
    {
        for(auto & c : blocks[b].code)
        {
            auto & op = ops[c];

            //debugOp(c);

            // sanity check that block/index are correct
            assert(op.index == c);
            assert(op.block == b);

            if(op.opcode == ops::phi)
            {
                int phiSourcesFound = 0;
                for(auto & s : blocks[b].args[op.phiIndex].alts)
                {
                    bool phiSourceInComeFrom = false;
                    for(auto cf : blocks[b].comeFrom)
                    {
                        if(s.src != cf) continue;
                        phiSourceInComeFrom = true;
                        break;
                    }
                    assert(phiSourceInComeFrom);
                    ++phiSourcesFound;
                }
                assert(phiSourcesFound == blocks[b].args[op.phiIndex].alts.size());
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
                
                assert(inputDominates);
                
                bool liveIn = (ops[op.in[i]].block == b);
                if(!liveIn)
                {
                    for(auto & in : blocks[b].livein)
                    {
                        if(in == op.in[i]) liveIn = true;
                    }
                }
                assert(liveIn);
            }
        }
    }

    printf(" SANE\n");
}