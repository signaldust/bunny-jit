
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "arch-arm64.h"

namespace bjit
{

static const uint8_t PC = 0xff;    // otherwise invalid, used for RIP relative

// encode register from our naming to X64 values
static uint8_t REG(int r)
{
    using namespace regs;

    switch(r)
    {
        case x0: case v0: return 0;
        case x1: case v1: return 1;
        case x2: case v2: return 2;
        case x3: case v3: return 3;
        case x4: case v4: return 4;
        case x5: case v5: return 5;
        case x6: case v6: return 6;
        case x7: case v7: return 7;
        case x8: case v8: return 8;
        case x9: case v9: return 9;
        
        case x10: case v10: return 10;
        case x11: case v11: return 11;
        case x12: case v12: return 12;
        case x13: case v13: return 13;
        case x14: case v14: return 14;
        case x15: case v15: return 15;
        case x16: case v16: return 16;
        case x17: case v17: return 17;
        case x18: case v18: return 18;
        case x19: case v19: return 19;
        
        case x20: case v20: return 20;
        case x21: case v21: return 21;
        case x22: case v22: return 22;
        case x23: case v23: return 23;
        case x24: case v24: return 24;
        case x25: case v25: return 25;
        case x26: case v26: return 26;
        case x27: case v27: return 27;
        case x28: case v28: return 28;
        
        case fp: case v29: return 29;

        case lr: case v30: return 30;
        case sp: case v31: return 31;

        // this is only used internally
        case PC: return PC;
    }

    BJIT_ASSERT(false);
    return 0;
}

#if 0

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

#endif

struct AsmArm64
{
    std::vector<uint8_t>   & out;

    AsmArm64(std::vector<uint8_t> & out, unsigned nBlocks) : out(out)
    {
        rodata32_index = nBlocks++;
        rodata64_index = nBlocks++;
        //rodata128_index = nBlocks++;
        blockOffsets.resize(nBlocks);
    }

    // separate .rodata for 128/64/32 bit constants
    // we will place the most aligned block first
/*
    std::vector<__m128>     rodata128;
    uint32_t                rodata128_index;    // index into blockOffsets
*/    
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

/*
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
*/

    void MOVri(int r, int64_t imm64)
    {
        if(imm64 == (0xffff & imm64))
        {
            // MOVZ
            emit32(0xD2800000 | REG(r) | ((0xffff & imm64) << 5));
            return;
        }

        if(imm64 == ~(0xffff & ~imm64))
        {
            // MOVN
            emit32(0x92800000 | REG(r) | ((0xffff & ~imm64) << 5));
            return;
        }

        if(imm64 == (uint32_t) imm64)
        {
            // LDR pc-relative .. imm19
            auto off = data32(imm64) >> 2;
            emit32(0x18000000 | REG(r) | ((0x7ffff & off) << 5));
            return;
        }
        
        if(imm64 == (int32_t) imm64)
        {
            // LDR pc-relative .. imm19
            auto off = data32(imm64) >> 2;
            emit32(0x98000000 | REG(r) | ((0x7ffff & off) << 5));
            return;
        }

        // general 64-bit LDR pc-relative .. imm19
        auto off = data64(imm64) >> 2;
        emit32(0x58000000 | REG(r) | ((0x7ffff & off) << 5));

    }

    void _rrr(uint32_t op, int r0, int r1, int r2)
    {
        emit32(op | REG(r0) | (REG(r1)<<5) | (REG(r2) << 16));

    }

    void MOVrr(int r0, int r1) { _rrr(0xAA0003E0, r0, 0, r1); }

    void ADDrr(int r0, int r1, int r2) { _rrr(0x8B000000, r0, r1, r2); }
    void SUBrr(int r0, int r1, int r2) { _rrr(0xCB000000, r0, r1, r2); }

    // SUB from zero reg
    void NEGr(int r0, int r1) { SUBrr(r0, regs::sp, r1); }
    
    void MULrr(int r0, int r1, int r2) { _rrr(0x9B007C00, r0, r1, r2); }
    void SDIVrr(int r0, int r1, int r2) { _rrr(0x9AC00C00, r0, r1, r2); }
    void UDIVrr(int r0, int r1, int r2) { _rrr(0x9AC00800, r0, r1, r2); }

    void MSUBrrr(int r0, int r1, int r2, int r3)
    { _rrr(0x9B008000 | (REG(r0)<<10), r1, r2, r3); }

    // this uses EON with zero register
    void NOTr(int r0, int r1) { _rrr(0xCA3F0000, r0, r1, 0); }
    
    void ANDrr(int r0, int r1, int r2) { _rrr(0x8A000000, r0, r1, r2); }
    void ORrr(int r0, int r1, int r2) { _rrr(0xAA000000, r0, r1, r2); }
    void XORrr(int r0, int r1, int r2) { _rrr(0xCA000000, r0, r1, r2); }
    
};

}