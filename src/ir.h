
#pragma once

#include <cstdint>
#include <vector>

#include "ir-ops.h"

#include "arch-x64.h"   // FIXME: check for arch

namespace bjit
{

#define BJIT_OP_ENUM(name,...) name
    namespace ops { enum { BJIT_OPS(BJIT_OP_ENUM) }; };
#undef BJIT_OP_ENUM

    struct Op
    {
        // input operands
        // NOTE: packing is sensitive (also FIXME: rethink this?)
        union
        {
            // 64-bit constants for load immediate
            int64_t     i64;
            uint64_t    u64;
            double      f64;
            
            // imm + two values
            // phi stores   in = { block, value  }
            // params store in = { nType, nTotal }
            struct {
                uint32_t    imm32;
                // 16-bits is likely enough?
                uint16_t    in[0];  // this is size 2, but silence clang
            };

        };

        // output data
        // NOTE: packing is sensitive (also relative to above)
        union
        {
            // NOTE: We let ssc alias on in[2] so that
            // ops without output can have 3 inputs
            //
            // We use this for call arguments, possibly stores in the future
            struct
            {
                uint16_t    scc;    // stack congruence class
                uint16_t    nUse;   // number of users
            };

            // jumps need labels
            uint16_t    label[2];
        };
        
        uint16_t    opcode;             // opcode, see ir-ops.h
        uint8_t     reg = regs::none;   // output register

        // Register type, needed for correct renames, etc..
        //
        // In the future, we could also use this to track single vs. double
        // precision floats and packed vector types for spills and reloads.
        //
        enum Type
        {
            _none,  // no output
            _ptr,   // pointer-sized integer (anything that fits GP regs)
            _f64    // double precision float
        };

        struct {
            Type    type    : 4;    // see above, packed into flags
            bool    keep    : 1;
            bool    spill   : 1;
        } flags = {};


        // regsMask returns a mask of registers that can hold this type
        RegMask     regsMask();     // in arch-XX-ops.cpp

        // regsIn, regsOut and regsLost return masks for the actual operation
        RegMask     regsIn(int i);  // in arch-XX-ops.cpp
        RegMask     regsOut();      // in arch-XX-ops.cpp
        RegMask     regsLost();     // in arch-XX-ops.cpp

        const char* strOpcode() const;
        
        unsigned    nInputs()   const;
        bool        hasOutput() const;

        bool    canCSE()        const;

        bool    hasImm32()      const;
        bool    hasI64()        const;
        bool    hasF64()        const;

        bool    hasSideFX()     const;
    };

    static const uint16_t   noVal = 0xffff;
    static const uint16_t   noSCC = 0xffff;

    // Variable rename tracker
    struct Rename
    {
        struct Map {
            uint16_t src, dst;
            Map(uint16_t s, uint16_t d) : src(s), dst(d) {}
        };
        std::vector<Map>    map;

        void add(uint16_t s, uint16_t d) { map.emplace_back(s,d); }

        Op & operator()(Op & op)
        {
            int n = op.nInputs();
            if(!n) return op;
            for(auto & r : map)
            {
                switch(n)
                {
                case 2: if(op.in[1] == r.src) op.in[1] = r.dst;
                case 1: if(op.in[0] == r.src) op.in[0] = r.dst;
                }
            }
            return op;
        }
    };
    
    // Use by Block to track actual phi-alternatives
    struct Phi
    {
        struct Alt
        {
            uint16_t    val;    // source value
            uint16_t    src;    // source block
        };

        std::vector<Alt>    alts;
        uint16_t            phiop;

        void add(uint16_t val, uint16_t block)
        {
            alts.emplace_back(Alt{val, block});
        }
    };

    // One basic block
    struct Block
    {
        std::vector<uint16_t>   code;
        std::vector<Phi>        args;

        std::vector<uint16_t>   livein;
        std::vector<uint16_t>   comeFrom;   // which blocks we come from?

        // register state on input
        uint16_t    regsIn[regs::nregs];

        // register state on output (used for shuffling)
        uint16_t    regsOut[regs::nregs];

        // dominators
        std::vector<uint16_t>   dom;

        struct {
            bool live       : 1;    // livescan uses this
            bool regsDone   : 1;    // reg-alloc uses this
            bool codeDone   : 1;    // backend uses this
        } flags = {};

        Block()
        {
            for(int i = 0; i < regs::nregs; ++i) regsIn[i] = regsOut[i] = noVal;
        }
    };

    struct Proc
    {
        Proc()
        {
            currentBlock = newLabel();
            emitLabel(currentBlock);
        }

        // debug.cpp
        void debug();
        void debugOp(uint16_t index);
        const char * regName(int r);

        void opt()
        {
            // do DCE first, then fold
            // repeat until neither does progress
            do opt_dce(); while(opt_fold());
        }

        void compile(std::vector<uint8_t> & bytes)
        {
            opt_dce();
            allocRegs();
            arch_emit(bytes);
        }

        std::vector<unsigned>   env;

        // generate a label
        unsigned newLabel()
        {
            unsigned i = blocks.size();
            blocks.resize(i + 1);
            
            blocks[i].args.resize(env.size());
            return i;
        }

        void emitLabel(unsigned label)
        {
            assert(label < blocks.size());
            // use live-flag to enforce "emit once"
            assert(!blocks[label].flags.live);
            blocks[label].flags.live = true;
            currentBlock = label;

            // check environment size, generate phis
            assert(blocks[label].args.size() == env.size());
            for(int i = 0; i < env.size(); ++i)
            {
                env[i] = addOp(ops::phi, ops[env[i]].flags.type);
                ops[env[i]].in[0] = label;
                ops[env[i]].in[1] = i;
                // regalloc wants this
                blocks[label].args[i].phiop = env[i];
            }
        }

        ///////////////////////////////
        // FRONT END OPCODE EMITTERS //
        ///////////////////////////////

        // CONSTANTS
        unsigned lci(int64_t imm)
        {
            unsigned i = addOp(ops::lci, Op::_ptr); ops[i].i64 = imm; return i;
        }

        unsigned lcu(uint64_t imm)  // unsigned alias to lci
        {
            unsigned i = addOp(ops::lci, Op::_ptr); ops[i].u64 = imm; return i;
        }

        unsigned lcf(double imm)
        {
            unsigned i = addOp(ops::lcf, Op::_f64); ops[i].f64 = imm; return i;
        }

        // JUMPS:
        void jmp(unsigned label)
        {
            unsigned i = addOp(ops::jmp, Op::_none); ops[i].label[0] = label;

            // add phi source
            assert(label < blocks.size());
            assert(env.size() == blocks[label].args.size());
            
            auto & args = blocks[label].args;
            for(int a = 0; a < env.size(); ++a)
            {
                assert(ops[args[a].phiop].flags.type == ops[env[a]].flags.type);
                args[a].add(env[a], currentBlock);
            }
        }

        void jz(unsigned v, unsigned labelThen, unsigned labelElse)
        {
            unsigned i = addOp(ops::jz, Op::_none);
            ops[i].in[0] = v;
            ops[i].label[0] = labelThen;
            ops[i].label[1] = labelElse;
            
            // add phi source
            assert(labelThen < blocks.size());
            assert(labelElse < blocks.size());
            assert(env.size() == blocks[labelThen].args.size());
            assert(env.size() == blocks[labelElse].args.size());
            
            auto & aThen = blocks[labelThen].args;
            auto & aElse = blocks[labelElse].args;
            for(int a = 0; a < env.size(); ++a)
            {
                assert(ops[aThen[a].phiop].flags.type == ops[env[a]].flags.type);
                assert(ops[aElse[a].phiop].flags.type == ops[env[a]].flags.type);
                aThen[a].add(env[a], currentBlock);
                aElse[a].add(env[a], currentBlock);
            }
        }

        // integer return
        void iret(unsigned v)
        { unsigned i = addOp(ops::iret, Op::_none); ops[i].in[0] = v; }

        // float return
        void fret(unsigned v)
        { unsigned i = addOp(ops::fret, Op::_none); ops[i].in[0] = v; }

        // indirect call to a function that returns an integer
        // the last 'n' values from 'env' are passed as parameters
        //
        // NOTE: we expect the parameters left-to-right array order
        // such that env.back() is the last parameter
        unsigned icallp(unsigned ptr, unsigned n)
        {
            nPassInt     = 0;
            nPassFloat   = 0;
            nPassTotal   = 0;
            
            // must do left-to-right if we want to count on the fly
            // might change this eventually
            for(int i = 0; i<n; ++i) passArg(env[env.size()-n+i]);

            unsigned i = addOp(ops::icallp, Op::_ptr);
            ops[i].in[0] = ptr;
            return i;
        }

        // same as icallp but functions returning floats
        unsigned fcallp(unsigned ptr, unsigned n)
        {
            nPassInt     = 0;
            nPassFloat   = 0;
            nPassTotal   = 0;
            
            // We probably want right-to-left once we do stack?
            for(int i = 0; i<n; ++i) passArg(env[env.size()-n+i]);

            unsigned i = addOp(ops::fcallp, Op::_f64);
            ops[i].in[0] = ptr;
            return i;
        }
        
        // integer and floating-point parameters to procedure
        //
        // FIXME: for now we only support 4 total so that we can assume
        // they fit into registers and we don't need to worry about
        // calling conventions too much
        unsigned iarg()
        {
            assert(nArgsTotal < 4);    // the most that will work on Windows
            assert(!currentBlock); // must be block zero
            assert(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::farg);
                
            auto i = addOp(ops::iarg, Op::_ptr);
            ops[i].in[0] = nArgsInt++;
            ops[i].in[1] = nArgsTotal++;
            return i;
        }
        
        unsigned farg()
        {
            assert(nArgsTotal < 4);    // the most that will work on Windows
            assert(!currentBlock); // must be block zero
            assert(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::farg);
                
            auto i = addOp(ops::farg, Op::_f64);
            ops[i].in[0] = nArgsFloat++;
            ops[i].in[1] = nArgsTotal++;
            return i;
        }

#define BJIT_OP1(x,t) \
    unsigned x(unsigned v0) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; return i; }
        
#define BJIT_OP2(x,t) \
    unsigned x(unsigned v0, unsigned v1) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; ops[i].in[1] = v1; return i; }

        BJIT_OP2(ilt,_ptr); BJIT_OP2(ige,_ptr);
        BJIT_OP2(igt,_ptr); BJIT_OP2(ile,_ptr);
        BJIT_OP2(ult,_ptr); BJIT_OP2(uge,_ptr);
        BJIT_OP2(ugt,_ptr); BJIT_OP2(ule,_ptr);
        BJIT_OP2(ieq,_ptr); BJIT_OP2(ine,_ptr);

        BJIT_OP2(flt,_ptr); BJIT_OP2(fge,_ptr);
        BJIT_OP2(fgt,_ptr); BJIT_OP2(fle,_ptr);
        BJIT_OP2(feq,_ptr); BJIT_OP2(fne,_ptr);
        
        BJIT_OP2(iadd,_ptr); BJIT_OP2(isub,_ptr); BJIT_OP2(imul,_ptr);
        BJIT_OP2(idiv,_ptr); BJIT_OP2(imod,_ptr);
        BJIT_OP2(udiv,_ptr); BJIT_OP2(umod,_ptr);
        
        BJIT_OP1(ineg,_ptr); BJIT_OP1(inot,_ptr);
        
        BJIT_OP2(iand,_ptr); BJIT_OP2(ior,_ptr); BJIT_OP2(ixor,_ptr);
        BJIT_OP2(ishl,_ptr); BJIT_OP2(ishr,_ptr); BJIT_OP2(ushr,_ptr);

        BJIT_OP2(fadd,_f64); BJIT_OP2(fsub,_f64); BJIT_OP1(fneg,_f64);
        BJIT_OP2(fmul,_f64); BJIT_OP2(fdiv,_f64);

        BJIT_OP1(cf2i,_f64); BJIT_OP1(bcf2i,_f64);
        BJIT_OP1(ci2f,_ptr); BJIT_OP1(bci2f,_ptr);

        BJIT_OP1(i8,_ptr); BJIT_OP1(i16,_ptr); BJIT_OP1(i32,_ptr);
        BJIT_OP1(u8,_ptr); BJIT_OP1(u16,_ptr); BJIT_OP1(u32,_ptr);

        // loads take pointer+offset
#define BJIT_LOAD(x, t) \
    unsigned x(unsigned v0, int32_t imm32) { \
        unsigned i = addOp(ops::x, Op::t); ops[i].in[0] = v0; \
        ops[i].imm32 = imm32; return i; }

        // stores take pointer+offset and value to store
#define BJIT_STORE(x) \
    unsigned x(unsigned ptr, int32_t imm32, unsigned val) { \
        unsigned i = addOp(ops::x, Op::_none); \
        ops[i].in[0] = ptr; ops[i].in[1] = val; \
        ops[i].imm32 = imm32; return i; }

        BJIT_LOAD(li8, _ptr); BJIT_LOAD(li16, _ptr);
        BJIT_LOAD(li32, _ptr); BJIT_LOAD(li64, _ptr);
        BJIT_LOAD(lu8, _ptr); BJIT_LOAD(lu16, _ptr);
        BJIT_LOAD(lu32, _ptr); BJIT_LOAD(lf64, _f64);

        BJIT_STORE(si8); BJIT_STORE(si16); BJIT_STORE(si32); BJIT_STORE(si64);
        BJIT_STORE(sf64);

    private:
        ////////////////
        // STATE DATA //
        ////////////////

        // used to encode in[0],in[1] for incoming parameters
        int     nArgsInt    = 0;
        int     nArgsFloat  = 0;
        int     nArgsTotal  = 0;

        // used to track in[1],in[2] for outgoing arguments
        int     nPassInt    = 0;
        int     nPassFloat  = 0;
        int     nPassTotal  = 0;
        
        bool    raDone = false;
        int     nSlots = 0;     // number of slots after raDone
        
        RegMask usedRegs = 0;   // for callee saved on prolog/epilog
        
        std::vector<uint16_t>   todo;   // this is used for block todos
        std::vector<uint16_t>   live;   // live blocks, used for stuff
    
        std::vector<Block>      blocks;
        std::vector<Op>         ops;

        unsigned currentBlock;
        
        unsigned newOp(uint16_t opcode, Op::Type type)
        {
            unsigned i = ops.size();
            assert(i < 0xffff);
            ops.resize(i + 1);
            ops[i].opcode = opcode;
            ops[i].flags.type = type;
            return i;
        }
        
        unsigned addOp(uint16_t opcode, Op::Type type)
        {
            uint16_t i = newOp(opcode, type);
            blocks[currentBlock].code.push_back(i);
            return i;
        }

        void passArg(unsigned val)
        {
            unsigned i = addOp(ops::nop, ops[val].flags.type);
            // in[0] must be the real value, so it gets reg-alloc
            ops[i].in[0] = val;

            if(ops[i].flags.type == Op::_ptr)
            { ops[i].opcode = ops::ipass; ops[i].in[1] = nPassInt++; }

            if(ops[i].flags.type == Op::_f64)
            { ops[i].opcode = ops::fpass; ops[i].in[1] = nPassFloat++; }
            
            ops[i].in[2] = nPassTotal++;

            assert(ops[i].opcode != ops::nop);
        }

        // opt-ra.cpp
        void allocRegs();
        void findSCC();     // resolve stack congruence classes

        // opt-fold.cpp
        bool opt_fold();

        // opt-dce.cpp
        void opt_dce();

        // opt-dce.cpp
        // compute live-in variables, set all nUse = 0
        void livescan();

        // initializes nUse, used by livescan() and allocRegs()
        // if inOnly then only live-in variables will have nUse != 0
        void findUsesBlock(int b, bool inOnly);

        // arch-XX-emit.cpp
        void arch_emit(std::vector<uint8_t> & bytes);

        
    };

}