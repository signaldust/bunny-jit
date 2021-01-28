
#include "bjit.h"

#include "front-parse.h"

int main()
{
    std::vector<uint8_t> codeOut;

    bjit::parse(codeOut);
    
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    
    printf(" - Wrote out.bin\n");
}