
// Opcode data definitions

#include "bjit.h"

#define BJIT_DATA(name, out, in) { #name, out, in }
static struct
{
    const char * name;

    unsigned    outputs;
    unsigned    inputs;
} opData[] = { BJIT_OPS(BJIT_DATA) };

const char * bjit::impl::Op::strOpcode() const
{
    return opData[this->opcode].name;
}

bool bjit::impl::Op::hasOutput() const
{
    return 0 != (opData[this->opcode].outputs & 0x3);
}

unsigned bjit::impl::Op::nInputs() const
{
    return opData[this->opcode].inputs & 0x3;   // mask the flags
}

bool bjit::impl::Op::hasImm32() const
{
    return 0 != (opData[this->opcode].inputs & BJIT_IMM32);
}

bool bjit::impl::Op::hasI64() const
{
    return 0 != (opData[this->opcode].inputs & BJIT_I64);
}

bool bjit::impl::Op::hasF64() const
{
    return 0 != (opData[this->opcode].inputs & BJIT_F64);
}

bool bjit::impl::Op::hasF32() const
{
    return 0 != (opData[this->opcode].inputs & BJIT_F32);
}

bool bjit::impl::Op::hasMemTag() const
{
    return (opData[this->opcode].inputs & BJIT_MEM);
}

bool bjit::impl::Op::hasSideFX() const
{
    return !opData[this->opcode].outputs
        || (opData[this->opcode].outputs & BJIT_SIDEFX);
}

bool bjit::impl::Op::canCSE() const
{
    return (opData[this->opcode].outputs & BJIT_CSE);
}


bool bjit::impl::Op::canMove() const
{
    return !(opData[this->opcode].outputs & BJIT_NOMOVE);
}
