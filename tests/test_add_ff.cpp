
#include "bjit.h"

int main()
{

    bjit::Module    module;
    bjit::Proc      proc(0, "ff");

    proc.fret(proc.fadd(proc.env[0], proc.env[1]));

    proc.opt();
    
    int i = module.compile(proc);

    assert(module.load());

    auto ptr = module.getPointer<double(double,double)>(i);

    printf(" 2 + 5 = %f\n", ptr(2, 5));

    assert(ptr(2,5) == 7);

    return 0;
}
