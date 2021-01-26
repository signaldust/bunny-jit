
// Opcode data definitions

#include "ir.h"

#define BJIT_DATA(name, out, in) { #name, out, in }
static struct
{
    const char * name;

    unsigned    outputs;
    unsigned    inputs;
} opData[] = { BJIT_OPS(BJIT_DATA) };

const char * bjit::Op::strOpcode()
{
    return opData[this->opcode].name;
}

bool bjit::Op::hasOutput()
{
    return 0 != (opData[this->opcode].outputs & 0xf);
}

unsigned bjit::Op::nInputs()
{
    return opData[this->opcode].inputs & 0xf;   // mask the flags
}

bool bjit::Op::hasImm32()
{
    return 0 != (opData[this->opcode].inputs & BJIT_IMM32);
}

bool bjit::Op::hasI64()
{
    return 0 != (opData[this->opcode].inputs & BJIT_I64);
}

bool bjit::Op::hasF64()
{
    return 0 != (opData[this->opcode].inputs & BJIT_F64);
}

bool bjit::Op::hasSideFX()
{
    return !opData[this->opcode].outputs
        || (opData[this->opcode].outputs & BJIT_SIDEFX);
}
