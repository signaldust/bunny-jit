
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <inttypes.h>

#include "front-lexer.h"
#include "front-parse.h"

#include "bjit.h"

namespace bjit
{
    struct Type
    {
        enum {
            // typecast logic relies on integers being first
            I8, I16, I32, I64,  // signed integers
            U8, U16, U32, U64,  // unsigned integers

            F32, F64,   // single and double precision floats

            // error type is after numeric types (we rely on this)
            // 
            // we use "error" as a type to indicate that we have already
            // identified a situation with invalid types and no further
            // diagnostic messages are necessary (or useful)
            ERROR,
            
            AUTO,       // needs to be inferred
            VOID,       // no type

            STRUCT
        };
        
        // Platform types as aliases for pointer sized integers
        static const int IPTR = I64;
        static const int UPTR = U64;    // must be last integer type

        static const int BOOL = IPTR;

        // kind is one of the enum types for primitives
        // for structures it's STRUCT+n where n is the index
        // of the structured type definition
        uint16_t    kind = AUTO;
        uint8_t     nptr = 0;   // level of pointer indirection
        uint8_t     flags = 0;  // reserved for const etc

        void debug()
        {
            if(nptr == 1) BJIT_LOG("%d*", nptr);
            
            switch(kind)
            {
            case ERROR: BJIT_LOG("error"); break;
            case AUTO: BJIT_LOG("auto"); break;
            case VOID: BJIT_LOG("void"); break;
            
            case I8: BJIT_LOG("i8"); break;
            case I16: BJIT_LOG("i16"); break;
            case I32: BJIT_LOG("i32"); break;
            case I64: BJIT_LOG("i64"); break;
            
            case U8: BJIT_LOG("u8"); break;
            case U16: BJIT_LOG("u16"); break;
            case U32: BJIT_LOG("u32"); break;
            case U64: BJIT_LOG("u64"); break;

            case F32: BJIT_LOG("f32"); break;
            case F64: BJIT_LOG("f64"); break;

            default: BJIT_LOG("struct-%d", kind - STRUCT); break;
            }
        }

        // we promote all signed integers to iptr
        // we promote all unsigned integers to uptr
        // if signed and unsigned are mixed, type is uptr
        // if floating points are used, type is largest floating point
        static int promoteNumeric(Type & typeA, Type & typeB)
        {
            // no pointers
            if(typeA.nptr || typeB.nptr) return ERROR;

            // no weird stuff
            if(typeA.kind >= ERROR || typeB.kind >= ERROR) return ERROR;
            
            // if either type is double, promote to double
            if(typeA.kind == F64 || typeB.kind == F64) return F64;

            // if either type is single, promote to single
            if(typeA.kind == F32 || typeB.kind == F32) return F32;

            // widen integer types
            if(typeA.kind >= I8 && typeA.kind <= IPTR) typeA.kind = IPTR;
            if(typeA.kind >= U8 && typeA.kind <= UPTR) typeA.kind = UPTR;

            if(typeB.kind >= I8 && typeB.kind <= IPTR) typeB.kind = IPTR;
            if(typeB.kind >= U8 && typeB.kind <= UPTR) typeB.kind = UPTR;

            // if either is unsigned, then result is unsigned
            if(typeA.kind == UPTR || typeB.kind == UPTR) return UPTR;
            // otherwise signed
            return IPTR;
        }
    };
    
    struct Variable
    {
        Symbol *symbol = 0;
        Type    type;

        Variable() {}
        Variable(Symbol * s, Type const & t) : symbol(s), type(t) {}
    };
    typedef std::vector<Variable> Env;

    struct CodeGen
    {
        Proc & proc;

        Label   labelBreak      = { noVal };
        Label   labelContinue   = { noVal };

        int     envBreak;
        int     envContinue;

        CodeGen(Proc & proc) : proc(proc) {}
    };

    // Baseclass for AST nodes
    struct Expr
    {
        typedef std::vector<std::unique_ptr<Expr>> Stack;

        Token   token;
        Type    type;

        Expr(Token const & t) : token(t) {}
        virtual ~Expr() {}

        virtual void typecheck(Parser & ps, Env & env) = 0;
        virtual void debug(int lvl = 0) = 0;
        virtual Value codeGen(CodeGen & cg) = 0;

        // l-values should implement these
        virtual bool canAssign() { return false; }
        virtual Value codeGenAssign(CodeGen & cg, Value v)
        { assert(false); return Value{noVal}; }

        void debugCommon()
        {
            BJIT_LOG("@%d:%d : ", token.posLine, token.posChar);
            type.debug();
        }
    };

    // type conversions
    struct ECast : Expr
    {
        std::unique_ptr<Expr>   v;

        ECast(Token const & t, std::unique_ptr<Expr> & e)
        : Expr(t) { v = std::move(e); }

        void typecheck(Parser & ps, Env & env)
        {
            // if expression already has a concrete type
            // then don't bother checking again
            if(v->type.kind == Type::AUTO) v->typecheck(ps, env);
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(typecast ", lvl, "");
            debugCommon();
            v->debug(lvl+2);
        }

        virtual Value codeGen(CodeGen & cg)
        {
            // we don't handle pointers yet
            assert(!type.nptr && !v->type.nptr);
        
            // we only really truly need to convert floats and ints
            // anything else is effectively a NOP
            auto vv = v->codeGen(cg);
            if(type.kind == Type::F32 || v->type.kind == Type::F32)
            {
                assert(false);  // not supported yet
            }

            // float -> int conversion
            if(type.kind <= Type::UPTR && v->type.kind == Type::F64)
                return cg.proc.cd2i(vv);

            // int -> float conversion
            if(type.kind == Type::F64 && v->type.kind <= Type::UPTR)
                return cg.proc.ci2d(vv);

            // anything else is pass-thru
            return vv;
        }
        
    };

    struct EConst : Expr
    {
        EConst(Token const & t) : Expr(t)
        {

        }

        void typecheck(Parser & ps, Env & env)
        {
            switch(token.type)
            {
            case Token::Tint: type.kind = Type::IPTR; break;
            case Token::Tuint: type.kind = Type::UPTR; break;
            case Token::Tfloat: type.kind = Type::F64; break;
            default: assert(false);
            }
        }

        void debug(int lvl)
        {
            switch(token.type)
            {
            case Token::Tint:
                BJIT_LOG("\n%*si:%" PRId64 " ", lvl, "", token.vInt); break;
            case Token::Tuint:
                BJIT_LOG("\n%*su:%" PRIu64 " ", lvl, "", token.vInt); break;
            case Token::Tfloat: BJIT_LOG("\n%*sf:%#g ", lvl, "", token.vFloat); break;

            default: assert(false);
            }

            debugCommon();
        }

        virtual Value codeGen(CodeGen & cg)
        {
            switch(token.type)
            {
            case Token::Tint:
            case Token::Tuint:
                return cg.proc.lci(token.vInt);
            
            case Token::Tfloat:
                return cg.proc.lcd(token.vFloat);
                
            default: assert(false); return Value{noVal};
            }
        }
    };

    // this always refers to an existing variable
    struct ESymbol : Expr
    {
        unsigned envIndex = 0;    // index in environment
        
        ESymbol(Token const & t) : Expr(t) {}

        void typecheck(Parser & ps, Env & env)
        {
            for(envIndex = env.size(); envIndex--;)
            {
                if(env[envIndex].symbol == token.symbol)
                {
                    type = env[envIndex].type;
                    return;
                }
            }
            ps.errorAt(token, "undefined variable");
            type.kind = Type::ERROR;
        }
        
        void debug(int lvl)
        {
            BJIT_LOG("\n%*ssym:%p:%s/%d ", lvl, "",
                token.symbol, token.symbol->string.data(), envIndex);
            debugCommon();
        }

        virtual Value codeGen(CodeGen & cg)
        {
            return cg.proc.env[envIndex];
        }

        virtual bool canAssign() { return true; }

        virtual Value codeGenAssign(CodeGen & cg, Value v)
        {
            return (cg.proc.env[envIndex] = v);
        }
    };
    
    struct EReturn : Expr
    {
        std::unique_ptr<Expr>   v;

        EReturn(Token const & t, Stack & s)
        : Expr(t) { v = std::move(s.back()); s.pop_back(); }

        void typecheck(Parser & ps, Env & env)
        {
            v->typecheck(ps, env);
            type = v->type;
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(return ", lvl, "");
            debugCommon();
            v->debug(lvl+2);
        }

        virtual Value codeGen(CodeGen & cg)
        {
            if(!type.nptr && type.kind == Type::F64)
                cg.proc.dret(v->codeGen(cg));
            else cg.proc.iret(v->codeGen(cg));
            return Value{noVal};
        }
        
    };

    struct ECall : Expr
    {
        std::unique_ptr<Expr>   fn;
        Stack args;

        ECall(Token const & t, Stack & s) : Expr(t)
        {
            args.resize(token.nArgs);
            for(int i = 0; i < token.nArgs; ++i)
            {
                args[i] = std::move(s[s.size() - token.nArgs + i]);
            }
            s.resize(s.size() - token.nArgs);
            
            fn = std::move(s.back()); s.pop_back();
        }
        
        void typecheck(Parser & ps, Env & env)
        {
            fn->typecheck(ps, env);
            for(auto & a : args) a->typecheck(ps, env);
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(call ", lvl, "");
            fn->debug(lvl+2);
            // indent arguments by two levels
            for(auto & a : args) a->debug(lvl+4);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            Value p = fn->codeGen(cg);
            for(auto & a : args) cg.proc.env.push_back(a->codeGen(cg));
            auto r = cg.proc.icallp(p, args.size());
            cg.proc.env.resize(cg.proc.env.size() - args.size());
            return r;
        }
    };

    struct EBlock : Expr
    {
        Stack   body;
        
        EBlock(Token const & t, Stack & s) : Expr(t)
        {
            body.resize(token.nArgs);
            for(int i = 0; i < token.nArgs; ++i)
            {
                body[i] = std::move(s[s.size() - token.nArgs + i]);
            }
            s.resize(s.size() - token.nArgs);
        }
        
        void typecheck(Parser & ps, Env & env)
        {
            auto es = env.size();   // block-scope save
            for(auto & e : body) e->typecheck(ps, env);
            env.resize(es);         // block-scope restore
            type.kind = Type::VOID;
        }
        
        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(block ", lvl, "");
            for(auto & a : body) a->debug(lvl+2);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            unsigned sz = cg.proc.env.size();
            for(auto & a : body) a->codeGen(cg);
            cg.proc.env.resize(sz);
            return Value{noVal};
        }

    };

    struct EIf : Expr
    {
        std::unique_ptr<Expr>   condition;
        std::unique_ptr<Expr>   sThen, sElse;

        EIf(Token const & t, Stack & s, bool hasElse) : Expr(t)
        {
            if(hasElse)
            {
                sElse = std::move(s.back()); s.pop_back();
            }
            sThen = std::move(s.back()); s.pop_back();
            condition = std::move(s.back()); s.pop_back();
        }
        
        void typecheck(Parser & ps, Env & env)
        {
            type.kind = Type::VOID;
            
            // FIXME: allow definitions in condition?
            auto ec = env.size();   // block-scope save
            condition->typecheck(ps, env);
            if(!condition->type.nptr && condition->type.kind > Type::UPTR)
            {
                ps.errorAt(condition->token,
                    "cannot convert floating point to a truth value");
                type.kind = Type::ERROR;
            }
            auto es = env.size();   // block-scope save
            sThen->typecheck(ps, env);
            if(sElse)
            {
                env.resize(es);     // block-scope restore
                sElse->typecheck(ps, env);
            }
            env.resize(ec);         // block-scope restore
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(if ", lvl, "");
            debugCommon();
            condition->debug(lvl+4);    // two level indent
            sThen->debug(lvl+2);
            if(sElse) sElse->debug(lvl+2);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            // do this before label creation, so we get scope
            auto ec = cg.proc.env.size();   // block-scope save
            auto cc = condition->codeGen(cg);

            auto lThen = cg.proc.newLabel();
            auto lElse = cg.proc.newLabel();
            auto lDone = cg.proc.newLabel();
            
            cg.proc.jz(cc, lElse, lThen);
            
            cg.proc.emitLabel(lThen);
            sThen->codeGen(cg);
            cg.proc.env.resize(ec);
            cg.proc.jmp(lDone);
            
            cg.proc.emitLabel(lElse);
            if(sElse)
            {
                sElse->codeGen(cg);
            }
            cg.proc.jmp(lDone);
            cg.proc.emitLabel(lDone);
            cg.proc.env.resize(ec);
            return Value{noVal};
        }
        
    };

    struct EWhile : Expr
    {
        std::unique_ptr<Expr>   condition;
        std::unique_ptr<Expr>   body;

        EWhile(Token const & t, Stack & s) : Expr(t)
        {
            body = std::move(s.back()); s.pop_back();
            condition = std::move(s.back()); s.pop_back();
        }

        void typecheck(Parser & ps, Env & env)
        {
            type.kind = Type::VOID;
            
            auto ec = env.size();   // block-scope save
            condition->typecheck(ps, env);
            if(!condition->type.nptr && condition->type.kind > Type::UPTR)
            {
                ps.errorAt(condition->token,
                    "cannot convert floating point to a truth value");
                type.kind = Type::ERROR;
            }
            body->typecheck(ps, env);
            env.resize(ec);         // block-scope restore
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(while ", lvl, "");
            debugCommon();
            condition->debug(lvl+4);    // two level indent
            body->debug(lvl+2);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            auto ec = cg.proc.env.size();
            auto lTest = cg.proc.newLabel();

            cg.proc.jmp(lTest);
            cg.proc.emitLabel(lTest);
            
            auto oldEnvContinue = cg.envContinue;
            auto oldLabelContinue = cg.labelContinue;
            cg.envContinue = cg.proc.env.size();
            cg.labelContinue = lTest;
            
            auto cc = condition->codeGen(cg);
            
            auto lBody = cg.proc.newLabel();
            auto lDone = cg.proc.newLabel();

            auto oldEnvBreak = cg.envBreak;
            auto oldLabelBreak = cg.labelBreak;
            cg.envBreak = cg.proc.env.size();
            cg.labelBreak = lDone;
            
            cg.proc.jz(cc, lDone, lBody);

            cg.proc.emitLabel(lBody);
            body->codeGen(cg);
            cg.proc.env.resize(ec);
            cg.proc.jmp(lTest);
            
            cg.proc.emitLabel(lDone);
            cg.proc.env.resize(ec);

            cg.envBreak = oldEnvBreak;
            cg.labelBreak = oldLabelBreak;

            cg.envContinue = oldEnvContinue;
            cg.labelContinue = oldLabelContinue;
            
            return Value{noVal};
        }
    };

    struct EBreak : Expr
    {
        EBreak(Token const & t) : Expr(t)
        {
        }

        void typecheck(Parser & ps, Env & env)
        {
            type.kind = Type::VOID;
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(break ", lvl, "");
            debugCommon();
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            // to deal with deadcode that might follow
            // we generate a new label, resize env, jump
            // then emit the newly created label to restore
            // a context that any following expressions expect
            auto l = cg.proc.newLabel();

            cg.proc.env.resize(cg.envBreak);
            cg.proc.jmp(cg.labelBreak);

            cg.proc.emitLabel(l);
        
            return Value{noVal};
        }
    };
    
    struct EContinue : Expr
    {
        EContinue(Token const & t) : Expr(t)
        {
        }

        void typecheck(Parser & ps, Env & env)
        {
            type.kind = Type::VOID;
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(continue ", lvl, "");
            debugCommon();
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            // see break
            auto l = cg.proc.newLabel();

            cg.proc.env.resize(cg.envContinue);
            cg.proc.jmp(cg.labelContinue);

            cg.proc.emitLabel(l);
            return Value{noVal};
        }
    };
    
    struct EDefine : Expr
    {
        std::unique_ptr<Expr>   sym;
        std::unique_ptr<Expr>   value;

        unsigned envIndex;
        
        EDefine(Token const & t, Stack & s) : Expr(t)
        {
            value = std::move(s.back()); s.pop_back();
            sym = std::move(s.back()); s.pop_back();
            assert(sym->token.type == Token::Tsymbol);
        }
        
        void typecheck(Parser & ps, Env & env)
        {
            value->typecheck(ps, env);
            type = sym->type = value->type;
            envIndex = env.size();
            env.emplace_back(token.symbol, type);
        }

        void debug(int lvl)
        {
            BJIT_LOG("\n%*s(def:%p:%s/%d ", lvl, "",
                token.symbol, token.symbol->string.data(), envIndex);
            debugCommon();
            value->debug(lvl+2);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            cg.proc.env.push_back(value->codeGen(cg));
            return cg.proc.env.back();
        }
        
    };

    struct EUnary : Expr
    {
        std::unique_ptr<Expr>   a;

        EUnary(Token const & t, Stack & s) : Expr(t)
        {
            a = std::move(s.back()); s.pop_back();
        }
        
        void typecheck(Parser & ps, Env & env)
        {
            a->typecheck(ps, env);
            switch(token.type)
            {
            case Token::TbitNot:
                {
                    type.kind = a->type.kind;
                    // don't allow bitwise on non-integer types
                    if(a->type.nptr || a->type.kind > Type::UPTR)
                    {
                        ps.errorAt(token, "invalid type to a bitwise operator");
                        a.reset(new ECast(token, a));
                        type.kind = Type::ERROR;
                    }
                }
                break;
            case Token::TlogNot:
                // require floating points to explicitly compare
                if(!a->type.nptr &&
                (a->type.kind == Type::F32 || a->type.kind == Type::F64))
                {
                    ps.errorAt(token,
                        "cannot convert floating point to a truth value");
                    type.kind = Type::ERROR;
                    
                } else type.kind = Type::BOOL;
                break;

            case Token::Tpos:   // FIXME: check for numeric types
            case Token::Tneg:
                if(a->type.nptr || a->type.kind >= Type::ERROR)
                {
                    ps.errorAt(token, "invalid type to a numeric operator");
                    type.kind = Type::ERROR;
                } else type.kind = a->type.kind;
                return;

            default: assert(false);
            }
        }

        void debug(int lvl)
        {
            const char * s = 0;
            switch(token.type)
            {
            case Token::TbitNot: s = "b:not"; break;
            case Token::TlogNot: s = "l:not"; break;

            case Token::Tpos: s = "num:pos"; break;
            case Token::Tneg: s = "num:neg"; break;
            
            default: assert(false);
            }

            BJIT_LOG("\n%*s(%s ", lvl, "", s);
            debugCommon();
            a->debug(lvl+2);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            switch(token.type)
            {
            case Token::TbitNot: return cg.proc.inot(a->codeGen(cg));
            case Token::TlogNot:
                {
                    auto z = cg.proc.lci(0);
                    return cg.proc.ieq(a->codeGen(cg), z);
                }

            case Token::Tpos: return a->codeGen(cg);
            case Token::Tneg:
                if(!type.nptr && type.kind == Type::F64)
                    return cg.proc.dneg(a->codeGen(cg));
                    
                if(!type.nptr && type.kind == Type::F32) assert(false);
                
                return cg.proc.ineg(a->codeGen(cg));
                
            default: assert(false); return Value{noVal};
            }
        }
        
    };

    struct EBinary : Expr
    {
        std::unique_ptr<Expr>   a,b;

        EBinary(Token const & t, Stack & s) : Expr(t)
        {
            b = std::move(s.back()); s.pop_back();
            a = std::move(s.back()); s.pop_back();
        }
        
        void typecheck(Parser & ps, Env & env)
        {
            a->typecheck(ps, env);
            b->typecheck(ps, env);
            
            // typecheck
            switch(token.type)
            {
            // any numeric types
            case Token::Tadd: case Token::Tsub: case Token::Tmul:
            case Token::Tdiv: case Token::Tmod:
                {
                    type.kind = Type::promoteNumeric(a->type, b->type);
                    if(type.kind == Type::ERROR
                    && a->type.kind != Type::ERROR && b->type.kind != Type::ERROR)
                    {
                        ps.errorAt(token,
                            "invalid types to a numeric operator");
                    }

                    if(type.kind == Type::F32 && token.type == Token::Tmod)
                    {
                        ps.errorAt(token,
                            "invalid types to an integer operator");
                        type.kind = Type::ERROR;
                        return;
                    }

                    if(type.kind == Type::F64 && token.type == Token::Tmod)
                    {
                        ps.errorAt(token,
                            "invalid types to an integer operator");
                        type.kind = Type::ERROR;
                        return;
                    }

                    if(a->type.kind != type.kind)
                    {
                        a.reset(new ECast(token, a));
                        a->type.kind = type.kind;
                    }
                    if(b->type.kind != type.kind)
                    {
                        b.reset(new ECast(token, b));
                        b->type.kind = type.kind;
                    }
                }
                break;

            // shifts and bitwise operations force integer type
            // but for shifts we only worry about signedness of 1st argument
            case Token::TshiftL:
            case Token::TshiftR:
            {
                    // silent check
                    if(a->type.kind == Type::ERROR || b->type.kind == Type::ERROR)
                    {
                        type.kind = Type::ERROR; return;
                    }

                    // disallow floats
                    if(a->type.kind == Type::F32 || a->type.kind == Type::F64)
                    {
                        type.kind = Type::ERROR;
                    }
                    
                    // disallow floats
                    if(b->type.kind == Type::F32 || b->type.kind == Type::F64)
                    {
                        type.kind = Type::ERROR;
                    }

                    // NOTE: we only promote on a!!
                    // FIXME: should we implement these for smaller types too?
                    type.kind = Type::promoteNumeric(a->type, a->type);
                    if(type.kind == Type::ERROR)
                    {
                        ps.errorAt(token,
                            "invalid types to a bitwise operator");
                        return;
                    }

                    if(a->type.kind != type.kind)
                    {
                        a.reset(new ECast(token, a));
                        a->type.kind = type.kind;
                    }
                    if(b->type.kind != type.kind)
                    {
                        b.reset(new ECast(token, b));
                        b->type.kind = type.kind;
                    }
                }
                break;
            case Token::TbitOr:
            case Token::TbitAnd:
            case Token::TbitXor:
                {
                    // silent check
                    if(a->type.kind == Type::ERROR || b->type.kind == Type::ERROR)
                    {
                        type.kind = Type::ERROR; return;
                    }

                    // disallow floats
                    if(a->type.kind == Type::F32 || a->type.kind == Type::F64)
                    {
                        type.kind = Type::ERROR;
                    }
                    
                    // disallow floats
                    if(b->type.kind == Type::F32 || b->type.kind == Type::F64)
                    {
                        type.kind = Type::ERROR;
                    }
                    
                    type.kind = Type::promoteNumeric(a->type, b->type);
                    if(type.kind == Type::ERROR)
                    {
                        ps.errorAt(token,
                            "invalid types to a bitwise operator");
                        return;
                    }
                }
                break;
    
            case Token::TlogAnd:
            case Token::TlogOr:
                {
                    // silent check
                    if(a->type.kind == Type::ERROR || b->type.kind == Type::ERROR)
                    {
                        type.kind = Type::ERROR; return;
                    }

                    // disallow floats
                    if(a->type.kind == Type::F32 || a->type.kind == Type::F64)
                    {
                        type.kind = Type::ERROR;
                    }
                    
                    // disallow floats
                    if(b->type.kind == Type::F32 || b->type.kind == Type::F64)
                    {
                        type.kind = Type::ERROR;
                    }
                    
                    type.kind = Type::promoteNumeric(a->type, b->type);
                    if(type.kind == Type::ERROR)
                    {
                        ps.errorAt(token,
                            "invalid types to a logical operator");
                        return;
                    }
                }
                break;
            
            case Token::Tassign:
                {
                    if(!a->canAssign())
                    {
                        ps.errorAt(a->token, "expression is not an l-value");
                        type.kind = Type::ERROR;
                        return;
                    }
                    type.kind = a->type.kind;
                    // FIXME: warnings?
                    if(b->type.kind != type.kind)
                    {
                        ps.warningAt(token, "implicit conversion in assigment");
                        b.reset(new ECast(token, b));
                        b->type = a->type;
                    }
                }
                break;

            // FIXME: these two should allow pointers
            case Token::Teq:
            case Token::TnotEq:
    
            case Token::Tless:
            case Token::TlessEq:
            case Token::Tgreater:
            case Token::TgreaterEq:
                {
                    type.kind = Type::promoteNumeric(a->type, b->type);
                    if(type.kind == Type::ERROR
                    && a->type.kind != Type::ERROR && b->type.kind != Type::ERROR)
                    {
                        ps.errorAt(token,
                            "invalid types to numeric operator");
                    }

                    if(a->type.kind != type.kind)
                    {
                        a.reset(new ECast(token, a));
                        a->type.kind = type.kind;
                    }
                    if(b->type.kind != type.kind)
                    {
                        b.reset(new ECast(token, b));
                        b->type.kind = type.kind;
                    }
                    
                    // actual result is always bool
                    type.kind = Type::BOOL;
                }
                break;
            default: assert(false);
            }
        }

        void debug(int lvl)
        {
            const char * s = 0;
            switch(token.type)
            {
            case Token::Tadd: s = "add"; break;
            case Token::Tsub: s = "sub"; break;
            case Token::Tmul: s = "mul"; break;
            case Token::Tdiv: s = "div"; break;
            case Token::Tmod: s = "mod"; break;
    
            case Token::TshiftL: s = "shL"; break;
            case Token::TshiftR: s = "shR"; break;
    
            case Token::TbitOr: s = "b:or"; break;
            case Token::TbitAnd: s = "b:and"; break;
            case Token::TbitXor: s = "b:xor"; break;
    
            case Token::TlogAnd: s = "l:and"; break;
            case Token::TlogOr: s = "l:or"; break;
            
            case Token::Tassign: s = "set"; break;
    
            case Token::Teq: s = "c:eq"; break;
            case Token::TnotEq: s = "c:neq"; break;
    
            case Token::Tless: s = "c:lt"; break;
            case Token::TlessEq: s = "c:le"; break;
            case Token::Tgreater: s = "c:gt"; break;
            case Token::TgreaterEq: s = "c:ge"; break;

            default: assert(false);
            }
            
            BJIT_LOG("\n%*s(%s ", lvl, "", s);
            debugCommon();
            a->debug(lvl+2);
            b->debug(lvl+2);
            BJIT_LOG(")");
        }
        
        virtual Value codeGen(CodeGen & cg)
        {
            assert(type.kind != Type::F32);

            // these do short-circuit, so need to check first
            if(token.type == Token::TlogAnd)
            {
                cg.proc.env.push_back(a->codeGen(cg));
                auto la = cg.proc.newLabel();
                auto lb = cg.proc.newLabel();

                cg.proc.jz(cg.proc.env.back(),lb,la);
                cg.proc.emitLabel(la);
                
                cg.proc.env.back() = b->codeGen(cg);
                cg.proc.jmp(lb);
                cg.proc.emitLabel(lb);
                
                auto v = cg.proc.env.back();
                cg.proc.env.pop_back();
                return v;
            }
            
            if(token.type == Token::TlogOr)
            {
                cg.proc.env.push_back(a->codeGen(cg));
                auto la = cg.proc.newLabel();
                auto lb = cg.proc.newLabel();
                
                cg.proc.jz(cg.proc.env.back(),la,lb);
                cg.proc.emitLabel(la);
                
                cg.proc.env.back() = b->codeGen(cg);
                cg.proc.jmp(lb);
                cg.proc.emitLabel(lb);
                
                auto v = cg.proc.env.back();
                cg.proc.env.pop_back();
                return v;
            }
            
            if(token.type == Token::Tassign)
            {
                auto vb = b->codeGen(cg);
                return a->codeGenAssign(cg, vb);
            }

            auto va = a->codeGen(cg);
            auto vb = b->codeGen(cg);
        
            switch(token.type)
            {
            case Token::Tadd:
                if(!type.nptr && type.kind == Type::F64)
                    return cg.proc.dadd(va, vb);
                else return cg.proc.iadd(va, vb);
                
            case Token::Tsub:
                if(!type.nptr && type.kind == Type::F64)
                    return cg.proc.dsub(va, vb);
                else return cg.proc.isub(va, vb);
                
            case Token::Tmul:
                if(type.kind == Type::F64) return cg.proc.dmul(va, vb);
                else return cg.proc.imul(va, vb);
                
            case Token::Tdiv:
                if(type.kind == Type::F64) return cg.proc.ddiv(va, vb);
                else if(type.kind == Type::UPTR) return cg.proc.udiv(va, vb);
                else return cg.proc.idiv(va, vb);
                
            case Token::Tmod:
                if(type.kind == Type::F64) assert(false);
                else if(type.kind == Type::UPTR) return cg.proc.umod(va, vb);
                else return cg.proc.imod(va, vb);
            
            case Token::TshiftL: return cg.proc.ishl(va, vb);
            case Token::TshiftR:
                if(type.kind == Type::UPTR) return cg.proc.ushr(va, vb);
                else return cg.proc.ishr(va, vb);
    
            case Token::TbitOr: return cg.proc.ior(va, vb);
            case Token::TbitAnd: return cg.proc.iand(va, vb);
            case Token::TbitXor: return cg.proc.ixor(va, vb);
    
            case Token::Teq:
                // note: we need to use argument (not result) type here
                if(a->type.kind == Type::F64) return cg.proc.deq(va, vb);
                else return cg.proc.ieq(va, vb);
            case Token::TnotEq:
                if(a->type.kind == Type::F64) return cg.proc.dne(va, vb);
                else return cg.proc.ine(va, vb);
            
            case Token::Tless:
                if(a->type.kind == Type::F64) return cg.proc.dlt(va, vb);
                else return cg.proc.ilt(va, vb);
            
            case Token::TlessEq:
                if(a->type.kind == Type::F64) return cg.proc.dle(va, vb);
                else return cg.proc.ile(va, vb);
            
            case Token::Tgreater:
                if(a->type.kind == Type::F64) return cg.proc.dgt(va, vb);
                else return cg.proc.igt(va, vb);
            
            case Token::TgreaterEq:
                if(a->type.kind == Type::F64) return cg.proc.dge(va, vb);
                else return cg.proc.ige(va, vb);

            default: assert(false); return Value{noVal};
            }
        }
        
    };
}