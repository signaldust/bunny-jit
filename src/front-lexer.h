
#pragma once

#include <cstdint>
#include <cstring>

namespace bjit
{
    
    struct Symbol
    {
        std::vector<char>   string;
    };
    
    // lexer tokens
    struct Token
    {
        enum Type
        {
            Teof,       // end of file

            // literals
            Tint,
            Tuint,
            Tfloat,

            Tsymbol,

            // keywords
            Tif,
            Telse,

            Twhile,
            Tbreak,
            Tcontinue,

            Treturn,

            // operators - only one use-case is defined here
            // typically this should be the binary operator
            
            ToParen,    // (
            TcParen,    // )

            ToIndex,    // [
            TcIndex,    // ]

            ToBlock,    // {
            TcBlock,    // }

            Tdot,       // .
            Tcolon,     // :
            
            Tcomma,     // ,
            Tsemicolon, // ;

            Tadd,       // +
            Tsub,       // -

            Tmul,       // *
            Tdiv,       // /
            Tmod,       // %

            TshiftL,    // <<
            TshiftR,    // >>

            TbitOr,     // |
            TbitAnd,    // &
            TbitXor,    // ^
            TbitNot,    // ~

            TlogNot,    // !
            TlogAnd,    // &&
            TlogOr,     // ||

            Tassign,    // =
            Tdefine,    // :=

            Teq,        // ==
            TnotEq,     // !=
            Tless,      // <
            TlessEq,    // <=
            Tgreater,   // >
            TgreaterEq, // >=

            // pseudo-token types for the parser
            // these are mostly alternatives to the above
            Tpos,       // unary +
            Tneg,       // unary -

            Tfuncall,   // opening paren for function calls

            TifBody,    // Tif after condition is done
            TwhileBody, // Twhile after condition is done

            Terror      // invalid token
            
        } type;

        int posChar;
        int posLine;

        union
        {
            int64_t     vInt;
            double      vFloat;

            int32_t     nArgs;  // for funcalls

            Symbol *    symbol;
        };
    };
}