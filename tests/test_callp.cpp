
#include "bjit.h"

#include <cstdint>

int isub2(int a, int b)
{
    printf("%d - %d = %d\n", a, b, a-b);
    return a - b;
}

double fsub2(double a, double b)
{
    printf("%f - %f = %f\n", a, b, a-b);
    return a - b;
}

int main()
{

    bjit::Module    module;

    printf("isub2 %lu, fsub2 %p\n", (uintptr_t)isub2, fsub2);

    {
        bjit::Proc      proc(0, "ii");
        proc.env[0] = proc.iadd(proc.env[0], proc.lci(1));
        proc.iret(proc.icallp(proc.lci(uintptr_t(isub2)), 2));
        proc.debug();
        proc.opt();
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.env[0] = proc.iadd(proc.env[0], proc.lci(1));
        proc.tcallp(proc.lci(uintptr_t(isub2)), 2);
        proc.debug();
        proc.opt();
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ff");
        proc.fret(proc.fcallp(proc.lci(uintptr_t(fsub2)), 2));
        proc.debug();
        proc.opt();
        module.compile(proc);
    }

    assert(module.load());

    auto codeOut = module.getBytes();
    if(codeOut.size())
    {
        FILE * f = fopen("out.bin", "wb");
        fwrite(codeOut.data(), 1, codeOut.size(), f);
        fclose(f);
        
        printf(" - Wrote out.bin\n");
    }
    
    auto ptr = module.getPointer<int(int,int)>(0);

    printf("icall\n");
    assert(module.getPointer<int(int,int)>(0)(5, 2) == 4);
    printf("tcall\n");
    assert(module.getPointer<int(int,int)>(1)(7, 1) == 7);
    printf("fcall\n");
    assert(module.getPointer<double(double,double)>(2)(5.5, 2) == 3.5);

    return 0;
}
