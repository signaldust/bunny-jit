
#include "bjit.h"

#include <cstdint>

struct TestData
{
    union
    {
        int8_t      i8;
        uint8_t     u8;
        uint64_t    i8_64;
    };
    union
    {
        int16_t     i16;
        uint16_t    u16;
        uint64_t    i16_64;
    };
    union
    {
        int32_t     i32;
        uint32_t    u32;
        uint64_t    i32_64;
    };
    uint64_t        i64;

    float   f32;
    double  f64;
};

int main()
{

    bjit::Module    module;

    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.li8(proc.env[0], offsetof(TestData,i8)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.li16(proc.env[0], offsetof(TestData,i16)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.li32(proc.env[0], offsetof(TestData,i32)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.lu8(proc.env[0], offsetof(TestData,u8)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.lu16(proc.env[0], offsetof(TestData,u16)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.lu32(proc.env[0], offsetof(TestData,u32)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.iret(proc.li64(proc.env[0], offsetof(TestData,i64)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.fret(proc.lf32(proc.env[0], offsetof(TestData,f32)));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "i");
        proc.dret(proc.lf64(proc.env[0], offsetof(TestData,f64)));
        module.compile(proc);
    }

    BJIT_ASSERT(module.load(0x10000));

    {
        auto & codeOut = module.getBytes();
        FILE * f = fopen("out.bin", "wb");
        fwrite(codeOut.data(), 1, codeOut.size(), f);
        fclose(f);
        printf(" - Wrote out.bin\n");
    }
    
    TestData    test;
    test.i8_64  = 0xc0c1c2c3c4c5c6c7;
    test.i16_64 = 0xd0d1d2d3d4d5d6d7;
    test.i32_64 = 0xe0e1e2e3e4e5e6e7;
    test.i64    = 0xf0f1f2f3f4f5f6f7;
    test.f32    = 1.5f;
    test.f64    = 3.14;

    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(0)(&test) == test.i8);
    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(1)(&test) == test.i16);
    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(2)(&test) == test.i32);

    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(3)(&test) == test.u8);
    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(4)(&test) == test.u16);
    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(5)(&test) == test.u32);
    
    BJIT_ASSERT(module.getPointer<uint64_t(TestData*)>(6)(&test) == test.i64);
    
    BJIT_ASSERT(module.getPointer<float(TestData*)>(7)(&test) == test.f32);
    BJIT_ASSERT(module.getPointer<double(TestData*)>(8)(&test) == test.f64);

    {
        bjit::Proc      proc(0, "ii");
        proc.si8(proc.env[1], proc.env[0], offsetof(TestData,i8));
        proc.iret(proc.lci(0));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.si16(proc.env[1], proc.env[0], offsetof(TestData,i16));
        proc.iret(proc.lci(0));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.si32(proc.env[1], proc.env[0], offsetof(TestData,i32));
        proc.iret(proc.lci(0));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "ii");
        proc.si64(proc.env[1], proc.env[0], offsetof(TestData,i64));
        proc.iret(proc.lci(0));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "if");
        proc.sf32(proc.env[1], proc.env[0], offsetof(TestData,f32));
        proc.iret(proc.lci(0));
        module.compile(proc);
    }
    {
        bjit::Proc      proc(0, "id");
        proc.sf64(proc.env[1], proc.env[0], offsetof(TestData,f64));
        proc.iret(proc.lci(0));
        module.compile(proc);
    }

    {
        auto & codeOut = module.getBytes();
        FILE * f = fopen("out.bin", "wb");
        fwrite(codeOut.data(), 1, codeOut.size(), f);
        fclose(f);
        printf(" - Wrote out.bin\n");
    }
    
    BJIT_ASSERT(module.patch());
    uint64_t v = 0xf0f1f2f3f4f5f6f7;

    module.getPointer<void(TestData*,uint64_t)>(9)(&test, v);
    BJIT_ASSERT(test.i8 == (int8_t) v);

    module.getPointer<void(TestData*,uint64_t)>(10)(&test, v);
    BJIT_ASSERT(test.i16 == (int16_t) v);
    
    module.getPointer<void(TestData*,uint64_t)>(11)(&test, v);
    BJIT_ASSERT(test.i32 == (int32_t) v);

    module.getPointer<void(TestData*,uint64_t)>(12)(&test, v);
    BJIT_ASSERT(test.i64 == v);

    module.getPointer<void(TestData*,float)>(13)(&test, 3.14f);
    BJIT_ASSERT(test.f32 == 3.14f);
    
    module.getPointer<void(TestData*,double)>(14)(&test, 1.5);
    BJIT_ASSERT(test.f64 == 1.5);
    
    module.unload();
    
    return 0;
}
