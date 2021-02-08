
#include "bjit.h"

int proc(int x, int y)
{
    int i = 0;

    while(true)
    {
        if((++i) >= x) break;
        if((++i) >= y) break;

        ++i;
    };

    return i;
}

int main()
{

    bjit::Module    module;
    {
        bjit::Proc  pr(0, "ii");

        pr.env.push_back(pr.lci(0));

        auto la = pr.newLabel();
        auto lb = pr.newLabel();
        auto lc = pr.newLabel();
        auto le = pr.newLabel();

        pr.jmp(la);

        pr.emitLabel(la);
        pr.env[2] = pr.iadd(pr.env[2], pr.lci(1));
        pr.jnz(pr.ige(pr.env[2], pr.env[0]), le, lb);
        
        pr.emitLabel(lb);
        pr.env[2] = pr.iadd(pr.env[2], pr.lci(1));
        pr.jnz(pr.ige(pr.env[2], pr.env[1]), le, lc);
        
        pr.emitLabel(lc);
        pr.env[2] = pr.iadd(pr.env[2], pr.lci(1));
        pr.jmp(la);

        pr.emitLabel(le);
        pr.iret(pr.env[2]);

        pr.debug();
        module.compile(pr);
    }
    auto & codeOut = module.getBytes();
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    printf(" - Wrote out.bin\n");
    BJIT_ASSERT(module.load());

    for(int i = 0; i < 16; ++i)
    {
        auto h = bjit::hash64(i+1);
        int x = h&0xff;
        int y = (h>>8)&0xff;
        int z = proc(x,y);
        int zjit = module.getPointer<int(int,int)>(0)(x,y);
        printf("proc(%d,%d) = %d (jit says %d)\n", x, y, z, zjit);
        BJIT_ASSERT(z == zjit);
    }

    return 0;
}
