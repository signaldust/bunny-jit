
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

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
            if(nptr == 1) printf("%d*", nptr);
            
            switch(kind)
            {
            case ERROR: printf("error"); break;
            case AUTO: printf("auto"); break;
            case VOID: printf("void"); break;
            
            case I8: printf("i8"); break;
            case I16: printf("i16"); break;
            case I32: printf("i32"); break;
            case I64: printf("i64"); break;
            
            case U8: printf("u8"); break;
            case U16: printf("u16"); break;
            case U32: printf("u32"); break;
            case U64: printf("u64"); break;

            case F32: printf("f32"); break;
            case F64: printf("f64"); break;

            default: printf("struct-%d", kind - STRUCT); break;
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
        virtual unsigned codeGen(Proc & proc) = 0;

        // l-values should implement these
        virtual bool canAssign() { return false; }
        virtual unsigned codeGenAssign(Proc & proc, unsigned v)
        { assert(false); }

        void debugCommon()
        {
            printf("@%d:%d : ", token.posLine, token.posChar);
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
            printf("\n%*s(typecast ", lvl, "");
            debugCommon();
            v->debug(lvl+2);
        }

        virtual unsigned codeGen(Proc & proc)
        {
            // we don't handle pointers yet
            assert(!type.nptr && !v->type.nptr);
        
            // we only really truly need to convert floats and ints
            // anything else is effectively a NOP
            auto vv = v->codeGen(proc);
            if(type.kind == Type::F32 || v->type.kind == Type::F32)
            {
                assert(false);  // not supported yet
            }

            // float -> int conversion
            if(type.kind <= Type::UPTR && v->type.kind == Type::F64)
                return proc.cd2i(vv);

            // int -> float conversion
            if(type.kind == Type::F64 && v->type.kind <= Type::UPTR)
                return proc.ci2d(vv);

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
            case Token::Tint: printf("\n%*si:%lli ", lvl, "", token.vInt); break;
            case Token::Tuint: printf("\n%*su:%llu ", lvl, "", token.vInt); break;
            case Token::Tfloat: printf("\n%*sf:%#g ", lvl, "", token.vFloat); break;

            default: assert(false);
            }

            debugCommon();
        }

        virtual unsigned codeGen(Proc & proc)
        {
            switch(token.type)
            {
            case Token::Tint:
            case Token::Tuint:
                return proc.lci(token.vInt);
            
            case Token::Tfloat:
                return proc.lcd(token.vFloat);
                
            default: assert(false);
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
            printf("\n%*ssym:%p:%s/%d ", lvl, "",
                token.symbol, token.symbol->string.data(), envIndex);
            debugCommon();
        }

        virtual unsigned codeGen(Proc & proc)
        {
            return proc.env[envIndex];
        }

        virtual bool canAssign() { return true; }

        virtual unsigned codeGenAssign(Proc & proc, unsigned v)
        {
            return (proc.env[envIndex] = v);
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
            printf("\n%*s(return ", lvl, "");
            debugCommon();
            v->debug(lvl+2);
        }

        virtual unsigned codeGen(Proc & proc)
        {
            if(!type.nptr && type.kind == Type::F64)
                proc.dret(v->codeGen(proc));
            else proc.iret(v->codeGen(proc));
            return ~0u;
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
            printf("\n%*s(call ", lvl, "");
            fn->debug(lvl+2);
            // indent arguments by two levels
            for(auto & a : args) a->debug(lvl+4);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            unsigned p = fn->codeGen(proc);
            for(auto & a : args) proc.env.push_back(a->codeGen(proc));
            unsigned r = proc.icallp(p, args.size());
            proc.env.resize(proc.env.size() - args.size());
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
            printf("\n%*s(block ", lvl, "");
            for(auto & a : body) a->debug(lvl+2);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            unsigned sz = proc.env.size();
            for(auto & a : body) a->codeGen(proc);
            proc.env.resize(sz);
            return ~0U;
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
            printf("\n%*s(if ", lvl, "");
            debugCommon();
            condition->debug(lvl+4);    // two level indent
            sThen->debug(lvl+2);
            if(sElse) sElse->debug(lvl+2);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            // do this before label creation, so we get scope
            auto ec = proc.env.size();   // block-scope save
            auto cc = condition->codeGen(proc);
            
            unsigned lThen = proc.newLabel();
            unsigned lElse = proc.newLabel();
            unsigned lDone = proc.newLabel();
            
            proc.jz(cc, lElse, lThen);
            
            proc.emitLabel(lThen);
            sThen->codeGen(proc);
            proc.env.resize(ec);
            proc.jmp(lDone);
            
            proc.emitLabel(lElse);
            if(sElse)
            {
                sElse->codeGen(proc);
            }
            proc.jmp(lDone);
            proc.emitLabel(lDone);
            proc.env.resize(ec);
            return ~0u;
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
            printf("\n%*s(while ", lvl, "");
            debugCommon();
            condition->debug(lvl+4);    // two level indent
            body->debug(lvl+2);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            auto ec = proc.env.size();
            unsigned lTest = proc.newLabel();

            proc.jmp(lTest);
            proc.emitLabel(lTest);
            
            auto cc = condition->codeGen(proc);
            
            unsigned lBody = proc.newLabel();
            unsigned lDone = proc.newLabel();
            proc.jz(cc, lDone, lBody);

            proc.emitLabel(lBody);
            body->codeGen(proc);
            proc.env.resize(ec);
            proc.jmp(lTest);
            
            proc.emitLabel(lDone);
            proc.env.resize(ec);
            return ~0u;
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
            printf("\n%*s(def:%p:%s/%d ", lvl, "",
                token.symbol, token.symbol->string.data(), envIndex);
            debugCommon();
            value->debug(lvl+2);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            proc.env.push_back(value->codeGen(proc));
            return proc.env.back();
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

            printf("\n%*s(%s ", lvl, "", s);
            debugCommon();
            a->debug(lvl+2);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            switch(token.type)
            {
            case Token::TbitNot: return proc.inot(a->codeGen(proc));
            case Token::TlogNot:
                {
                    unsigned z = proc.lci(0);
                    return proc.ieq(a->codeGen(proc), z);
                }

            case Token::Tpos: return a->codeGen(proc);
            case Token::Tneg:
                if(!type.nptr && type.kind == Type::F64)
                    return proc.dneg(a->codeGen(proc));
                    
                if(!type.nptr && type.kind == Type::F32) assert(false);
                
                return proc.ineg(a->codeGen(proc));
                
            default: assert(false);
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
            
            printf("\n%*s(%s ", lvl, "", s);
            debugCommon();
            a->debug(lvl+2);
            b->debug(lvl+2);
            printf(")");
        }
        
        virtual unsigned codeGen(Proc & proc)
        {
            assert(type.kind != Type::F32);

            // these do short-circuit, so need to check first
            if(token.type == Token::TlogAnd)
            {
                proc.env.push_back(a->codeGen(proc));
                auto la = proc.newLabel();
                auto lb = proc.newLabel();

                proc.jz(proc.env.back(),lb,la);
                proc.emitLabel(la);
                
                proc.env.back() = b->codeGen(proc);
                proc.jmp(lb);
                proc.emitLabel(lb);
                
                auto v = proc.env.back();
                proc.env.pop_back();
                return v;
            }
            
            if(token.type == Token::TlogOr)
            {
                proc.env.push_back(a->codeGen(proc));
                auto la = proc.newLabel();
                auto lb = proc.newLabel();
                
                proc.jz(proc.env.back(),la,lb);
                proc.emitLabel(la);
                
                proc.env.back() = b->codeGen(proc);
                proc.jmp(lb);
                proc.emitLabel(lb);
                
                auto v = proc.env.back();
                proc.env.pop_back();
                return v;
            }
            
            if(token.type == Token::Tassign)
            {
                auto vb = b->codeGen(proc);
                return a->codeGenAssign(proc, vb);
            }

            auto va = a->codeGen(proc);
            auto vb = b->codeGen(proc);
        
            switch(token.type)
            {
            case Token::Tadd:
                if(!type.nptr && type.kind == Type::F64)
                    return proc.dadd(va, vb);
                else return proc.iadd(va, vb);
                
            case Token::Tsub:
                if(!type.nptr && type.kind == Type::F64)
                    return proc.dsub(va, vb);
                else return proc.isub(va, vb);
                
            case Token::Tmul:
                if(type.kind == Type::F64) return proc.dmul(va, vb);
                else return proc.imul(va, vb);
                
            case Token::Tdiv:
                if(type.kind == Type::F64) return proc.ddiv(va, vb);
                else if(type.kind == Type::UPTR) return proc.udiv(va, vb);
                else return proc.idiv(va, vb);
                
            case Token::Tmod:
                if(type.kind == Type::F64) assert(false);
                else if(type.kind == Type::UPTR) return proc.umod(va, vb);
                else return proc.imod(va, vb);
            
            case Token::TshiftL: return proc.ishl(va, vb);
            case Token::TshiftR:
                if(type.kind == Type::UPTR) return proc.ushr(va, vb);
                else return proc.ishr(va, vb);
    
            case Token::TbitOr: return proc.ior(va, vb);
            case Token::TbitAnd: return proc.iand(va, vb);
            case Token::TbitXor: return proc.ixor(va, vb);
    
            case Token::Teq:
                // note: we need to use argument (not result) type here
                if(a->type.kind == Type::F64) return proc.deq(va, vb);
                else return proc.ieq(va, vb);
            case Token::TnotEq:
                if(a->type.kind == Type::F64) return proc.dne(va, vb);
                else return proc.ine(va, vb);
            
            case Token::Tless:
                if(a->type.kind == Type::F64) return proc.dlt(va, vb);
                else return proc.ilt(va, vb);
            
            case Token::TlessEq:
                if(a->type.kind == Type::F64) return proc.dle(va, vb);
                else return proc.ile(va, vb);
            
            case Token::Tgreater:
                if(a->type.kind == Type::F64) return proc.dgt(va, vb);
                else return proc.igt(va, vb);
            
            case Token::TgreaterEq:
                if(a->type.kind == Type::F64) return proc.dge(va, vb);
                else return proc.ige(va, vb);

            default: assert(false);
            }
        }
        
    };
}