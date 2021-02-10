
#include "bjit.h"

int main()
{

    bjit::Module    module;
    {
        bjit::Proc  pr(0, "i");

        // these should get CSE
        auto a = pr.iadd(pr.li32(pr.env[0],0), pr.li32(pr.env[0],0));

        // this should break CSE on loads
        pr.si32(pr.env[0], 0, pr.lci(1));

        pr.iret(pr.iadd(a, pr.li32(pr.env[0],0)));

        pr.debug();
        module.compile(pr);
    }
    {
        bjit::Proc  pr(0, "i");

        // these should get CSE
        auto a = pr.iadd(pr.li32(pr.env[0],0), pr.li32(pr.env[0],0));

        // this should break CSE on loads
        pr.si32(pr.env[0], 0, pr.lci(1));

        pr.iret(pr.iadd(a, pr.li32(pr.env[0],0)));

        pr.debug();
        module.compile(pr);
    }
    auto & codeOut = module.getBytes();
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    printf(" - Wrote out.bin\n");
    BJIT_ASSERT(module.load());

    int v = 42;

    BJIT_ASSERT((2*42+1) == module.getPointer<int(int*)>(0)(&v));
    BJIT_ASSERT(v == 1);
    
    return 0;
}
