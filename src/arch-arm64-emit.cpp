
#ifdef __aarch64__

#include "bjit.h"
#include "arch-arm64-asm.h"

using namespace bjit;


void Module::arch_compileStub(uintptr_t address)
{
}

void Module::arch_patchStub(void * ptr, unsigned offset, uintptr_t address)
{
}


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
            a64.addReloc(label);
            a64.emit(0x14000000 | (0x3ffffff & -(out.size() >> 4)));
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
            
            case ops::iarg: // incoming arguments
            case ops::farg: // incoming arguments
            case ops::darg: break; // these are nops

            case ops::ipass: // outgoing arguments
            case ops::fpass: // outgoing arguments
            case ops::dpass: break; // these are nops for now

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

            case ops::iadd:
                a64.ADDrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;

            case ops::iaddI:
                // use x16 (not currently allocated) as temporary
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64.ADDrr(i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::isub:
                a64.SUBrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::isubI:
                // use x16 (not currently allocated) as temporary
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64.SUBrr(i.reg, ops[i.in[0]].reg, regs::x16);
                break;

            case ops::ineg: a64.NEGr(i.reg, ops[i.in[0]].reg); break;
                
            case ops::imul:
                a64.MULrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::imulI:
                // use x16 (not currently allocated) as temporary
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64.MULrr(i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::idiv:
                a64.SDIVrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
                
            case ops::udiv:
                a64.UDIVrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;

            case ops::imod:
                a64.SDIVrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64.MSUBrrr(i.reg, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
                
            case ops::umod:
                a64.UDIVrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64.MSUBrrr(i.reg, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;

            case ops::inot: a64.NOTr(i.reg, ops[i.in[0]].reg); break;
                
            case ops::iand:
                a64.ANDrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::iandI:
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64.ANDrr(i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::ior:
                a64.ORrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::iorI:
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64.ORrr(i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::ixor:
                a64.XORrr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::ixorI:
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64.XORrr(i.reg, ops[i.in[0]].reg, regs::x16);
                break;

            // for floating point just encode here directly
            case ops::dadd:
                a64._rrr(0x1E602800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::dsub:
                a64._rrr(0x1E603800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::dneg:
                a64._rrr(0x1E614000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::dmul:
                a64._rrr(0x1E600800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::ddiv:
                a64._rrr(0x1E601800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            
            case ops::fadd:
                a64._rrr(0x1E202800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::fsub:
                a64._rrr(0x1E203800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::fneg:
                a64._rrr(0x1E214000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::fmul:
                a64._rrr(0x1E200800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::fdiv:
                a64._rrr(0x1E201800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;

            case ops::ci2f:
                a64._rrr(0x1E220000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::cf2i:
                a64._rrr(0x1E380000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
                
            case ops::ci2d:
                a64._rrr(0x9E620000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::cd2i:
                a64._rrr(0x9E780000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;

            case ops::cf2d:
                a64._rrr(0x1E22C000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::cd2f:
                a64._rrr(0x1E624000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
                

            case ops::bci2f:
                a64._rrr(0x1E260000, i.reg, ops[i.in[0]].reg, 0);
                break;
            case ops::bcf2i:
                a64._rrr(0x1E270000, i.reg, ops[i.in[0]].reg, 0);
                break;
                
            case ops::bci2d:
                a64._rrr(0x9E660000, i.reg, ops[i.in[0]].reg, 0);
                break;
            case ops::bcd2i:
                a64._rrr(0x9E670000, i.reg, ops[i.in[0]].reg, 0);
                break;

            case ops::rename:
                if(i.reg == ops[i.in[0]].reg) break;
                if(i.flags.type == Op::_ptr)
                    a64.MOVrr(i.reg, ops[i.in[0]].reg);
                else if(i.flags.type == Op::_f32)
                    a64._rrr(0x1E204000, i.reg, ops[i.in[0]].reg, 0);
                else if(i.flags.type == Op::_f64)
                    a64._rrr(0x1E604000, i.reg, ops[i.in[0]].reg, 0);
                else BJIT_ASSERT(false);
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
