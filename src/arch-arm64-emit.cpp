
#ifdef __aarch64__

#include "bjit.h"
#include "arch-arm64-asm.h"

using namespace bjit;


void Module::arch_compileStub(uintptr_t address)
{
    // LDR x16, #8
    bytes.push_back(0x50);
    bytes.push_back(0x00);
    bytes.push_back(0x00);
    bytes.push_back(0x58);

    // BR x16
    bytes.push_back(0x00);
    bytes.push_back(0x02);
    bytes.push_back(0x1F);
    bytes.push_back(0xD6);

    for(int i = 0; i < 8; ++i)
    {
        bytes.push_back(address & 0xff);
        address >>= 8;
    }
}

void Module::arch_patchStub(void * ptr, uintptr_t address)
{
    // should be aligned
    ((uint64_t*)ptr)[1] = address;
}

void Module::arch_patchNear(void * ptr, int32_t delta)
{
    auto & code = *(uint32_t*) ptr;
    
    // everything here except ADR has imm26
    if((code & 0xfc000000) != 0x10000000)
    {
        auto offset = (code & 0x3ffffff) + (delta >> 2);
        code = (code & 0xfc000000) | (offset & 0x3ffffff);
    }
    else
    {
        // ADR is imm19
        auto offset = (code >> 5) + (delta >> 2);
        code &=~ (0x7ffff << 5);
        code |= (offset & 0x7ffff) << 5;
    }
}

namespace bjit
{
    namespace regs
    {
        // the vector registers need only be saved 64 bit wide
        // so these are all very much just .. 64 bit all the way
        //
        static int calleeSaved[] =
        {
            x19, x20, x21, x22, x23, x24,
            x25, x26, x27, x28,

            v8, v9, v10, v11, v12, v13, v14, v15,
            
            none
        };
    };
}

void Proc::arch_emit(std::vector<uint8_t> & out)
{
    rebuild_dom();
    findUsedRegs();
    
    for(auto & b : blocks) { b.flags.codeDone = false; }

    AsmArm64 a64(out, blocks.size());

    // figure out what we need to save
    std::vector<int>    savedRegs;
    for(int i = 0; regs::calleeSaved[i] != regs::none; ++i)
    {
        if(usedRegs & R2Mask(regs::calleeSaved[i]))
        {
            savedRegs.push_back(regs::calleeSaved[i]);
        }
    }

    // create a stack frame (push fp, lr)
    // this only allows offset for up to 63 slots
    // so deal with ops::alloc and variable slots separately
    //
    // this is not ideal.. but like whatever
    int nPush = 2+savedRegs.size();
    if(nPush&1) nPush += 1;
    
    // allocate space for slots and frame
    BJIT_ASSERT(ops[0].opcode == ops::alloc);
    unsigned    frameOffset = ((ops[0].imm32+0xf)&~0xf);
    int frameBytes = 8*nSlots + frameOffset;
    if(nSlots & 1) frameBytes += 8;

    // if we don't need to save anything, skip the stack frame
    bool needFrame = frameBytes || savedRegs.size()
        || (usedRegs & R2Mask(regs::lr));

    if(needFrame)
    {
        a64.emit32(0xA9807BFD | ((0x7f & -nPush) << 15));
    
        // mov fp, sp
        a64.emit32(0x910003fd);
    
        for(int i = 0; i < savedRegs.size(); ++i)
        {
            if(R2Mask(savedRegs[i]) & regs::mask_int)
                a64._mem(0xF9000000, savedRegs[i], regs::sp, 16 + 8*i, 3);
            else if(R2Mask(savedRegs[i]) & regs::mask_float)
                a64._mem(0xFD000000, savedRegs[i], regs::sp, 16 + 8*i, 3);
            else BJIT_ASSERT(false);
        }
        
        if(frameBytes)
        {
            // FIXME: we could usually fit immediate here :)
            a64.MOVri(regs::x16, frameBytes);
            a64.emit32(0xCB3063FF); // SUB sp,sp,x16
        }
    }

    auto restoreFrame = [&]()
    {
        if(!needFrame) return;

        // mov sp, fp
        if(frameBytes) a64.emit32(0x910003BF);
        
        for(int i = 0; i < savedRegs.size(); ++i)
        {
            if(R2Mask(savedRegs[i]) & regs::mask_int)
                a64._mem(0xF9400000, savedRegs[i], regs::sp, 16 + 8*i, 3);
            else if(R2Mask(savedRegs[i]) & regs::mask_float)
                a64._mem(0xFD400000, savedRegs[i], regs::sp, 16 + 8*i, 3);
            else BJIT_ASSERT(false);
        }
        
        a64.emit32(0xA8C07BFD | ((0x7f & nPush) << 15));
    };

    // block todo-stack
    std::vector<unsigned>   todo;
    
    // schedule entry-point
    todo.push_back(0);
    blocks[0].flags.codeDone = true;

    auto threadJump = [&](unsigned label) -> unsigned
    {
        // otherwise see if we can thread
        bool progress = true;
        while(progress)
        {
            progress = false;
            for(auto c : blocks[label].code)
            {
                // if this is a simple phi, skip
                if(ops[c].opcode == ops::phi && !ops[c].flags.spill)
                    continue;
    
                if(ops[c].opcode == ops::jmp)
                {
                    label = ops[c].label[0];
                    progress = true;
                }
                break;
            }
        }

        return label;
    };

    // schedules a block and checks if it ends in
    // unconditional jump to unscheduled block
    // we put such blocks below stack top
    //
    // this should place one shuffle just before it's target block
    auto scheduleBlock = [&](unsigned label) -> unsigned
    {
        label = threadJump(label);
        
        if(blocks[label].flags.codeDone) return label;

        blocks[label].flags.codeDone = true;
        todo.push_back(label);
        
        auto & b = blocks[todo.back()];
        auto & j = ops[b.code.back()];
        if(j.opcode == ops::jmp && !blocks[j.label[0]].flags.codeDone)
        {
            auto top = todo.back();
            todo.back() = j.label[0];
            blocks[todo.back()].flags.codeDone = true;
            todo.push_back(top);
        }

        return label;
    };

    auto doJump = [&](unsigned label)
    {
        // if we'll emit this block next
        // there's no need to generate jmp
        if(todo.size() && todo.back() == label) return;

        label = threadJump(label);
        
        if(!blocks[label].flags.codeDone)
        {
            blocks[label].flags.codeDone = true;
            todo.push_back(label);
        }
        else
        {
            a64.addReloc(label);
            a64.emit32(0x14000000 | (0x3ffffff & -(out.size() >> 2)));
        }
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
            
            // this seems to be the winner rule in general
            if(blocks[i.label[1]].pdom == i.label[0]) swap = true;
            else if(blocks[i.label[0]].pdom == i.label[1]) swap = false;
            else if(blocks[i.label[0]].pdom == i.label[1]) swap = false;
            else if(blocks[i.block].pdom == i.label[1]) swap = true;

            if(done1 && !done0) swap = true;
            if(done0 && !done1) swap = false;
            
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

            case ops::icallp:
            case ops::fcallp:
            case ops::dcallp:
                // BLR
                a64.emit32(0xD63F0000 | (REG(ops[i.in[0]].reg)<<5));
                break;

            case ops::tcallp:
                // BR
                restoreFrame();
                a64.emit32(0xD61F0000 | (REG(ops[i.in[0]].reg)<<5));
                break;

            case ops::icalln:
            case ops::fcalln:
            case ops::dcalln:
                // BL
                nearReloc.emplace_back(
                    NearReloc{(uint32_t)out.size(), (uint32_t) i.imm32});
                a64.emit32(0x94000000 | (0x3ffffff & -(out.size() >> 2)));
                break;

            case ops::tcalln:
                // B
                restoreFrame();
                nearReloc.emplace_back(
                    NearReloc{(uint32_t)out.size(), (uint32_t) i.imm32});
                a64.emit32(0x14000000 | (0x3ffffff & -(out.size() >> 2)));
                break;

            case ops::lnp:
                // ADR
                {
                    nearReloc.emplace_back(
                        NearReloc{(uint32_t)out.size(), (uint32_t) i.imm32});
                    // code is aligned, so immlo is always 00b
                    // remaining reloc is then imm19 like condition jumps
                    a64.emit32(0x10000000 | REG(i.reg)
                        | (0xffffe0 & -(out.size() << 3)));
                }
                break;

            case ops::jmp:
                doJump(i.label[0]);
                break;

            case ops::jilt:
            case ops::jige:
            case ops::jigt:
            case ops::jile:

            case ops::jult:
            case ops::juge:
            case ops::jugt:
            case ops::jule:

            case ops::jine:
            case ops::jieq:
                a64.CMPrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(0x54000000
                    | _CC(i.opcode)
                    | ((0x7ffff & -(out.size()>>2))<<5));

                doJump(i.label[1]);
                break;

            case ops::jz:
            case ops::jnz:

            #if 0
                //a64.TSTrr(ops[i.in[0]].reg, ops[i.in[0]].reg);

                // compare immediate seems fine too..
                a64._rri12(0xF1000000, regs::sp, ops[i.in[0]].reg, 0);
                
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(0x54000000
                    | _CC(i.opcode)
                    | ((0x7ffff & -(out.size()>>2))<<5));
            #else
                // CBZ / CBNZ - prefer smaller code?
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(
                    (i.opcode == ops::jz ? 0xB4000000 : 0xB5000000)
                    | REG(ops[i.in[0]].reg)
                    | ((0x7ffff & -(out.size()>>2))<<5));
            #endif
                doJump(i.label[1]);
                break;
                
            case ops::jiltI:
            case ops::jigeI:
            case ops::jigtI:
            case ops::jileI:

            case ops::jultI:
            case ops::jugeI:
            case ops::jugtI:
            case ops::juleI:
            
            case ops::jineI:
            case ops::jieqI:
                if(i.imm32 == (0xfff & i.imm32))
                {
                    // SUBS immediate with zero output
                    a64._rri12(0xF1000000, regs::sp, ops[i.in[0]].reg, i.imm32);
                }
                else if(-i.imm32 == (0xfff & -i.imm32))
                {
                    // ADDs -immediate with zero output
                    a64._rri12(0xB1000000, regs::sp, ops[i.in[0]].reg, -i.imm32);
                }
                else
                {
                    a64.MOVri(regs::x16, (int32_t) i.imm32);
                    a64.CMPrr(ops[i.in[0]].reg, regs::x16);
                }
                
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(0x54000000
                    | _CC(i.opcode+ops::jilt-ops::jiltI)
                    | ((0x7ffff & -(out.size()>>2))<<5));

                doJump(i.label[1]);
                break;
                
            case ops::jflt:
            case ops::jfge:
            case ops::jfgt:
            case ops::jfle:

            case ops::jfne:
            case ops::jfeq:
                a64.FCMPss(ops[i.in[0]].reg, ops[i.in[1]].reg);
                
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(0x54000000
                    | _CC(i.opcode)
                    | ((0x7ffff & -(out.size()>>2))<<5));
                
                doJump(i.label[1]);
                break;
                
            case ops::jdlt:
            case ops::jdge:
            case ops::jdgt:
            case ops::jdle:

            case ops::jdne:
            case ops::jdeq:
                a64.FCMPdd(ops[i.in[0]].reg, ops[i.in[1]].reg);
                
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(0x54000000
                    | _CC(i.opcode)
                    | ((0x7ffff & -(out.size()>>2))<<5));
                
                doJump(i.label[1]);
                break;
                
            case ops::ilt:
            case ops::ige:
            case ops::igt:
            case ops::ile:

            case ops::ult:
            case ops::uge:
            case ops::ugt:
            case ops::ule:

            case ops::ine:
            case ops::ieq:
                a64.CMPrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64._rrr(a64._CSET^((_CC(i.opcode + ops::jilt - ops::ilt)) << 12),
                    i.reg, regs::sp, regs::sp);
                break;

            case ops::iltI:
            case ops::igeI:
            case ops::igtI:
            case ops::ileI:

            case ops::ultI:
            case ops::ugeI:
            case ops::ugtI:
            case ops::uleI:
            
            case ops::ineI:
            case ops::ieqI:
                if(i.imm32 == (0xfff & i.imm32))
                {
                    // SUBS immediate with zero output
                    a64._rri12(0xF1000000, regs::sp, ops[i.in[0]].reg, i.imm32);
                }
                else if(-i.imm32 == (0xfff & -i.imm32))
                {
                    // ADDs -immediate with zero output
                    a64._rri12(0xB1000000, regs::sp, ops[i.in[0]].reg, -i.imm32);
                }
                else
                {
                    a64.MOVri(regs::x16, (int32_t) i.imm32);
                    a64.CMPrr(ops[i.in[0]].reg, regs::x16);
                }
                a64._rrr(a64._CSET^((_CC(i.opcode + ops::jilt - ops::iltI)) << 12),
                    i.reg, regs::sp, regs::sp);
                break;
                
            case ops::flt:
            case ops::fge:
            case ops::fgt:
            case ops::fle:

            case ops::fne:
            case ops::feq:
                a64.FCMPss(ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64._rrr(a64._CSET^((_CC(i.opcode + ops::jilt - ops::ilt)) << 12),
                    i.reg, regs::sp, regs::sp);
                break;
                
            case ops::dlt:
            case ops::dge:
            case ops::dgt:
            case ops::dle:

            case ops::dne:
            case ops::deq:
                a64.FCMPdd(ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64._rrr(a64._CSET^((_CC(i.opcode + ops::jilt - ops::ilt)) << 12),
                    i.reg, regs::sp, regs::sp);
                break;

            case ops::iretI:
                a64.MOVri(regs::x0, i.imm32);
                // fall through
            case ops::iret:
            case ops::fret:
            case ops::dret:
                restoreFrame();
                a64.emit32(0xD65F03C0);
                break;

            case ops::iadd:
                a64._rrr(a64._ADD, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::iaddI:
                if(i.imm32 == (0xfff & i.imm32))
                {
                    // ADD with immediate
                    a64._rri12(0x91000000, i.reg, ops[i.in[0]].reg, i.imm32);
                    break;
                }
                if(-i.imm32 == (0xfff & -i.imm32))
                {
                    // SUB with -immediate
                    a64._rri12(0xD1000000, i.reg, ops[i.in[0]].reg, -i.imm32);
                    break;
                }
                // general: use x16 (not currently allocated) as temporary
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64._rrr(a64._ADD, i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::isub:
                a64._rrr(a64._SUB, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::isubI:
                if(i.imm32 == (0xfff & i.imm32))
                {
                    // SUB with immediate
                    a64._rri12(0xD1000000, i.reg, ops[i.in[0]].reg, i.imm32);
                    break;
                }
                if(-i.imm32 == (0xfff & -i.imm32))
                {
                    // ADD with -immediate
                    a64._rri12(0x91000000, i.reg, ops[i.in[0]].reg, -i.imm32);
                    break;
                }
                // general: use x16 (not currently allocated) as temporary
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64._rrr(a64._SUB, i.reg, ops[i.in[0]].reg, regs::x16);
                break;

            case ops::ineg: a64.NEGr(i.reg, ops[i.in[0]].reg); break;
                
            case ops::imul:
                a64._rrr(a64._MUL, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::imulI:
                // use x16 (not currently allocated) as temporary
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64._rrr(a64._MUL, i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::idiv:
                a64._rrr(a64._SDIV, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
                
            case ops::udiv:
                a64._rrr(a64._UDIV, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;

            case ops::imod:
                // FIXME: force distinct regs?
                a64._rrr(a64._SDIV, regs::x16, ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64.MSUBrrrr(i.reg, regs::x16, ops[i.in[1]].reg, ops[i.in[0]].reg);
                break;
                
            case ops::umod:
                // FIXME: force distinct regs?
                a64._rrr(a64._UDIV, regs::x16, ops[i.in[0]].reg, ops[i.in[1]].reg);
                a64.MSUBrrrr(i.reg, regs::x16, ops[i.in[1]].reg, ops[i.in[0]].reg);
                break;

            case ops::inot: a64.NOTr(i.reg, ops[i.in[0]].reg); break;
                
            case ops::iand:
                a64._rrr(a64._AND, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::iandI:
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64._rrr(a64._AND, i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::ior:
                a64._rrr(a64._OR, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::iorI:
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64._rrr(a64._OR, i.reg, ops[i.in[0]].reg, regs::x16);
                break;
                
            case ops::ixor:
                a64._rrr(a64._XOR, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::ixorI:
                a64.MOVri(regs::x16, (int32_t) i.imm32);
                a64._rrr(a64._XOR, i.reg, ops[i.in[0]].reg, regs::x16);
                break;

            case ops::ishl:
                a64._rrr(0x9AC02000, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::ishlI:
                {
                    int shift = i.imm32 & 0x3f;
                    a64._rrr(0xD3400000
                        | ((0x3f & -shift) << 16) | ((0x3f - shift) << 10),
                        i.reg, ops[i.in[0]].reg, regs::x0);
                }
                break;

            case ops::ishr:
                a64._rrr(0x9AC02800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::ishrI:
                a64._rrr(0x9340FC00 | ((i.imm32 & 0x3f) << 16),
                    i.reg, ops[i.in[0]].reg, regs::x0);
                break;
                
            case ops::ushr:
                a64._rrr(0x9AC02400, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::ushrI:
                a64._rrr(0xD340FC00 | ((i.imm32 & 0x3f) << 16),
                    i.reg, ops[i.in[0]].reg, regs::x0);
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
            case ops::dabs:
                a64._rrr(0x1E60C000, i.reg, ops[i.in[0]].reg, regs::x0);
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
            case ops::fabs:
                a64._rrr(0x1E20C000, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::fmul:
                a64._rrr(0x1E200800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
            case ops::fdiv:
                a64._rrr(0x1E201800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                break;
                
            case ops::lci:
                a64.MOVri(i.reg, i.i64);
                break;

            case ops::lcf:
                a64.emit32(0x1C000000 | REG(i.reg) | ((0x7ffff &
                    (a64.data32(reinterpret_cast<uint32_t&>(i.f32))>>2)) << 5));
                break;
            case ops::lcd:
                a64.emit32(0x5C000000 | REG(i.reg) | ((0x7ffff &
                    (a64.data64(reinterpret_cast<uint64_t&>(i.f64))>>2)) << 5));
                break;

            case ops::i8:
                a64._rrr(0x93401C00, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::i16:
                a64._rrr(0x93403C00, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::i32:
                a64._rrr(0x93407C00, i.reg, ops[i.in[0]].reg, regs::x0);
                break;

            case ops::u8:
                a64._rrr(0x53001C00, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::u16:
                a64._rrr(0x53003C00, i.reg, ops[i.in[0]].reg, regs::x0);
                break;
            case ops::u32:
                // this is simply 32-bit MOVrr ..
                a64._rrr(0x2A0003E0, i.reg, regs::x0, ops[i.in[0]].reg);
                break;
                
            case ops::li8:
                a64._mem(0x39800000, i.reg, ops[i.in[0]].reg, i.off16, 0);
                break;
            case ops::li16:
                a64._mem(0x79800000, i.reg, ops[i.in[0]].reg, i.off16, 1);
                break;
            case ops::li32:
                a64._mem(0xB9800000, i.reg, ops[i.in[0]].reg, i.off16, 2);
                break;
            case ops::li64:
                a64._mem(0xF9400000, i.reg, ops[i.in[0]].reg, i.off16, 3);
                break;

            case ops::lu8:
                a64._mem(0x39400000, i.reg, ops[i.in[0]].reg, i.off16, 0);
                break;
            case ops::lu16:
                a64._mem(0x79400000, i.reg, ops[i.in[0]].reg, i.off16, 1);
                break;
            case ops::lu32:
                a64._mem(0xB9400000, i.reg, ops[i.in[0]].reg, i.off16, 2);
                break;

            case ops::lf32:
                a64._mem(0xBD400000, i.reg, ops[i.in[0]].reg, i.off16, 2);
                break;
            case ops::lf64:
                a64._mem(0xFD400000, i.reg, ops[i.in[0]].reg, i.off16, 3);
                break;
                
            case ops::si8:
                a64._mem(0x39000000, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16, 0);
                break;
            case ops::si16:
                a64._mem(0x79000000, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16, 1);
                break;
            case ops::si32:
                a64._mem(0xB9000000, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16, 2);
                break;
            case ops::si64:
                a64._mem(0xF9000000, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16, 3);
                break;

            case ops::sf32:
                a64._mem(0xBD000000, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16, 2);
                break;
            case ops::sf64:
                a64._mem(0xFD000000, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16, 3);
                break;

            case ops::l2i8:
                a64._mem2(0x38A06800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2i16:
                a64._mem2(0x78A06800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2i32:
                a64._mem2(0xB8A06800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2i64:
                a64._mem2(0xF8606800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;

            case ops::l2u8:
                a64._mem2(0x38606800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2u16:
                a64._mem2(0x78606800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2u32:
                a64._mem2(0xB8606800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;

            case ops::l2f32:
                a64._mem2(0xBC606800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2f64:
                a64._mem2(0xFC606800, i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
                
            case ops::s2i8:
                a64._mem2(0x38206800, ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2i16:
                a64._mem2(0x78206800, ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2i32:
                a64._mem2(0xB8206800, ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2i64:
                a64._mem2(0xF8206800, ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;

            case ops::s2f32:
                a64._mem2(0xBC206800, ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2f64:
                a64._mem2(0xFC206800, ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
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

            case ops::fence:
                // We issue DMB ISH = full barrier, inner shareable
                // This is what clang seems to use as well..
                a64.emit32(0xD5033BBF);
                break;

            case ops::phi: break;   // this is just NOP here

            case ops::reload:
                BJIT_ASSERT(ops[i.in[0]].scc != noSCC);
                if(i.flags.type == Op::_f64)
                    a64._mem(0xFD400000, i.reg, regs::sp,
                        frameOffset + 8*ops[i.in[0]].scc, 3);
                else if(i.flags.type == Op::_f32)
                    a64._mem(0xBD400000, i.reg, regs::sp,
                        frameOffset + 8*ops[i.in[0]].scc, 2);
                else if(i.flags.type == Op::_ptr)
                    a64._mem(0xF9400000, i.reg, regs::sp,
                        frameOffset + 8*ops[i.in[0]].scc, 3);
                else BJIT_ASSERT(false);
                break;

            case ops::rename:
                if(i.reg == ops[i.in[0]].reg) break;
                if(i.flags.type == Op::_ptr)
                    a64.MOVrr(i.reg, ops[i.in[0]].reg);
                else if(i.flags.type == Op::_f32)
                    // this is rename (not shuffle) 'cos zeroes upper components
                    a64._rrr(0x1E204000, i.reg, ops[i.in[0]].reg, 0);
                else if(i.flags.type == Op::_f64)
                    // this is rename (not shuffle) 'cos zeroes upper components
                    a64._rrr(0x1E604000, i.reg, ops[i.in[0]].reg, 0);
                else BJIT_ASSERT(false);
                break;

            default:
                {
                    printf("Unimplemented: %s\n", i.strOpcode());
                    BJIT_ASSERT(false);
                }
        }
        
        // if marked for spill, store to stack
        if(i.flags.spill)
        {
            BJIT_ASSERT(i.scc != noSCC);
            
            if(i.flags.type == Op::_f64)
                a64._mem(0xFD000000, i.reg, regs::sp,
                    frameOffset + 8*i.scc, 3);
            else if(i.flags.type == Op::_f32)
                a64._mem(0xBD000000, i.reg, regs::sp,
                    frameOffset + 8*i.scc, 2);
            else if(i.flags.type == Op::_ptr)
                a64._mem(0xF9000000, i.reg, regs::sp,
                    frameOffset + 8*i.scc, 3);
            else BJIT_ASSERT(false);
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
    
    // align to 16-bytes
    while(out.size() & 0xf) a64.emit32(0);
    
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

    // align to 16-bytes
    while(out.size() & 0xf) a64.emit32(0);

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
            data &=~ (0x7ffff << 5);
            data |= (offset & 0x7ffff) << 5;
        }
        
        *((uint32_t*)(out.data() + r.codeOffset)) = data;
    }
}

#endif
