
#pragma once

#include <cstdint>
#include <vector>

// If BJIT_NO_ASSERT is defined, we disable ALL error checking.
#ifdef BJIT_NO_ASSERT
#  define BJIT_ASSERT_T(x,y)    do{if(x);}while(0)
#endif

// Otherwise assert() if exceptions are disabled, otherwise throw.
#ifndef BJIT_ASSERT_T
# if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
#  include <cassert>
#  define BJIT_ASSERT_T(x,y)    assert(x)
# else
#  define BJIT_ASSERT_T(x,y)    do{if(x); else throw y();}while(0)
# endif
#endif
#define BJIT_ASSERT(x)          BJIT_ASSERT_T(x, bjit::internal_error);

// This is used for some checks that are borderline paranoid
#define BJIT_ASSERT_MORE(x)     BJIT_ASSERT(x)

#ifndef BJIT_LOG
#  include <cstdio>
#  define BJIT_LOG(...)     fprintf(stderr, __VA_ARGS__)
#endif

namespace bjit
{
    struct too_many_ops {};
    struct internal_error {};
};

#include "bjit-impl.h"

namespace bjit
{
    // These add a bit of type-safety to the interface only.
    //
    struct Value { uint16_t index; };
    struct Label { uint16_t index; };

    struct Proc
    {
        // These are used everywhere, so import them into Proc
        typedef impl::Op        Op;
        typedef impl::OpCSE     OpCSE;
        typedef impl::Block     Block;
        typedef impl::NearReloc NearReloc;
        
        // allocBytes is the size of an optional block allocated from the stack
        // for function local data (eg. arrays, variables with address taken)
        // the SSA value 0 will always be the pointer to this block
        //
        // args is string describing argument: 'i' for int, 'f' for double
        //  - env[0..n] are the SSA values for the arguments
        //
        Proc(unsigned allocBytes, const char * args)
        {
            // reserve space for maximum number of ops
            // NOTE: opt-ra and opt-fold assume we don't realloc
            ops.reserve(noVal);
            currentBlock = newLabel().index;
            emitLabel(Label{currentBlock});

            // front-ends can use the invariant this is always SSA value 0
            alloc(allocBytes);

            if(args) for(;*args;++args)
            {
                switch(*args)
                {
                case 'i': env.push_back(iarg()); break;
                case 'f': env.push_back(farg()); break;
                case 'd': env.push_back(darg()); break;
                default: BJIT_ASSERT(false);
                }
            }
        }

        // sanity.cpp: checks internal invariants
        void sanity();

        // debug.cpp
        void debug() const;
        void debugOp(uint16_t index) const;
        const char * regName(int r) const;

        // Compiles code into 'bytes' (does not truncate).
        //
        // Takes optimization level:
        //  - 0: DCE only
        //  - 1: "safe" optimizations only (default)
        //  - 2: "unsafe" optimizations also (eg. fast-math, no div-by-zero)
        //
        // Consider using Module::compile() instead
        void compile(std::vector<uint8_t> & bytes, unsigned levelOpt)
        {
            bool unsafeOpt = levelOpt > 1;
            
            if(levelOpt) opt(unsafeOpt);
            
            allocRegs(unsafeOpt);
            arch_emit(bytes);
        }

        std::vector<Value>  env;

        // generate a label
        Label newLabel()
        {
            BJIT_ASSERT(blocks.size() < noVal);
            uint16_t label = blocks.size();
            
            blocks.resize(label + 1);
            
            blocks[label].args.reserve(env.size());

            // generate phis
            for(int i = 0; i < env.size(); ++i)
            {
                auto phi = addOp(ops::phi, ops[env[i].index].flags.type, label);
                blocks[label].args.push_back(phi);
                ops[phi].phiIndex = i;
                ops[phi].iv = noVal;
            }
            
            return Label{label};
        }

        void emitLabel(Label label)
        {
            BJIT_ASSERT(label.index < blocks.size());
            // use live-flag to enforce "emit once"
            BJIT_ASSERT(!blocks[label.index].flags.live);
            blocks[label.index].flags.live = true;
            currentBlock = label.index;

            env.resize(blocks[label.index].args.size());
            for(int i = 0; i < env.size(); ++i)
            {
                env[i].index = blocks[label.index].args[i].phiop;
            }
        }

        // used by Module
        std::vector<NearReloc> const & getReloc()
        {
            return nearReloc;
        }

        ///////////////////////////////
        // FRONT END OPCODE EMITTERS //
        ///////////////////////////////

        // CONSTANTS
        Value lci(int64_t imm)
        {
            auto i = addOp(ops::lci, Op::_ptr); ops[i].i64 = imm; return Value{i};
        }

        Value lcu(uint64_t imm)  // unsigned alias to lci
        {
            auto i = addOp(ops::lci, Op::_ptr); ops[i].u64 = imm; return Value{i};
        }

        Value lcf(float imm)
        {
            auto i = addOp(ops::lcd, Op::_f32); ops[i].f32 = imm; return Value{i};
        }
        
        Value lcd(double imm)
        {
            auto i = addOp(ops::lcd, Op::_f64); ops[i].f64 = imm; return Value{i};
        }

        Value lnp(uint32_t idx)
        {
            auto i = addOp(ops::lnp, Op::_ptr); ops[i].imm32 = idx; return Value{i};
        }
        
        // JUMPS:
        void jmp(Label label)
        {
            auto i = addOp(ops::jmp, Op::_none); ops[i].label[0] = label.index;

            // add phi source
            BJIT_ASSERT(label.index < blocks.size());
            BJIT_ASSERT(env.size() == blocks[label.index].args.size());
            
            auto & args = blocks[label.index].args;
            for(int a = 0; a < env.size(); ++a)
            {
                BJIT_ASSERT(ops[args[a].phiop].flags.type
                    == ops[env[a].index].flags.type);
                blocks[label.index].newAlt(
                    args[a].phiop, currentBlock, env[a].index);
            }
        }

        // write this as wrapper so we don't need to duplicate code
        void jnz(Value v, Label labelThen, Label labelElse)
        {
            jz(v, labelElse, labelThen);
        }

        void jz(Value v, Label labelThen, Label labelElse)
        {
            auto i = addOp(ops::jz, Op::_none);
            ops[i].in[0] = v.index;
            ops[i].label[0] = labelThen.index;
            ops[i].label[1] = labelElse.index;
            
            // add phi source
            BJIT_ASSERT(labelThen.index < blocks.size());
            BJIT_ASSERT(labelElse.index < blocks.size());
            BJIT_ASSERT(env.size() == blocks[labelThen.index].args.size());
            BJIT_ASSERT(env.size() == blocks[labelElse.index].args.size());
            
            auto & aThen = blocks[labelThen.index].args;
            auto & aElse = blocks[labelElse.index].args;
            for(int a = 0; a < env.size(); ++a)
            {
                BJIT_ASSERT(ops[aThen[a].phiop].flags.type
                    == ops[env[a].index].flags.type);
                BJIT_ASSERT(ops[aElse[a].phiop].flags.type
                    == ops[env[a].index].flags.type);
                blocks[labelThen.index].newAlt(
                    aThen[a].phiop, currentBlock, env[a].index);
                blocks[labelElse.index].newAlt(
                    aElse[a].phiop, currentBlock, env[a].index);
            }
        }

        // integer return
        void iret(Value v)
        { auto i = addOp(ops::iret, Op::_none); ops[i].in[0] = v.index; }

        // float return
        void fret(Value v)
        { auto i = addOp(ops::fret, Op::_none); ops[i].in[0] = v.index; }
        
        // float return
        void dret(Value v)
        { auto i = addOp(ops::dret, Op::_none); ops[i].in[0] = v.index; }

        // indirect call to a function that returns an integer
        // the last 'n' values from 'env' are passed as parameters
        //
        // NOTE: we expect the parameters left-to-right array order
        // such that env.back() is the last parameter
        Value icallp(Value ptr, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::icallp, Op::_ptr);
            ops[i].in[0] = ptr.index;
            return Value{i};
        }

        // near call version
        // first argument is (immediate) index into module-table
        Value icalln(int index, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::icalln, Op::_ptr);
            ops[i].imm32 = index;
            return Value{i};
        }

        // same as icallp but functions returning floats
        Value fcallp(Value ptr, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::fcallp, Op::_f32);
            ops[i].in[0] = ptr.index;
            return Value{i};
        }

        // near-call version
        Value fcalln(int index, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::fcalln, Op::_f32);
            ops[i].imm32 = index;
            return Value{i};
        }
        
        // same as icallp but functions returning doubles
        Value dcallp(Value ptr, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::dcallp, Op::_f64);
            ops[i].in[0] = ptr.index;
            return Value{i};
        }

        // near-call version
        Value dcalln(int index, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::dcalln, Op::_f64);
            ops[i].imm32 = index;
            return Value{i};
        }

        // same as icallp/fcallp but tail-call: does not return
        void tcallp(Value ptr, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::tcallp, Op::_none);
            ops[i].in[0] = ptr.index;
        }

        // near-call version
        void tcalln(int index, unsigned n)
        {
            passArgs(n);
            auto i = addOp(ops::tcalln, Op::_none);
            ops[i].imm32 = index;
        }
        
        
#define BJIT_OP1(x,t,t0) \
    Value x(Value v0) { \
        auto i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0.index; BJIT_ASSERT(ops[v0.index].flags.type==Op::t0); \
        return Value{i}; }
        
#define BJIT_OP2(x,t,t0,t1) \
    Value x(Value v0, Value v1) { \
        auto i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0.index; BJIT_ASSERT(ops[v0.index].flags.type==Op::t0); \
        ops[i].in[1] = v1.index; BJIT_ASSERT(ops[v1.index].flags.type==Op::t1); \
        return Value{i}; }

        BJIT_OP2(ilt,_ptr,_ptr,_ptr); BJIT_OP2(ige,_ptr,_ptr,_ptr);
        BJIT_OP2(igt,_ptr,_ptr,_ptr); BJIT_OP2(ile,_ptr,_ptr,_ptr);
        BJIT_OP2(ult,_ptr,_ptr,_ptr); BJIT_OP2(uge,_ptr,_ptr,_ptr);
        BJIT_OP2(ugt,_ptr,_ptr,_ptr); BJIT_OP2(ule,_ptr,_ptr,_ptr);
        BJIT_OP2(ieq,_ptr,_ptr,_ptr); BJIT_OP2(ine,_ptr,_ptr,_ptr);

        BJIT_OP2(flt,_ptr,_f32,_f32); BJIT_OP2(fge,_ptr,_f32,_f32);
        BJIT_OP2(fgt,_ptr,_f32,_f32); BJIT_OP2(fle,_ptr,_f32,_f32);
        BJIT_OP2(feq,_ptr,_f32,_f32); BJIT_OP2(fne,_ptr,_f32,_f32);
        
        BJIT_OP2(dlt,_ptr,_f64,_f64); BJIT_OP2(dge,_ptr,_f64,_f64);
        BJIT_OP2(dgt,_ptr,_f64,_f64); BJIT_OP2(dle,_ptr,_f64,_f64);
        BJIT_OP2(deq,_ptr,_f64,_f64); BJIT_OP2(dne,_ptr,_f64,_f64);
        
        BJIT_OP2(iadd,_ptr,_ptr,_ptr); BJIT_OP2(isub,_ptr,_ptr,_ptr);
        BJIT_OP2(imul,_ptr,_ptr,_ptr);
        BJIT_OP2(idiv,_ptr,_ptr,_ptr); BJIT_OP2(imod,_ptr,_ptr,_ptr);
        BJIT_OP2(udiv,_ptr,_ptr,_ptr); BJIT_OP2(umod,_ptr,_ptr,_ptr);
        
        BJIT_OP1(ineg,_ptr,_ptr); BJIT_OP1(inot,_ptr,_ptr);
        
        BJIT_OP2(iand,_ptr,_ptr,_ptr); BJIT_OP2(ior,_ptr,_ptr,_ptr);
        BJIT_OP2(ixor,_ptr,_ptr,_ptr); BJIT_OP2(ishl,_ptr,_ptr,_ptr);
        BJIT_OP2(ishr,_ptr,_ptr,_ptr); BJIT_OP2(ushr,_ptr,_ptr,_ptr);

        BJIT_OP2(fadd,_f32,_f32,_f32); BJIT_OP2(fsub,_f32,_f32,_f32);
        BJIT_OP1(fneg,_f32,_f32); BJIT_OP1(fabs,_f32,_f32);
        BJIT_OP2(fmul,_f32,_f32,_f32); BJIT_OP2(fdiv,_f32,_f32,_f32);

        BJIT_OP2(dadd,_f64,_f64,_f64); BJIT_OP2(dsub,_f64,_f64,_f64);
        BJIT_OP1(dneg,_f64,_f64); BJIT_OP1(dabs,_f64,_f64);
        BJIT_OP2(dmul,_f64,_f64,_f64); BJIT_OP2(ddiv,_f64,_f64,_f64);

        BJIT_OP1(cd2i,_ptr,_f64); BJIT_OP1(bcd2i,_ptr,_f64);
        BJIT_OP1(ci2d,_f64,_ptr); BJIT_OP1(bci2d,_f64,_ptr);

        BJIT_OP1(cf2d,_f64,_f32); BJIT_OP1(cd2f,_f32,_f64);
        
        BJIT_OP1(cf2i,_ptr,_f32); BJIT_OP1(bcf2i,_ptr,_f32);
        BJIT_OP1(ci2f,_f32,_ptr); BJIT_OP1(bci2f,_f32,_ptr);

        BJIT_OP1(i8,_ptr,_ptr); BJIT_OP1(i16,_ptr,_ptr); BJIT_OP1(i32,_ptr,_ptr);
        BJIT_OP1(u8,_ptr,_ptr); BJIT_OP1(u16,_ptr,_ptr); BJIT_OP1(u32,_ptr,_ptr);

        // loads take pointer+offset
#define BJIT_LOAD(x, t) \
    Value x(Value v0, uint16_t off16) { \
        auto i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0.index; BJIT_ASSERT(ops[v0.index].flags.type == Op::_ptr); \
        ops[i].off16 = off16; ops[i].memtag = noVal; return Value{i}; }

        // stores take pointer+offset and value to store
#define BJIT_STORE(x, t) \
    void x(Value val, Value ptr, uint16_t off16) { \
        auto i = addOp(ops::x, Op::_none); \
        ops[i].in[0] = val.index; BJIT_ASSERT(ops[val.index].flags.type == Op::t); \
        ops[i].in[1] = ptr.index; BJIT_ASSERT(ops[ptr.index].flags.type == Op::_ptr); \
        ops[i].off16 = off16; }

        BJIT_LOAD(li8, _ptr); BJIT_LOAD(li16, _ptr);
        BJIT_LOAD(li32, _ptr); BJIT_LOAD(li64, _ptr);
        BJIT_LOAD(lu8, _ptr); BJIT_LOAD(lu16, _ptr);
        BJIT_LOAD(lu32, _ptr);
        BJIT_LOAD(lf32, _f32); BJIT_LOAD(lf64, _f64);

        BJIT_STORE(si8, _ptr); BJIT_STORE(si16, _ptr);
        BJIT_STORE(si32, _ptr); BJIT_STORE(si64, _ptr);
        BJIT_STORE(sf32, _f32); BJIT_STORE(sf64, _f64);

        void fence() { addOp(ops::fence, Op::_none); }

    private:
        ////////////////
        // STATE DATA //
        ////////////////

        // this are filled in for icalln/fcalln/pcalln
        // and possibly something else in the future
        std::vector<NearReloc>  nearReloc;

        // used to encode indexType, indexTotal for incoming parameters
        int     nArgsInt    = 0;
        int     nArgsFloat  = 0;
        int     nArgsTotal  = 0;

        // used to track indexType, indexTotal for outgoing arguments
        int     nPassInt    = 0;
        int     nPassFloat  = 0;
        int     nPassTotal  = 0;
        
        bool    raDone = false;
        int     nSlots = 0;     // number of slots after raDone

        int     liveOps = 0;    // collect count in DCE
        
        RegMask usedRegs = 0;   // for callee saved on prolog/epilog

        HashTable<OpCSE>        cseTable;
        
        std::vector<uint16_t>   todo;   // this is used for block todos
        std::vector<uint16_t>   live;   // live blocks, used for stuff
    
        std::vector<Block>      blocks;
        std::vector<Op>         ops;

        uint16_t    getOpIndex(Op & op)
        {
            // this is somewhat ugly, but saves us a field in Op
            auto i = (uintptr_t(&op) - uintptr_t(ops.data())) / sizeof(Op);
            BJIT_ASSERT_MORE(i == (uint16_t) i);
            return (uint16_t) i;
        }

        uint16_t    currentBlock;

        void opt(bool unsafeOpt = false)
        {
            // check sanity limit (eg. don't let tests hang)
            int iterOpt = 0;

            // do we want to repeat?
            bool repeat = true;
            opt_dce(unsafeOpt);
            
            while(repeat)
            {
                BJIT_ASSERT(++iterOpt < 0x100);

                repeat = false;
                
                // do fold
                if(opt_fold(unsafeOpt)) repeat = true;
                
                // if we only made progress, then cleanup
                if(repeat) opt_dce(unsafeOpt);

                // reassoc pass (tends to improve CSE)
                if(opt_reassoc(unsafeOpt)) repeat = true;

                // then do CSE
                if(opt_cse(unsafeOpt)) repeat = true;

                // if we only made progress, then cleanup
                if(repeat) opt_dce(unsafeOpt);

                // check jumps, only does at most one at a time
                while(opt_jump()) { repeat = true; }
            }

            // this should not currently enable further optimization
            // so iterating the rest afterwards is wasted CPU
            opt_sink(unsafeOpt);
        }

        // used to break critical edges, returns the new block
        // tries to fix most info, but not necessarily all
        uint16_t breakEdge(uint16_t from, uint16_t to)
        {
            uint16_t b = blocks.size();
            BJIT_LOG(" BCE[%d:%d:%d]", from, b, to);
            blocks.resize(blocks.size() + 1);

            blocks[b].comeFrom.push_back(from);
            auto & jmp = ops[addOp(ops::jmp, Op::_none, b)];
            jmp.label[0] = to;

            // if original jump is no-opt then also mark
            // the new jump as no_opt so we don't try to
            // reoptimize loops when sink breaks an edge
            if(ops[blocks[from].code.back()].flags.no_opt)
                jmp.flags.no_opt = true;

            // fix live-in for edge block
            blocks[b].livein = blocks[to].livein;

            // fix doms, don't care about pdoms
            blocks[b].dom = blocks[from].dom;
            blocks[b].dom.push_back(b);

            // fix memtags (eg. CSE/RA)
            blocks[b].memtag = blocks[from].memout;
            blocks[b].memout = blocks[from].memout;

            blocks[b].idom = from;
            blocks[b].pdom = to;
            blocks[b].flags.live = true;
            live.push_back(b);

            if(blocks[to].idom == from)
            {
                blocks[to].idom = b;
                blocks[to].dom.back() = b;
                blocks[to].dom.push_back(to);
            }

            if(blocks[from].pdom == to) blocks[from].pdom = b;

            // fix target comeFrom
            for(auto & cf : blocks[to].comeFrom) if(cf == from) cf = b;

            // for target phis
            for(auto & s : blocks[to].alts)
            {
                if(s.src == from)
                {
                    s.src = b;
                    // value must be live-in :)
                    blocks[b].livein.push_back(s.val);
                }
            }

            return b;
        }
        
        uint16_t newOp(uint16_t opcode, Op::Type type, uint16_t block)
        {
            BJIT_ASSERT_T(ops.size() < noVal, too_many_ops);
            
            uint16_t i = ops.size();
            
            ops.resize(i + 1);
            ops[i].opcode = opcode;
            ops[i].in[0] = noVal;
            ops[i].in[1] = noVal;
            ops[i].scc = noSCC;
            ops[i].flags.type = type;
            ops[i].block = block;
            return i;
        }
        
        uint16_t addOp(uint16_t opcode, Op::Type type, uint16_t inBlock = noVal)
        {
            if(inBlock == noVal) inBlock = currentBlock;
            
            uint16_t i = newOp(opcode, type, inBlock);
            blocks[inBlock].code.push_back(i);
            return i;
        }

        void passArgs(unsigned n)
        {
            nPassInt     = 0;
            nPassFloat   = 0;
            nPassTotal   = 0;
            
            // We probably want right-to-left once we do stack?
            for(int i = 0; i<n; ++i) passNextArg(env[env.size()-n+i].index);
        }

        void passNextArg(unsigned val)
        {
            unsigned i = addOp(ops::nop, ops[val].flags.type);
            ops[i].in[0] = val;

            if(ops[i].flags.type == Op::_ptr)
            { ops[i].opcode = ops::ipass; ops[i].indexType = nPassInt++; }

            if(ops[i].flags.type == Op::_f64)
            { ops[i].opcode = ops::dpass; ops[i].indexType = nPassFloat++; }
            
            if(ops[i].flags.type == Op::_f32)
            { ops[i].opcode = ops::fpass; ops[i].indexType = nPassFloat++; }
            
            ops[i].indexTotal = nPassTotal++;

            BJIT_ASSERT(ops[i].opcode != ops::nop);
        }

        // integer and floating-point parameters to procedure
        //
        // FIXME: up to 4 until we have better calling convention support
        Value iarg()
        {
            BJIT_ASSERT(nArgsTotal < 4);    // the most that will work on Windows
            BJIT_ASSERT(!currentBlock); // must be block zero
            BJIT_ASSERT(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::alloc
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::farg
                || ops[blocks[0].code.back()].opcode == ops::darg);
                
            auto i = addOp(ops::iarg, Op::_ptr);
            ops[i].indexType = nArgsInt++;
            ops[i].indexTotal = nArgsTotal++;
            return Value{i};
        }

        Value farg()
        {
            BJIT_ASSERT(nArgsTotal < 4);    // the most that will work on Windows
            BJIT_ASSERT(!currentBlock); // must be block zero
            BJIT_ASSERT(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::alloc
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::farg
                || ops[blocks[0].code.back()].opcode == ops::darg);
                
            auto i = addOp(ops::farg, Op::_f32);
            ops[i].indexType = nArgsFloat++;
            ops[i].indexTotal = nArgsTotal++;
            return Value{i};
        }
        
        Value darg()
        {
            BJIT_ASSERT(nArgsTotal < 4);    // the most that will work on Windows
            BJIT_ASSERT(!currentBlock); // must be block zero
            BJIT_ASSERT(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::alloc
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::farg
                || ops[blocks[0].code.back()].opcode == ops::darg);
                
            auto i = addOp(ops::darg, Op::_f64);
            ops[i].indexType = nArgsFloat++;
            ops[i].indexTotal = nArgsTotal++;
            return {i};
        }

        // user-requested stack frame
        Value alloc(unsigned size)
        {
            auto i = addOp(ops::alloc, Op::_ptr);
            ops[i].imm32 = size;
            return Value{i};
        }

        // opt-ra.cpp
        void allocRegs(bool unsafeOpt);
        void findSCC();         // resolve stack congruence classes
        void findUsedRegs();    // usedRegs post final DCE

        // opt-fold.cpp
        bool opt_fold(bool unsafeOpt);
        bool opt_reassoc(bool unsafeOpt);

        // opt-jump.cpp
        bool opt_jump_be(uint16_t b);
        bool opt_jump();
        void find_ivs();

        // opt-cse.cpp
        void rebuild_memtags(bool unsafeOpt);
        bool opt_cse(bool unsafeOpt);

        // opt-sink.cpp
        bool opt_sink(bool unsafeOpt);

        // opt-dce.cpp
        void opt_dce(bool unsafeOpt = false);

        // opt-dom.cpp - used by opt-dce()
        void rebuild_cfg();
        void rebuild_dom();

        // opt-dce.cpp
        // compute live-in variables, set all nUse = 0
        void rebuild_livein();

        // initializes nUse, used by livescan() and allocRegs()
        // if inOnly then only live-in variables will have nUse != 0
        void findUsesBlock(int b, bool inOnly, bool localOnly);

        // arch-XX-emit.cpp
        void arch_emit(std::vector<uint8_t> & bytes);
    };

    // This will eventually become a proper module linking class.
    // For now it handles loading code into executable memory.
    //
    // Note that we don't allow compiling additional Procs while
    // the module is loaded, but we DO allow compiling them again
    // if the module is temporarily unloaded.
    struct Module
    {
        ~Module() { if(exec_mem) unload(); }

        // load compiled procedures into executable memory
        //
        // returns address of the allocated block on success
        // returns zero on failure (system error or unsupported platform)
        //
        // use getPointer() to get pointers to procedures
        //
        // load() always allocates enough executable memory to load
        // the module, but always at least mmapSizeMin bytes, see patch()
        uintptr_t load(unsigned mmapSizeMin = 0);

        // attempt to patch changes to a currently loaded module
        //
        // a module can be patched if any additional code fits into
        // the allocated block, or if only stub-targets have changed
        //
        // returns true on success
        //
        // patch() will temporarily adjust memory access to read-write
        // and no-execute, and should not be called while another thread
        // is executing code in the module, but patching a module with
        // suspended stack-frames (callers or another thread) is fine
        // as patch() will never attempt to move the module
        //
        // if the function fails because the module cannot be patched,
        // then patch will not adjust the executable memory in any way
        // (ie. if code doesn't fit, it won't touch stubs either)
        //
        // in this case you need to unload() and load() the module again
        // (and patch any pending return addresses in the stack yourself;
        // note that unload() and load() will give you the needed offsets)
        //
        // currently patch() will BJIT_ASSERT() if a system error occurs when
        // trying to change memory permissions (in either direction)
        //
        bool patch();

        // returns true if module is currently loaded
        bool isLoaded() { return 0 != exec_mem; }

        // unload module from executable memory
        //
        // returns the block that was unallocated
        uintptr_t unload();

        // returns the address of a proc in executable memory
        template <typename T>
        T * getPointer(unsigned index)
        {
            BJIT_ASSERT(exec_mem);
            BJIT_ASSERT(offsets[index] < loadSize);
            
            void    *vptr = offsets[index] + (uint8_t*)exec_mem;
            return reinterpret_cast<T*&>(vptr);
        }

        // patch a stub with new address
        // you will also need to either patch() or unload()+load()
        // the module for the changes to becomes active
        void patchStub(unsigned index, uintptr_t address)
        {
            arch_patchStub(offsets[index] + bytes.data(), address);
            
            // store this for patch() to also patch live
            if(isLoaded()) stubPatches.emplace_back(PatchStub{index, address});
        }

        // patch all calls to oldTarget to call newTarget instead
        // you will also need to either patch() or unload()+load()
        // the module for the changes to become active
        void patchCalls(unsigned oldTarget, unsigned newTarget)
        {
            if(isLoaded())
            {
                nearPatches.emplace_back(
                    PatchNear{oldTarget, newTarget,
                    0, (unsigned) bytes.size()});
            }
            else
            {
                for(auto & r : relocs)
                {
                    if(r.procIndex == oldTarget) r.procIndex = newTarget;
                }
            }
        }

        // same as patchCalls() but only patch calls in a given proc
        void patchCallsIn(unsigned inProc, unsigned oldTarget, unsigned newTarget)
        {
            unsigned rangeStart = offsets[inProc];
            unsigned rangeEnd = (inProc+1 < offsets.size())
                ? offsets[inProc+1] : (unsigned) bytes.size();
                
            if(isLoaded())
            {
                nearPatches.emplace_back(
                    PatchNear{oldTarget, newTarget, rangeStart, rangeEnd});
            }
            else
            {
                for(auto & r : relocs)
                {
                    if(r.codeOffset < rangeStart
                    || r.codeOffset >= rangeEnd) continue;
                    
                    if(r.procIndex == oldTarget) r.procIndex = newTarget;
                }
            }
        }

        // returns Proc index
        // levelOpt: 0:DCE, 1:all-safe, 2:all, see Proc::compile
        int compile(Proc & proc, unsigned levelOpt = 2)
        {
            int index = offsets.size();
            offsets.push_back(bytes.size());
            
            proc.compile(bytes, levelOpt);

            auto & procReloc = proc.getReloc();
            relocs.insert(relocs.end(), procReloc.begin(), procReloc.end());

            return index;
        }

        // compile a stub, this counts as a procedure in terms of
        // near-indexes, but only contains a jump to an external address
        int compileStub(uintptr_t address)
        {
            int index = offsets.size();
            offsets.push_back(bytes.size());

            arch_compileStub(address);
            return index;
        }

        const std::vector<uint8_t> & getBytes() const { return bytes; }
        
    private:
        typedef impl::NearReloc NearReloc;
    
        struct PatchStub
        {
            unsigned    procIndex;
            uintptr_t   newAddress;
        };
        std::vector<PatchStub>  stubPatches;

        struct PatchNear
        {
            unsigned    oldTarget;
            unsigned    newTarget;

            unsigned    offsetStart;
            unsigned    offsetEnd;
        };
        std::vector<PatchNear>  nearPatches;
        
        std::vector<NearReloc>  relocs;
        
        std::vector<uint32_t>   offsets;
        std::vector<uint8_t>    bytes;

        
        void        *exec_mem = 0;
        unsigned    loadSize = 0;
        unsigned    mmapSize = 0;

        // in arch-XX-emit.cpp
        void arch_compileStub(uintptr_t address);

        void arch_patchStub(void * ptr, uintptr_t address);
        void arch_patchNear(void * ptr, int32_t offset);

    };

}