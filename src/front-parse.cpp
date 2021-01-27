
#include "front-parse.h"
#include "front-ast.h"

using namespace bjit;

// precedence classes, order from high to low
//
// NOTE: parens are logically in the highest precedence class
// but we treat them as the lowest internally, so that we can
// reduce everything up to the opening paren before checking match
enum Precedence
{
    P_unary,    // unary operators
    
    P_product,  // mul / div / mod
    P_sum,      // add / sub
    P_shift,    // bitshifts

    P_compare,  // relative comparisons
    P_equal,    // equality comparisons

    P_bitAnd,   // bitwise and
    P_bitXor,   // bitwise xor
    P_bitOr,    // bitwise or

    P_logAnd,   // logical and
    P_logOr,    // logical or

    P_assign,   // assignments
    
    P_comma,    // comma

    P_flow      // parens, control flow constructs
};

static int getPrecede(Token const & t)
{
    switch(t.type)
    {
    case Token::Tpos: case Token::Tneg:
        return P_unary;
        
    case Token::Tmul: case Token::Tdiv: case Token::Tmod:
        return P_product;
        
    case Token::Tadd: case Token::Tsub:
        return P_sum;

    case Token::TshiftL: case Token::TshiftR:
        return P_shift;

    case Token::Tless: case Token::TlessEq:
    case Token::Tgreater: case Token::TgreaterEq:
        return P_compare;

    case Token::Teq: case Token::TnotEq:
        return P_equal;

    case Token::TbitAnd: return P_bitAnd;
    case Token::TbitXor: return P_bitXor;
    case Token::TbitOr: return P_bitOr;
    case Token::TlogAnd: return P_logAnd;
    case Token::TlogOr: return P_logOr;
    case Token::Tassign: case Token::Tdefine: return P_assign;
    case Token::Tcomma: return P_comma;

    case Token::ToParen: case Token::ToBlock: case Token::ToIndex:
    case Token::Tif: case Token::TifBody: case Token::Telse:
    case Token::Twhile: case Token::TwhileBody:
    case Token::Tfuncall: case Token::Treturn:
        return P_flow;

    default: printf("TT: %d\n", t.type); assert(false);
    }

}

static void defer(Parser & ps)
{
    ps.defer.push_back(ps.token);
}

static void deferAs(Parser & ps, Token::Type t)
{
    ps.token.type = t;
    ps.defer.push_back(ps.token);
}

static void fragment(Parser & ps, Token const & t)
{
    switch(t.type)
    {
    case Token::Tint: ps.frags.emplace_back(new EConst(t)); break;
    case Token::Tuint: ps.frags.emplace_back(new EConst(t)); break;
    case Token::Tfloat: ps.frags.emplace_back(new EConst(t)); break;
    case Token::Tsymbol: ps.frags.emplace_back(new ESymbol(t)); break;

    case Token::Tadd: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::Tsub: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::Tmul: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::Tdiv: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::Tmod: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;

    case Token::TshiftL: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TshiftR: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;

    case Token::TbitOr: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TbitAnd: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TbitXor: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    
    case Token::TbitNot: ps.frags.emplace_back(new EUnary(t, ps.frags)); break;
    case Token::TlogNot: ps.frags.emplace_back(new EUnary(t, ps.frags)); break;
    
    case Token::TlogAnd: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TlogOr: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    
    case Token::Tassign: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::Tdefine: ps.frags.emplace_back(new EDefine(t, ps.frags)); break;

    case Token::Teq: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TnotEq: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;

    case Token::Tless: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TlessEq: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::Tgreater: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;
    case Token::TgreaterEq: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;

    case Token::Tpos: ps.frags.emplace_back(new EUnary(t, ps.frags)); break;
    case Token::Tneg: ps.frags.emplace_back(new EUnary(t, ps.frags)); break;

    case Token::Treturn: ps.frags.emplace_back(new EReturn(t, ps.frags)); break;
    case Token::Tfuncall: ps.frags.emplace_back(new ECall(t, ps.frags)); break;

    case Token::ToIndex: ps.frags.emplace_back(new EBinary(t, ps.frags)); break;

    case Token::TifBody: ps.frags.emplace_back(new EIf(t, ps.frags, false)); break;
    case Token::Telse: ps.frags.emplace_back(new EIf(t, ps.frags, true)); break;

    case Token::TwhileBody: ps.frags.emplace_back(new EWhile(t, ps.frags)); break;
    case Token::ToBlock: ps.frags.emplace_back(new EBlock(t, ps.frags)); break;

    default: printf("TT: %d\n", t.type); assert(false);
    }
    
}

void reduce(Parser & ps, int precede)
{
    // then reduce everything at current or higher priority
    while(ps.defer.size())
    {
        auto & t = ps.defer.back();
        if(getPrecede(t) > precede) return;

        fragment(ps, ps.defer.back());
        ps.defer.pop_back();
    }
}

// forward declare all states
static void psStatement(Parser & ps);   // function top-level statement
static void psMaybeAssign(Parser & ps); // expect variable value or comma
static void psInfix(Parser & ps);       // infix operators
static void psExpr(Parser & ps);        // expression
static void psCondition(Parser & ps);   // if/while condition context
static void psMaybeElse(Parser & ps);   // statement or else for an if

void bjit::parse(std::vector<uint8_t> & codeOut)
{
    Parser ps;

    ps.state = psStatement;
    
    while(true)
    {
        lexToken(ps);

        if(ps.token.type == Token::Teof)
        {
            // FIXME: this should be removed once statements go into functions
            while(ps.defer.size() && ps.defer.back().type == Token::TifBody)
            {
                fragment(ps, ps.defer.back()); ps.defer.pop_back();
            }
            break;
        }
        if(ps.token.type == Token::Terror) continue;

        ps.state(ps);
    }

    Env env(1); // must match what we pass proc
    for(auto & e : ps.frags)
    {
        e->typecheck(ps, env);
        e->debug(0);
    }
    printf("\n");

    if(ps.nErrors) return;

    Proc p(0, "");
    
    for(auto & e : ps.frags)
        e->codeGen(p);

    p.iret(p.lci(0));  // must always have return
    p.debug();

    printf("-- Optimizing:\n");
    p.opt();

    p.compile(codeOut);
    p.debug();
}

// helper to figure out what to do with statements
// we'll need this also from psStatement once we add blocks
static void reduceStatement(Parser & ps)
{
    ps.state = psStatement; // default to statement
    
    while(ps.defer.size())
    {
        switch(ps.defer.back().type)
        {
        // for these we've completed the control-flow construct
        case Token::Telse:
        case Token::TwhileBody:
        case Token::Treturn:
            fragment(ps, ps.defer.back());
            ps.defer.pop_back();
            // only break, don't return, we might need to reduce more
            break;

        case Token::TifBody: ps.state = psMaybeElse; return;
        case Token::ToBlock: ++ps.defer.back().nArgs; return;

        default:
            ps.errorAt(ps.token, "unexpected ';'");
            ps.errorAt(ps.defer.back(), "incomplete expression here");
            
            // reset state to avoid error cascade
            while(getPrecede(ps.defer.back()) < P_flow)
                ps.defer.pop_back();
            return;
        }
    }
}

static void psStatement(Parser & ps)
{
    switch(ps.token.type)
    {
    case Token::TcBlock:
        if(!ps.defer.size() || ps.defer.back().type != Token::ToBlock)
        {
            ps.errorAt(ps.token, "unexpected '}'"); return;
        }
        fragment(ps, ps.defer.back()); ps.defer.pop_back();
        reduceStatement(ps);
        break;
        
    case Token::ToBlock: ps.token.nArgs = 0; defer(ps); break;
    case Token::Tif: defer(ps); ps.state = psCondition; break;
    case Token::Twhile: defer(ps); ps.state = psCondition; break;
    case Token::Treturn: defer(ps); ps.state = psExpr; break;

    case Token::Tsymbol: fragment(ps, ps.token); ps.state = psMaybeAssign; break;
    
    default: ps.state = psExpr; psExpr(ps); break;
    }
}

static void psMaybeAssign(Parser & ps)
{
    switch(ps.token.type)
    {
    case Token::Tassign:
        reduce(ps, P_assign-1); defer(ps); ps.state = psExpr; break;

    case Token::Tdefine:
        reduce(ps, P_assign-1); defer(ps); ps.state = psExpr; break;

    default: ps.state = psInfix; psInfix(ps); break;
    }
}

static void psInfix(Parser & ps)
{
    switch(ps.token.type)
    {
    case Token::ToParen:
        ps.token.nArgs = 0;
        deferAs(ps, Token::Tfuncall);
        ps.state = psExpr; break;
    
    case Token::TcParen:
        reduce(ps, P_flow-1);
        if(!ps.defer.size())
        {
            ps.errorAt(ps.token, "mismatched ')'");
            break;
        }
        switch(ps.defer.back().type)
        {
            case Token::ToParen:
                ps.defer.pop_back(); break;

            case Token::Tfuncall:
                ++ps.defer.back().nArgs;
                fragment(ps, ps.defer.back());
                ps.defer.pop_back(); break;

            case Token::Tif:    // is this a condition for if-statement?
                ps.defer.back().type = Token::TifBody;
                ps.state = psStatement;
                break;
            case Token::Twhile: // is this a condition for while-statement?
                ps.defer.back().type = Token::TwhileBody;
                ps.state = psStatement;
                break;

            default:
                ps.errorAt(ps.token, "mismatched ')'");
                break;
        }
        break;

    case Token::Tcomma:
        reduce(ps, P_comma);
        // check that this is a context where comma is valid
        switch(ps.defer.back().type)
        {
        case Token::Tfuncall: ++ps.defer.back().nArgs; ps.state = psExpr; break;
        default: ps.errorAt(ps.token, "unexpected ','");
        }
        break;

    case Token::ToIndex: defer(ps); ps.state = psExpr; break;
    
    case Token::TcIndex:
        reduce(ps, P_flow-1);
        if(!ps.defer.size())
        {
            ps.errorAt(ps.token, "mismatched ']'");
            break;
        }
        switch(ps.defer.back().type)
        {
            case Token::ToIndex:
                fragment(ps, ps.defer.back());
                ps.defer.pop_back(); break;

            default:
                ps.errorAt(ps.token, "mismatched ']'");
                break;
        }
        break;
    
    case Token::Tsemicolon:
        reduce(ps, P_assign);
        reduceStatement(ps);
        break;

    case Token::Tadd:
    case Token::Tsub:
        reduce(ps, P_sum); defer(ps); ps.state = psExpr; break;
        
    case Token::Tmul:
    case Token::Tdiv:
    case Token::Tmod:
        reduce(ps, P_product); defer(ps); ps.state = psExpr; break;

    case Token::TshiftL:
    case Token::TshiftR:
        reduce(ps, P_shift); defer(ps); ps.state = psExpr; break;
    
    case Token::TbitOr:
        reduce(ps, P_bitOr); defer(ps); ps.state = psExpr; break;
        
    case Token::TbitAnd:
        reduce(ps, P_bitAnd); defer(ps); ps.state = psExpr; break;
        
    case Token::TbitXor:
        reduce(ps, P_bitXor); defer(ps); ps.state = psExpr; break;
        
    case Token::TlogOr:
        reduce(ps, P_logOr); defer(ps); ps.state = psExpr; break;
        
    case Token::TlogAnd:
        reduce(ps, P_logAnd); defer(ps); ps.state = psExpr; break;

    case Token::Tless:
    case Token::TlessEq:
    case Token::Tgreater:
    case Token::TgreaterEq:
        reduce(ps, P_compare); defer(ps); ps.state = psExpr; break;

    case Token::Teq:
    case Token::TnotEq:
        reduce(ps, P_equal); defer(ps); ps.state = psExpr; break;

        
    case Token::TcBlock:
        // this is always an error, but we'll recover the case where
        // we are simply missing a semicolon
        reduce(ps, P_assign);
        if(ps.defer.back().type == Token::ToBlock)
        {
            ps.errorAt(ps.token, "missing ';'");
            ++ps.defer.back().nArgs;
            psStatement(ps);
            return;
        }
        // fall-thru to default error
    default:
        ps.errorAt(ps.token, "unexpected token - expecting operator"); break;
    }
}

static void psExpr(Parser & ps)
{
    // special case for closing paren of function calls
    if(ps.token.type == Token::TcParen)
    {
        if(ps.defer.back().type == Token::Tfuncall && ps.defer.back().nArgs == 0)
        {
            reduce(ps, P_flow); ps.state = psInfix; return;
        }
    }

    switch(ps.token.type)
    {
    case Token::ToParen: defer(ps); break;

    // no reduce for unary operators as they are already the highest
    // and should be reduced right-to-left
    case Token::Tadd: deferAs(ps, Token::Tpos); break;
    case Token::Tsub: deferAs(ps, Token::Tneg); break;
    case Token::TlogNot: defer(ps); break;
    case Token::TbitNot: defer(ps); break;

    case Token::Tint: 
    case Token::Tuint:
    case Token::Tfloat:
    case Token::Tsymbol:
        fragment(ps, ps.token); ps.state = psInfix; break;
        
    default: ps.errorAt(ps.token, "unexpected token - expecting expression"); break;
    }
}

static void psCondition(Parser & ps)
{
    assert(ps.defer.back().type == Token::Tif
    || ps.defer.back().type == Token::Twhile);
    
    switch(ps.token.type)
    {
    case Token::ToParen:
        ps.state = psExpr;
        break;

    default:
        ps.errorAt(ps.token, "expected '(' for condition");
        // try to recover by ignoring the previous keyword
        ps.defer.pop_back();
        psStatement(ps);
        break;
    }
}

static void psMaybeElse(Parser & ps)
{
    if(ps.token.type == Token::Telse)
    {
        assert(ps.defer.back().type == Token::TifBody);
        ps.defer.back().type = Token::Telse;
        ps.state = psStatement;
        return;
    }

    // no else keyword, reduce any conditions
    while(ps.defer.size() && ps.defer.back().type == Token::TifBody)
    {
        fragment(ps, ps.defer.back());
        ps.defer.pop_back();
    }

    reduceStatement(ps);
    
    psStatement(ps);
}
