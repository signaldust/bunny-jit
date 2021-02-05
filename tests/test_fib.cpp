
#include "bjit.h"

int fib(int x)
{
    if( x <= 1 ) return 1;
    return fib(x-1) + fib(x-2);
}

int main()
{

    bjit::Module    module;
    {
        bjit::Proc  pr(0, "i");

        auto lt = pr.newLabel();
        auto le = pr.newLabel();

        pr.jnz(pr.ile(pr.env[0], pr.lci(1)), lt, le);

        pr.emitLabel(lt);
        pr.iret(pr.lci(1));

        pr.emitLabel(le);

        pr.env.push_back(pr.isub(pr.env[0], pr.lci(1)));
        auto a = pr.icalln(0, 1);   // recursive fib(x-1)
        pr.env.pop_back();
        
        pr.env.push_back(pr.isub(pr.env[0], pr.lci(2)));
        auto b = pr.icalln(0, 1);   // recursive fib(x-2)
        pr.env.pop_back();

        pr.iret(pr.iadd(a,b));

        module.compile(pr);
    }
    auto & codeOut = module.getBytes();
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    printf(" - Wrote out.bin\n");
    BJIT_ASSERT(module.load());

    auto x = 16;
    auto y = fib(x);
    printf("C-fib: %d\n", y);

    BJIT_ASSERT(y == module.getPointer<int(int)>(0)(x));

    return 0;
}
