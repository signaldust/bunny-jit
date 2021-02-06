
#include "bjit.h"

#include <inttypes.h>

using namespace bjit;

// register names
#define none ---
#define BJIT_STR(x) #x
#define BJIT_REGS_NAME(x) BJIT_STR(x)
static const char * regNames[] = { BJIT_REGS(BJIT_REGS_NAME) };
#undef none

const char * bjit::Proc::regName(int r) const { return regNames[r]; }

void bjit::Proc::debugOp(uint16_t iop) const
{
    if(iop == noVal) { BJIT_LOG("           -- removed op -- \n"); return; }
    auto & op = ops[iop];

    if(op.hasOutput())
    {
        if(op.flags.spill) BJIT_LOG("=[%04x]= ", op.scc);
        else if(op.scc == noSCC) BJIT_LOG("  ----   ");
        else BJIT_LOG(" (%04x)  ", op.scc);
        //else BJIT_LOG("        ");
    }
    else BJIT_LOG("         ");

    // make it clear which renames actually cause moves
    bool nopRename = false;
    if(op.opcode == ops::rename && op.reg == ops[op.in[0]].reg)
        nopRename = true;
    BJIT_LOG("%04x %6s %8s %c", iop,
        op.hasOutput() ? regName(op.reg) : "",
        nopRename ? " - " : op.strOpcode(),
        op.flags.no_opt ? '*' : ' ');

    switch(op.flags.type)
    {
        case Op::_none: BJIT_LOG("          "); break;
        case Op::_ptr:  BJIT_LOG(" %3d  ptr ", op.nUse); break;
        case Op::_f32:  BJIT_LOG(" %3d  f32 ", op.nUse); break;
        case Op::_f64:  BJIT_LOG(" %3d  f64 ", op.nUse); break;
    };

    // this should now hold
    if(!op.hasI64() && !op.hasF64())
    {
        if(op.nInputs() < 2) BJIT_ASSERT(op.in[1] == noVal);
        if(op.nInputs() < 1) BJIT_ASSERT(op.in[0] == noVal);
    }

    // special-case reload to not print register
    if(op.opcode == ops::reload)
        BJIT_LOG(" [%04x]:%04x", ops[op.in[0]].scc, op.in[0]);
    else
    {
        switch(op.nInputs())
        {
            case 1: BJIT_LOG(" %s:%04x",
                regNames[ops[op.in[0]].reg], op.in[0]);
                if(ops[op.in[0]].opcode == ops::nop) BJIT_LOG(" <BAD0>");
                break;
            case 2: BJIT_LOG(" %s:%04x %s:%04x",
                regNames[ops[op.in[0]].reg], op.in[0],
                regNames[ops[op.in[1]].reg], op.in[1]);
                if(ops[op.in[0]].opcode == ops::nop) BJIT_LOG(" <BAD:0>");
                if(ops[op.in[1]].opcode == ops::nop) BJIT_LOG(" <BAD:1>");
                break;
            case 0: break;
            default: BJIT_ASSERT(false);
        }
    }

    if(op.opcode == ops::icalln
    || op.opcode == ops::fcalln
    || op.opcode == ops::dcalln
    || op.opcode == ops::tcalln) BJIT_LOG(" near: %d", op.imm32);
    else if(op.hasImm32()) BJIT_LOG(" %+d", op.imm32);
    
    if(op.hasI64()) BJIT_LOG(" i64:%" PRId64, op.i64);
    if(op.hasF32()) BJIT_LOG(" f32:%.8e", op.f32);
    if(op.hasF64()) BJIT_LOG(" f64:%.8e", op.f64);

    if(op.opcode == ops::phi)
    {
        for(auto & a : blocks[op.block].args[op.phiIndex].alts)
        {
            if(ops[a.val].scc != noSCC)
                BJIT_LOG(" L%d:[%04x]:%04x", a.src, ops[a.val].scc, a.val);
            else 
                BJIT_LOG(" L%d:[----]:%04x", a.src, a.val);
        }
    }

    if(op.opcode == ops::iarg || op.opcode == ops::farg || op.opcode == ops::darg)
    {
        BJIT_LOG(" #%d total #%d", op.indexType, op.indexTotal);
    }

    if(op.opcode <= ops::jmp) BJIT_LOG(" L%d", op.label[0]);
    if(op.opcode < ops::jmp) BJIT_LOG(" L%d", op.label[1]);

    BJIT_LOG("\n");
}

void bjit::Proc::debug() const
{
    BJIT_LOG("\n;----");
    if(raDone) BJIT_LOG(" Slots: %d\n", nSlots); else BJIT_LOG("\n");
    
    if(live.size())
    {
        for(auto b : live)
        {
            BJIT_LOG("L%d:", b);
            for(auto s : blocks[b].comeFrom) BJIT_LOG(" <L%d", s);
            BJIT_LOG("\n; Dom: L%d,", blocks[b].idom);
            if(blocks[b].pdom != noVal) BJIT_LOG(" PDom: L%d", blocks[b].pdom);
            else BJIT_LOG(" PDom: exit");
            //BJIT_LOG("\n; "); for(auto s : blocks[b].dom) BJIT_LOG(" ^L%d", s);
            //BJIT_LOG("\n; "); for(auto s : blocks[b].pdom) BJIT_LOG(" L%d^", s);
            //if(0)
            for(int i = 0; i < blocks[b].livein.size(); ++i)
            {
                if(!(0x7&(i))) BJIT_LOG("\n; Live: ");
                if(ops[blocks[b].livein[i]].scc != noSCC)
                    BJIT_LOG(" [%04x]:%04x",
                        ops[blocks[b].livein[i]].scc, blocks[b].livein[i]);
                else BJIT_LOG(" [----]:%04x",blocks[b].livein[i]);
            }
            //if(0)
            if(raDone)
            {
                BJIT_LOG("\n; In:");
                for(int i = 0; i < regs::nregs; ++i)
                {
                    if(blocks[b].regsIn[i] != 0xffff)
                    {
                        BJIT_LOG(" %s:%04x", regNames[i], blocks[b].regsIn[i]);
                    }
                }
            }
            BJIT_LOG("\n");
            BJIT_LOG("; SLOT  VALUE    REG       OP   USE TYPE  ARGS\n");
            for(auto & iop : blocks[b].code) { debugOp(iop); }
            //if(0)
            if(raDone)
            {
                BJIT_LOG("; Out:");
                for(int i = 0; i < regs::nregs; ++i)
                {
                    if(blocks[b].regsOut[i] != 0xffff)
                    {
                        BJIT_LOG(" %s:%04x", regNames[i], blocks[b].regsOut[i]);
                    }
                }
                BJIT_LOG("\n");
            }
            BJIT_LOG("\n");
        }
    }
    else for(int b = 0; b < blocks.size(); ++b)
    {
        if(!blocks[b].flags.live) continue;
        BJIT_LOG("L%d:%s\n", b, blocks[b].flags.live ? "" : " -- dead --");
        BJIT_LOG("; SLOT  VALUE    REG       OP USE TYPE  ARGS\n");
        for(auto & iop : blocks[b].code) { debugOp(iop); }
    }
    BJIT_LOG(";----\n");
    
}
