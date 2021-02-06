
#pragma once

#include "hash.h"
#include "ir-ops.h"
#include "arch-x64.h"   // FIXME: check for arch

namespace bjit
{

#define BJIT_OP_ENUM(name,...) name
    namespace ops { enum { BJIT_OPS(BJIT_OP_ENUM) }; };
#undef BJIT_OP_ENUM

    static const uint16_t   noVal = 0xffff;
    static const uint16_t   noSCC = 0xffff;
    
    namespace impl
    {
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
                bool    no_opt  : 1;    // don't unroll or hoist further
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
                    imm32 = (op.hasImm32()||op.hasF32()) ? op.imm32 : 0;
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
                bool live       : 1;    // used/reset by DCE, RA
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
    };
};