
#pragma once

#include <cstdint>
#include <vector>
#include <cstdio>

#ifdef BJIT_NO_ASSERT
#  define BJIT_ASSERT(x)    do{}while(0)
#endif

#ifndef BJIT_ASSERT
#  include <cassert>
#  define BJIT_ASSERT(x)    assert(x)
#endif

#ifndef BJIT_LOG
#  include <cstdio>
#  define BJIT_LOG(...)     fprintf(stderr, __VA_ARGS__)
#endif

#include "hash.h"
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
        
        bool    hasF32()        const;
        bool    hasF64()        const;

        void    makeNOP() { opcode = ops::nop; u64 = ~0ull; }
    };

    static const uint16_t   noVal = 0xffff;
    static const uint16_t   noSCC = 0xffff;

    // This stores the data CSE needs in our hash table.
    // Only used by CSE, but defined here so that we can
    // allocate the hash table just once.
    struct OpCSE
    {
        // not included in hash
        uint16_t index = noVal;
        uint16_t block = noVal;
    
        // rest is hashed
        union
        {
            struct {
                uint32_t imm32 = 0;
                uint16_t in[2] = { noVal, noVal };
            };
    
            // for lci/lcf
            int64_t     i64;
        };
        uint16_t opcode = noVal;
    
        OpCSE() {}
        OpCSE(Op const & op) { set(op); }
    
        void set(Op const & op)
        {
            index = op.index;
            block = op.block;
            opcode = op.opcode;
            if(op.hasI64() || op.hasF64())
            {
                i64 = op.i64;
            }
            else
            {
                in[0] = op.nInputs() >= 1 ? op.in[0] : noVal;
                in[1] = op.nInputs() >= 2 ? op.in[1] : noVal;
                imm32 = op.hasImm32() ? op.imm32 : 0;
            }
        }
    
        // NOTE: we need temporary to force the "noVals"
        bool isEqual(Op const & op) const
        { OpCSE tmp(op); return isEqual(tmp); }
    
        // NOTE: we need temporary to force the "noVals"
        static uint64_t getHash(Op const & op)
        { OpCSE tmp(op); return getHash(tmp); }
        
        bool isEqual(OpCSE const & op) const
        { return i64 == op.i64 && opcode == op.opcode; }
    
        static uint64_t getHash(OpCSE const & op)
        { return hash64(op.i64 + op.opcode); }
    };

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

    // used to communicate relocations from Proc to Module
    struct NearReloc
    {
        uint32_t    codeOffset;     // where to add offset
        uint32_t    procIndex;    // which offset to add
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
            if(levelOpt) opt(levelOpt > 1);
            
            allocRegs();
            arch_emit(bytes);
        }

        std::vector<unsigned>   env;

        // generate a label
        unsigned newLabel()
        {
            unsigned label = blocks.size();
            BJIT_ASSERT(label < noVal);
            
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
            BJIT_ASSERT(label < blocks.size());
            // use live-flag to enforce "emit once"
            BJIT_ASSERT(!blocks[label].flags.live);
            blocks[label].flags.live = true;
            currentBlock = label;

            env.resize(blocks[label].args.size());
            for(int i = 0; i < env.size(); ++i)
            {
                env[i] = blocks[label].args[i].phiop;
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
            BJIT_ASSERT(label < blocks.size());
            BJIT_ASSERT(env.size() == blocks[label].args.size());
            
            auto & args = blocks[label].args;
            for(int a = 0; a < env.size(); ++a)
            {
                BJIT_ASSERT(ops[args[a].phiop].flags.type == ops[env[a]].flags.type);
                args[a].add(env[a], currentBlock);
            }
        }

        // write this as wrapper so we don't need to duplicate code
        void jnz(unsigned v, unsigned labelThen, unsigned labelElse)
        {
            jz(v, labelElse, labelThen);
        }

        void jz(unsigned v, unsigned labelThen, unsigned labelElse)
        {
            unsigned i = addOp(ops::jz, Op::_none);
            ops[i].in[0] = v;
            ops[i].label[0] = labelThen;
            ops[i].label[1] = labelElse;
            
            // add phi source
            BJIT_ASSERT(labelThen < blocks.size());
            BJIT_ASSERT(labelElse < blocks.size());
            BJIT_ASSERT(env.size() == blocks[labelThen].args.size());
            BJIT_ASSERT(env.size() == blocks[labelElse].args.size());
            
            auto & aThen = blocks[labelThen].args;
            auto & aElse = blocks[labelElse].args;
            for(int a = 0; a < env.size(); ++a)
            {
                BJIT_ASSERT(ops[aThen[a].phiop].flags.type == ops[env[a]].flags.type);
                BJIT_ASSERT(ops[aElse[a].phiop].flags.type == ops[env[a]].flags.type);
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
            passArgs(n);
            unsigned i = addOp(ops::icallp, Op::_ptr);
            ops[i].in[0] = ptr;
            return i;
        }

        // near call version
        // first argument is (immediate) index into module-table
        unsigned icalln(unsigned index, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::icalln, Op::_ptr);
            ops[i].imm32 = index;
            return i;
        }

        // same as icallp but functions returning floats
        unsigned fcallp(unsigned ptr, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::fcallp, Op::_f32);
            ops[i].in[0] = ptr;
            return i;
        }

        // near-call version
        unsigned fcalln(unsigned index, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::fcalln, Op::_f32);
            ops[i].imm32 = index;
            return i;
        }
        
        // same as icallp but functions returning doubles
        unsigned dcallp(unsigned ptr, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::dcallp, Op::_f64);
            ops[i].in[0] = ptr;
            return i;
        }

        // near-call version
        unsigned dcalln(unsigned index, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::dcalln, Op::_f64);
            ops[i].imm32 = index;
            return i;
        }

        // same as icallp/fcallp but tail-call: does not return
        void tcallp(unsigned ptr, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::tcallp, Op::_none);
            ops[i].in[0] = ptr;
        }

        // near-call version
        void tcalln(int index, unsigned n)
        {
            passArgs(n);
            unsigned i = addOp(ops::tcalln, Op::_none);
            ops[i].imm32 = index;
        }
        
        
#define BJIT_OP1(x,t,t0) \
    unsigned x(unsigned v0) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; BJIT_ASSERT(ops[v0].flags.type==Op::t0); return i; }
        
#define BJIT_OP2(x,t,t0,t1) \
    unsigned x(unsigned v0, unsigned v1) { \
        unsigned i = addOp(ops::x, Op::t); \
        ops[i].in[0] = v0; BJIT_ASSERT(ops[v0].flags.type==Op::t0); \
        ops[i].in[1] = v1; BJIT_ASSERT(ops[v1].flags.type==Op::t1); return i; }

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
        BJIT_OP1(fneg,_f32,_f32);
        BJIT_OP2(fmul,_f32,_f32,_f32); BJIT_OP2(fdiv,_f32,_f32,_f32);

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
        ops[i].in[0] = v0; BJIT_ASSERT(ops[v0].flags.type == Op::_ptr); \
        ops[i].imm32 = imm32; return i; }

        // stores take pointer+offset and value to store
#define BJIT_STORE(x, t) \
    unsigned x(unsigned ptr, int32_t imm32, unsigned val) { \
        unsigned i = addOp(ops::x, Op::_none); \
        ops[i].in[0] = ptr; BJIT_ASSERT(ops[ptr].flags.type == Op::_ptr); \
        ops[i].in[1] = val; BJIT_ASSERT(ops[val].flags.type == Op::t); \
        ops[i].imm32 = imm32; return i; }

        BJIT_LOAD(li8, _ptr); BJIT_LOAD(li16, _ptr);
        BJIT_LOAD(li32, _ptr); BJIT_LOAD(li64, _ptr);
        BJIT_LOAD(lu8, _ptr); BJIT_LOAD(lu16, _ptr);
        BJIT_LOAD(lu32, _ptr);
        BJIT_LOAD(lf32, _f32); BJIT_LOAD(lf64, _f64);

        BJIT_STORE(si8, _ptr); BJIT_STORE(si16, _ptr);
        BJIT_STORE(si32, _ptr); BJIT_STORE(si64, _ptr);
        BJIT_STORE(sf32, _f32); BJIT_STORE(sf64, _f64);

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

        unsigned currentBlock;

        void opt(bool unsafe = false)
        {
            // do DCE first, then fold
            // repeat until neither does progress
            // check sanity limit for better test-automation
            int iterOpt = 0;
            do
            {
                BJIT_ASSERT(++iterOpt < 0x100);
                opt_dce(unsafe);
            } while(opt_fold(unsafe) || opt_cse(unsafe) || opt_sink(unsafe));
        }

        // used to break critical edges, returns the new block
        // tries to fix most info, but not necessarily all
        uint16_t breakEdge(uint16_t from, uint16_t to)
        {
            BJIT_LOG(" BCE[%d,%d]", from, to);
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
            BJIT_ASSERT(ops.size() < noVal);
#else
            if(ops.size() == noVal) throw too_many_ops();
#endif
            unsigned i = ops.size();
            
            ops.resize(i + 1);
            ops[i].opcode = opcode;
            ops[i].in[0] = noVal;
            ops[i].in[1] = noVal;
            ops[i].scc = noSCC;
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

        void passArgs(unsigned n)
        {
            nPassInt     = 0;
            nPassFloat   = 0;
            nPassTotal   = 0;
            
            // We probably want right-to-left once we do stack?
            for(int i = 0; i<n; ++i) passNextArg(env[env.size()-n+i]);
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
        unsigned iarg()
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
            return i;
        }

        unsigned farg()
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
            return i;
        }
        
        unsigned darg()
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
        bool opt_fold(bool unsafe);

        // opt-cse.cpp
        bool opt_cse(bool unsafe);

        // opt-sink.cpp
        bool opt_sink(bool unsafe);

        // opt-dce.cpp
        void opt_dce(bool unsafe = false);

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
            arch_patchStub(bytes.data(), offsets[index], address);
            
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
        int compile(Proc & proc, unsigned levelOpt = 1)
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

        void arch_patchStub(void * ptr, unsigned offset, uintptr_t address);

    };

}