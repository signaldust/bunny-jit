
#include "bjit.h"

int main()
{

    bjit::Module    module;

    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.idiv(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.imod(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.udiv(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
   {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.umod(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
   
    auto & codeOut = module.getBytes();
    
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    
    printf(" - Wrote out.bin\n");

    BJIT_ASSERT(module.load());

    int64_t     s = -3249421;
    uint64_t    u = 55425439;

    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(0)(s,3) == (s/3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(1)(s,3) == (s%3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(1)(s,-3) == (s%-3));
    
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(2)(u,3) == (u/3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(3)(u,3) == (u%3));
    BJIT_ASSERT(module.getPointer<uint64_t(uint64_t,uint64_t)>(3)(u,-3) == (u%-3));

    return 0;
}
