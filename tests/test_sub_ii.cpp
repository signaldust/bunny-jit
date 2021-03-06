
#include "bjit.h"

int main()
{

    bjit::Module    module;
    bjit::Proc      proc(0, "ii");

    proc.iret(proc.isub(proc.env[0], proc.env[1]));

    int i = module.compile(proc);

    BJIT_ASSERT(module.load());

    auto ptr = module.getPointer<int(int,int)>(i);

    printf(" 5 - 2 = %d\n", ptr(5, 2));

    BJIT_ASSERT(ptr(5,2) == 3);

    return 0;
}
