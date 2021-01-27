
#pragma once

#include <cstdint>
#include <vector>
#include <memory>

#include "hash.h"
#include "front-lexer.h"

namespace bjit
{

    // printf into std::vector<char>
    //
    // appends to the end of the vector, does NOT zero-terminate
    static inline void vformat(std::vector<char> & out, const char * fmt, ...)
    {
        va_list va;
    
        // get length - we need the va_crap twice on x64
        va_start(va, fmt);
        int len = vsnprintf(0, 0, fmt, va);
        va_end(va);
    
        // get the offset
        int vOff = out.size();
        // resize to fit string + null
        out.resize(vOff + len + 1);
    
        va_start(va, fmt);
        vsnprintf(out.data() + vOff, len + 1, fmt, va);
        va_end(va);
    
        // remove null-termination
        out.pop_back();
    }

    struct SymbolPtr
    {
        std::unique_ptr<Symbol> ptr;

        bool isEqual(const SymbolPtr & s) { return isEqual(s.ptr->string); }
        bool isEqual(const std::vector<char> & k)
        {
            return (k.size() == ptr->string.size())
                && !(memcmp(k.data(), ptr->string.data(), k.size()));
        }

        static uint64_t getHash(const SymbolPtr & s) { return getHash(s.ptr->string); }
        static uint64_t getHash(const std::vector<char> & k)
        {
            return stringHash64((uint8_t*)k.data(), k.size());
        }
    };

    struct Parser
    {
        // keep input for error reporting
        std::vector<char>   inputBuffer;
        std::vector<int>    inputLines = { 0 };

        HashTable<SymbolPtr> symbols;
        
        int peek() { return peekChar; }
        void consume()
        {
            if(peekChar == '\n')
            {
                ++posLine; posChar = 0;
    
                // interactive prompt only on TTY
                if (isatty(fileno(stdin))) printf("%6d> ", posLine);
                
                peekPos = inputBuffer.size();
                while(true)
                {
                    int ch = fgetc(stdin);
                    if(0 <= ch) inputBuffer.push_back(ch);
                    if(ch < 0 || ch == '\n') break;
                }
                inputLines.push_back(inputBuffer.size());
            }
            else ++posChar;
    
            if(peekPos < inputBuffer.size())
                peekChar = inputBuffer[peekPos++];
            else peekChar = -1;
        }
    
        unsigned    peekPos = 0;
        int peekChar = '\n';    // make initial consume do a prompt
    
        int posLine = 0;
        int posChar = 0;
    
        // record all errors
        std::vector<char>   errorBuffer;
        std::vector<char>   formatBuffer;

        int nErrors = 0;

        // FIXME: redirect to the error buffer
        void doError(const char * file, int line, int col,
            const char * type, const char * what)
        {
            formatBuffer.clear();
            vformat(formatBuffer, "%s:%d:%d: %s: %s\n",
                file, line, col, type, what);
            vformat(formatBuffer, "    ");
            assert(line < inputLines.size());
            int i = inputLines[line-1];
            int j = inputLines[line];
            while(i < j) formatBuffer.push_back(inputBuffer[i++]);
            if(formatBuffer.back() != '\n') formatBuffer.push_back('\n');
            vformat(formatBuffer, "%*s^\n", 4+col, "");
    
            // append to collected error buffer
            errorBuffer.insert(errorBuffer.end(),
                formatBuffer.begin(), formatBuffer.end());
    
            // then print .. need null terminate for puts
            formatBuffer.push_back(0);
            fflush(stdout); // keep debugs cleaner
            fprintf(stderr, "%s", formatBuffer.data());
        }

        void errorAt(Token & t, const char * what)
        {
            ++nErrors;
            doError("<stdin>", t.posLine, t.posChar, "error", what);
        }

        void warningAt(Token & t, const char * what)
        {
            doError("<stdin>", t.posLine, t.posChar, "warning", what);
        }
        
        // current token
        Token   token;

        // defer is a stack of tokens not yet reduced
        std::vector<Token>  defer;

        // frags is a stack of AST fragments that eventually get
        // consumed when the defer-stack is reduced
        std::vector<std::unique_ptr<struct Expr>> frags;

        void (*state)(Parser &) = 0;
    };

    void lexToken(Parser & ps);

    void parse(std::vector<uint8_t> & codeOut);
}