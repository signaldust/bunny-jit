
#include "bjit.h"

int main()
{

    bjit::Module    module;

    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.ishl(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.ishr(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.ushr(proc.env[0], proc.env[1]));
        module.compile(proc);
    }

    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.ishl(proc.env[0], proc.lci(3)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.ishr(proc.env[0], proc.lci(3)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.ushr(proc.env[0], proc.lci(3)));
        module.compile(proc);
    }
    
    auto & codeOut = module.getBytes();
    
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    
    printf(" - Wrote out.bin\n");

    BJIT_ASSERT(module.load());

    int64_t     s = 3;
    uint64_t    u = 5;

    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(0)(s,3) == (s<<3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(1)(~s,3) == (~s>>3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(2)(~u,3) == (~u>>3));

    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t)>(3)(s) == (s<<3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t)>(4)(~s) == (~s>>3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t)>(5)(~u) == (~u>>3));
    
    return 0;
}
