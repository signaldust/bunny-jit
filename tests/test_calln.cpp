
#include "bjit.h"

#include <cstdint>


int main()
{

    bjit::Module    module;

    // proc 0, does actual sub
    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.isub(proc.env[0], proc.env[1]));
        module.compile(proc);
    }
    // proc 1, icalln
    {
        bjit::Proc      proc(0, "ii");
        proc.iret(proc.icalln(2, 2));
        module.compile(proc);
    }
    // proc 2: tcalln
    {
        bjit::Proc      proc(0, "ii");
        proc.tcalln(0, 2);
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

    assert(module.getPointer<int(int,int)>(1)(5, 2) == 3);
    assert(module.getPointer<int(int,int)>(2)(7, 3) == 4);

    return 0;
}
