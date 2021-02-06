
#include "bjit.h"

#include "front-parse.h"

int main()
{
    std::vector<uint8_t> codeOut;

    bjit::parse(codeOut);

    if(codeOut.size())
    {
        FILE * f = fopen("out.bin", "wb");
        fwrite(codeOut.data(), 1, codeOut.size(), f);
        fclose(f);
        
        BJIT_LOG(" - Wrote out.bin\n");
        return 0;
    }
    else
    {
        return 1;   // probably syntax errors
    }
}