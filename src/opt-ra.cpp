
#include <cstring>

#include "bjit.h"

using namespace bjit;

static const bool ra_debug = false;     // print lots of debug spam
static const bool scc_debug = false;    // print lots of debug spam

static const bool fix_sanity = true;    // whether to fix sanity for shuffles

void Proc::allocRegs(bool unsafeOpt)
{
    // explicitly do one DCE so non-optimized builds work
    opt_dce();
    findSCC();
    find_ivs();

    // rebuild the other stuff
    rebuild_dom();
    rebuild_livein();

    BJIT_LOG(" RA:PHI");
    
    // reintroduce phis to all blocks with live-in variables
    impl::Rename rename;
    impl::Rename renameBlock;
    for(auto b : live)
    {
        if(!blocks[b].livein.size()) continue;
        
        // make room for new phi
        blocks[b].code.insert(blocks[b].code.begin(),
            blocks[b].livein.size(), noVal);

        for(int i = 0; i < blocks[b].livein.size(); ++i)
        {
            auto opIndex = blocks[b].livein[i];
            auto & op = ops[opIndex];
            
            blocks[b].code[i] = newOp(ops::phi, op.flags.type, b);
            ops[blocks[b].code[i]].phiIndex = blocks[b].args.size();
            ops[blocks[b].code[i]].iv = noVal;
            ops[blocks[b].code[i]].scc = op.scc;
            blocks[b].args.emplace_back(impl::Phi(blocks[b].code[i]));

            for(auto cf : blocks[b].comeFrom)
            {
                blocks[b].newAlt(blocks[b].code[i], cf, opIndex);
            }

            rename.add(opIndex, blocks[b].code[i]);
        }
        
        blocks[b].livein.clear();
    }

    for(auto b : live)
    {
        renameBlock.map.clear();
        
        // we need to do this manually here
        // because we want the LAST rename that dominates
        for(int j = rename.map.size(); j--;)
        {
            auto & r = rename.map[j];
            
            bool found = false;
            int idom = b;
            while(idom)
            {
                if(ops[r.dst].block == idom) found = true;
                idom = blocks[idom].idom;
            }
            if(!found) continue;

            renameBlock.map.push_back(r);
        }

        // rename the block
        for(int i = 0; i < blocks[b].code.size(); ++i)
        {
            auto & op = ops[blocks[b].code[i]];

            renameBlock(op);

            if(op.opcode <= ops::jmp)
            for(int k = 0; k < 2; ++k)
            {
                if(k && op.opcode == ops::jmp) break;

                BJIT_ASSERT(!blocks[op.label[k]].livein.size());

                for(auto & a : blocks[op.label[k]].alts)
                {
                    if(a.src != b) continue;
                    for(auto & r : renameBlock.map)
                    {
                        if(a.val == r.src) a.val = r.dst;
                    }
                }
            }
        }
    }

    rebuild_memtags(unsafeOpt);

    BJIT_LOG(" RA:BB");

    std::vector<uint16_t>   codeOut;

    for(auto b : live)
    {
        // we should no longer have any live-in variables
        // as we reintroduced degenerate phis for everything
        BJIT_ASSERT(!blocks[b].livein.size());

        // clear phi-in use-counts
        for(auto & s : blocks[b].alts)
        {
            ops[s.val].nUse = 0;
        }
        
        // work out the actual use-counts
        findUsesBlock(b, false, false);
        
        // mark anything used by any phi
        for(auto & s : blocks[b].alts)
        {
            ops[s.val].nUse += 1;
        }

        // cleanup stale renames
        for(auto & r : rename.map)
        {
            bool found = false;
            int idom = b;
            while(idom)
            {
                idom = blocks[idom].idom;
                if(ops[r.dst].block == idom) found = true;
            }

            if(!found) r.src = r.dst;   // won't match anything
        }

        {
            int j = 0;
            for(int i = 0; i < rename.map.size(); ++i)
            {
                if(rename.map[i].dst == rename.map[i].src) continue;
                if(i != j) rename.map[j] = rename.map[i];
                ++j;
            }
        }

        if(ra_debug) BJIT_LOG("L%d:\n", b);

        // initialize memtag for rematerialization of loads
        uint16_t    memtag = blocks[b].memtag;

        uint16_t    regstate[regs::nregs];
        memcpy(regstate, blocks[b].regsIn, sizeof(regstate));
        blocks[b].flags.regsDone = true;

        // cleanup stuff that is not a phi-arg
        for(int r = 0; r < regs::nregs; ++r)
        {
            bool found = false;
            if(!found) for(auto & s : blocks[b].alts)
            {
                if(s.src != b && s.val != regstate[r]) continue;
                found = true;
                break;
            }

            if(!found) regstate[r] = noVal;
        }

        // find best satisfying mask, prefer reg, scan from c
        // if val!=noVal then we mask lost regs until first use
        // and try to satisfy input constraints
        auto findBest = [&](RegMask mask, int reg, int c, uint16_t val=noVal) -> int
        {
            if(!mask) return regs::nregs;

            // if preferred register is impossible, clear it
            if(reg != regs::nregs && !(R2Mask(reg)&mask))
            {
                if(ra_debug)
                    BJIT_LOG("incompatible: mask %012llx (reg: %012llx)\n",
                        mask, R2Mask(reg));
                reg = regs::nregs;
            }

            if(reg < regs::nregs) return reg;

            // normally we pick the first free register we'll find
            // but if we are trying to save a value, then there is no
            // point in saving somewhere we're going to lose soon or
            // that we'll have to move again to satisfy next use
            //
            // so in this case, go to the extra effort to try to pick
            // a register that is actually going to be useful
            RegMask freeMask = 0;
            for(int r = 0; r < regs::nregs; ++r)
            {
                if((R2Mask(r) & mask) && regstate[r] == noVal)
                {
                    if(ra_debug) BJIT_LOG("found free: %s\n", regName(r));
                    if(val == noVal) return r;
                    freeMask |= R2Mask(r);
                }
            }
            if(freeMask) { mask = freeMask; }
            
            // is there only a single register left?
            if(!(mask & (mask - 1)))
            {
                unsigned r = 0, s = 64;
                while(s >>= 1) if(mask & ~(R2Mask(s)-1))
                    { r += s; mask >>= s; }
                if(ra_debug) BJIT_LOG("one reg: %s\n", regName(r));
                return r;
            }

            for(int i = c; i < blocks[b].code.size(); ++i)
            {
                auto & op = ops[blocks[b].code[i]];
                bool foundUse = false;
                for(int j = 0; j < op.nInputs(); ++j)
                {
                    // if op wants the value we're trying to save, mask input
                    // constraints if this doesn't completely clear mask
                    if(op.in[j] == val)
                    {
                        if(mask & op.regsIn(j)) mask &= op.regsIn(j);
                        foundUse = false;
                    }
                    else if(ops[op.in[j]].reg != regs::nregs
                            && op.in[j] == regstate[ops[op.in[j]].reg])
                    {
                        if(ra_debug)
                        BJIT_LOG("op uses reg: %s\n", regName(ops[op.in[j]].reg));
    
                        mask &=~ R2Mask(ops[op.in[j]].reg);
                    } else continue;
                    
                    // is there only a single register left?
                    if(!(mask & (mask - 1)))
                    {
                        unsigned r = 0, s = 64;
                        while(s >>= 1) if(mask & ~(R2Mask(s)-1))
                            { r += s; mask >>= s; }
                        if(ra_debug) BJIT_LOG("last reg reg: %s\n", regName(r));
                        return r;
                    }
                }

                // prefer registers we're not going to lose soon
                if(mask &~ op.regsLost()) mask &=~ op.regsLost();
                else if(val != noVal)
                {
                    if(ra_debug) BJIT_LOG("losing all before %04x used\n", val);
                    if(ra_debug) BJIT_LOG("%08x and lost %08x\n",
                        (uint32_t)mask, (uint32_t)op.regsLost());
                    return regs::nregs;
                }
                else break; // doesn't matter what we pick

                // if we're just looking for best free, break when we find
                // the next use as anything valid at that point is fine
                if(foundUse) break;
            }

            // FIXME: check live-out set?

            // assume the rest are equally bad
            // unless we can find a constant
            int anyValid = regs::nregs;
            for(int r = 0; r < regs::nregs; ++r)
            {
                if(R2Mask(r) & mask)
                {
                    if(regstate[r] == noVal) return r;
                    
                    anyValid = r;
                    if(!ops[regstate[r]].nInputs())
                    {
                        if(ra_debug)
                            BJIT_LOG("drop %04x in %s (constant)\n",
                                regstate[r], regName(r));
                        return r;
                    }
                }
            }

            BJIT_ASSERT_MORE(anyValid != regs::none);
            return anyValid;
        };
        
        RegMask keepIn = 0;
        RegMask usedRegsBlock = 0;
        
        for(int c = 0; c < blocks[b].code.size(); ++c)
        {
            auto opIndex = blocks[b].code[c];
            auto & op = ops[opIndex];

            if(ra_debug) if(codeOut.size()) debugOp(codeOut.back());

            rename(op);

            if(op.opcode <= ops::jmp)
            for(int k = 0; k < 2; ++k)
            {
                if(k && op.opcode == ops::jmp) break;

                for(auto & s : blocks[op.label[k]].alts)
                for(auto & r : rename.map)
                {
                    if(s.src == b && s.val == r.src)
                    {
                        s.val = r.dst;
                    }
                }
            }

            // copy - we'll fix this below if necessary
            codeOut.push_back(blocks[b].code[c]);

            // try to keep IVs in the correct register
            for(int i = op.nInputs(); i--;)
            {
                auto & a = ops[op.in[i]];

                // sanity check other inputs
                bool bad = false;
                for(int j = i; j--;)
                {
                    if(op.in[j] != op.in[i]) continue;
                    bad = true;
                }
                
                if(!bad && a.opcode == ops::phi && a.iv == opIndex
                && a.nUse > 1 && regstate[a.reg] == op.in[i])
                {
                    auto s = findBest(a.regsMask(), regs::nregs, c+1, op.in[i]);

                    // if we can't find a good reg, then bail out
                    if(s == regs::nregs || s == a.reg) break;

                    if(ra_debug) BJIT_LOG("; saving IV %04x (%s -> %s) \n",
                            op.in[i], regName(a.reg), regName(s));
                    uint16_t sr = newOp(ops::rename, a.flags.type, b);
                    
                    ops[sr].in[0] = op.in[i];
                    ops[sr].reg = s;
                    ops[sr].scc = a.scc;
                    regstate[s] = sr;
                    usedRegsBlock |= R2Mask(s);

                    ops[sr].nUse = a.nUse - 1;
                    a.nUse = 1;
                    
                    rename.add(op.in[i], sr);
                    // do NOT rename this op
                    if(ra_debug) debugOp(sr);
                    
                    std::swap(codeOut.back(), sr);
                    codeOut.push_back(sr);
                }
            }

            // Do we have the inputs we need?
            for(int i = 0; i < op.nInputs(); ++i)
            {
                auto mask = op.regsIn(i);

                // avoid putting multiple inputs in the same register
                for(int j = i; j--;)
                    if(op.in[i] != op.in[j])
                        mask &=~R2Mask(ops[op.in[j]].reg);

                // if this is a local phi, then try to patch?
                if(ops[op.in[i]].opcode == ops::phi
                && ops[op.in[i]].block == b
                && ops[op.in[i]].reg == regs::nregs)
                {
                    for(int r = 0; r < regs::nregs; ++r)
                    {
                        if((R2Mask(r) & (mask &~usedRegsBlock))
                        && regstate[r] == noVal)
                        {
                            regstate[r] = op.in[i];
                            ops[op.in[i]].reg = r;
                            usedRegsBlock |= R2Mask(r);
                            break;
                        }
                    }
                }

                int wr = ops[op.in[i]].reg;
                if(regstate[wr] != op.in[i])
                {
                    // if value is not in it's original register
                    // then we need to mask for the allowable regs
                    // because the op itself might allow special too
                    wr = regs::nregs;
                    mask &= ops[op.in[i]].regsMask();
                }
                else if(blocks[b].regsIn[wr] == op.in[i]) keepIn |= R2Mask(wr);
                
                int r = findBest(mask, wr, c);

                if(wr != regs::nregs && r != wr)
                {
                    if(ra_debug) BJIT_LOG("; need rename for %04x (%s -> %s) \n",
                        op.in[i], regName(ops[op.in[i]].reg), regName(r));

                    // should we try to save existing?
                    // CSE + no inputs is a constant, which we can always remat
                    if(regstate[r] != noVal
                    && (ops[regstate[r]].nInputs() || !ops[regstate[r]].canCSE())
                    )
                    {
                        RegMask smask = ops[regstate[r]].regsMask();

                        // findBest should return current if no better
                        // but play safe and explicitly disallow lost regs
                        //
                        // also disallow regs that we are going to lose "soon"
                        // (eg. because we're preparing for a function call)
                        //
                        // FIXME: ideally findBest() should handle this by
                        // not considering registers that are lost before the
                        // first use for the value we're trying to save
                        smask &=~ op.regsLost();
                            
                        // if we're moving 2nd operand to make room for
                        // the first then also take that mask into account
                        if(!i && op.nInputs()>1 && regstate[r] == op.in[1])
                        {
                            smask &= op.regsIn(1);
                            // but not r
                            smask &=~R2Mask(r);
                        }
                        // if the best choice after this op is r
                        // then we're going to lose this no matter what
                        int s = findBest(smask, regs::nregs, c+1, regstate[r]);
                        if(s != regs::nregs && s != r && s != wr)
                        {
                            if(ra_debug) BJIT_LOG("; saving %04x (%s -> %s) \n",
                                    regstate[r], regName(r), regName(s));
                            uint16_t sr = newOp(ops::rename,
                                ops[regstate[r]].flags.type, b);
                            
                            ops[sr].in[0] = regstate[r];
                            ops[sr].reg = s;
                            ops[sr].scc = ops[regstate[r]].scc;
                            regstate[s] = sr;
                            usedRegsBlock |= R2Mask(s);

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
                                BJIT_LOG("; could not save %04x\n", regstate[r]);
                        }
                    }

                    // was this the last op we just output?
                    // try to patch it in case it's something simple
                    if(op.in[i] == (codeOut.size() > 1
                        ? codeOut[codeOut.size()-2] : noVal)
                    && (ops[op.in[i]].nInputs()<2
                        || ops[ops[op.in[i]].in[1]].reg != r)
                    && (ops[op.in[i]].regsOut() & R2Mask(r)))
                    {
                        if(ra_debug) BJIT_LOG("; Can patch...\n");
                        regstate[ops[op.in[i]].reg] = noVal;
                        ops[op.in[i]].reg = r;
                        regstate[r] = op.in[i];
                    }
                    // if it's not a constant..
                    else if(!ops[op.in[i]].canCSE() || ops[op.in[i]].nInputs())
                    {
                        if(ra_debug)
                        {
                            BJIT_LOG("; Can't patch: ");
                            if(op.in[i] != (codeOut.size() > 1
                                ? codeOut[codeOut.size()-2] : noVal))
                                BJIT_LOG("last %04x\n",
                                    (codeOut.size() > 1
                                    ? codeOut[codeOut.size()-2] : noVal));
                            if(!((ops[op.in[i]].nInputs()<2
                            || ops[ops[op.in[i]].in[1]].reg != r)))
                                BJIT_LOG("in[1] reg\n");
                            if(!(ops[op.in[i]].regsOut() & R2Mask(r)))
                                BJIT_LOG("out mask\n");
                        }
                        uint16_t rr = newOp(ops::rename, ops[op.in[0]].flags.type, b);
                        
                        ops[rr].in[0] = op.in[i];
                        ops[rr].reg = r;
                        regstate[r] = rr;
                        usedRegsBlock |= R2Mask(r);

                        // we're satisfying constraints, so don't throw the
                        // original away (often the renamed one will be lost)
                        ops[rr].nUse = 1;
                        if(!--ops[op.in[i]].nUse)
                            regstate[ops[op.in[i]].reg] = noVal;

                        // only rename this op locally here
                        op.in[i] = rr;
                        if(ra_debug) debugOp(rr);
                        
                        std::swap(codeOut.back(), rr);
                        codeOut.push_back(rr);
                    }
                }

                if(regstate[r] != op.in[i])
                {
                    auto in = op.in[i];
                        
                    // undo renames/reloads before reloading
                    // we don't want to flag those for spill
                    while(ops[in].opcode == ops::rename
                    || ops[in].opcode == ops::reload)
                    {
                        // do not follow renames to a different SCC
                        if(ops[ops[in].in[0]].scc != ops[op.in[i]].scc) break;

                        in = ops[in].in[0];
                    }
                    
                    if(ra_debug) BJIT_LOG("; need reload for %04x = %04x into %s",
                        op.in[i], in, regName(r));

                #if 0
                    auto & rop = ops[in];
                    uint16_t rr = newOp(ops::reload, rop.flags.type, b);

                    // can we rematerialize?
                    bool canRemat = false;
                #else
                    // can we rematerialize?
                    bool canRemat = false;

                    // back-trace degenerate phis..
                    //
                    // FIXME: this is bit of hack and we should probably
                    // also see if we can backtrace the arguments too
                    auto ropi = in;
                    while(blocks[ops[ropi].block].comeFrom.size() == 1
                    && ops[ropi].opcode == ops::phi)
                    {
                        for(auto & a : blocks[ops[ropi].block].alts)
                        {
                            if(a.phi == ropi)
                            {
                                ropi = a.val;
                                break; // inner loop
                            }
                        }
                    }
                    
                    auto & rop = ops[ropi];
                    uint16_t rr = newOp(ops::reload, rop.flags.type, b);
                #endif
                
                    // we can remat ops where CSE is valid and inputs are intact
                    // but don't bother if the op is marked for sideFX even if
                    // "unsafeOpt" because it's probably a division that's expensive
                    //
                    // we don't try to deal with renames here, we'll just give up
                    // if the inputs are not in their original locations anymore
                    if(rop.canCSE() && !rop.hasSideFX()
                    && (!rop.hasMemTag() || rop.memtag == memtag))
                    {
                        canRemat = true;
                        // check input validity
                        for(int j = 0; j < rop.nInputs(); ++j)
                        {
                            if(regstate[ops[rop.in[j]].reg] == rop.in[j]) continue;
                            
                            canRemat = false;
                            break;
                        }
                    }

                    if(canRemat)
                    {
                        if(ra_debug) BJIT_LOG(" - rematerialized\n");
                        ops[rr].opcode = rop.opcode;
                        ops[rr].i64 = rop.i64;
                        ops[rr].scc = rop.scc;

                        // if we remat a phi, it resolves to constant
                        // and there's no need to spill it
                        //
                        // FIXME: this is bit of a hack
                        if(ops[in].opcode == ops::phi)
                        {
                            ops[in].flags.spill = false;
                        }
                    }
                    else
                    {
                        if(ra_debug) BJIT_LOG(" - reloaded\n");
                        ops[in].flags.spill = true;
                        ops[rr].in[0] = in;
                        ops[rr].scc = ops[in].scc;
                    }

                    ops[rr].reg = r;
                    regstate[r] = rr;
                    usedRegsBlock |= R2Mask(r);
                    ops[rr].nUse = rop.nUse;

                    rename.add(op.in[i], rr);
                    rename(op);

                    std::swap(codeOut.back(), rr);
                    codeOut.push_back(rr);
                }
            }

            // sanity check that we got the renames right
            for(int i = 0; i < op.nInputs(); ++i)
            {
                BJIT_ASSERT_MORE(regstate[ops[op.in[i]].reg] == op.in[i]);
            }

            // preferential output reg
            int prefer = regs::nregs;
            
            // check to free once all inputs are done
            for(int i = 0; i < op.nInputs(); ++i)
            {
                if(!--ops[op.in[i]].nUse)
                {
                    if(ra_debug) BJIT_LOG("last use %04x (%s)\n",
                        op.in[i], regName(ops[op.in[i]].reg));
                    if(regstate[ops[op.in[i]].reg] == op.in[i])
                    {
                        regstate[ops[op.in[i]].reg] = noVal;
                        if(op.opcode == ops::rename
                        || (!i && !arch_explicit_output_regs))
                            prefer = ops[op.in[i]].reg;
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
                    notlost &=~R2Mask(ops[op.in[i]].reg);
                }
                
                for(int r = 0; r < regs::nregs; ++r)
                {
                    usedRegsBlock |= R2Mask(r);
                    if(regstate[r] == noVal || R2Mask(r)&~lost) continue;

                    // if this is a constant, then don't save (can always remat)
                    if(ops[regstate[r]].canCSE() && !ops[regstate[r]].nInputs())
                    {
                        regstate[r] = noVal;
                        continue;
                    }

                    // scan from current op, don't wanna overwrite inputs
                    int s = findBest(notlost & ops[regstate[r]].regsMask(),
                        regs::nregs, c, regstate[r]);
    
                    if(s < regs::nregs)
                    {
                        if(ra_debug) BJIT_LOG("; saving lost %04x (%s -> %s) \n",
                                regstate[r], regName(r), regName(s));
                    
                        uint16_t sr = newOp(ops::rename,
                            ops[regstate[r]].flags.type, b);
                        
                        ops[sr].in[0] = regstate[r];
                        ops[sr].reg = s;
                        ops[sr].scc = ops[regstate[r]].scc;
                        regstate[s] = sr;
    
                        ops[sr].nUse = ops[regstate[r]].nUse;
                        
                        rename.add(regstate[r], sr);
                        // do NOT rename the current op here
                        if(ra_debug) debugOp(sr);
                        
                        std::swap(codeOut.back(), sr);
                        codeOut.push_back(sr);
    
                    }
                    else if(ra_debug) BJIT_LOG("; could not save lost v%04x (%s)\n",
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
                        if(ra_debug)
                            BJIT_LOG("passing %04x in %s to %d\n",
                                regstate[i], regName(i), op.label[0]);
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
                        if(ra_debug)
                            BJIT_LOG("passing %04x in %s to %d\n",
                                    regstate[i], regName(i), op.label[1]);
                        blocks[op.label[1]].regsIn[i] = regstate[i];
                    }
                }
            }

            if(!op.hasOutput()) continue;

            // do we need to bump memtag?
            if(op.hasSideFX() && (!unsafeOpt || !op.canCSE())) memtag = opIndex;

            if(op.opcode == ops::phi)
            {
                for(auto & a : blocks[b].alts)
                {
                    if(a.phi != opIndex) continue;
                    
                    if(op.reg == regs::nregs)
                    for(int r = 0; r < regs::nregs; ++r)
                    {
                        uint16_t v = regstate[r];
                        
                        while(v != noVal && v != a.val
                        && (ops[v].opcode == ops::rename
                            || ops[v].opcode == ops::reload))
                        {
                            v = ops[v].in[0];
                        }
                    
                        if(v != a.val) continue;
                        op.reg = r;
                        break;
                    }

                    // cleanup other alternatives
                    for(int r = 0; r < regs::nregs; ++r)
                    {
                        if(regstate[r] == a.val) regstate[r] = noVal;
                    }
                }

                if(op.reg < regs::nregs)
                {
                    // store this as incoming register
                    blocks[b].regsIn[op.reg]
                        = regstate[op.reg] = blocks[b].code[c];
                    keepIn |= R2Mask(op.reg);
                }
                
                // never forcibly allocate a register to phi
                continue;
            }

            RegMask mask = op.regsOut();

            // try to mask second operand if possible
            // if the ISA globs the first register
            if(!arch_explicit_output_regs && !op.anyOutReg()
            && op.nInputs()>1 && op.in[0] != op.in[1]
            && (mask &~R2Mask(ops[op.in[1]].reg)))
                mask &=~R2Mask(ops[op.in[1]].reg);

            op.reg = findBest(mask, prefer, c+1);

            BJIT_ASSERT(op.reg < regs::nregs);
            regstate[op.reg] = opIndex; // blocks[b].code[c];
            usedRegsBlock |= R2Mask(op.reg);
        }

        // force spills on phi's without registers
        for(auto & a : blocks[b].args)
        {
            // These happen because opt-jump is lazy
            if(a.phiop == noVal) continue;
            if(ops[a.phiop].reg == regs::nregs)
            {
                ops[a.phiop].flags.spill = true;
            }
        }

        std::swap(blocks[b].code, codeOut);
        codeOut.clear();
        for(int i = 0; i < regs::nregs; ++i)
        {
            if(!(keepIn&R2Mask(i)))
            {
                if(ra_debug && blocks[b].regsIn[i] != noVal)
                    BJIT_LOG("drop %04x in %s - not needed?\n",
                            blocks[b].regsIn[i], regName(i));
                blocks[b].regsIn[i] = noVal;
            }
            else
            {
                if(blocks[b].regsIn[i] != noVal
                && ops[blocks[b].regsIn[i]].block == b)
                {
                    // this happens with phis
                    if(ra_debug)
                        BJIT_LOG("drop %04x in %s - def in block\n",
                            blocks[b].regsIn[i], regName(i));
                    blocks[b].regsIn[i] = noVal;
                }
                else if(ra_debug)
                    BJIT_LOG("keep %04x in %s\n", blocks[b].regsIn[i], regName(i));
            }
        }
    }

    // do NOT DCE here, we need to keep "useless" phi's for shuffling
    if(ra_debug) debug();

    BJIT_LOG(" RA:JMP");

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

            impl::Rename rename;  // for correcting target PHIs

            if(ra_debug) BJIT_LOG("Args L%d (L%d)-> L%d\n", b, out, target);

            for(auto & s : blocks[target].alts)
            {
                if(s.src != b) continue;

                // if we need a spill, can we do a trivial one?
                if(ops[s.phi].flags.spill
                && ops[s.phi].reg == regs::nregs)
                {
                    // if both are spilled same SCC then it's fine
                    if(ops[s.val].scc == ops[s.phi].scc) continue;
                     
                    // don't spill phi-to-phi, this'll cause
                    // problems with scc
                    if(!ops[s.val].flags.spill
                    && ops[s.val].opcode != ops::phi)
                    {
                        ops[s.val].flags.spill = true;
                        ops[s.val].scc = ops[s.phi].scc;
                    }
                    else
                    {
                        BJIT_LOG("PHI LOOP: phi %04x, Broken SCCs?\n", s.phi);
                        BJIT_ASSERT(false);
                    }
                }
                
                if(ops[s.phi].reg == regs::nregs) continue;
                tregs[ops[s.phi].reg] = s.val;
            }
            
            // this can be a bit too spammy to put into ra_debug
            if(0) for(int r = 0; r < regs::nregs; ++r)
            {
                if(tregs[r] == noVal && sregs[r] == noVal) continue;

                BJIT_LOG(" before clean: %s src: %04x dst: %04x\n",
                    regName(r), sregs[r], tregs[r]);
            }

            // throw away source registers that are not needed by target
            for(int s = 0; s < regs::nregs; ++s)
            {
                if(sregs[s] == noVal) continue;
                
                bool found = false;
                for(int t = 0; t < regs::nregs; ++t)
                {
                    if(tregs[t] != sregs[s]) continue;
                    found = true;
                    break;
                }

                if(!found) sregs[s] = noVal;
            }

            // this can be a bit too spammy to put into ra_debug
            if(0) for(int r = 0; r < regs::nregs; ++r)
            {
                if(tregs[r] == noVal && sregs[r] == noVal) continue;

                BJIT_LOG(" after clean: %s src: %04x dst: %04x\n",
                    regName(r), sregs[r], tregs[r]);
            }

            bool done = false;
            while(!done)
            {
                if(ra_debug) BJIT_LOG("Shuffle L%d:\n", b);
                done = true;

                // this can be way too spammy to put into ra_debug
                if(0) for(int r = 0; r < regs::nregs; ++r)
                {
                    if(tregs[r] == noVal && sregs[r] == noVal) continue;

                    BJIT_LOG(" %s src: %04x dst: %04x\n",
                        regName(r), sregs[r], tregs[r]);
                }
                
                // first try: free target moves only
                for(int t = 0; t < regs::nregs; ++t)
                {
                    if(tregs[t] == noVal) continue;
                    if(sregs[t] == tregs[t]) continue;

                    for(int s = 0; s < regs::nregs; ++s)
                    {
                        // can we move without overwriting
                        if(sregs[s] == tregs[t])
                        {
                            if(sregs[t] != noVal) continue;

                            uint16_t rr = newOp(ops::rename,
                                ops[sregs[s]].flags.type, out);
                            
                            if(ra_debug) BJIT_LOG("move: %s:%04x -> %s:%04x\n",
                                regName(s), sregs[s], regName(t), rr);

                            rename.add(sregs[s], rr);

                            ops[rr].reg = t;
                            ops[rr].in[0] = sregs[s];
                            ops[rr].scc = ops[sregs[s]].scc;
                            tregs[t] = rr;
                            sregs[t] = tregs[t];
                            sregs[s] = noVal;

                            std::swap(rr, blocks[out].code.back());
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

                            // if source register is free in target
                            // then we don't need to move it
                            if(tregs[s] == noVal) continue;

                            // try to find a free source register
                            // that is also a free target register
                            int r = 0;
                            for(;r < regs::nregs;++r)
                            {
                                if((R2Mask(r)&mask)
                                && sregs[r] == noVal
                                && tregs[r] == noVal) break;
                            }
                            
                            // if no free regs, just force correct register
                            // so we break the cycle through memory
                            if(r == regs::nregs) r = t;

                            uint16_t rr = newOp(ops::rename,
                                ops[sregs[s]].flags.type, out);
                            
                            if(ra_debug) BJIT_LOG(
                                "move: %s:%04x -> %s:%04x, cycle breaker: %04x\n",
                                regName(s), sregs[s], regName(r), rr, sregs[r]);

                            rename.add(sregs[s], rr);
                            
                            ops[rr].reg = r;
                            ops[rr].in[0] = sregs[s];
                            ops[rr].scc = ops[sregs[s]].scc;
                            tregs[t] = rr;
                            sregs[r] = tregs[t];
                            sregs[s] = noVal;
    
                            std::swap(rr, blocks[out].code.back());
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
                    //BJIT_ASSERT(sregs[t] == noVal);
                    
                    if(ra_debug) BJIT_LOG("reload -> %s:%04x (%04x)",
                            regName(t), tregs[t], sregs[t]);

                    // can we rematerialize?
                    bool canRemat = false;

                    // back-trace degenerate phis..
                    // FIXME: this is bit of hack and we should probably
                    // also see if we can backtrace the arguments too
                    auto ropi = tregs[t];
                    while(blocks[ops[ropi].block].comeFrom.size() == 1
                    && ops[ropi].opcode == ops::phi)
                    {
                        for(auto & a : blocks[ops[ropi].block].alts)
                        {
                            if(a.phi == ropi)
                            {
                                ropi = a.val;
                                break; // inner loop
                            }
                        }
                    }
                    
                    auto & rop = ops[ropi];
                    uint16_t rr = newOp(ops::reload, rop.flags.type, out);

                    // same logic as main reg alloc, except we use memout
                    // as there's no sideFX in shuffle blocks
                    if(rop.canCSE() && !rop.hasSideFX()
                    && (R2Mask(t) & rop.regsOut())
                    && (!rop.hasMemTag() || rop.memtag == blocks[b].memout))
                    {
                        canRemat = true;
                        // check input validity
                        for(int j = 0; j < rop.nInputs(); ++j)
                        {
                            // FIXME: is sregs correct here?
                            if(sregs[ops[rop.in[j]].reg] == rop.in[j]) continue;
                            canRemat = false;
                            break;
                        }
                    }

                    if(canRemat)
                    {
                        if(ra_debug) BJIT_LOG(" - rematerialized\n");
                        ops[rr].opcode = rop.opcode;
                        ops[rr].i64 = rop.i64;
                        ops[rr].reg = t;

                        // if we remat a phi, it resolves to constant
                        // and there's no need to spill it
                        //
                        // FIXME: this is bit of a hack
                        if(ops[tregs[t]].opcode == ops::phi)
                        {
                            ops[tregs[t]].flags.spill = false;
                        }
                    }
                    else
                    {
                        if(ra_debug) BJIT_LOG(" - reloaded\n");
                        ops[rr].reg = t;
                        ops[rr].in[0] = tregs[t];
                        ops[tregs[t]].flags.spill = true;
                    }
                    
                    ops[rr].scc = rop.scc;
                    sregs[t] = tregs[t];
                    rename.add(tregs[t], rr);
                    
                    std::swap(rr, blocks[out].code.back());
                    blocks[out].code.push_back(rr);
                }
            }

            // rename target PHI values (but not block, we do it below)
            for(auto & s : blocks[target].alts)
            for(auto & r : rename.map)
            {
                if(s.val == r.src) s.val = r.dst;
            }
            
            memcpy(blocks[out].regsOut, sregs, sizeof(sregs));
        };

        // make a copy, since we'll be adding new ops
        const auto op = ops[blocks[b].code.back()];

        // is this a return?
        if(op.opcode > ops::jmp) continue;

        // FIXME: edges are never critical anymore, so do we really need this?
        if(op.opcode < ops::jmp)
        {
            // create some shuffle blocks
            // try to fix enough stuff for debug() to be happy
            int b0 = blocks.size();
            int b1 = blocks.size() + 1;
            blocks.resize(blocks.size() + 2);

            blocks[b0].code.push_back(newOp(ops::jmp, Op::_none, b0));
            ops[blocks[b0].code.back()].label[0] = op.label[0];
            memcpy(blocks[b0].regsIn, blocks[b].regsOut, sizeof(sregs));
            jumpShuffle(b0, op.label[0]);

            // keep shuffle-block if we added shuffles
            if(blocks[b0].code.size() > 1)
            {
                // fix target comeFrom, satisfy sanity
                for(auto & cf : blocks[op.label[0]].comeFrom) if(cf == b) cf = b0;
                
                // fix dominators, satisfy sanity
                // DCE won't rebuild, 'cos nothing dead
                if(fix_sanity)
                {
                    blocks[b0].dom = blocks[b].dom;
                    blocks[b0].dom.push_back(b1);
                    blocks[b0].idom = b;
                    blocks[b0].pdom = blocks[b].pdom;
                    blocks[b0].comeFrom.push_back(b);
                    blocks[b0].flags.live = true;
                
                    // rename target PHI sources, satisfy sanity
                    for(auto & s : blocks[op.label[0]].alts)
                    {
                        if(ops[s.val].block == b0) s.src = b0;
                    }
                }
                
                ops[blocks[b].code.back()].label[0] = b0;
                newBlocks.push_back(b0);
            }

            blocks[b1].code.push_back(newOp(ops::jmp, Op::_none, b1));
            ops[blocks[b1].code.back()].label[0] = op.label[1];
            memcpy(blocks[b1].regsIn, blocks[b].regsOut, sizeof(sregs));
            jumpShuffle(b1, op.label[1]);

            // keep shuffle-block if we added shuffles
            if(blocks[b1].code.size() > 1)
            {
                // fix target comeFrom, satisfy sanity
                for(auto & cf : blocks[op.label[1]].comeFrom) if(cf == b) cf = b1;

                // fix dominators, satisfy sanity
                // DCE won't rebuild, 'cos nothing dead
                if(fix_sanity)
                {
                    blocks[b1].dom = blocks[b].dom;
                    blocks[b1].dom.push_back(b1);
                    blocks[b1].idom = b;
                    blocks[b1].pdom = blocks[b].pdom;
                    blocks[b1].comeFrom.push_back(b);
                    blocks[b1].flags.live = true;
                    
                    // rename target PHI sources, satisfy sanity
                    for(auto & s : blocks[op.label[1]].alts)
                    {
                        if(ops[s.val].block == b1) s.src = b1;
                    }
                }
                
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

    // try to move phi-spills to sources instead
    // this sometimes allows us to eliminate respills in loops
    // if the source op is a reload from the same SCC
    {
        bool done = false;
        while(!done)
        {
            done = true;

            for(auto b : live)
            {

                // check which phis have all their sources from the same SCC
                for(auto & a : blocks[b].alts)
                {
                    // if this phi doesn't spill or can't opt, skip
                    if(!ops[a.phi].flags.spill
                    || ops[a.phi].flags.no_opt) continue;
    
                    // any source SCC that doesn't match prevents optimization
                    if(ops[a.val].scc != ops[a.phi].scc)
                    {
                        ops[a.phi].flags.no_opt = true;
                    }
                }
    
                // move spills to sources
                for(auto & a : blocks[b].alts)
                {
                    if(!ops[a.phi].flags.spill
                    || ops[a.phi].flags.no_opt) continue;
    
                    // we want to iterate if we spill an earlier phi
                    if(ops[a.val].opcode == ops::phi
                    && !ops[a.val].flags.spill)
                    {
                        done = false;
                    }
    
                    ops[a.val].flags.spill = true;
                }
    
                // clear spill-flag on phis we optimized
                // but don't optimize the same phi twice
                for(auto & a : blocks[b].args)
                {
                    if(a.phiop == noVal) continue;
                    if(!ops[a.phiop].flags.no_opt
                    && ops[a.phiop].flags.spill)
                    {
                        ops[a.phiop].flags.spill = false;
                        ops[a.phiop].flags.no_opt = true;
                    }
                }
            }
        }
    }

    // find slots
    std::vector<bool>   sccUsed;
    
    // Cleanup and find slots
    for(auto & op : ops)
    {
        if(!op.hasOutput()) continue;
        
        // clear spill-flag from PHIs with no register
        // the source-site will handle the spill
        if(op.opcode == ops::phi && op.reg == regs::nregs)
            op.flags.spill = false;

        // clear spill-flag on reloads where scc matches
        // this can happen when we reload for a src-spilled phi
        if(op.opcode == ops::reload && op.scc == ops[op.in[0]].scc)
            op.flags.spill = false;

        if(op.scc >= sccUsed.size()) sccUsed.resize(op.scc + 1, false);
        if(op.flags.spill) sccUsed[op.scc] = true;
    }

    std::vector<uint16_t>   slots(sccUsed.size(), 0xffff);
    BJIT_ASSERT(!nSlots);
    for(int s = 0; s < slots.size(); ++s)
        if(sccUsed[s]) slots[s] = nSlots++;

    for(auto & op : ops) if(op.hasOutput()) op.scc = slots[op.scc];

    if(ra_debug) BJIT_LOG("\n");
    rename.map.clear();
    // do a cleanup pass to get rid of renames that just hurt assembler
    for(auto b : live)
    {
        for(auto c : blocks[b].code)
        {
            rename(ops[c]);

            if(ops[c].opcode == ops::rename
            && ops[c].reg == ops[ops[c].in[0]].reg
            && ops[c].scc == noSCC)
            {
                if(ra_debug) BJIT_LOG("Rename is useless:");
                if(ra_debug) debugOp(c);
                rename.add(c, ops[c].in[0]);
                c = noVal;
                continue;
            }

            if(ops[c].opcode <= ops::jmp)
            for(auto & s : blocks[ops[c].label[0]].alts)
            for(auto & r : rename.map)
            {
                if(s.val == r.src && s.src == b) s.val = r.dst;
            }
            
            if(ops[c].opcode < ops::jmp)
            for(auto & s : blocks[ops[c].label[0]].alts)
            for(auto & r : rename.map)
            {
                if(s.val == r.src && s.src == b) s.val = r.dst;
            }
        }
    }
    
    // do a little bit of cleanup in case we accidentally
    // end up renaming constants..
    for(auto b : live)
    {
        for(auto i : blocks[b].code)
        {
            if(ops[i].opcode == ops::rename)
            {
                auto r = ops[i].in[0];
                if(ops[r].opcode == ops::lci
                && ops[r].i64 == 0)
                {
                    ops[i].opcode = ops::lci;
                    ops[i].i64 = 0;
                }
                else
                if(ops[r].opcode == ops::lcf
                && ops[r].f32 == 0)
                {
                    ops[i].opcode = ops::lcf;
                    ops[i].f32 = 0;
                }
                else
                if(ops[r].opcode == ops::lcd
                && ops[r].f64 == 0)
                {
                    ops[i].opcode = ops::lcd;
                    ops[i].f64 = 0;
                }
            }
        }
    }

    if(ra_debug) debug();

    opt_dce();

    raDone = true;
    BJIT_LOG(" DONE\n");
    if(ra_debug) debug();

    // this won't work unless we fixed it :)
    if(fix_sanity) sanity();
}

void Proc::findSCC()
{
    rebuild_livein(); // need live-in registers

    BJIT_ASSERT(!raDone);
    BJIT_LOG(" RA:SCC");
    //debug();

    std::vector<bool>   sccUsed;

    // keep this as sanity check for now, we can remove it later
    for(auto & op : ops) if(op.hasOutput()) BJIT_ASSERT_MORE(op.scc == noSCC);
    
    // livescan live
    for(auto bi : live)
    {
        auto & b = blocks[bi];

        for(auto l : b.livein) ops[l].nUse = 0;
        for(auto c : b.code) if(ops[c].hasOutput()) { ops[c].nUse = 0; }

        findUsesBlock(bi, false, false);

        // assume all classes are free
        for(int i = 0; i < sccUsed.size(); ++i) sccUsed[i] = false;

        // reserve all SCCs of live-in registers
        for(auto in : b.livein)
        {
            // since we're doing live-scan order any live-in
            // variables should already have their SCC allocated
            // unless it's a loop-thru PHI which we fix below
            bool useAfterDefine = (ops[in].scc != noSCC
            || (ops[in].opcode == ops::phi && ops[in].block == bi));
            if(!useAfterDefine)
            {
                BJIT_LOG("\nNo SCC for %04x in L%d\n", in, bi);
                debug();
            }
            BJIT_ASSERT(useAfterDefine);
            // this is just a sanity check
            BJIT_ASSERT_MORE(ops[in].scc < sccUsed.size());
            sccUsed[ops[in].scc] = true;

            if(scc_debug) BJIT_LOG("Live in: %04x:[%04x]\n", in, ops[in].scc);
        }

        for(auto c : b.code)
        {
            // at least one phi-alternative should have SCC
            if(ops[c].opcode == ops::phi)
            {
                for(auto & s : b.alts)
                {
                    if(s.phi != c) continue;
                    
                    // check for validity, pick first possible
                    if(ops[s.val].scc != noSCC && !sccUsed[ops[s.val].scc])
                    {
                        ops[c].scc = ops[s.val].scc;
                        sccUsed[ops[c].scc] = true;
                        break;
                    }
                    else
                    {
                        if(scc_debug) BJIT_LOG("not valid: %04x:[%04x]\n",
                            s.val, ops[s.val].scc);
                    }
                }

                // if we can't find one, then pick a new-one
                if(ops[c].scc == noSCC)
                {
                    if(scc_debug) BJIT_LOG("need new SCC for phi\n");
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
                    BJIT_ASSERT(ops[ops[c].in[i]].scc < sccUsed.size());
                    sccUsed[ops[ops[c].in[i]].scc] = false;
                }
            }
        
            if(ops[c].hasOutput())
            {
                // find a free SCC
                BJIT_ASSERT(ops[c].scc == noSCC);
                
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

    // second pass, break critical edges
    for(int li = 0, liveSz = live.size(); li < liveSz; ++li)
    {
        auto b = live[li];
        auto c = blocks[b].code.back();
        
        if(ops[c].opcode < ops::jmp)
        for(int k = 0; k < 2; ++k)
        {
            if(blocks[ops[c].label[k]].comeFrom.size() < 2) continue;

            ops[c].label[k] = breakEdge(b, ops[c].label[k]);
        }
    }

    // third pass, add renames to invalid jmps
    for(auto b : live)
    {
        BJIT_ASSERT(blocks[b].flags.live);
        
        int nSCC = sccUsed.size();
        
        auto c = blocks[b].code.back();

        auto p = blocks[b].code.size() - 1;
        
        if(ops[c].opcode <= ops::jmp)
        for(int k = 0; k < 2; ++k)
        {
            if(k && ops[c].opcode == ops::jmp) break;
    
            for(auto & s : blocks[ops[c].label[k]].alts)
            {
                if(s.src == b && ops[s.val].scc != ops[s.phi].scc)
                {
                    if(scc_debug)
                        BJIT_LOG("rename jump: %04x:[%04x] -> %04x:[%04x]\n",
                        s.val, ops[s.val].scc, s.phi, ops[s.phi].scc);
    
                    uint16_t rr = newOp(ops::rename, ops[s.val].flags.type, b);
                    // play safe: alloc a fresh one, if we're going to spill
                    // we're most of the time going to have to spill either way
                    ops[rr].scc = nSCC++;
                    ops[rr].in[0] = s.val; s.val = rr;

                    // this should usually result in a better order?
                    blocks[b].code.insert(blocks[b].code.begin() + p, rr);
                }
            }
        }
    }
    
    if(scc_debug) debug();
}

void Proc::findUsedRegs()
{
    BJIT_LOG(" FindRegs");
    usedRegs = 0;
    for(auto b : live)
    {
        for(auto c : blocks[b].code)
        {
            usedRegs |= ops[c].regsLost();
            if(ops[c].hasOutput())
            {
                usedRegs |= R2Mask(ops[c].reg);
            }
        }
    }
}
