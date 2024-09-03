
#ifdef __x86_64__

#include "bjit.h"
#include "arch-x64-asm.h"

using namespace bjit;

namespace bjit
{
    namespace regs
    {
        /*
         From MS:
         The x64 ABI considers registers
            RBX, RBP, RDI, RSI, RSP, R12, R13, R14, R15,
            and XMM6-XMM15 nonvolatile. They must be saved
            and restored by a function that uses them.

        */
        
        // we use "none" as a list terminator
        static int calleeSaved[] =
        {
            // These are callee saved both Win64 and SysV
            rbp, rbx, r12, r13, r14, r15,

#ifdef _WIN32
            rsi, rdi,

            // these are callee saved SSE wide (128bit)
            xmm6, xmm7, xmm8, xmm9, xmm10,
            xmm11, xmm12, xmm13, xmm14, xmm15,
#endif
            none
        };
    };
};

void Module::arch_compileStub(uintptr_t address)
{
    // MOVABS + indirect jump, always use RAX as the register
    bytes.push_back(0x48);  // REX.W ..
    bytes.push_back(0xB8);  // MOVABS RAX, imm64
    for(int i = 0; i < 8; ++i)
    {
        bytes.push_back(address & 0xff);
        address >>= 8;
    }
    bytes.push_back(0xFF);  // indirect jump
    bytes.push_back(0xE0);  // r=4, rm=RAX
}

void Module::arch_patchStub(void * ptr, uintptr_t address)
{
    for(int i = 0; i < 8; ++i)
    {
        (2+(uint8_t*)ptr)[i] = (address & 0xff);
        address >>= 8;
    }
}

void Module::arch_patchNear(void * ptr, int32_t delta)
{
    auto addr = (uint32_t*) ptr;
    *addr += delta;
}

void Proc::arch_emit(std::vector<uint8_t> & out)
{
    rebuild_dom();
    
    for(auto & b : blocks) { b.flags.codeDone = false; }

    AsmX64 a64(out, blocks.size());

    std::vector<int>    savedRegs;

    // push callee-saved registers, goes first
    int nPush = 0;
    for(int i = 0; regs::calleeSaved[i] != regs::none; ++i)
    {
        if(usedRegs & R2Mask(regs::calleeSaved[i]))
        {
            savedRegs.push_back(regs::calleeSaved[i]);
            if(R2Mask(savedRegs.back()) & regs::mask_float)
            {
                // force alignment if necessary, we need 128-bit
                if(!(nPush&1))
                {
                    savedRegs.back() = regs::none;
                    savedRegs.push_back(regs::calleeSaved[i]);
                    _SUBri(regs::rsp, 16+8);
                    ++nPush;
                }
                else
                {
                    _SUBri(regs::rsp, 16);
                }
                _store_f128(savedRegs.back(), regs::rsp, 0);
                nPush += 2; // takes two slots
            }
            else
            {
                _PUSH(savedRegs.back());
                nPush += 1;
            }
        }
    }

    // this tracks additional stack offset
    // when we need to adjust stack during calls
    BJIT_ASSERT(ops[0].opcode == ops::alloc);
    unsigned    frameOffset = ((ops[0].imm32+0xf)&~0xf);

    // need 8 mod 16 - add slots, emit "prelude" if necessary
    if(!((nPush+nSlots) & 1)) frameOffset += 8;
    // add user-requested frame on top
    int frameBytes = 8*nSlots + frameOffset;
    if(frameBytes) { _SUBri(regs::rsp, frameBytes); }

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
            a64.emit(0xE9);
            a64.addReloc(label);
            a64.emit32(-4-out.size());
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
#ifdef _WIN32
                // "home locations" for registers
                _SUBri(regs::rsp, 4 * sizeof(uint64_t));
#endif
                // generate indirect near-call: FF /2
                a64._RR(0, 2, REG(ops[i.in[0]].reg), 0xFF);
#ifdef _WIN32
                // "home locations" for registers
                _ADDri(regs::rsp, 4 * sizeof(uint64_t));
#endif
                break;
            
            case ops::icalln:
            case ops::fcalln:
            case ops::dcalln:
#ifdef _WIN32
                // "home locations" for registers
                _SUBri(regs::rsp, 4 * sizeof(uint64_t));
#endif
                // RIP-relative call
                a64.emit(0xE8);
                nearReloc.emplace_back(
                    NearReloc{(uint32_t)out.size(), (uint32_t) i.imm32});
                a64.emit32(-4-out.size());
#ifdef _WIN32
                // "home locations" for registers
                _ADDri(regs::rsp, 4 * sizeof(uint64_t));
#endif
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
                // compare
                _CMPrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then jump
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode));
                // relocs
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(-4-out.size());
                doJump(i.label[1]);
                break;

            case ops::jz:
            case ops::jnz:
                _TESTrr(ops[i.in[0]].reg, ops[i.in[0]].reg);
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode));
                // relocs
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(-4-out.size());
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
                // compare
                _CMPri(ops[i.in[0]].reg, (int32_t) i.imm32);
                // then jump
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode+ops::jilt-ops::jiltI));
                // relocs
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(-4-out.size());
                doJump(i.label[1]);
                break;
                            
            case ops::jdlt:
            case ops::jdge:
            case ops::jdgt:
            case ops::jdle:
            case ops::jdne:
            case ops::jdeq:
                // UCOMISD (scalar double compare)
                _UCOMISDxx(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then jump
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode));
                // relocs
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(-4-out.size());
                doJump(i.label[1]);
                break;

            case ops::jflt:
            case ops::jfge:
            case ops::jfgt:
            case ops::jfle:
            case ops::jfne:
            case ops::jfeq:
                // UCOMISD (scalar double compare)
                _UCOMISSxx(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then jump
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode));
                // relocs
                a64.addReloc(scheduleBlock(i.label[0]));
                a64.emit32(-4-out.size());
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
                // compare
                _CMPrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode + ops::jilt - ops::ilt));
                // zero-extend
                _MOVZX_8(i.reg, i.reg);
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
                // compare
                _CMPri(ops[i.in[0]].reg, (int32_t) i.imm32);
                // then emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode + ops::jilt - ops::iltI));
                // zero-extend
                _MOVZX_8(i.reg, i.reg);
                break;

            case ops::dlt:
            case ops::dge:
            case ops::dgt:
            case ops::dle:

            case ops::dne:
            case ops::deq:
                // xor destination
                // NOTE: safe, 'cos can't alias on xmm inputs
                _XORrr(i.reg, i.reg);
                
                // UCOMISD (scalar double compare)
                _UCOMISDxx(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode - ops::jmp));
                break;
                
            case ops::flt:
            case ops::fge:
            case ops::fgt:
            case ops::fle:

            case ops::fne:
            case ops::feq:
                // NOTE: safe, 'cos can't alias on xmm inputs
                _XORrr(i.reg, i.reg);
                
                // UCOMISD (scalar double compare)
                _UCOMISSxx(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode - ops::jmp));
                break;

            case ops::iretI:
                if(!i.imm32) _XORrr(regs::rax, regs::rax);
                else _MOVri(regs::rax, (int32_t) i.imm32);
                // fall through
            case ops::iret:
            case ops::fret:
            case ops::dret:
                if(frameBytes) { _ADDri(regs::rsp, frameBytes); }
                for(int r = savedRegs.size(); r--;)
                {
                    if(R2Mask(savedRegs[r]) & regs::mask_float)
                    {
                    
                        _load_f128(savedRegs[r], regs::rsp, 0);
                        // we might have used regs:none for alignment
                        if(r && savedRegs[r-1] == regs::none)
                        {
                            _ADDri(regs::rsp, 16+8); --r;
                        }
                        else _ADDri(regs::rsp, 16);
                    }
                    else _POP(savedRegs[r]);
                }
                a64.emit(0xC3);
                break;
                
            case ops::tcallp:
                if(frameBytes) { _ADDri(regs::rsp, frameBytes); }
                for(int r = savedRegs.size(); r--;)
                {
                    if(R2Mask(savedRegs[r]) & regs::mask_float)
                    {
                        _load_f128(savedRegs[r], regs::rsp, 0);
                        // we might have used regs:none for alignment
                        if(r && savedRegs[r-1] == regs::none)
                        {
                            _ADDri(regs::rsp, 16+8); --r;
                        }
                        else _ADDri(regs::rsp, 16);
                    }
                    else _POP(savedRegs[r]);
                }
                // indirect jump
                a64._RR(0, 4, REG(ops[i.in[0]].reg), 0xFF);
                break;
                
            case ops::tcalln:
                if(frameBytes) { _ADDri(regs::rsp, frameBytes); }
                for(int r = savedRegs.size(); r--;)
                {
                    if(R2Mask(savedRegs[r]) & regs::mask_float)
                    {
                        _load_f128(savedRegs[r], regs::rsp, 0);
                        // we might have used regs:none for alignment
                        if(r && savedRegs[r-1] == regs::none)
                        {
                            _ADDri(regs::rsp, 16+8); --r;
                        }
                        else _ADDri(regs::rsp, 16);
                    }
                    else _POP(savedRegs[r]);
                }
                // near jump
                a64.emit(0xE9);
                nearReloc.emplace_back(
                    NearReloc{(uint32_t)out.size(), (uint32_t) i.imm32});
                a64.emit32(-4-out.size());
                break;

            case ops::iadd:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _ADDrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _ADDrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _LEArr(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg);
                }
                break;

            case ops::iaddI:
                // use LEA for different input/output registers
                if(i.reg == ops[i.in[0]].reg)
                {
                    if(1 == i.imm32) { _INC(i.reg); }
                    else if(-1 == i.imm32) { _DEC(i.reg); }
                    else _ADDri(i.reg, (int32_t) i.imm32);
                }
                else _LEAri(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
                
            case ops::isub:
                if(i.reg == ops[i.in[0]].reg)
                {
                    // simple
                    _SUBrr(i.reg, ops[i.in[1]].reg);
                }
                else
                {
                    // general case
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                    _SUBrr(i.reg, ops[i.in[1]].reg);
                }
                break;

            case ops::isubI:
                {
                    bool sameReg = (i.reg == ops[i.in[0]].reg);
                        
                    if(i.imm32 == 0x80000000)
                    {
                        if(!sameReg) _MOVrr(i.reg, ops[i.in[0]].reg);
                        _SUBri(i.reg, (int32_t) i.imm32);
                    }
                    else if(!sameReg)
                    {
                        _LEAri(i.reg, ops[i.in[0]].reg, -i.imm32);
                    }
                    else if(i.imm32 == 1) { _DEC(i.reg); }
                    else if(i.imm32 == -1) { _INC(i.reg); }
                    else _SUBri(i.reg, (int32_t) i.imm32);
                }
                break;
            
            case ops::ineg:
                if(i.reg != ops[i.in[0]].reg)
                {
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                }
                _NEGr(i.reg);
                break;

            case ops::imul:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _IMULrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _IMULrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                    _IMULrr(i.reg, ops[i.in[1]].reg);
                }
                break;
                
            case ops::imulI:
                {
                    // fold converts pow2 multiplies to shl
                    // so here we check for the non-pow2 cases
                    //
                    // NOTE: LEA with RBP/R13 needs disp8 which is gives
                    // one cycle penalty and means IMUL is better (less uops)
                    auto rin = ops[i.in[0]].reg;
                    if(i.imm32 > 0 && REG(regs::rbp) != (0x7 & REG(rin)))
                    {
                        int shift = 0, imm32 = i.imm32;
                        while(!(imm32&1)) { ++shift; imm32 >>= 1; }
                        switch(imm32)
                        {
                        case 3: // lea [reg+2*reg]
                            _LEArrs(i.reg, rin, rin, 1);
                            if(shift) { _SHLri8(i.reg); a64.emit(shift); }
                            break;
                        case 5: // lea [reg+4*reg]
                            _LEArrs(i.reg, rin, rin, 2);
                            if(shift) { _SHLri8(i.reg); a64.emit(shift); }
                            break;
                        case 9: // lea [reg+8*reg]
                            _LEArrs(i.reg, rin, rin, 3);
                            if(shift) { _SHLri8(i.reg); a64.emit(shift); }
                            break;
                        default:
                            _IMULrri(i.reg, ops[i.in[0]].reg, i.imm32);
                        }
                    }
                    else _IMULrri(i.reg, ops[i.in[0]].reg, i.imm32);
                }
                break;
                
            case ops::idiv:
            case ops::imod:
                // DIV and MOD only differ by i.reg
                // which is handled by constraints
                if(ops[i.in[0]].reg != regs::rax)
                {
                    _MOVrr(regs::rax, ops[i.in[0]].reg);
                }
                // CDQ/CQO to sign-extend RAX -> RDX:RAX
                a64.emit(0x48); a64.emit(0x99);
                _IDIVr(ops[i.in[1]].reg);
                break;

            case ops::udiv:
            case ops::umod:
                // DIV and MOD only differ by i.reg
                // which is handled by constraints
                if(ops[i.in[0]].reg != regs::rax)
                {
                    _MOVrr(regs::rax, ops[i.in[0]].reg);
                }
                // clear RDX
                _XORrr(regs::rdx, regs::rdx);
                _DIVr(ops[i.in[1]].reg);
                break;

            case ops::inot:
                if(i.reg != ops[i.in[0]].reg)
                {
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                }
                _NOTr(i.reg);
                break;

            case ops::iand:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _ANDrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _ANDrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                    _ANDrr(i.reg, ops[i.in[1]].reg);
                }
                break;
            case ops::iandI:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _ANDri(i.reg, (int32_t) i.imm32);
                break;

            case ops::ior:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _ORrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _ORrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                    _ORrr(i.reg, ops[i.in[1]].reg);
                }
                break;
            case ops::iorI:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _ORri(i.reg, (int32_t) i.imm32);
                break;

            case ops::ixor:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _XORrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _XORrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                    _XORrr(i.reg, ops[i.in[1]].reg);
                }
                break;
            case ops::ixorI:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _XORri(i.reg, (int32_t) i.imm32);
                break;

            case ops::ishl:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SHLr(i.reg); // in[1] is always CL
                break;

            case ops::ishr:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SARr(i.reg); // in[1] is always CL
                break;

            case ops::ushr:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SHRr(i.reg); // in[1] is always CL
                break;

            case ops::ishlI:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SHLri8(i.reg);
                a64.emit(i.imm32); // just one byte
                break;

            case ops::ishrI:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SARri8(i.reg);
                a64.emit(i.imm32); // just one byte
                break;

            case ops::ushrI:
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SHRri8(i.reg);
                a64.emit(i.imm32); // just one byte
                break;

            case ops::dadd:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _ADDSDxx(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _ADDSDxx(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSDxx(i.reg, ops[i.in[0]].reg);
                    _ADDSDxx(i.reg, ops[i.in[1]].reg);
                }
                break;
                
            case ops::dsub:
                if(i.reg == ops[i.in[0]].reg)
                {
                    // simple
                    _SUBSDxx(i.reg, ops[i.in[1]].reg);
                }
                else
                {
                    // general case
                    _MOVSDxx(i.reg, ops[i.in[0]].reg);
                    _SUBSDxx(i.reg, ops[i.in[1]].reg);
                }
                break;
                
            case ops::dneg:
                if(i.reg == ops[i.in[0]].reg)
                {
                    uint64_t signBit = ((uint64_t)1)<<63;
                    _XORPSxi(i.reg, _mm_set1_epi64x(signBit));
                }
                else
                {
                    _XORPSxx(i.reg, i.reg);
                    _SUBSDxx(i.reg, ops[i.in[0]].reg);
                }
                break;
                
            case ops::dabs:
                if(i.reg == ops[i.in[0]].reg)
                {
                    uint64_t signBit = ((uint64_t)1)<<63;
                    _ANDPSxi(i.reg, _mm_set1_epi64x(~signBit));
                }
                else
                {
                    uint64_t signBit = ((uint64_t)1)<<63;
                    _MOVSDxx(i.reg, ops[i.in[0]].reg);
                    _ANDPSxi(i.reg, _mm_set1_epi64x(~signBit));
                }
                break;

            case ops::dmul:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _MULSDxx(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _MULSDxx(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSDxx(i.reg, ops[i.in[0]].reg);
                    _MULSDxx(i.reg, ops[i.in[1]].reg);
                }
                break;

            case ops::ddiv:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _DIVSDxx(i.reg, ops[i.in[1]].reg);
                }
                else
                {
                    _MOVSDxx(i.reg, ops[i.in[0]].reg);
                    _DIVSDxx(i.reg, ops[i.in[1]].reg);
                }
                break;
                
            case ops::fadd:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _ADDSSxx(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _ADDSSxx(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSSxx(i.reg, ops[i.in[0]].reg);
                    _ADDSSxx(i.reg, ops[i.in[1]].reg);
                }
                break;
                
            case ops::fsub:
                if(i.reg == ops[i.in[0]].reg)
                {
                    // simple
                    _SUBSSxx(i.reg, ops[i.in[1]].reg);
                }
                else
                {
                    // general case
                    _MOVSSxx(i.reg, ops[i.in[0]].reg);
                    _SUBSSxx(i.reg, ops[i.in[1]].reg);
                }
                break;
                
            case ops::fneg:
                if(i.reg == ops[i.in[0]].reg)
                {
                    uint32_t signBit = ((uint32_t)1)<<31;
                    _XORPSxi(i.reg, _mm_set1_epi32(signBit));
                }
                else
                {
                    _XORPSxx(i.reg, i.reg);
                    _SUBSSxx(i.reg, ops[i.in[0]].reg);
                }
                break;
                
            case ops::fabs:
                if(i.reg == ops[i.in[0]].reg)
                {
                    uint32_t signBit = ((uint32_t)1)<<31;
                    _ANDPSxi(i.reg, _mm_set1_epi32(~signBit));
                }
                else
                {
                    uint32_t signBit = ((uint32_t)1)<<31;
                    _MOVSSxx(i.reg, ops[i.in[0]].reg);
                    _ANDPSxi(i.reg, _mm_set1_epi32(~signBit));
                }
                break;

            case ops::fmul:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _MULSSxx(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _MULSSxx(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSSxx(i.reg, ops[i.in[0]].reg);
                    _MULSSxx(i.reg, ops[i.in[1]].reg);
                }
                break;

            case ops::fdiv:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _DIVSSxx(i.reg, ops[i.in[1]].reg);
                }
                else
                {
                    _MOVSSxx(i.reg, ops[i.in[0]].reg);
                    _DIVSSxx(i.reg, ops[i.in[1]].reg);
                }
                break;

            case ops::lci:
                if(!i.i64)
                {
                    _XORrr(i.reg, i.reg);
                }
                else
                {
                    _MOVri(i.reg, i.i64);
                }
                break;
                
            case ops::lcf:
                if(0 != i.f32)
                {
                    _XORPSxx(i.reg, i.reg);
                }
                else
                {
                    _MOVSSxi(i.reg, i.f32);
                }
                break;

            case ops::lcd:
                if(i.f64 == 0.0)
                {
                    _XORPSxx(i.reg, i.reg);
                }
                else
                {
                    _MOVSDxi(i.reg, i.f64);
                }
                break;

            case ops::lnp:
                {
                    // force 32-bit offset
                    _LEAri(i.reg, RIP, 1<<31);
                    // pop the disp32 field
                    a64.out.resize(a64.out.size() - 4);
                    // add reloc
                    nearReloc.emplace_back(
                        NearReloc{(uint32_t)out.size(), (uint32_t) i.imm32});
                    a64.emit32(-4-out.size());
                }
                break;

            case ops::i8: _MOVSX_8(i.reg, ops[i.in[0]].reg); break;
            case ops::i16: _MOVSX_16(i.reg, ops[i.in[0]].reg); break;
            case ops::i32: _MOVSX_32(i.reg, ops[i.in[0]].reg); break;
            
            case ops::u8: _MOVZX_8(i.reg, ops[i.in[0]].reg); break;
            case ops::u16: _MOVZX_16(i.reg, ops[i.in[0]].reg); break;
            case ops::u32: _MOVZX_32(i.reg, ops[i.in[0]].reg); break;

            case ops::li8:
                _load_i8(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::li16:
                _load_i16(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::li32:
                _load_i32(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::li64:
                _load_i64(i.reg, ops[i.in[0]].reg, i.off16);
                break;

            case ops::lu8:
                _load_u8(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::lu16:
                _load_u16(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::lu32:
                _load_u32(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::lf32:
                _load_f32(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            case ops::lf64:
                _load_f64(i.reg, ops[i.in[0]].reg, i.off16);
                break;
            
            case ops::si8:
                _store_i8(ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::si16:
                _store_i16(ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::si32:
                _store_i32(ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::si64:
                _store_i64(ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::sf32:
                _store_f32(ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::sf64:
                _store_f64(ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
                
            case ops::l2i8:
                _load2_i8(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2i16:
                _load2_i16(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2i32:
                _load2_i32(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2i64:
                _load2_i64(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;

            case ops::l2u8:
                _load2_u8(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2u16:
                _load2_u16(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2u32:
                _load2_u32(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2f32:
                _load2_f32(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            case ops::l2f64:
                _load2_f64(i.reg, ops[i.in[0]].reg, ops[i.in[1]].reg, i.off16);
                break;
            
            case ops::s2i8:
                _store2_i8(ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2i16:
                _store2_i16(ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2i32:
                _store2_i32(ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2i64:
                _store2_i64(ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2f32:
                _store2_f32(ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;
            case ops::s2f64:
                _store2_f64(ops[i.in[0]].reg, ops[i.in[1]].reg, ops[i.in[2]].reg, i.off16);
                break;

            case ops::ci2d:
                _CVTSI2SDxr(i.reg, ops[i.in[0]].reg);
                break;
            case ops::cd2i:
                _CVTTSD2SIrx(i.reg, ops[i.in[0]].reg);
                break;

            case ops::bci2d:
                _MOVQrx(i.reg, ops[i.in[0]].reg);
                break;
            case ops::bcd2i:
                _MOVQxr(i.reg, ops[i.in[0]].reg);
                break;
                
            case ops::bci2f:
                _MOVDrx(i.reg, ops[i.in[0]].reg);
                break;
            case ops::bcf2i:
                _MOVDxr(i.reg, ops[i.in[0]].reg);
                break;
                
            case ops::ci2f:
                _CVTSI2SSxr(i.reg, ops[i.in[0]].reg);
                break;
            case ops::cf2i:
                _CVTTSS2SIrx(i.reg, ops[i.in[0]].reg);
                break;

            case ops::cf2d:
                _CVTSS2SDxx(i.reg, ops[i.in[0]].reg);
                break;
            case ops::cd2f:
                _CVTSD2SSxx(i.reg, ops[i.in[0]].reg);
                break;

            case ops::fence: break; // just a compiler fence on x64
            
            /* Pseudo-ops: these need to check value types */
            case ops::phi: break;   // this is just NOP here
            
            case ops::reload:
                BJIT_ASSERT(ops[i.in[0]].scc != noSCC);
                if(i.flags.type == Op::_f64)
                    _load_f64(i.reg, regs::rsp, frameOffset + 8*ops[i.in[0]].scc);
                else if(i.flags.type == Op::_f32)
                    _load_f32(i.reg, regs::rsp, frameOffset + 8*ops[i.in[0]].scc);
                else if(i.flags.type == Op::_ptr)
                    _load_i64(i.reg, regs::rsp, frameOffset + 8*ops[i.in[0]].scc);
                else BJIT_ASSERT(false);
                break;

            case ops::rename:
                // dummy-renames are normal
                if(i.reg == ops[i.in[0]].reg) break;
                
                if(i.flags.type == Op::_f64)
                    //_MOVSDxx(i.reg, ops[i.in[0]].reg);
                    // prefer rename over shuffle:
                    _MOVAPSxx(i.reg, ops[i.in[0]].reg);
                else if(i.flags.type == Op::_f32)
                    //_MOVSSxx(i.reg, ops[i.in[0]].reg);
                    // prefer rename over shuffle:
                    _MOVAPSxx(i.reg, ops[i.in[0]].reg);
                else if(i.flags.type == Op::_ptr)
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                else BJIT_ASSERT(false);
                break;
                
            default: BJIT_ASSERT(false);
        }

        // if marked for spill, store to stack
        if(i.flags.spill)
        {
            BJIT_ASSERT(i.scc != noSCC);
            // use the flagged type for spills, so that we'll BJIT_ASSERT
            // if we forget to add stuff here if we add more types
            if(i.flags.type == Op::_f64)
                _store_f64(i.reg, regs::rsp, frameOffset + 8*i.scc);
            else if(i.flags.type == Op::_f32)
                _store_f32(i.reg, regs::rsp, frameOffset + 8*i.scc);
            else if(i.flags.type == Op::_ptr)
                _store_i64(i.reg, regs::rsp, frameOffset + 8*i.scc);
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
    
    // pad with NOPs until desired alignment alignment
    // NOTE: this is important for constants!
    unsigned align = (a64.rodata128.size() ? 0xf : 0x7);
    while(out.size() & align) a64.emit(0x90);

    // emit 128-bit point constants
    a64.blockOffsets[a64.rodata128_index] = out.size();
    for(__m128 & bits : a64.rodata128)
    {
        a64.emit32(((uint32_t*)&bits)[0]);
        a64.emit32(((uint32_t*)&bits)[1]);
        a64.emit32(((uint32_t*)&bits)[2]);
        a64.emit32(((uint32_t*)&bits)[3]);
    }

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
    while(out.size() & 0xf) a64.emit(0x90);
    
    // for every relocation, ADD the block offset
    // FIXME: different relocation constant sizes?
    for(auto & r : a64.relocations)
    {
        *((uint32_t*)(out.data() + r.codeOffset))
            += a64.blockOffsets[r.blockIndex];
    }
    
}

#endif