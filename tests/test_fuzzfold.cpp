
#include "bjit.h"

// Define to only run one (eg. known to fail) case
// and enable dumping the code into a file
//#define ONECASE 93495

uintptr_t iFuzzSeed(uint64_t seed, int opt)
{
    bjit::Module    module;
    bjit::Proc      proc(0, "iiii");

    auto random = [&]() -> uint64_t { return bjit::hash64(seed++); };

    for(int i = 0; i < 64; ++i)
    {
        int op = random() % 11;

        // skip the alloc
        int a0 = 1 + (random() % (proc.env.size()-1));
        int a1 = 1 + (random() % (proc.env.size()-1));

        switch(op)
        {
        case 0: proc.env.push_back(proc.lci(random())); break;
        case 1: proc.env.push_back(proc.lci(0x1<<31)); break;
        case 2: proc.env.push_back(proc.lci((uint32_t)random())); break;
        
        case 3: proc.env.push_back(proc.iadd(proc.env[a0], proc.env[a1])); break;
        case 4: proc.env.push_back(proc.isub(proc.env[a0], proc.env[a1])); break;
        case 5: proc.env.push_back(proc.imul(proc.env[a0], proc.env[a1])); break;
        
        case 6: proc.env.push_back(proc.iand(proc.env[a0], proc.env[a1])); break;
        case 7: proc.env.push_back(proc.ior(proc.env[a0], proc.env[a1])); break;
        case 8: proc.env.push_back(proc.ixor(proc.env[a0], proc.env[a1])); break;

        case 9: proc.env.push_back(proc.ineg(proc.env[a0])); break;
        case 10: proc.env.push_back(proc.inot(proc.env[a0])); break;
        }
    }

    proc.iret(proc.env[1 + (random() % (proc.env.size()-1))]);
    module.compile(proc, opt);
#ifdef ONECASE
    if(opt)
#else
    if(false)
#endif
    {
        auto & codeOut = module.getBytes();
        FILE * f = fopen("out.bin", "wb");
        fwrite(codeOut.data(), 1, codeOut.size(), f);
        fclose(f);
        printf(" - Wrote out.bin\n");
    }
    module.load();
    auto ptr = module.getPointer<uintptr_t(int,int,int,int)>(0);

    int p0 = random();
    int p1 = random();
    int p2 = random();
    int p3 = random();
    return ptr(p0, p1, p2, p3);
}

int main()
{
#ifdef ONECASE
    for(int i = ONECASE; i == ONECASE; ++i)
#else
    for(int i = 0; i < 123456; ++i)
#endif
    {
        auto seed = bjit::hash64(i);
        auto fuzz0 = iFuzzSeed(seed, 0);
        auto fuzz2 = iFuzzSeed(seed, 2);
        BJIT_LOG("\nTest iter %d\n", i);
        if(fuzz0 != fuzz2)
            BJIT_LOG(" %p != %p\n", (void*)fuzz0, (void*)fuzz2);
        BJIT_ASSERT(fuzz0 == fuzz2);

        BJIT_LOG(" OK: %d\n", i);
    }

    return 0;
}
