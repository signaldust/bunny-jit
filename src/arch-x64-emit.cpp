

#include "ir.h"
#include "arch-x64-asm.h"

using namespace bjit;

namespace bjit
{
    namespace regs
    {
        // we use "none" as a list terminator
        static int calleeSaved[] =
        {
            rbp, rbx, r12, r13, r14, r15,

            xmm1, xmm2, xmm3,   // these are for testing :)
#ifdef _WIN32
            rsi, rdi,
            
            // these are caller saved SSE wide, volatile otherwise
            xmm6, xmm7, xmm8, xxm9, xmm10,
            xmm11, xmm12, xmm13, xmm14, xmm15
#endif
            none
        };
    };
};

void Proc::arch_emit(std::vector<uint8_t> & out)
{
    out.clear();
    
    for(auto & b : blocks) { b.flags.codeDone = false; }

    AsmX64 a64(out, blocks.size());

    std::vector<int>    savedRegs;

    // push callee-saved registers
    int nPush = 0;
    for(int i = 0; regs::calleeSaved[i] != regs::none; ++i)
    {
        if(usedRegs & (1ull<<regs::calleeSaved[i]))
        {
            savedRegs.push_back(regs::calleeSaved[i]);
            if((1ull<<savedRegs.back()) & regs::mask_float)
            {
                // force alignment if necessary
                if(!(nSlots&1))
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
                _PUSH(savedRegs.back()); ++nPush;
            }
        }
    }

    // need 8 mod 16 - emit "prelude" if necessary
    nSlots += 1 ^ ((nPush+nSlots) & 1);
    if(nSlots) { _SUBri(regs::rsp, 8*nSlots); }

    // this tracks additional stack offset
    // when we need to adjust stack during calls
    unsigned    frameOffset = 0;

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
            a64.emit(0xE9);
            a64.addReloc(label);
            a64.emit32(-4-out.size());
        }
        scheduleThreading();
    };

    auto emitOp = [&](Op & i)
    {
        // for conditionals, if first branch is not done
        // then schedule it as the fall-thru by inverting..
        // usually this should give better code ordering,
        // although realistically it's bit of a hack
        //
        // if neither branch is done, we should pick the fall-thru
        // that is 
        if(i.opcode < ops::jmp
        && !blocks[i.label[0]].flags.codeDone)
        {
            i.opcode ^= 1;
            std::swap(i.label[0], i.label[1]);
        }

        switch(i.opcode)
        {
            case ops::iarg: // incoming arguments
            case ops::farg:
                break;  // these are nops

            case ops::ipass: // outgoing arguments
            case ops::fpass:
                break;  // these are nops
                
            case ops::icallp:
            case ops::fcallp:
                // generate indirect near-call: FF /2
                a64._RR(0, 2, REG(ops[i.in[0]].reg), 0xFF);
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
                a64.addReloc(i.label[0]);
                a64.emit32(-4-out.size());
                // schedule target
                if(!blocks[i.label[0]].flags.codeDone)
                {
                    blocks[i.label[0]].flags.codeDone = true;
                    todo.push_back(i.label[0]);
                    scheduleThreading();
                }
                doJump(i.label[1]);
                break;

            case ops::jz:
            case ops::jnz:
                _TESTrr(ops[i.in[0]].reg, ops[i.in[0]].reg);
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode));
                // relocs
                a64.addReloc(i.label[0]);
                a64.emit32(-4-out.size());
                // schedule target
                if(!blocks[i.label[0]].flags.codeDone)
                {
                    blocks[i.label[0]].flags.codeDone = true;
                    todo.push_back(i.label[0]);
                    scheduleThreading();
                }
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
                _CMPri(ops[i.in[0]].reg, i.imm32);
                // then jump
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode+ops::jilt-ops::jiltI));
                // relocs
                a64.addReloc(i.label[0]);
                a64.emit32(-4-out.size());
                // schedule target
                if(!blocks[i.label[0]].flags.codeDone)
                {
                    blocks[i.label[0]].flags.codeDone = true;
                    todo.push_back(i.label[0]);
                    scheduleThreading();
                }
                doJump(i.label[1]);
                break;
                            
            case ops::jflt:
            case ops::jfge:
            case ops::jfgt:
            case ops::jfle:

            case ops::jfne:
            case ops::jfeq:
                // UCOMISD (scalar double compare)
                _UCOMISDrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then jump
                a64.emit(0x0F);
                a64.emit(0x80 | _CC(i.opcode));
                // relocs
                a64.addReloc(i.label[0]);
                a64.emit32(-4-out.size());
                // schedule target
                if(!blocks[i.label[0]].flags.codeDone)
                {
                    blocks[i.label[0]].flags.codeDone = true;
                    todo.push_back(i.label[0]);
                    scheduleThreading();
                }
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
                // xor destination
                _XORrr(i.reg, i.reg);
                // compare
                _CMPrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode + ops::jilt - ops::ilt));
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
                // xor destination
                _XORrr(i.reg, i.reg);
                // compare
                _CMPri(ops[i.in[0]].reg, i.imm32);
                // then emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode + ops::jilt - ops::ilt));
                break;

            case ops::flt:
            case ops::fge:
            case ops::fgt:
            case ops::fle:

            case ops::fne:
            case ops::feq:
                // xor destination
                _XORrr(i.reg, i.reg);
                
                // UCOMISD (scalar double compare)
                _UCOMISDrr(ops[i.in[0]].reg, ops[i.in[1]].reg);
                // then emit SETcc
                a64._RR(3, 3, REG(i.reg), 0x0F,
                    0x90 | _CC(i.opcode - ops::jmp));
                break;

            case ops::iretI:
                if(!i.imm32) _XORrr(regs::rax, regs::rax);
                else _MOVri(regs::rax, i.imm32);
                // fall through
            case ops::iret:
            case ops::fret:
                if(nSlots) { _ADDri(regs::rsp, 8*nSlots); }
                for(int r = savedRegs.size(); r--;)
                {
                    if((1ull<<savedRegs[r]) & regs::mask_float)
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
                    // LEA might or might not be better
                    // but we would need RBP/R13 special cases
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                    _ADDrr(i.reg, ops[i.in[1]].reg);
                }
                break;

            case ops::iaddI:
                // use LEA for different input/output registers
                // we don't track CCs anyway
                _LEA(i.reg, ops[i.in[0]].reg, i.imm32);
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
                if(i.reg != ops[i.in[0]].reg) _MOVrr(i.reg, ops[i.in[0]].reg);
                _SUBri(i.reg, i.imm32);
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
                // this curiously doesn't need a move
                // if we have imm8 or imm32
                _IMULrri(i.reg, ops[i.in[0]].reg, i.imm32);
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
                _ADDri(i.reg, i.imm32);
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
                _ORri(i.reg, i.imm32);
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
                _XORri(i.reg, i.imm32);
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

            case ops::fadd:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _ADDSDrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _ADDSDrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSDrr(i.reg, ops[i.in[0]].reg);
                    _ADDSDrr(i.reg, ops[i.in[1]].reg);
                }
                break;
                
                /**** Can't fit these in current OP format
            case ops::faddI:
                if(i.reg != ops[i.in[0]].reg) _MOVSDrr(i.reg, ops[i.in[0]].reg);
                _ADDSDri(i.reg, i.f64);
                break;
                */
            case ops::fsub:
                if(i.reg == ops[i.in[0]].reg)
                {
                    // simple
                    _SUBSDrr(i.reg, ops[i.in[1]].reg);
                }
                else
                {
                    // general case
                    _MOVSDrr(i.reg, ops[i.in[0]].reg);
                    _SUBSDrr(i.reg, ops[i.in[1]].reg);
                }
                break;
                /**** Can't fit these in current OP format
            case ops::fsubI:
                if(i.reg != ops[i.in[0]].reg) _MOVSDrr(i.reg, ops[i.in[0]].reg);
                _SUBSDri(i.reg, i.f64);
                break;
                */
                
            case ops::fneg:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _XORPSri(i.reg, _mm_set_epi64x(1ull<<63, 1ull<<63));
                }
                else
                {
                    _XORPSrr(i.reg, i.reg);
                    _SUBSDrr(i.reg, ops[i.in[0]].reg);
                }
                break;

            case ops::fmul:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _MULSDrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _MULSDrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSDrr(i.reg, ops[i.in[0]].reg);
                    _MULSDrr(i.reg, ops[i.in[1]].reg);
                }
                break;
                /**** Can't fit these in current OP format
            case ops::fmulI:
                if(i.reg != ops[i.in[0]].reg) _MOVSDrr(i.reg, ops[i.in[0]].reg);
                _MULSDri(i.reg, i.f64);
                break;
                */

            case ops::fdiv:
                if(i.reg == ops[i.in[0]].reg)
                {
                    _DIVSDrr(i.reg, ops[i.in[1]].reg);
                }
                else if(i.reg == ops[i.in[1]].reg)
                {
                    _DIVSDrr(i.reg, ops[i.in[0]].reg);
                }
                else
                {
                    _MOVSDrr(i.reg, ops[i.in[0]].reg);
                    _DIVSDrr(i.reg, ops[i.in[1]].reg);
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
                if(i.f64 == 0.0)
                {
                    _XORPSrr(i.reg, i.reg);
                }
                else
                {
                    _MOVSDri(i.reg, i.f64);
                }
                break;

            case ops::li8:
                _load_i8(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::li16:
                _load_i16(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::li32:
                _load_i32(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::li64:
                _load_i64(i.reg, ops[i.in[0]].reg, i.imm32);
                break;

            case ops::lu8:
                _load_u8(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::lu16:
                _load_u16(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::lu32:
                _load_u32(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::lf64:
                _load_f64(i.reg, ops[i.in[0]].reg, i.imm32);
                break;
            
            case ops::si8:
                _store_i8(ops[i.in[1]].reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::si16:
                _store_i16(ops[i.in[1]].reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::si32:
                _store_i32(ops[i.in[1]].reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::si64:
                _store_i64(ops[i.in[1]].reg, ops[i.in[0]].reg, i.imm32);
                break;
            case ops::sf64:
                _store_f64(ops[i.in[1]].reg, ops[i.in[0]].reg, i.imm32);
                break;

            case ops::ci2f:
                _CVTSI2SD_rr(i.reg, ops[i.in[0]].reg);
                break;
            case ops::cf2i:
                _CVTTSD2SI_rr(i.reg, ops[i.in[0]].reg);
                break;

            /* Pseudo-ops: these need to check value types */
            case ops::phi: break;   // this is just NOP here
            
            case ops::reload:
                assert(ops[i.in[0]].scc != noSCC);
                if((1ull<<i.reg)&regs::mask_float)
                    _load_f64(i.reg, regs::rsp, frameOffset + 8*ops[i.in[0]].scc);
                else if((1ull<<i.reg)&regs::mask_int)
                    _load_i64(i.reg, regs::rsp, frameOffset + 8*ops[i.in[0]].scc);
                else assert(false);
                break;

            case ops::rename:
                // dummy-renames are normal
                if(i.reg == ops[i.in[0]].reg) break;
                
                if((1ull<<i.reg)&regs::mask_float)
                    _MOVSDrr(i.reg, ops[i.in[0]].reg);
                else if((1ull<<i.reg)&regs::mask_int)
                    _MOVrr(i.reg, ops[i.in[0]].reg);
                else assert(false);
                break;
                
            default: assert(false);
        }

        // if marked for spill, store to stack
        if(i.flags.spill)
        {
            assert(i.scc != noSCC);
            // use the flagged type for spills, so that we'll assert
            // if we forget to add stuff here if we add more types
            if(i.flags.type == Op::_f64)
                _store_f64(i.reg, regs::rsp, frameOffset + 8*i.scc);
            else if(i.flags.type == Op::_ptr)
                _store_i64(i.reg, regs::rsp, frameOffset + 8*i.scc);
            else assert(false);
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

    a64.blockOffsets[a64.rodata128_index] = out.size();
    for(__m128 & bits : a64.rodata128)
    {
        a64.emit32(((uint32_t*)&bits)[0]);
        a64.emit32(((uint32_t*)&bits)[1]);
        a64.emit32(((uint32_t*)&bits)[2]);
        a64.emit32(((uint32_t*)&bits)[3]);
    }

    // emit floating point constants
    a64.blockOffsets[a64.rodata64_index] = out.size();
    for(uint64_t bits : a64.rodata64)
    {
        a64.emit32(bits);
        a64.emit32(bits>>32);
    }

    // for every relocation, ADD the block offset
    // FIXME: different relocation constant sizes?
    for(auto & r : a64.relocations)
    {
        *((uint32_t*)(out.data() + r.codeOffset))
            += a64.blockOffsets[r.blockIndex];
    }
    
}
