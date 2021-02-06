
#pragma once

/*

//
// Basic instruction order is:
//   REX | OP | ModRM | SIB | DISP | IMM
//
// REX encoding: 0 1 0 0 W R X B
//  - W: op size override (1 = 64 bit)
//  - R: prefix ModRM.reg field
//  - X: prefix SIB.index
//  - B: prefix ModRM.rm field

This is the table for 32-bit mode, with RIP-relative patched in,
since rest of it is exactly the same except for REX bytes

[---] means SIB byte follows

r8(/r)                       AL   CL   DL   BL   AH   CH   DH   BH
r16(/r)                      AX   CX   DX   BX   SP   BP   SI   DI
r32(/r)                      EAX  ECX  EDX  EBX  ESP  EBP  ESI  EDI
mm(/r)                       MM0  MM1  MM2  MM3  MM4  MM5  MM6  MM7
xmm(/r)                      XMM0 XMM1 XMM2 XMM3 XMM4 XMM5 XMM6 XMM7
(In decimal) /digit (Opcode) 0    1    2    3    4    5    6    7
(In binary) REG =            000  001  010  011  100  101  110  111
Effective Address Mod R/M    Value of ModR/M Byte (in Hexadecimal)
[EAX]              00 000    00   08   10   18   20   28   30   38
[ECX]                 001    01   09   11   19   21   29   31   39
[EDX]                 010    02   0A   12   1A   22   2A   32   3A
[EBX]                 011    03   0B   13   1B   23   2B   33   3B
[---]                 100    04   0C   14   1C   24   2C   34   3C
[RIP]+disp32          101    05   0D   15   1D   25   2D   35   3D
[ESI]                 110    06   0E   16   1E   26   2E   36   3E
[EDI]                 111    07   0F   17   1F   27   2F   37   3F
[EAX]+disp8        01 000    40   48   50   58   60   68   70   78
[ECX]+disp8           001    41   49   51   59   61   69   71   79
[EDX]+disp8           010    42   4A   52   5A   62   6A   72   7A
[EBX]+disp8           011    43   4B   53   5B   63   6B   73   7B
[---]+disp8           100    44   4C   54   5C   64   6C   74   7C
[EBP]+disp8           101    45   4D   55   5D   65   6D   75   7D
[ESI]+disp8           110    46   4E   56   5E   66   6E   76   7E
[EDI]+disp8           111    47   4F   57   5F   67   6F   77   7F
[EAX]+disp32       10 000    80   88   90   98   A0   A8   B0   B8
[ECX]+disp32          001    81   89   91   99   A1   A9   B1   B9
[EDX]+disp32          010    82   8A   92   9A   A2   AA   B2   BA
[EBX]+disp32          011    83   8B   93   9B   A3   AB   B3   BB
[---]+disp32          100    84   8C   94   9C   A4   AC   B4   BC
[EBP]+disp32          101    85   8D   95   9D   A5   AD   B5   BD
[ESI]+disp32          110    86   8E   96   9E   A6   AE   B6   BE
[EDI]+disp32          111    87   8F   97   9F   A7   AF   B7   BF
EAX/AX/AL/MM0/XMM0 11 000    C0   C8   D0   D8   E0   E8   F0   F8
ECX/CX/CL/MM/XMM1     001    C1   C9   D1   D9   E1   E9   F1   F9
EDX/DX/DL/MM2/XMM2    010    C2   CA   D2   DA   E2   EA   F2   FA
EBX/BX/BL/MM3/XMM3    011    C3   CB   D3   DB   E3   EB   F3   FB
ESP/SP/AH/MM4/XMM4    100    C4   CC   D4   DC   E4   EC   F4   FC
EBP/BP/CH/MM5/XMM5    101    C5   CD   D5   DD   E5   ED   F5   FD
ESI/SI/DH/MM6/XMM6    110    C6   CE   D6   DE   E6   EE   F6   FE
EDI/DI/BH/MM7/XMM7    111    C7   CF   D7   DF   E7   EF   F7   FF

*/

namespace bjit
{

    // we use this for types, etc
    typedef uint64_t    RegMask;

    namespace regs
    {
        // List of registers for register allocator
        // These should be in preference order
        //
        // FIXME: order is not necessarily ideal
        //
#define BJIT_REGS(_) \
        /* caller saved (rsi & rdi callee saved on win32) */ \
        _(rax), _(rsi), _(rdi), _(rcx), _(rdx), \
         _(r8), _(r9), _(r10), _(r11), \
        /* callee saved */ \
        _(rbx), _(rbp), _(r12), _(r13), _(r14), _(r15), _(rsp), \
        /* floating point */ \
        _(xmm0), _(xmm1), _(xmm2), _(xmm3), \
        _(xmm4), _(xmm5), _(xmm6), _(xmm7), \
        _(xmm8), _(xmm9), _(xmm10), _(xmm11), \
        _(xmm12), _(xmm13), _(xmm14), _(xmm15), \
        /* placeholder */ \
        _(none)

#define BJIT_REGS_ENUM(x) x
        // set nregs as the first non-register value
        enum { BJIT_REGS(BJIT_REGS_ENUM), nregs = none };

        // dummy type for ops without outputs
        static const RegMask type_none = 0;

        
        // Integer register mask
        static const RegMask mask_int
            =(1ull<<rax)
            |(1ull<<rdx)
            |(1ull<<rbx)
            |(1ull<<rcx)
            |(1ull<<rsi)
            |(1ull<<rdi)
            |(1ull<<rbp)
            |(1ull<<r8)
            |(1ull<<r9)
            |(1ull<<r10)
            |(1ull<<r11)
            |(1ull<<r12)
            |(1ull<<r13)
            |(1ull<<r14)
            |(1ull<<r15) //*/
            ;

        // Float register masks
        static const RegMask mask_float
            =(1ull<<xmm0)
            |(1ull<<xmm1)
            |(1ull<<xmm2)
            |(1ull<<xmm3)
            |(1ull<<xmm4)
            |(1ull<<xmm5)
            |(1ull<<xmm6)
            |(1ull<<xmm7)
            |(1ull<<xmm8)
            |(1ull<<xmm9)
            |(1ull<<xmm10)
            |(1ull<<xmm11)
    		|(1ull<<xmm12)
            |(1ull<<xmm13)
            |(1ull<<xmm14)
            |(1ull<<xmm15)
            ;

        // Caller saved (lost on function call)
        //
        // NOTE: We treat all xmm registers as volatile when calling functions.
        // The backend uses separate logic for callee_saved when we are callee.
        //
        // NOTE: This list MUST include any registers used for arguments.
        static const RegMask caller_saved
            =(1ull<<rax)
#ifndef _WIN32
            |(1ull<<rsi)
            |(1ull<<rdi)
#endif
            |(1ull<<rcx)
            |(1ull<<rdx)
            |(1ull<<r8)
            |(1ull<<r9)
            |(1ull<<r10)
            |(1ull<<r11)
            |mask_float;
            
        const char * getName(int reg);
    };

};

