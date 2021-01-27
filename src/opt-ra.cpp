
#include "ir.h"

using namespace bjit;

static const bool ra_debug = false;     // print lots of debug spam
static const bool scc_debug = false;    // print lots of debug spam

void Proc::allocRegs()
{
    findSCC();
    livescan();
    
    printf(" RA:BB"); if(ra_debug) printf("\n");

    
    std::vector<uint16_t>   codeOut;

    Rename rename;

    for(auto b : live)
    {
        // FIXME: this is terribly inefficient
        for(auto & op : ops) if(op.opcode > ops::jmp) op.nUse = 0;
        
        // work out the actual use-counts
        findUsesBlock(b, false);

        // mark anything used by any phi
        for(auto & a : blocks[b].args)
        {
            for(auto & s : a.alts)
            {
                ops[s.val].nUse = 1;
            }
        }

        if(ra_debug) printf("L%d:\n", b);

        uint16_t    regstate[regs::nregs];
        memcpy(regstate, blocks[b].regsIn, sizeof(regstate));
        blocks[b].flags.regsDone = true;

        // FIXME: this should do a parallel search
        auto findBest = [&](RegMask mask, int reg, int c) -> int
        {
            if(!mask) return regs::nregs;

            // if preferred register is impossible, clear it
            if(reg != regs::nregs && !((1ull<<reg)&mask))
            {
                //printf("incompatible: mask %012llx (reg: %012llx)\n",
                //    mask, (1ull<<reg));
                reg = regs::nregs;
            }

            if(reg < regs::nregs) return reg;

            for(int r = 0; r < regs::nregs; ++r)
            {
                if(((1ull<<r) & mask) && regstate[r] == noVal)
                {
                    if(ra_debug) printf("found free: %s\n", regName(r));
                    return r;
                }
            }
            
            // is there only a single register left?
            if(!(mask & (mask - 1)))
            {
                unsigned r = 0, s = 64;
                while(s >>= 1) if(mask & ~((1ull<<s)-1))
                    { r += s; mask >>= s; }
                if(ra_debug) printf("one reg: %s\n", regName(r));
                return r;
            }
            
            for(int i = c; i < blocks[b].code.size(); ++i)
            {
                auto & op = ops[blocks[b].code[i]];
                for(int j = 0; j < op.nInputs(); ++j)
                {
                    if(op.in[j] != regstate[ops[op.in[j]].reg]) continue;

                    if(ra_debug)
                    printf("op uses reg: %s\n", regName(ops[op.in[j]].reg));

                    mask &=~ (1ull<<ops[op.in[j]].reg);
                    
                    // is there only a single register left?
                    if(!(mask & (mask - 1)))
                    {
                        unsigned r = 0, s = 64;
                        while(s >>= 1) if(mask & ~((1ull<<s)-1))
                            { r += s; mask >>= s; }
                        if(ra_debug) printf("last reg reg: %s\n", regName(r));
                        return r;
                    }
                }
            }

            // FIXME: check live-out set?

            // assume the rest are equally bad
            for(int r = 0; r < regs::nregs; ++r)
            {
                if((1ull<<r) & mask)
                {
                    if(ra_debug) printf("any random: %s\n", regName(r));
                    return r;
                }
            }

            assert(false);
        };
        
        
        for(int i = 0; i < regs::nregs; ++i)
        {
            if(regstate[i] == noVal) continue;
            if(!ops[regstate[i]].nUse)
            {
                if((ops[regstate[i]].opcode == ops::rename
                    || ops[regstate[i]].opcode == ops::reload)
                && (ops[ops[regstate[i]].in[0]].nUse))
                {
                    ops[regstate[i]].nUse = ops[ops[regstate[i]].in[0]].nUse;
                    rename.add(ops[regstate[i]].in[0], regstate[i]);
                }
                else blocks[b].regsIn[i] = regstate[i] = noVal;
            }
        }

        RegMask keepIn = 0;
        
        for(int c = 0; c < blocks[b].code.size(); ++c)
        {
            // reserve some excess, so we can keep reference
            // should be max ~5, but alloc safe
            ops.reserve(ops.size() + 20);
            auto & op = ops[blocks[b].code[c]];

            uint16_t lastOp = codeOut.size() ? codeOut.back() : 0xffff;
            if(ra_debug) if(codeOut.size()) debugOp(codeOut.back());

            rename(op);

            if(op.opcode <= ops::jmp)
            for(int k = 0; k < 2; ++k)
            {
                if(k && op.opcode == ops::jmp) break;

                for(auto & a : blocks[op.label[k]].args)
                for(auto & s : a.alts)
                for(auto & r : rename.map)
                {
                    if(s.val == r.src) s.val = r.dst;
                }
            }
            if(ra_debug) debugOp(blocks[b].code[c]);

            // copy - we'll fix this below if necessary
            codeOut.push_back(blocks[b].code[c]);

            int prefer = regs::nregs;
            // Do we have the inputs we need?
            for(int i = 0; i < op.nInputs(); ++i)
            {
                auto mask = op.regsIn(i);

                // avoid putting second operand in the same register as first
                if(i && (op.in[0] != op.in[1]))
                    mask &=~(1ull<<ops[op.in[0]].reg);

                int wr = ops[op.in[i]].reg;
                if(regstate[wr] != op.in[i]) wr = regs::nregs;
                else if(blocks[b].regsIn[wr] == op.in[i]) keepIn |= (1ull<<wr);
                
                int r = findBest(mask, wr, c);

                if(wr != regs::nregs && r != wr)
                {
                    if(ra_debug) printf("; need rename for %04x (%s -> %s) \n",
                        op.in[i], regName(ops[op.in[i]].reg), regName(r));

                    // should we try to save existing?
                    if(regstate[r] != noVal)
                    {
                        RegMask smask = ops[regstate[r]].regsMask();
                            
                        // if we're moving 2nd operand to make room for
                        // the first then also take that mask into account
                        if(!i && op.nInputs()>1 && regstate[r] == op.in[1])
                        {
                            smask &= op.regsIn(1);
                            // but not r
                            smask &=~(1ull<<r);
                        }
                        // if the best choice after this op is r
                        // then we're going to lose this no matter what
                        int s = findBest(smask, regs::nregs, c+1);
                        if(s != r && s != wr)
                        {
                            if(ra_debug) printf("; saving %04x (%s -> %s) \n",
                                    regstate[r], regName(r), regName(s));
                            uint16_t sr = newOp(ops::rename,
                                ops[regstate[r]].flags.type);
                            
                            ops[sr].in[0] = regstate[r];
                            ops[sr].reg = s;
                            regstate[s] = sr;

                            ops[sr].nUse = ops[regstate[r]].nUse;
                            
                            rename.add(regstate[r], sr);
                            rename(op);
                            if(ra_debug) debugOp(sr);
                            
                            std::swap(codeOut.back(), sr);
                            codeOut.push_back(sr);
                        }
                        else
                        {
                            // FIXME: we can XCHG but we'll need 2-out ops
                            if(ra_debug)
                                printf("; could not save %04x\n", regstate[r]);
                        }
                    }

                    // was this the last op we just output?
                    // try to patch it in case it's something simple
                    if(op.in[i] == lastOp && ops[op.in[i]].nInputs()<2
                    && (ops[op.in[i]].regsOut() & (1ull<<r)))
                    {
                        regstate[ops[op.in[i]].reg] = noVal;
                        ops[op.in[i]].reg = r;
                        regstate[r] = op.in[i];
                    }
                    else
                    {
                        uint16_t rr = newOp(ops::rename, ops[op.in[0]].flags.type);
                        
                        ops[rr].in[0] = op.in[i];
                        ops[rr].reg = r;
                        regstate[r] = rr;
                            
                        ops[rr].nUse = ops[op.in[i]].nUse;
                        
                        rename.add(op.in[i], rr);
                        rename(op);
                        if(ra_debug) debugOp(rr);
                        
                        std::swap(codeOut.back(), rr);
                        codeOut.push_back(rr);
                    }

                }

                if(regstate[r] != op.in[i])
                {
                    if(ra_debug) printf("; need reload for %04x into %s\n",
                        op.in[i], regName(r));
                        
                    // undo renames/reloads before reloading
                    // we don't want to flag those for spill
                    while(ops[op.in[i]].opcode == ops::rename
                    || ops[op.in[i]].opcode == ops::reload)
                        op.in[i] = ops[op.in[i]].in[0];
                        
                    if(ra_debug) printf("; need reload for %04x into %s\n",
                        op.in[i], regName(r));

                    uint16_t rr = newOp(ops::reload, ops[op.in[1]].flags.type);
                    
                    // don't do a true reload if we can remat constant
                    if(ops[op.in[i]].opcode == ops::lci
                    || ops[op.in[i]].opcode == ops::lcf)
                    {
                        ops[rr].opcode = ops[op.in[i]].opcode;
                        ops[rr].i64 = ops[op.in[i]].i64;
                    }
                    else
                    {
                        ops[op.in[i]].flags.spill = true;
                        ops[rr].in[0] = op.in[i];
                        ops[rr].scc = ops[op.in[i]].scc;
                    }

                    ops[rr].reg = r;
                    regstate[r] = rr;
                    ops[rr].nUse = ops[op.in[i]].nUse;
                    
                    rename.add(op.in[i], rr);
                    rename(op);
                    if(ra_debug) debugOp(rr);

                    std::swap(codeOut.back(), rr);
                    codeOut.push_back(rr);
                }
            }

            // check to free once all inputs are done
            for(int i = 0; i < op.nInputs(); ++i)
            {
                if(!--ops[op.in[i]].nUse)
                {
                    if(ra_debug) printf("last use %04x (%s)\n",
                        op.in[i], regName(ops[op.in[i]].reg));
                    if(regstate[ops[op.in[i]].reg] == op.in[i])
                    {
                        regstate[ops[op.in[i]].reg] = noVal;
                        if(!i) prefer = ops[op.in[i]].reg;
                    }
                }
            }

            // clobbers - could try to save, but whatever
            RegMask lost = op.regsLost();
            if(lost)
            {
                RegMask notlost = ~lost;
                for(int i = 0; i < op.nInputs(); ++i)
                {
                    // protect inputs
                    notlost &=~(1ull<<ops[op.in[i]].reg);
                }
                
                for(int r = 0; r < regs::nregs; ++r)
                {
                    if(regstate[r] == noVal || (1ull<<r)&~lost) continue;
    
                    // scan from current op, don't wanna overwrite inputs
                    int s = findBest(
                        notlost & ops[regstate[r]].regsMask(), regs::nregs, c);
    
                    if(s < regs::nregs && regstate[s] == noVal)
                    {
                        if(ra_debug) printf("; saving lost %04x (%s -> %s) \n",
                                regstate[r], regName(r), regName(s));
                    
                        uint16_t sr = newOp(ops::rename,
                            ops[regstate[r]].flags.type);
                        
                        ops[sr].in[0] = regstate[r];
                        ops[sr].reg = s;
                        regstate[s] = sr;
    
                        ops[sr].nUse = ops[regstate[r]].nUse;
                        
                        rename.add(regstate[r], sr);
                        // do NOT rename the current op here
                        if(ra_debug) debugOp(sr);
                        
                        std::swap(codeOut.back(), sr);
                        codeOut.push_back(sr);
    
                    }
                    else if(ra_debug) printf("; could not save lost v%04x (%s)\n",
                        regstate[r], regName(r));
                    
                    regstate[r] = noVal;
                }
            }

            // on jumps, store regs out, but only after processing
            // any inputs for a conditional jump
            if(op.opcode <= ops::jmp)
            {
                memcpy(blocks[b].regsOut, regstate, sizeof(regstate));
            }

            // do these as well after inputs
            if(op.opcode <= ops::jmp && !blocks[op.label[0]].flags.regsDone)
            {
                for(int i = 0; i < regs::nregs; ++i)
                {
                    if(regstate[i] != noVal)
                    {
                        blocks[op.label[0]].regsIn[i] = regstate[i];
                    }
                }
            }
            if(op.opcode < ops::jmp && !blocks[op.label[1]].flags.regsDone)
            {
                for(int i = 0; i < regs::nregs; ++i)
                {
                    if(regstate[i] != noVal)
                    {
                        blocks[op.label[1]].regsIn[i] = regstate[i];
                    }
                }
            }
            
            if(!op.hasOutput()) continue;

            if(op.opcode == ops::phi)
            {
                assert(op.in[0] == b);
                auto & alts =  blocks[b].args[op.in[1]].alts;
                for(int i = 0; i < alts.size(); ++i)
                {
                    if(op.reg == regs::nregs)
                    for(int r = 0; r < regs::nregs; ++r)
                    {
                        uint16_t v = regstate[r];
                        
                        while(v != noVal && v != alts[i].val
                        && (ops[v].opcode == ops::rename
                            || ops[v].opcode == ops::reload))
                        {
                            v = ops[v].in[0];
                        }
                    
                        if(v != alts[i].val) continue;
                        op.reg = r;
                        break;
                    }

                    // cleanup other alternatives
                    for(int r = 0; r < regs::nregs; ++r)
                    {
                        if(regstate[r] == alts[i].val) regstate[r] = noVal;
                    }
                }

                if(op.reg < regs::nregs)
                {
                    // store this as incoming register
                    blocks[b].regsIn[op.reg]
                        = regstate[op.reg] = blocks[b].code[c];
                    keepIn |= (1ull<<op.reg);
                }
                
                // never forcibly allocate a register to phi
                continue;
            }


            RegMask mask = op.regsOut();

            // try to mask second operand if possible
            if(op.nInputs()>1 && op.in[0] != op.in[1]
            && (mask &~(1ull<<ops[op.in[1]].reg)))
                mask &=~(1ull<<ops[op.in[1]].reg);
            
            op.reg = findBest(mask, prefer, c+1);
            assert(op.reg < regs::nregs);
            regstate[op.reg] = blocks[b].code[c];
        }

        std::swap(blocks[b].code, codeOut);
        codeOut.clear();
        for(int i = 0; i < regs::nregs; ++i)
        {
            if(!(keepIn&(1ull<<i)))
            {
                blocks[b].regsIn[i] = noVal;
            }
            else
            {
                //printf("keep %04x in %s\n", blocks[b].regsIn[i], regName(i));
            }
        }
    }

    // FIXME: is this always safe?
    while(opt_dce());

    // cleanup stuff that DCE decided is dead
    for(auto b : live)
    {
        for(int i = 0; i < regs::nregs; ++i)
        {
            if(blocks[b].regsOut[i] != noVal
            && ops[blocks[b].regsOut[i]].opcode == ops::nop)
            {
                blocks[b].regsOut[i] = noVal;
            }
        }
    }

    printf(" RA:JMP"); if(ra_debug) printf("\n");

    std::vector<uint16_t>   newBlocks;
    
    // compute jump-shuffles
    for(auto b : live)
    {
        uint16_t    sregs[regs::nregs];
        uint16_t    tregs[regs::nregs];

        auto jumpShuffle = [&](uint16_t out, uint16_t target)
        {
            memcpy(sregs, blocks[b].regsOut, sizeof(sregs));
            memcpy(tregs, blocks[target].regsIn, sizeof(tregs));

            if(ra_debug) printf("Args L%d (L%d)-> L%d\n", b, out, target);

            for(auto & a : blocks[target].args)
            {
                for(auto & s : a.alts)
                {
                    if(s.src != b) continue;

                    // if we need a spill, can we do a trivial one?
                    if(ops[a.phiop].flags.spill
                    && ops[a.phiop].reg == regs::nregs)
                    {
                        // if both are spilled same SCC then it's fine
                        if(ops[s.val].flags.spill
                        && ops[s.val].scc == ops[a.phiop].scc) continue;
                         
                        // don't spill phi-to-phi, this'll cause
                        // problems with scc
                        if(!ops[s.val].flags.spill
                        && ops[s.val].opcode != ops::phi)
                        {
                            ops[s.val].flags.spill = true;
                            ops[s.val].scc = ops[a.phiop].scc;
                        }
                        else
                        {
                            printf("PHI LOOP: Broken SCCs?\n");
                            assert(false);
                        }
                    }
                    
                    if(ops[a.phiop].reg == regs::nregs) continue;
                    tregs[ops[a.phiop].reg] = s.val;
                }
            }

            // if we have any renames or reloads, trace tham back
            // but only if the SCCs match
            for(int i = 0; i < regs::nregs; ++i)
            {
                while(tregs[i] != noVal &&
                ( ops[tregs[i]].opcode == ops::rename
                || ops[tregs[i]].opcode == ops::reload)
                && ops[tregs[i]].scc == ops[ops[tregs[i]].in[0]].scc)
                {
                    tregs[i] = ops[tregs[i]].in[0];
                }
                
                while(sregs[i] != noVal &&
                ( ops[sregs[i]].opcode == ops::rename
                || ops[sregs[i]].opcode == ops::reload)
                && ops[sregs[i]].scc == ops[ops[sregs[i]].in[0]].scc)
                {
                    sregs[i] = ops[sregs[i]].in[0];
                }
            }

            bool done = false;
            while(!done)
            {
                if(ra_debug) printf("Shuffle L%d:\n", b);
                done = true;

                // first try: free moves only
                for(int t = 0; t < regs::nregs; ++t)
                {
                    if(tregs[t] == noVal) continue;
                    if(sregs[t] == tregs[t]) continue;
    
                    for(int s = 0; s < regs::nregs; ++s)
                    {
                        // can we move without overwriting
                        if(sregs[s] == tregs[t])
                        {
                            if(sregs[t] != noVal) break;

                            uint16_t rr = newOp(ops::rename,
                                ops[sregs[s]].flags.type);
                            
                            if(ra_debug) printf("move: %s:%04x -> %s:%04x\n",
                                regName(s), sregs[s], regName(t), rr);

                            ops[rr].reg = t;
                            ops[rr].in[0] = sregs[s];
                            ops[rr].scc = ops[sregs[s]].scc;
                            sregs[t] = tregs[t];
                            sregs[s] = noVal;
    
                            std::swap(rr, blocks[b].code.back());
                            blocks[out].code.push_back(rr);
                            done = false;
                            break;
                        }
                    }
                }
                
                if(!done) continue; // did some progress

                // cycle breaker
                for(int t = 0; t < regs::nregs; ++t)
                {
                    if(tregs[t] == noVal) continue;
                    if(sregs[t] == tregs[t]) continue;
    
                    for(int s = 0; s < regs::nregs; ++s)
                    {
                        if(sregs[s] == tregs[t])
                        {
                            // figure out the correct register type
                            RegMask mask = ops[tregs[t]].regsMask();
                                
                            // if source register is already free
                            // then don't move it to another free
                            // this will just cause infinite cycles
                            int r = tregs[s] != noVal ? 0 : regs::nregs;
                            while(r < regs::nregs)
                            {
                                if(((1ull<<r)&mask) && sregs[r] == noVal) break;
                                ++r;
                            }
                            // if no free regs, just force correct register
                            // so we break the cycle through memory
                            if(r == regs::nregs) r = t;

                            uint16_t rr = newOp(ops::rename,
                                ops[sregs[s]].flags.type);
                            
                            if(ra_debug) printf(
                                "move: %s:%04x -> %s:%04x, cycle breaker: %04x\n",
                                regName(s), sregs[s], regName(r), rr, sregs[r]);
                            
                            ops[rr].reg = r;
                            ops[rr].in[0] = sregs[s];
                            ops[rr].scc = ops[sregs[s]].scc;
                            sregs[r] = tregs[t];
                            sregs[s] = noVal; // do NOT mark this as free!
    
                            std::swap(rr, blocks[b].code.back());
                            blocks[out].code.push_back(rr);
                            done = false;
                            break;
                        }
                        
                        if(!done) break;
                    }
                    
                    if(!done) break; // go back to regular moves
                }

                if(!done) continue; // broke a cycle?

                // restores
                for(int t = 0; t < regs::nregs; ++t)
                {
                    if(tregs[t] == noVal) continue;
                    if(sregs[t] == tregs[t]) continue;

                    // at this point simple restore should work?
                    //assert(sregs[t] == noVal);
                    
                    if(ra_debug) printf("reload -> %s:%04x (%04x)\n",
                            regName(t), tregs[t], sregs[t]);

                    uint16_t rr = newOp(ops::reload, ops[tregs[t]].flags.type);
                    
                    ops[rr].reg = t;
                    ops[rr].in[0] = tregs[t];
                    ops[rr].scc = ops[tregs[t]].scc;
                    sregs[t] = tregs[t];
                    ops[tregs[t]].flags.spill = true;

                    std::swap(rr, blocks[out].code.back());
                    blocks[out].code.push_back(rr);
                }
            }
            memcpy(blocks[out].regsOut, sregs, sizeof(sregs));
        };

        assert(b < blocks.size());
    
        // make a copy, since we'll be adding new ops
        auto op = ops[blocks[b].code.back()];

        // is this a return?
        if(op.opcode > ops::jmp) continue;

        if(op.opcode < ops::jmp)
        {
            // create some shuffle blocks
            // try to fix enough stuff for debug() to be happy
            int b0 = blocks.size();
            int b1 = blocks.size() + 1;
            blocks.resize(blocks.size() + 2);

            blocks[b0].code.push_back(newOp(ops::jmp, Op::_none));
            ops[blocks[b0].code.back()].label[0] = op.label[0];
            memcpy(blocks[b0].regsIn, blocks[b].regsOut, sizeof(sregs));
            jumpShuffle(b0, op.label[0]);

            // keep shuffle-block if we added shuffles
            if(blocks[b0].code.size() > 1)
            {
                blocks[b0].comeFrom.push_back(b);
                blocks[b0].flags.live = true;
                ops[blocks[b].code.back()].label[0] = b0;
                newBlocks.push_back(b0);
            }
            
            blocks[b1].code.push_back(newOp(ops::jmp, Op::_none));
            ops[blocks[b1].code.back()].label[0] = op.label[1];
            memcpy(blocks[b1].regsIn, blocks[b].regsOut, sizeof(sregs));
            jumpShuffle(b1, op.label[1]);

            // keep shuffle-block if we added shuffles
            if(blocks[b1].code.size() > 1)
            {
                blocks[b1].comeFrom.push_back(b);
                blocks[b1].flags.live = true;
                ops[blocks[b].code.back()].label[1] = b1;
                newBlocks.push_back(b1);
            }
        }

        // standard jmp
        if(op.opcode == ops::jmp)
        {
            jumpShuffle(b, op.label[0]);
        }
    }

    // add the new blocks after loop
    for(auto n : newBlocks) live.push_back(n);

    // find slots
    std::vector<bool>   sccUsed;
    
    // Cleanup and find slots
    for(auto & op : ops)
    {
        // clear spill-flag from PHIs with no register
        // the source-site will handle the spill
        if(op.opcode == ops::phi && op.reg == regs::nregs)
            op.flags.spill = false;

        // clear spill-flag on reloads where scc matches
        // this can happen when a PHI has no register
        if(op.opcode == ops::reload && op.scc == ops[op.in[0]].scc)
            op.flags.spill = false;

        if(op.scc >= sccUsed.size()) sccUsed.resize(op.scc + 1);
        if(op.flags.spill) sccUsed[op.scc] = true;

        if(op.hasOutput()) usedRegs |= (1ull<<op.reg);
    }

    std::vector<uint16_t>   slots(sccUsed.size(), 0xffff);
    assert(!nSlots);
    for(int s = 0; s < slots.size(); ++s)
        if(sccUsed[s]) slots[s] = nSlots++;

    for(auto & op : ops) if(op.hasOutput()) op.scc = slots[op.scc];

    printf(" DONE\n");
    raDone = true;
}

void Proc::findSCC()
{
    livescan(); // need live-in registers

    assert(!raDone);
    printf(" RA:SCC");

    std::vector<bool>   sccUsed;

    // reset all ops
    for(auto & op : ops) if(op.hasOutput()) op.scc = noSCC;
    
    // livescan live
    for(auto bi : live)
    {
        auto & b = blocks[bi];

        // assume all classes are free
        for(int i = 0; i < sccUsed.size(); ++i) sccUsed[i] = false;

        // reserve all SCCs of live-in registers
        for(auto in : b.livein)
        {
            // since we're doing live-scan live any live-in
            // variables should already have their SCC allocated
            assert(ops[in].scc != noSCC);
            // this is just a sanity check
            assert(ops[in].scc < sccUsed.size());
            sccUsed[ops[in].scc] = true;

            if(scc_debug) printf("Live in: %04x:[%04x]\n", in, ops[in].scc);
        }

        for(auto c : b.code)
        {
            // at least one phi-alternative should have SCC
            if(ops[c].opcode == ops::phi)
            {
                assert(ops[c].in[0] == bi);
                for(auto & s : b.args[ops[c].in[1]].alts)
                {
                    // check for validity, pick first possible
                    if(ops[s.val].scc != noSCC && !sccUsed[ops[s.val].scc])
                    {
                        ops[c].scc = ops[s.val].scc;
                        sccUsed[ops[c].scc] = true;
                        break;
                    }
                    else
                    {
                        if(scc_debug) printf("not valid: %04x:[%04x]\n",
                            s.val, ops[s.val].scc);
                    }
                }

                // if we can't find one, then pick a new-one
                if(ops[c].scc == noSCC)
                {
                    if(scc_debug) printf("need new SCC for phi\n");
                    ops[c].scc = sccUsed.size();
                    sccUsed.push_back(true);
                }

                if(scc_debug) debugOp(c);
                continue;
            }
        
            for(int i = 0; i < ops[c].nInputs(); ++i)
            {
                // if this is last-use, free the SCC
                if(!--ops[ops[c].in[i]].nUse)
                {
                    assert(ops[ops[c].in[i]].scc < sccUsed.size());
                    sccUsed[ops[ops[c].in[i]].scc] = false;
                }
            }
        
            if(ops[c].hasOutput())
            {
                // find a free SCC
                assert(ops[c].scc == noSCC);
                
                for(int i = 0; i < sccUsed.size(); ++i)
                {
                    if(!sccUsed[i])
                    {
                        ops[c].scc = i;
                        sccUsed[i] = true;
                        break;
                    }
                }

                // do we need to alloc a new one?
                if(ops[c].scc == noSCC)
                {
                    ops[c].scc = sccUsed.size();
                    sccUsed.push_back(true);
                }

                if(scc_debug) debugOp(c);
            }

        }
    }

    // second pass, add renames to invalid jmps
    for(auto b : live)
    {
        assert(blocks[b].flags.live);
        
        int nSCC = sccUsed.size();
        auto c = blocks[b].code.back();
        
        if(ops[c].opcode <= ops::jmp)
        for(int k = 0; k < 2; ++k)
        {
            if(k && ops[c].opcode == ops::jmp) break;
    
            for(auto & a : blocks[ops[c].label[k]].args)
            for(auto & s : a.alts)
            {
                if(s.src == b && ops[s.val].scc != ops[a.phiop].scc)
                {
                    if(scc_debug)
                        printf("rename jump: %04x:[%04x] -> %04x:[%04x]\n",
                        s.val, ops[s.val].scc, a.phiop, ops[a.phiop].scc);
    
                    uint16_t rr = newOp(ops::rename, ops[s.val].flags.type);
                    ops[rr].scc = nSCC++;   // play safe: alloc a fresh one
                    ops[rr].in[0] = s.val; s.val = rr;

                    std::swap(rr, blocks[b].code.back());
                    blocks[b].code.push_back(rr);
                }
            }
        }
    }
    
    if(scc_debug) debug();
}