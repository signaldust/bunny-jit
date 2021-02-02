
#pragma once

#include <cassert>
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
        //
        // NOTE: CSE only copies this union when it moves ops
        //
        // NOTE: packing is sensitive (also FIXME: rethink this?)
        union
        {
            // 64-bit constants for load immediate
            int64_t     i64;
            uint64_t    u64;
            double      f64;
            
            // imm + two values
            struct {
                union
                {
                    float       f32;
                    
                    uint32_t    imm32;
                    uint32_t    phiIndex;
                    
                    struct  // used by arguments
                    {
                        uint16_t    indexType;
                        uint16_t    indexTotal;
                    };
                };
                // 16-bits is likely enough?
                uint16_t    in[2];
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

        uint16_t    index;  // index in ops[] array (ie. backref)
        uint16_t    block;  // block in which the op currently lives
        
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
            _f32,   // single precision float
            _f64    // double precision float
        };

        struct {
            Type    type    : 4;    // see above, packed into flags
            bool    spill   : 1;
        } flags = {};


        // regsMask returns a mask of registers that can hold this type
        RegMask     regsMask();     // in arch-XX-ops.cpp

        // regsIn, regsOut and regsLost return masks for the actual operation
        RegMask     regsIn(int i);  // in arch-XX-ops.cpp
        RegMask     regsOut();      // in arch-XX-ops.cpp
        RegMask     regsLost();     // in arch-XX-ops.cpp

        // rest are in ir-ops.cpp
        const char* strOpcode() const;
        
        unsigned    nInputs()   const;
        bool        hasOutput() const;

        bool    canCSE()        const;
        bool    canMove()       const;

        bool    hasSideFX()     const;

        bool    hasImm32()      const;
        bool    hasI64()        const;
        bool    hasF64()        const;

        void    makeNOP() { opcode = ops::nop; u64 = ~0ull; }
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
        std::vector<uint16_t>   liveout;
        std::vector<uint16_t>   comeFrom;   // which blocks we come from?

        // register state on input
        uint16_t    regsIn[regs::nregs];

        // register state on output (used for shuffling)
        uint16_t    regsOut[regs::nregs];

        // dominators
        std::vector<uint16_t>   dom;
        
        uint16_t                idom;   // immediate dominator
        uint16_t                pdom;  // immediate post-dominator

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

    struct too_many_ops {};

    struct Proc
    {
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
            currentBlock = newLabel();
            emitLabel(currentBlock);

            // front-ends can use the invariant this is always 0
            alloc(allocBytes);

            if(args) for(;*args;++args)
            {
                switch(*args)
                {
                case 'i': env.push_back(iarg()); break;
                case 'd': env.push_back(darg()); break;
                default: assert(false);
                }
            }
        }

        // sanity.cpp: checks internal invariants
        void sanity();

        // debug.cpp
        void debug() const;
        void debugOp(uint16_t index) const;
        const char * regName(int r) const;

        void opt()
        {
            // do DCE first, then fold
            // repeat until neither does progress
            do opt_dce(); while(opt_fold() || opt_sink());
        }

        void compile(std::vector<uint8_t> & bytes)
        {
            allocRegs();
            arch_emit(bytes);
        }

        std::vector<unsigned>   env;

        // generate a label
        unsigned newLabel()
        {
            unsigned label = blocks.size();
            assert(label < noVal);
            
            blocks.resize(label + 1);
            
            blocks[label].args.resize(env.size());

            // generate phis
            for(int i = 0; i < env.size(); ++i)
            {
                auto phi = addOp(ops::phi, ops[env[i]].flags.type, label);
                blocks[label].args[i].phiop = phi;
                ops[phi].phiIndex = i;
            }
            
            return label;
        }

        void emitLabel(unsigned label)
        {
            assert(label < blocks.size());
            // use live-flag to enforce "emit once"
            assert(!blocks[label].flags.live);
            blocks[label].flags.live = true;
            currentBlock = label;

            env.resize(blocks[label].args.size());
            for(int i = 0; i < env.size(); ++i)
            {
                env[i] = blocks[label].args[i].phiop;
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

        unsigned lcf(float imm)
        {
            unsigned i = addOp(ops::lcd, Op::_f32); ops[i].f32 = imm; return i;
        }
        
        unsigned lcd(double imm)
        {
            unsigned i = addOp(ops::lcd, Op::_f64); ops[i].f64 = imm; return i;
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
        void dret(unsigned v)
        { unsigned i = addOp(ops::dret, Op::_none); ops[i].in[0] = v; }

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
        unsigned dcallp(unsigned ptr, unsigned n)
        {
            nPassInt     = 0;
            nPassFloat   = 0;
            nPassTotal   = 0;
            
            // We probably want right-to-left once we do stack?
            for(int i = 0; i<n; ++i) passArg(env[env.size()-n+i]);

            unsigned i = addOp(ops::dcallp, Op::_f64);
            ops[i].in[0] = ptr;
            return i;
        }

        // same as icallp/fcallp but tail-call: does not return
        void tcallp(unsigned ptr, unsigned n)
        {
            nPassInt     = 0;
            nPassFloat   = 0;
            nPassTotal   = 0;
            
            // We probably want right-to-left once we do stack?
            for(int i = 0; i<n; ++i) passArg(env[env.size()-n+i]);

            unsigned i = addOp(ops::tcallp, Op::_none);
            ops[i].in[0] = ptr;
        }
        
        
#define BJIT_OP1(x,t,t0) \
    unsigned x(unsigned v0) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; assert(ops[v0].flags.type==Op::t0); return i; }
        
#define BJIT_OP2(x,t,t0,t1) \
    unsigned x(unsigned v0, unsigned v1) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; assert(ops[v0].flags.type==Op::t0); \
        ops[i].in[1] = v1; assert(ops[v1].flags.type==Op::t1); return i; }

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

        BJIT_OP2(dadd,_f64,_f64,_f64); BJIT_OP2(dsub,_f64,_f64,_f64);
        BJIT_OP1(dneg,_f64,_f64);
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
    unsigned x(unsigned v0, int32_t imm32) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; assert(ops[v0].flags.type == Op::_ptr); \
        ops[i].imm32 = imm32; return i; }

        // stores take pointer+offset and value to store
#define BJIT_STORE(x, t) \
    unsigned x(unsigned ptr, int32_t imm32, unsigned val) { \
        unsigned i = addOp(ops::x, Op::_none); \
        ops[i].in[0] = ptr; assert(ops[ptr].flags.type == Op::_ptr); \
        ops[i].in[1] = val; assert(ops[val].flags.type == Op::t); \
        ops[i].imm32 = imm32; return i; }

        BJIT_LOAD(li8, _ptr); BJIT_LOAD(li16, _ptr);
        BJIT_LOAD(li32, _ptr); BJIT_LOAD(li64, _ptr);
        BJIT_LOAD(lu8, _ptr); BJIT_LOAD(lu16, _ptr);
        BJIT_LOAD(lu32, _ptr); BJIT_LOAD(lf64, _f64);

        BJIT_STORE(si8, _ptr); BJIT_STORE(si16, _ptr);
        BJIT_STORE(si32, _ptr); BJIT_STORE(si64, _ptr);
        BJIT_STORE(sf64, _f64);

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

        // used to break critical edges, returns the new block
        // tries to fix most info, but not necessarily all
        uint16_t breakEdge(uint16_t from, uint16_t to)
        {
            printf(" BCE[%d,%d]", from, to);
            uint16_t b = blocks.size();
            blocks.resize(blocks.size() + 1);

            blocks[b].comeFrom.push_back(from);
            auto & jmp = ops[addOp(ops::jmp, Op::_none, b)];
            jmp.label[0] = to;

            // fix live-in for edge block
            blocks[b].livein = blocks[to].livein;

            // fix doms, don't care about pdoms
            blocks[b].dom = blocks[from].dom;
            blocks[b].dom.push_back(b);

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
            for(auto & a : blocks[to].args)
            for(auto & s : a.alts) if(s.src == from) s.src = b;

            return b;
        }
        
        unsigned newOp(uint16_t opcode, Op::Type type, uint16_t block)
        {
#if !defined(__cpp_exceptions) && !defined(_CPPUNWIND)
            assert(ops.size() < noVal);
#else
            if(ops.size() == noVal) throw too_many_ops();
#endif
            unsigned i = ops.size();
            
            ops.resize(i + 1);
            ops[i].opcode = opcode;
            ops[i].in[0] = noVal;
            ops[i].in[1] = noVal;
            ops[i].flags.type = type;
            ops[i].index = i;
            ops[i].block = block;
            return i;
        }
        
        unsigned addOp(uint16_t opcode, Op::Type type, uint16_t inBlock = noVal)
        {
            if(inBlock == noVal) inBlock = currentBlock;
            
            uint16_t i = newOp(opcode, type, inBlock);
            blocks[inBlock].code.push_back(i);
            return i;
        }

        void passArg(unsigned val)
        {
            unsigned i = addOp(ops::nop, ops[val].flags.type);
            ops[i].in[0] = val;

            if(ops[i].flags.type == Op::_ptr)
            { ops[i].opcode = ops::ipass; ops[i].indexType = nPassInt++; }

            if(ops[i].flags.type == Op::_f64)
            { ops[i].opcode = ops::dpass; ops[i].indexType = nPassFloat++; }
            
            ops[i].indexTotal = nPassTotal++;

            assert(ops[i].opcode != ops::nop);
        }

        // integer and floating-point parameters to procedure
        //
        // FIXME: up to 4 until we have better calling convention support
        unsigned iarg()
        {
            assert(nArgsTotal < 4);    // the most that will work on Windows
            assert(!currentBlock); // must be block zero
            assert(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::alloc
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::darg);
                
            auto i = addOp(ops::iarg, Op::_ptr);
            ops[i].indexType = nArgsInt++;
            ops[i].indexTotal = nArgsTotal++;
            return i;
        }
        unsigned darg()
        {
            assert(nArgsTotal < 4);    // the most that will work on Windows
            assert(!currentBlock); // must be block zero
            assert(!blocks[0].code.size()
                || ops[blocks[0].code.back()].opcode == ops::alloc
                || ops[blocks[0].code.back()].opcode == ops::iarg
                || ops[blocks[0].code.back()].opcode == ops::darg);
                
            auto i = addOp(ops::darg, Op::_f64);
            ops[i].indexType = nArgsFloat++;
            ops[i].indexTotal = nArgsTotal++;
            return i;
        }

        // user-requested stack frame
        unsigned alloc(unsigned size)
        {
            auto i = addOp(ops::alloc, Op::_ptr);
            ops[i].imm32 = size;
            return i;
        }

        // opt-ra.cpp
        void allocRegs();
        void findSCC();     // resolve stack congruence classes

        // opt-fold.cpp
        bool opt_fold();

        // opt-sink.cpp
        bool opt_sink();

        // opt-dce.cpp
        void opt_dce();

        // opt-dom.cpp - used by opt-dce()
        void opt_dom();

        // opt-dce.cpp
        // compute live-in variables, set all nUse = 0
        void livescan();

        // initializes nUse, used by livescan() and allocRegs()
        // if inOnly then only live-in variables will have nUse != 0
        void findUsesBlock(int b, bool inOnly);

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
        // returns true on success
        //
        // use getProcPtr() to get pointers to procedures
        bool load();

        // unload module from executable memory
        void unload();

        // returns the address of a proc in executable memory
        template <typename T>
        T * getPointer(int index)
        {
            void    *vptr = offsets[index] + (uint8_t*)exec_mem;
            return reinterpret_cast<T*&>(vptr);
        }

        // returns Proc index
        int compile(Proc & proc)
        {
            assert(!exec_mem);
            
            int index = offsets.size();
            offsets.push_back(bytes.size());
            
            proc.compile(bytes);

            return index;
        }

        const std::vector<uint8_t> & getBytes() const { return bytes; }
        
    private:
        std::vector<uint32_t>   offsets;
        std::vector<uint8_t>    bytes;
        void    *exec_mem = 0;

    };

}