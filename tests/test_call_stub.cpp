
#include "bjit.h"

#include <cstdint>
#include <cstdio>

int hello()
{
    printf("Hello world\n");

    return 42;
}

int helloAgain()
{
    printf("Hello world, again\n");

    return 45;
}

int main()
{

    bjit::Module    module;

    // proc 0, stub
    module.compileStub(0);

    // proc 1, near-call stub
    {
        bjit::Proc      proc(0, "");
        proc.iret(proc.icalln(0, 0));
        module.compile(proc);
    }
    
    BJIT_ASSERT(module.load());

    auto codeOut = module.getBytes();
    if(codeOut.size())
    {
        FILE * f = fopen("out.bin", "wb");
        fwrite(codeOut.data(), 1, codeOut.size(), f);
        fclose(f);
        
        printf(" - Wrote out.bin\n");
    }

    module.patchStub(0, (uintptr_t)&hello);
    module.patch();
    
    BJIT_ASSERT(module.getPointer<int()>(1)() == 42);

    module.unload();
    
    module.patchStub(0, (uintptr_t)&helloAgain);
    module.load();
    BJIT_ASSERT(module.getPointer<int()>(1)() == 45);

    return 0;
}
