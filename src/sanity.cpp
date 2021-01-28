
#include "bjit.h"

using namespace bjit;

// Internal sanity checker
//
// This checks (some of the) invariants that we rely on.
//
// Note that in some cases we can violate invariants temporarily,
// but they should always check out after each DCE pass.
// 
void Proc::sanity() const
{
    assert(live.size());    // must have one pass DCE

    //debug();
    
    for(auto & b : live)
    {
        for(auto & c : blocks[b].code)
        {
            auto & op = ops[c];

            //debugOp(c);

            // sanity check that block/index are correct
            assert(op.index == c);
            assert(op.block == b);

            // sanity check that definitions dominate uses
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
            }
        }
    }

    printf(" SANE");
}