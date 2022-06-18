
#ifdef __aarch64__

#include "bjit.h"
#include "arch-arm64-asm.h"

using namespace bjit;

void Proc::arch_emit(std::vector<uint8_t> & out)
{
    for(auto & b : blocks) { b.flags.codeDone = false; }

    AsmArm64 a64(out, blocks.size());

    std::vector<int>    savedRegs;


    // block todo-stack
    std::vector<unsigned>   todo;
    
    // schedule entry-point
    todo.push_back(0);
    blocks[0].flags.codeDone = true;

    // this checks if todo-stack top ends in
    // unconditional jump to unscheduled block
    // we put such blocks below stack top
    //
    // this should place one shuffle just before it's target block
    auto scheduleThreading = [&]()
    {
        if(!todo.size()) return;
        
        auto & b = blocks[todo.back()];
        auto & j = ops[b.code.back()];
        if(j.opcode == ops::jmp && !blocks[j.label[0]].flags.codeDone)
        {
            auto top = todo.back();
            todo.back() = j.label[0];
            blocks[todo.back()].flags.codeDone = true;
            todo.push_back(top);
        }
    };

    auto doJump = [&](unsigned label)
    {
        // if we'll emit this block next
        // there's no need to generate jmp
        if(todo.size() && todo.back() == label) return;
        if(!blocks[label].flags.codeDone)
        {
            blocks[label].flags.codeDone = true;
            todo.push_back(label);
        }
        else
        {
            a64.emit(0x14000000 | (0x3ffffff & -(out.size() >> 4)));
            a64.addReloc(label);
        }
        scheduleThreading();
    };

    auto emitOp = [&](Op & i)
    {
        // for conditionals, if one of the blocks is done
        // place the "not done" as the fall-thu path
        //
        // if neither block is done, see if they unconditionally
        // jump to blocks that are done..
        //
        if(i.opcode < ops::jmp)
        {
            bool swap = false;
            
            bool done0 = blocks[i.label[0]].flags.codeDone;
            bool done1 = blocks[i.label[1]].flags.codeDone;

            auto & j0 = ops[blocks[i.label[0]].code.back()];
            auto & j1 = ops[blocks[i.label[1]].code.back()];
            
            if(!done0 && !done1)
            {
                if(j0.opcode == ops::jmp
                && blocks[j0.label[0]].flags.codeDone) done0 = true;
                if(j1.opcode == ops::jmp
                && blocks[j1.label[0]].flags.codeDone) done1 = true;
            }

            if(done1 && !done0) swap = true;

            // if either ends with a return, then use that first
            if(j0.opcode > ops::jmp) swap = false;
            if(j1.opcode > ops::jmp) swap = true;

            if(swap)
            {
                i.opcode ^= 1;
                std::swap(i.label[0], i.label[1]);
            }
        }
        
        switch(i.opcode)
        {
            case ops::alloc: break; // stack frame, nop

            case ops::jmp:
                doJump(i.label[0]);
                break;

            case ops::lci:
                a64.MOVri(i.reg, i.i64);
                break;

            case ops::iretI:
                a64.MOVri(regs::x0, i.imm32);
                // fall through
            case ops::iret:
            case ops::fret:
            case ops::dret:
                a64.emit32(0xD65F03C0);
                break;

            default: BJIT_ASSERT(false);
        }
        
        // if marked for spill, store to stack
        if(i.flags.spill)
        {
            BJIT_ASSERT(i.scc != noSCC);

        }
        
    };

    while(todo.size())
    {
        int bi = todo.back(); todo.pop_back();
        a64.blockOffsets[bi] = out.size();

        auto & b = blocks[bi];

        for(auto i : b.code)
        {
            //debugOp(i);
            emitOp(ops[i]);
        }
    }

    // should always be in multiples of 4 bytes
    BJIT_ASSERT(!(out.size() & 0x3));
    
    // align to 8-bytes
    if(out.size() & 0xf) a64.emit32(0xD503201F);
    
    // emit 64-bit point constants
    a64.blockOffsets[a64.rodata64_index] = out.size();
    for(uint64_t bits : a64.rodata64)
    {
        a64.emit32(bits);
        a64.emit32(bits>>32);
    }
    
    // emit 32-bit point constants
    a64.blockOffsets[a64.rodata32_index] = out.size();
    for(uint32_t bits : a64.rodata32)
    {
        a64.emit32(bits);
    }

    while(out.size() & 0xf) a64.emit32(0xD503201F);

    // for every relocation, ADD the block offset
    for(auto & r : a64.relocations)
    {
        auto data = *((int32_t*)(out.data() + r.codeOffset));

        // unconditional branch has imm26
        if((data & 0xfc000000) == 0x14000000)
        {
            auto offset = (data & 0x3ffffff)
                + (a64.blockOffsets[r.blockIndex] >> 2);
            data = 0x14000000 | (offset & 0x3ffffff);
        }
        else
        {
            // rest should be imm19?
            auto offset = (data >> 5) + (a64.blockOffsets[r.blockIndex] >> 2);
            data &=~ (0x3ffff << 5);
            data |= (offset & 0x3ffff) << 5;
        }
        
        *((uint32_t*)(out.data() + r.codeOffset)) = data;
    }
}

#endif
