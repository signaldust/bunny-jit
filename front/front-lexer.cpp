
#include "front-parse.h"
#include "front-lexer.h"

#include <cmath>

using namespace bjit;

static bool isDigit(int ch)
{
    return ch >= '0' && ch <= '9';
}

static bool isSymbolChar(int ch)
{
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || (ch == '_')
        ;
}

static void eatSpace(Parser & ps)
{
    while(true)
    {
        int ch = ps.peek();

        // anything from # to end of line is a comment
        if(ch == '#')
        {
            while(ps.peek() != '\n') ps.consume();
            continue;
        }

        // break if this is not one of the whitespace characters
        if(ch != ' '
        && ch != '\n'
        && ch != '\r'
        && ch != '\t') break;

        ps.consume();
    }
}

static void lexNumber(Parser & ps, bool leadingDot)
{
    bool valid = false; // do we have at least one digit

    // if there are too many digits, then we want
    // different behavior depending on int/float
    // so collect first, until we know what to do
    std::vector<char>   mantissaDigits;

    // this is only relevant for integers
    int base = 10;

    // this checks for hex contants
    if(!leadingDot && ps.peek() == '0')
    {
        // integers are octals
        ps.consume();

        base = 8;

        if(ps.peek() == 'x' || ps.peek() == 'X')
        {
            base = 16;

            while(true)
            {
                ps.consume();
                int ch = ps.peek();
                
                if(isDigit(ch))
                {
                    mantissaDigits.push_back(ch - '0');
                    continue;
                }

                if(ch >= 'a' && ch <= 'f')
                {
                    mantissaDigits.push_back(ch + 10 - 'a');
                    continue;
                }
                
                if(ch >= 'A' && ch <= 'F')
                {
                    mantissaDigits.push_back(ch + 10 - 'A');
                    continue;
                }
                
                break;
            }
        }
        else
        {
            mantissaDigits.push_back(0);
        }
    }

    // if this is not hex, parse digits
    if(!leadingDot && base < 16)
    {
        while(true)
        {
            int ch = ps.peek();
            if(!isDigit(ch)) break;
    
            mantissaDigits.push_back(ch - '0');
            ps.consume();
        }
    }

    if(mantissaDigits.size()) valid = true;

    // do we have a decimal point or exponent?
    if((base < 16)
    && (leadingDot || ps.peek() == '.' || ps.peek() == 'e' || ps.peek() == 'E'))
    {
        double m = 0, d = 1;
        // collect the integer part of mantissa
        for(auto & d : mantissaDigits) { m = 10*m + d; }

        // handle digits after decimal point if any
        if(leadingDot || ps.peek() == '.')
        {
            if(!leadingDot) ps.consume();
            while(true)
            {
                int ch = ps.peek();
                if(!isDigit(ch)) break;

                m = 10*m + (ch - '0');
                d = 10*d;

                valid = true;

                ps.consume();
            }

            // if there are no digits around,
            // then a dot is a binary operator
            if(!valid)
            {
                ps.token.type = Token::Tdot;
                return;
            }

            // fix the decimal point
            m /= d;
        }

        double e = 0;
        if(ps.peek() == 'e' || ps.peek() == 'E')
        {
            ps.consume();

            // get sign of exponent
            bool negate = false;
            if(ps.peek() == '-') negate = true;
            if(negate || ps.peek() == '+') ps.consume();

            // consume digits
            while(true)
            {
                int ch = ps.peek();
                if(!isDigit(ch)) break;

                e = 10*e + (ch - '0');
                ps.consume();
            }

            // apply exponent to mantissa
            m *= pow(10., negate ? -e : e);
        }

        ps.token.type = Token::Tfloat;
        ps.token.vFloat = m;
    }
    else
    {
        
        // plain old integer
        int64_t i = 0;

        // here we use variable base
        for(auto & d : mantissaDigits)
        {
            if(d >= base) valid = false;
            i = base*i + d;
        }

        ps.token.type = Token::Tint;
        ps.token.vInt = i;

        if(ps.peek() == 'U' || ps.peek() == 'u')
        {
            ps.token.type = Token::Tuint;
            ps.consume();
        }

        // this can happen with 0x without digits
        // or when octal has digits 8 or 9
        if(!valid)
        {
            ps.errorAt(ps.token, "invalid numeric literal");

            ps.token.type = Token::Terror;
        }
    }

    // check that there isn't trailing garbage
    if(isSymbolChar(ps.peek()))
    {
        ps.errorAt(ps.token, "invalid character in numeric literal");

        // eat it all, so we can try to continue
        while(isSymbolChar(ps.peek())) ps.consume();
    }
}

static struct {
    const char * str;
    Token::Type  ttype;
} keywords[] = {
    { "if",         Token::Tif          },
    { "else",       Token::Telse        },

    { "while",      Token::Twhile       },
    { "break",      Token::Tbreak       },
    { "continue",   Token::Tcontinue    },

    { "return",     Token::Treturn      },

    { 0, Token::Terror } // end of list marker
};

static void lexSymbol(Parser & ps)
{
    std::vector<char>   symbol;

    while(true)
    {
        int ch = ps.peek();
        if(!isSymbolChar(ch)) break;

        symbol.push_back(ch);
        ps.consume();
    }

    if(!symbol.size())
    {
        ps.errorAt(ps.token, "invalid syntax");
        ps.consume();
        return;
    }

    symbol.push_back(0);    // null-termination
    
    ps.token.type = Token::Tsymbol;

    for(int i = 0; keywords[i].str; ++i)
    {
        if(!strcmp(symbol.data(), keywords[i].str))
        {
            ps.token.type = keywords[i].ttype;
            return;
        }
    }

    // if we didn't match a keyboard, intern symbol
    SymbolPtr * sptr = ps.symbols.find(symbol);
    if(sptr) ps.token.symbol = sptr->ptr.get();
    else
    {
        ps.token.symbol = new Symbol;
        std::swap(ps.token.symbol->string, symbol);
        
        SymbolPtr newSym;
        newSym.ptr.reset(ps.token.symbol);
        ps.symbols.insert(newSym);
    }
}

void bjit::lexToken(Parser & ps)
{
    eatSpace(ps);

    ps.token.posChar = ps.posChar;
    ps.token.posLine = ps.posLine;

    ps.token.type = Token::Terror;
    
    switch(int ch = ps.peek())
    {
    case EOF: ps.token.type = Token::Teof; return;

    case '!':
        ps.consume();
        switch(ps.peek())
        {
        case '=': ps.token.type = Token::TnotEq; ps.consume(); return;
        default: ps.token.type = Token::TlogNot; return;
        }
    case '<':
        ps.consume();
        switch(ps.peek())
        {
        case '=': ps.token.type = Token::TlessEq; ps.consume(); return;
        case '<': ps.token.type = Token::TshiftL; ps.consume(); return;
        default: ps.token.type = Token::Tless; return;
        }
    case '>':
        ps.consume();
        switch(ps.peek())
        {
        case '=': ps.token.type = Token::TgreaterEq; ps.consume(); return;
        case '>': ps.token.type = Token::TshiftR; ps.consume(); return;
        default: ps.token.type = Token::Tgreater; return;
        }
    case '=':
        ps.consume();
        switch(ps.peek())
        {
        case '=': ps.token.type = Token::Teq; ps.consume(); return;
        default: ps.token.type = Token::Tassign; return;
        }

    case '{': ps.token.type = Token::ToBlock; ps.consume(); return;
    case '}': ps.token.type = Token::TcBlock; ps.consume(); return;

    case '[': ps.token.type = Token::ToIndex; ps.consume(); return;
    case ']': ps.token.type = Token::TcIndex; ps.consume(); return;
    
    case '(': ps.token.type = Token::ToParen; ps.consume(); return;
    case ')': ps.token.type = Token::TcParen; ps.consume(); return;

    case '+': ps.token.type = Token::Tadd; ps.consume(); return;
    case '-': ps.token.type = Token::Tsub; ps.consume(); return;

    case '*': ps.token.type = Token::Tmul; ps.consume(); return;
    case '/': ps.token.type = Token::Tdiv; ps.consume(); return;
    case '%': ps.token.type = Token::Tmod; ps.consume(); return;

    case '~': ps.token.type = Token::TbitNot; ps.consume(); return;
    case '^': ps.token.type = Token::TbitXor; ps.consume(); return;
    
    case '&':
        ps.consume();
        switch(ps.peek())
        {
        case '&': ps.token.type = Token::TlogAnd; ps.consume(); return;
        default: ps.token.type = Token::TbitAnd; return;
        }
    case '|':
        ps.consume();
        switch(ps.peek())
        {
        case '|': ps.token.type = Token::TlogOr; ps.consume(); return;
        default: ps.token.type = Token::TbitOr; return;
        }

    case '.':
        ps.consume();
        if(isDigit(ps.peek())) lexNumber(ps, true);
        else ps.token.type = Token::Tdot;
        return;
    case ':':
        ps.consume();
        switch(ps.peek())
        {
        case '=': ps.token.type = Token::Tdefine; ps.consume(); return;
        default: ps.token.type = Token::Tcolon; return;
        }
        
    case ',': ps.token.type = Token::Tcomma; ps.consume(); return;
    case ';': ps.token.type = Token::Tsemicolon; ps.consume(); return;
    default:
        if(isDigit(ps.peek())) lexNumber(ps, false);
        else lexSymbol(ps);
        return;
    }

}