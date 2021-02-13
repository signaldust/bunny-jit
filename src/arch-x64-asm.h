
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <x86intrin.h>

#include "arch-x64.h"

namespace bjit
{

static const uint8_t RIP = 0xff;    // otherwise invalid, used for RIP relative

// encode register from our naming to X64 values
static uint8_t REG(int r)
{
    using namespace regs;

    switch(r)
    {
        case xmm0: case rax: return 0;
        case xmm1: case rcx: return 1;
        case xmm2: case rdx: return 2;
        case xmm3: case rbx: return 3;
        case xmm4: case rsp: return 4;
        case xmm5: case rbp: return 5;
        case xmm6: case rsi: return 6;
        case xmm7: case rdi: return 7;

        case xmm8: case r8: return 8;
        case xmm9: case r9: return 9;
        case xmm10: case r10: return 10;
        case xmm11: case r11: return 11;
        case xmm12: case r12: return 12;
        case xmm13: case r13: return 13;
        case xmm14: case r14: return 14;
        case xmm15: case r15: return 15;

        // this is only used internally
        case RIP: return RIP;
    }

    BJIT_ASSERT(false);
    return 0;
}

// return the 4-bit Condition Code part of conditional ops
uint8_t _CC(uint8_t opcode)
{
    switch(opcode)
    {
        case ops::jilt: return 0xC;
        case ops::jige: return 0xD;
        case ops::jigt: return 0xF;
        case ops::jile: return 0xE;

        // floating point conditions match unsigned(!)
        case ops::jult: case ops::jdlt: case ops::jflt: return 0x2;
        case ops::juge: case ops::jdge: case ops::jfge: return 0x3;
        case ops::jugt: case ops::jdgt: case ops::jfgt: return 0x7;
        case ops::jule: case ops::jdle: case ops::jfle: return 0x6;

        case ops::jine: case ops::jdne: case ops::jfne: case ops::jnz: return 0x5;
        case ops::jieq: case ops::jdeq: case ops::jfeq: case ops::jz:  return 0x4;

        default: break;
    }
    // silence warning if assert is nop
    BJIT_ASSERT(false); return 0;
}

// see arch-x64.h for instruction encoding notes
struct AsmX64
{
    std::vector<uint8_t>   & out;

    AsmX64(std::vector<uint8_t> & out, unsigned nBlocks) : out(out)
    {
        rodata32_index = nBlocks++;
        rodata64_index = nBlocks++;
        rodata128_index = nBlocks++;
        blockOffsets.resize(nBlocks);
    }

    // separate .rodata for 128/64/32 bit constants
    // we will place the most aligned block first
    std::vector<__m128>     rodata128;
    uint32_t                rodata128_index;    // index into blockOffsets
    
    std::vector<uint64_t>   rodata64;
    uint32_t                rodata64_index;     // index into blockOffsets
    
    std::vector<uint64_t>   rodata32;
    uint32_t                rodata32_index;     // index into blockOffsets

    // stores byteOffsets to each basic block for relocation
    std::vector<uint32_t>   blockOffsets;

    struct Reloc
    {
        uint32_t    codeOffset;
        uint32_t    blockIndex;
    };

    std::vector<Reloc>      relocations;

    void emit(uint8_t byte) { out.push_back(byte); }
    void emit32(uint32_t data)
    {
        out.push_back(data & 0xff); data >>= 8;
        out.push_back(data & 0xff); data >>= 8;
        out.push_back(data & 0xff); data >>= 8;
        out.push_back(data & 0xff);
    }

    // add relocation entry
    void addReloc(uint32_t block)
    {
        relocations.resize(relocations.size()+1);
        relocations.back().codeOffset = out.size();
        relocations.back().blockIndex = block;
    }
    
    // store 32-bit constant into .rodata32
    // add relocation and return offset for RIP relative
    uint32_t data32(uint32_t data)
    {
        unsigned index = rodata32.size();
        // try to find an existing constant with same value
        for(unsigned i = 0; i < rodata64.size(); ++i)
        {
            if(rodata32[i] == data) { index = i; break; }
        }
        
        if(index == rodata32.size()) rodata32.push_back(data);
        addReloc(rodata32_index);
        return index*sizeof(uint32_t);
    }

    // store 64-bit constant into .rodata64
    // add relocation and return offset for RIP relative
    uint32_t data64(uint64_t data)
    {
        unsigned index = rodata64.size();
        // try to find an existing constant with same value
        for(unsigned i = 0; i < rodata64.size(); ++i)
        {
            if(rodata64[i] == data) { index = i; break; }
        }
        
        if(index == rodata64.size()) rodata64.push_back(data);
        addReloc(rodata64_index);
        return index*sizeof(uint64_t);
    }

    uint32_t data32f(float data)
    {
        return data32(*reinterpret_cast<uint32_t*>(&data));
    }

    uint32_t data64f(double data)
    {
        return data64(*reinterpret_cast<uint64_t*>(&data));
    }

    uint32_t data128(__m128 data)
    {
        unsigned index = rodata128.size();
        // try to find an existing constant with same value
        for(unsigned i = 0; i < rodata128.size(); ++i)
        {
            if(!memcmp(&rodata128[i],&data,sizeof(__m128))) { index = i; break; }
        }
        
        if(index == rodata128.size()) rodata128.push_back(data);
        addReloc(rodata128_index);
        return index*sizeof(__m128);
    }

    // if wide == 0, then don't set REX.W and omit byte if possible
    // if wide == 1, then set REX.W
    // if wide == 2, then we force REX if reg is RSP/RBP/RDI/RSI
    // if wide == 3, then we force REX if rm is RSP/RBP/RDI/RSI
    void _REX(int wide, uint8_t reg, uint8_t rm, uint8_t sib = 0)
    {
        uint8_t flags = ((reg&8)>>1) | ((sib&8)>>2) | ((rm&8)>>3);
        if(wide == 1) flags |= 0x8;
    
        // ignore forced REX if the register doesn't need it
        if(wide == 2)
        {
            if(reg != REG(regs::rsp)
            && reg != REG(regs::rbp)
            && reg != REG(regs::rdi)
            && reg != REG(regs::rsi)) wide = 0;
        }
    
        // same thing, but used when register goes in rm
        if(wide == 3)
        {
            if(rm != REG(regs::rsp)
            && rm != REG(regs::rbp)
            && rm != REG(regs::rdi)
            && rm != REG(regs::rsi)) wide = 0;
        }
    
        // don't emit REX if it's useless
        if(flags || wide) emit(0x40 | flags);
    }

    // prefixes that go before REX, modifies arguments
    void _PREFIX(int & op0, int & op1, int & op2)
    {
        // 0x66 is opsize prefix, 0xF2 and 0xF3 are SSE prefixes
        if(op0 == 0x66 || op0 == 0xF2 || op0 == 0xF3)
        {
            emit(op0); op0 = op1; op1 = op2; op2 = -1;
        }
    }

    // emit up to 3 opcode bytes
    void _OP(int op0, int op1, int op2)
    {
        emit(op0); if(op1 == -1) return;
        emit(op1); if(op2 == -1) return;
        emit(op2);
    }

    // emit ModRM
    void _ModRM(uint8_t mod, uint8_t reg, uint8_t rm)
    {
        emit(((mod&3)<<6) | ((reg&7)<<3) | (rm&7) );
    }

    // SIB bytes: [base+index*2^scale]
    void _SIB(uint8_t base, uint8_t index, uint8_t scale)
    {
        _ModRM(scale, index, base);
    }

    // encode reg-reg instructions with up to 3 bytes
    void _RR(int wide, int r0, int r1, int op0, int op1 = -1, int op2 = -1)
    {
        _PREFIX(op0, op1, op2);
        _REX(wide, r0, r1);
        _OP(op0, op1, op2);
        _ModRM(3, r0, r1);
    }

    // this encodes r, [r+r*(1<<scale)] cases (eg. for LEA)
    void _RRRs(int w, int r0, int r1, int r2, int scale,
        int op0, int op1 = -1, int op2 = -1)
    {
        if((0x7 & r1) == REG(regs::rbp) && !scale
        && (0x7 & r2) != REG(regs::rsp)) std::swap(r1, r2);
        
        bool disp8 = ((0x7 & r1) == REG(regs::rbp));
    
        _PREFIX(op0, op1, op2);
        _REX(w, r0, r1, r2);
        _OP(op0, op1, op2);

        // 0, r, 4 = SIB [r+r] or 1, r, 4 = SIB [r+r+disp8]
        _ModRM(disp8 ? 1 : 0, r0, 4);
        _SIB(r1, r2, scale);
        if(disp8) emit(0);
    }

    void emitOffset(int offset, int offsetMode)
    {
        switch(offsetMode)
        {
            case 0: return;
            case 1: emit(offset); return;      // disp8
            case 2: emit32(offset); return;    // disp32
        }
    }
    
    // encode reg-mem [base + offset] instruction with up to 3 bytes
    // takes architectural registers (use REG to convert)
    //
    // specify "RIP" for RIP-relative addressing with imm32
    // then use dataXX() to get offset relative to
    // relocation baseOffset (ie. beginning of block)
    //
    // we'll patch the relocation address here
    void _RM(int wide, int reg, int base, int offset,
        int op0, int op1 = -1, int op2 = -1)
    {
        _PREFIX(op0, op1, op2);
    
        // check for RIP relative, always with disp32
        // these are normally relocated
        if(base == RIP)
        {
            _REX(wide, reg, 0);
            _OP(op0, op1, op2);
            _ModRM(0, reg, 5); // [rip + disp32]
            // bump relocation address
            relocations.back().codeOffset = out.size();
            // adjust offset relative to next instruction
            emit32(offset - (out.size() + sizeof(uint32_t)));
            return;
        }
    
        // check if we need to encode offset
        int offsetMode = 2;     // disp32
        if(!offset) offsetMode = 0;
    
        // if register is RBP/R13, we're stuck with offset
        // because the [EBP] case is used for RIP-relative
        if((base&07) == REG(regs::rbp)) offsetMode = 2;
    
        // try to pick short offset instead if we are in the valid range
        if(offsetMode && (offset >= -128 && offset <= 127)) { offsetMode = 1; }
    
        // if this is RSP/R12, we must use SIB
        if((base&0x7) == REG(regs::rsp))
        {
            // always encode "scaled index" as none
            _REX(wide, reg, base);
            _OP(op0, op1, op2);
            // [SIB] or [SIB + disp32]
            _ModRM(offsetMode, reg, 4);
            emit(0x24);        // SIB: rsp / r12
            emitOffset(offset, offsetMode);
            return;
        }
    
        // otherwise do the general case
        _REX(wide, reg, base);
        _OP(op0, op1, op2);
        _ModRM(offsetMode, reg, base);  // [base + disp32]
        emitOffset(offset, offsetMode);
    }

    // integer ops with immediates: 0x81 / 0x83 opcodes
    void _XXriX(int op, int r, int64_t v)
    {
        if(v == (int8_t) v) { _RR(1, op, r, 0x83); emit(v); return; }
        if(v == (int32_t) v) { _RR(1, op, r, 0x81); emit32(v); return; }
        // general case, store constant
        // fortunately we can synthesize the operation code
        _RM(1, r, RIP, data64(v), 3 + (op << 3));
    }

    // this takes architecture registers.. use macro below
    void _IMULrriXX(int r0, int r1, int64_t v)
    {
        if(v == (int8_t) v) { _RR(1, r0, r1, 0x6B); emit(v); return; }
        if(v == (int32_t) v) {  _RR(1, r0, r1, 0x69); emit32(v); return; }
        // general case, MOV(?) + IMUL reg, [RIP+offset]
        if(r0 != r1) _RR(1, r0, r1, 0x8B); // need MOV
        _RM(1, r0, RIP, data64(v), 0x0F, 0xAF);
    }

    // this takes architecture registers, use macro below
    void emitMOVri64(int reg, int64_t imm, bool force64 = false)
    {
        int encode = 0;
    
        uint32_t immHi = imm >> 32;
        
        if(!immHi) encode = 1;
        if(!~immHi && 0x80000000 & imm) encode = 2;
    
        // there doesn't seem to be a byte-encoding
        // using LEA with SIB byte is probably waste of time
    
        if(force64) encode = 0; // forced
        
        switch(encode)
        {
        case 2: // 32-bit signed, saves 3 bytes over 64-bit
            {
                // need REX.W set to sign extend
                _REX( 1, 0, reg);   // apparently in mod.rm field
                emit(0xC7);
                _ModRM(3, 0, reg); // apparent mod=3
                emit32(imm);
            }
            break;
        case 1: // 32-bit unsigned, saves 4-5 bytes over 64-bit
            {
                // regular 32-bit, gets zero-extend automatically
                _REX(0, 0, reg);   // apparently in mod.rm field
                emit(0xB8 + (reg & 7));
                emit32(imm);
            }
            break;
        default: // full 64-bit
            {
                _REX(1, 0, reg);   // apparently in mod.rm field
                emit(0xB8 + (reg & 7));
                emit32(imm);
                emit32(immHi);
            }
            break;
        }
    }
    
    void emitPush(int reg) { _REX(0, 0, reg); emit(0x50 + (reg & 7)); }
    void emitPop(int reg) { _REX(0, 0, reg); emit(0x58 + (reg & 7)); }
    
};

// instruction macros.. these expect "a64" to be a local variable

#define _MOVri(r,i)         a64.emitMOVri64(REG(r),i)

#define _PUSH(r)            a64.emitPush(REG(r))
#define _POP(r)             a64.emitPop(REG(r))

#define _MOVrr(r0,r1)       a64._RR(1, REG(r0), REG(r1), 0x8B)
#define _CMPrr(r0,r1)       a64._RR(1, REG(r0), REG(r1), 0x3B)
#define _TESTrr(r0,r1)      a64._RR(1, REG(r0), REG(r1), 0x85)
#define _XCHGrr(r0,r1)      a64._RR(1, REG(r0), REG(r1), 0x87)

#define _ADDrr(r0,r1)       a64._RR(1, REG(r0), REG(r1), 0x03)
#define _SUBrr(r0,r1)       a64._RR(1, REG(r0), REG(r1), 0x2B)
#define _NEGr(r0)           a64._RR(1, 3, REG(r0), 0xF7)

#define _IMULrr(r0, r1)     a64._RR(1, REG(r0), REG(r1), 0x0F, 0xAF)
#define _DIVr(r0)           a64._RR(1, 6, REG(r0), 0xF7)
#define _IDIVr(r0)          a64._RR(1, 7, REG(r0), 0xF7)

#define _NOTr(r0)           a64._RR(1, 2, REG(r0), 0xF7)
#define _ANDrr(r0,r1)       a64._RR(1, REG(r0), REG(r1), 0x23)
#define _ORrr(r0,r1)        a64._RR(1, REG(r0), REG(r1), 0x0B)

// we can avoid REX.W for XOR-clears and apparently on some architectures
// this is actually necessary in order to get a dependency breaker
#define _XORrr(r0,r1)       a64._RR((r0==r1)?0:1, REG(r0), REG(r1), 0x33)

// increment, decrement
#define _INC(r0)            a64._RR(1, 0, REG(r0), 0xFF)
#define _DEC(r0)            a64._RR(1, 1, REG(r0), 0xFF)

// these do either imm8, imm32 or RIP-relative .rodata64
#define _ADDri(r0,v)        a64._XXriX(0, REG(r0), v)
#define _SUBri(r0,v)        a64._XXriX(5, REG(r0), v)
#define _CMPri(r0,v)        a64._XXriX(7, REG(r0), v)
#define _ANDri(r0,v)        a64._XXriX(4, REG(r0), v)
#define _ORri(r0,v)         a64._XXriX(1, REG(r0), v)
#define _XORri(r0,v)        a64._XXriX(6, REG(r0), v)

// we currently use this for iaddI
#define _LEAri(r, ptr, off) a64._RM(1, REG(r), REG(ptr), off, 0x8D)
#define _LEArr(r0, r1, r2)  a64._RRRs(1, REG(r0), REG(r1), REG(r2), 0, 0x8D)
#define _LEArrs(r0,r1,r2,s) a64._RRRs(1, REG(r0), REG(r1), REG(r2), s, 0x8D)

#define _IMULrri(r0,r1,v)   a64._IMULrriXX(REG(r0),REG(r1),v)

// these take second operand fixed in CL
#define _SHLr(r0)           a64._RR(1, 4, REG(r0), 0xD3)
#define _SARr(r0)           a64._RR(1, 7, REG(r0), 0xD3)
#define _SHRr(r0)           a64._RR(1, 5, REG(r0), 0xD3)

// versions with immediate byte (not automatically emitted)
#define _SHLri8(r0)         a64._RR(1, 4, REG(r0), 0xC1)
#define _SARri8(r0)         a64._RR(1, 7, REG(r0), 0xC1)
#define _SHRri8(r0)         a64._RR(1, 5, REG(r0), 0xC1)

#define _CVTSI2SSxr(xr, gr)  a64._RR(1, REG(xr), REG(gr), 0xF3, 0x0F, 0x2A)
#define _CVTTSS2SIrx(gr, xr) a64._RR(1, REG(gr), REG(xr), 0xF3, 0x0F, 0x2C)

#define _MOVDxr(r0, r1)     a64._RR(0, REG(r0), REG(r1), 0x66, 0x0F, 0x6E)
#define _MOVDrx(r0, r1)     a64._RR(0, REG(r0), REG(r1), 0x66, 0x0F, 0x7E)

#define _MOVSSxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF3, 0x0F, 0x10)
#define _MOVSSxi(r0, c)     a64._RM(0, REG(r0), RIP, a64.data32f(c), 0xF3, 0x0F, 0x10)
#define _UCOMISSxx(r0, r1)  a64._RR(0, REG(r0), REG(r1), 0x0F, 0x2E)

#define _ADDSSxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF3, 0x0F, 0x58)
#define _SUBSSxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF3, 0x0F, 0x5C)
#define _MULSSxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF3, 0x0F, 0x59)
#define _DIVSSxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF3, 0x0F, 0x5E)

#define _CVTSI2SDxr(xr, gr)  a64._RR(1, REG(xr), REG(gr), 0xF2, 0x0F, 0x2A)
#define _CVTTSD2SIrx(gr, xr) a64._RR(1, REG(gr), REG(xr), 0xF2, 0x0F, 0x2C)

#define _CVTSD2SSxx(r0, r1)  a64._RR(0, REG(r0), REG(r1), 0xF2, 0x0F, 0x5A)
#define _CVTSS2SDxx(r0, r1)  a64._RR(0, REG(r0), REG(r1), 0xF3, 0x0F, 0x5A)

#define _MOVQxr(r0, r1)     a64._RR(1, REG(r0), REG(r1), 0x66, 0x0F, 0x6E)
#define _MOVQrx(r0, r1)     a64._RR(1, REG(r0), REG(r1), 0x66, 0x0F, 0x7E)

#define _MOVSDxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF2, 0x0F, 0x10)
#define _MOVSDxi(r0, c)     a64._RM(0, REG(r0), RIP, a64.data64f(c), 0xF2, 0x0F, 0x10)
#define _UCOMISDxx(r0, r1)  a64._RR(0, REG(r0), REG(r1), 0x66, 0x0F, 0x2E)

#define _ADDSDxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF2, 0x0F, 0x58)
#define _SUBSDxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF2, 0x0F, 0x5C)
#define _MULSDxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF2, 0x0F, 0x59)
#define _DIVSDxx(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0xF2, 0x0F, 0x5E)

// these are not currently used?
#define _ADDSDxi(r0, c)     a64._RM(0, REG(r0), RIP, a64.data64f(c), 0xF2, 0x0F, 0x58)
#define _SUBSDxi(r0, c)     a64._RM(0, REG(r0), RIP, a64.data64f(c), 0xF2, 0x0F, 0x5C)
#define _MULSDxi(r0, c)     a64._RM(0, REG(r0), RIP, a64.data64f(c), 0xF2, 0x0F, 0x59)
#define _DIVSDxi(r0, c)     a64._RM(0, REG(r0), RIP, a64.data64f(c), 0xF2, 0x0F, 0x5E)

// this is one byte shorter than XORPD, which does the same thing
#define _XORPSrr(r0, r1)    a64._RR(0, REG(r0), REG(r1), 0x0F, 0x57)
#define _XORPSri(r0, c)     a64._RM(0, REG(r0), RIP, a64.data128(c), 0x0F, 0x57)

// treat as smaller and sign-extend (same as loads, just _RR)
// these need REX.W to sign-extend all the way
#define _MOVSX_32(r0, r1)   a64._RR(1, REG(r0), REG(r1), 0x63)
#define _MOVSX_16(r0, r1)   a64._RR(1, REG(r0), REG(r1), 0x0F, 0xBF)
#define _MOVSX_8(r0, r1)    a64._RR(1, REG(r0), REG(r1), 0x0F, 0xBE)

// treat as smaller and zero-extend (same as loads, just _RR)
#define _MOVZX_32(r0, r1)   a64._RR(0, REG(r0), REG(r1), 0x8B)
#define _MOVZX_16(r0, r1)   a64._RR(0, REG(r0), REG(r1), 0x0F, 0xB7)
#define _MOVZX_8(r0, r1)    a64._RR(3, REG(r0), REG(r1), 0x0F, 0xB6)

// explicit sizes for memory ops - signed loads, MOVSX for sign-extend
// these need REX.W to sign-extend all the way
#define _load_i64(r, ptr, off)  a64._RM(1, REG(r), REG(ptr), off, 0x8B)
#define _load_i32(r, ptr, off)  a64._RM(1, REG(r), REG(ptr), off, 0x63)
#define _load_i16(r, ptr, off)  a64._RM(1, REG(r), REG(ptr), off, 0x0F, 0xBF)
#define _load_i8(r, ptr, off)   a64._RM(1, REG(r), REG(ptr), off, 0x0F, 0xBE)

// unsigned loads - 32bits is just non-wide regular load, rest are MOVZX
// NOTE: using REX.W for the 16/8 bit MOVZX doesn't really make a difference
#define _load_u32(r, ptr, off)  a64._RM(0, REG(r), REG(ptr), off, 0x8B)
#define _load_u16(r, ptr, off)  a64._RM(0, REG(r), REG(ptr), off, 0x0F, 0xB7)
#define _load_u8(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0x0F, 0xB6)

#define _load_f32(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0xF3, 0x0F, 0x10)
#define _load_f64(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0xF2, 0x0F, 0x10)
#define _load_f128(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0x0F, 0x28)

// integer stores - only 64bits needs REX.W here, opsize prefix for 16bit
// for 8bit we force REX-prefix for RSP/RBP/RDI/RSI
#define _store_i64(r, ptr, off) a64._RM(1, REG(r), REG(ptr), off, 0x89)
#define _store_i32(r, ptr, off) a64._RM(0, REG(r), REG(ptr), off, 0x89)
#define _store_i16(r, ptr, off) a64._RM(0, REG(r), REG(ptr), off, 0x66, 0x89)
#define _store_i8(r, ptr, off)  a64._RM(2, REG(r), REG(ptr), off, 0x88)

#define _store_f32(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0xF3, 0x0F, 0x11)
#define _store_f64(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0xF2, 0x0F, 0x11)
#define _store_f128(r, ptr, off)   a64._RM(0, REG(r), REG(ptr), off, 0x0F, 0x29)


}