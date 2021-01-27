
#include "ir.h"

using namespace bjit;

// register names
#define none ---
#define BJIT_STR(x) #x
#define BJIT_REGS_NAME(x) BJIT_STR(x)
static const char * regNames[] = { BJIT_REGS(BJIT_REGS_NAME) };
#undef none

const char * bjit::Proc::regName(int r) { return regNames[r]; }

void bjit::Proc::debugOp(uint16_t iop)
{
    Op & op = ops[iop];

    if(op.hasOutput())
    {
        if(op.flags.spill) printf("=[%04x]= ", op.scc);
        else printf(" (%04x)  ", op.scc);
        //else printf("        ");
    }
    else printf("         ");

    // make it clear which renames actually cause moves
    bool nopRename = false;
    if(op.opcode == ops::rename && op.reg == ops[op.in[0]].reg)
        nopRename = true;
    printf("%04x %6s %8s", iop, regName(op.reg),
        nopRename ? " - " : op.strOpcode());

    switch(op.flags.type)
    {
        case Op::_none: printf("          "); break;
        case Op::_ptr:  printf(" %3d  ptr ", op.nUse); break;
        case Op::_f64:  printf(" %3d  f64 ", op.nUse); break;
    };

    // special-case reload to not print register
    if(op.opcode == ops::reload)
        printf(" [%04x]:%04x", ops[op.in[0]].scc, op.in[0]);
    else
    {
        switch(op.nInputs())
        {
            case 1: printf(" %s:%04x",
                regNames[ops[op.in[0]].reg], op.in[0]);
                if(ops[op.in[0]].opcode == ops::nop) printf(" <BAD0>");
                break;
            case 2: printf(" %s:%04x %s:%04x",
                regNames[ops[op.in[0]].reg], op.in[0],
                regNames[ops[op.in[1]].reg], op.in[1]);
                if(ops[op.in[0]].opcode == ops::nop) printf(" <BAD:0>");
                if(ops[op.in[1]].opcode == ops::nop) printf(" <BAD:1>");
                break;
            case 0: break;
            default: assert(false);
        }
    }

    if(op.hasImm32()) printf(" +%d", op.imm32);
    if(op.hasI64()) printf(" i64:%lld", op.i64);
    if(op.hasF64()) printf(" f64:%.8e", op.f64);

    if(op.opcode == ops::phi)
    {
        for(auto & a : blocks[op.in[0]].args[op.in[1]].alts)
        {
            printf(" L%d:[%04x]:%04x", a.src, ops[a.val].scc, a.val);
        }
    }

    if(op.opcode == ops::iparam || op.opcode == ops::fparam)
    {
        printf(" #%d total #%d", op.in[1], op.in[0]);
    }


    if(op.opcode <= ops::jmp) printf(" L%d", op.label[0]);
    if(op.opcode < ops::jmp) printf(" L%d", op.label[1]);

    printf("\n");
}

void bjit::Proc::debug()
{
    printf("\n;----");
    if(raDone) printf(" Slots: %d\n", nSlots); else printf("\n");
    
    if(live.size())
    {
        for(auto b : live)
        {
            printf("L%d:", b);
            for(auto s : blocks[b].comeFrom) printf(" <L%d", s);
            //if(0)
            for(int i = 0; i < blocks[b].livein.size(); ++i)
            {
                if(!(0x7&(i))) printf("\n; In: ");
                printf(" [%04x]:%04x",
                    ops[blocks[b].livein[i]].scc, blocks[b].livein[i]);
            }
            //if(0)
            if(raDone)
            {
                printf("\n; Regs:");
                for(int i = 0; i < regs::nregs; ++i)
                {
                    if(blocks[b].regsIn[i] != 0xffff)
                    {
                        printf(" %s:%04x", regNames[i], blocks[b].regsIn[i]);
                    }
                }
            }
            printf("\n");
            printf("; SLOT  VALUE    REG       OP USE TYPE  ARGS\n");
            for(auto & iop : blocks[b].code) { debugOp(iop); }
            //if(0)
            if(raDone)
            {
                printf("; Out:");
                for(int i = 0; i < regs::nregs; ++i)
                {
                    if(blocks[b].regsOut[i] != 0xffff)
                    {
                        printf(" %s:%04x", regNames[i], blocks[b].regsOut[i]);
                    }
                }
                printf("\n\n");
            }
        }
    }
    else for(int b = 0; b < blocks.size(); ++b)
    {
        if(!blocks[b].flags.live) continue;
        printf("L%d:%s\n", b, blocks[b].flags.live ? "" : " -- dead --");
        for(auto & iop : blocks[b].code) { debugOp(iop); }
    }
    printf(";----\n");
    
}
