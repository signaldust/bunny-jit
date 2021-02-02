
#include "bjit.h"

int main()
{

    bjit::Module    module;

    {
        bjit::Proc      proc(0, "ff");
        proc.fret(proc.fadd(proc.env[0], proc.env[1]));
        proc.opt();
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "dd");
        proc.dret(proc.dadd(proc.env[0], proc.env[1]));
        proc.opt();
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "dd");
        proc.dret(proc.cf2d(proc.fadd(
            proc.cd2f(proc.env[0]), proc.cd2f(proc.env[1]))));
        proc.opt();
        module.compile(proc);
    }
    
    auto & codeOut = module.getBytes();
    
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    
    printf(" - Wrote out.bin\n");

    assert(module.load());
    
    assert(module.getPointer<float(float,float)>(0)(1.f, 5.5f) == 6.5f);
    assert(module.getPointer<double(double,double)>(1)(2.5, 3.25) == 5.75);
    assert(module.getPointer<double(double,double)>(2)(3.25, 4.5) == 7.75);

    return 0;
}
