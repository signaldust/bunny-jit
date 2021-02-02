
#include "bjit.h"

int main()
{

    bjit::Module    module;
    bjit::Proc      proc(0, "if");

    proc.iret(proc.cf2i(proc.fadd(proc.ci2f(proc.env[0]), proc.env[1])));

    proc.opt();
    
    int i = module.compile(proc);

    assert(module.load());

    auto ptr = module.getPointer<int(int,double)>(i);

    printf(" 2 + 5 = %d\n", ptr(2, 5));

    assert(ptr(2,5) == 7);

    return 0;
}
