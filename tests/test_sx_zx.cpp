
#include "bjit.h"

#include <cstdint>

int main()
{

    bjit::Module    module;

    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.u8(proc.env[0]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.u16(proc.env[0]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.u32(proc.env[0]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.i8(proc.env[0]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.i16(proc.env[0]));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.i32(proc.env[0]));
        module.compile(proc);
    }

    auto & codeOut = module.getBytes();
    
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    
    printf(" - Wrote out.bin\n");

    assert(module.load());

    uint64_t v = 0xfedcba9876543210ull;

    assert(module.getPointer<uint64_t(uint64_t)>(0)(v) == 0x10);
    assert(module.getPointer<uint64_t(uint64_t)>(1)(v) == 0x3210);
    assert(module.getPointer<uint64_t(uint64_t)>(2)(v) == 0x76543210);

    assert(module.getPointer<uint64_t(uint64_t)>(3)(0x2ff) == ~0ull);
    assert(module.getPointer<uint64_t(uint64_t)>(4)(0x2ffff) == ~0ull);
    assert(module.getPointer<uint64_t(uint64_t)>(5)(0x2ffffffff) == ~0ull);
    
    return 0;
}
