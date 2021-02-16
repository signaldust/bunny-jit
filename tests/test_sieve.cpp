
#include "bjit.h"

#ifdef _WIN32
#pragma comment(lib,"winmm.lib")
static inline unsigned getTimeMs()
{
	return timeGetTime();
}
#else
#include <sys/time.h>
static inline unsigned getTimeMs()
{
	timeval time;
	gettimeofday(&time, NULL);
	return (time.tv_sec * 1000) + (time.tv_usec / 1000);
}
#endif

int sieve(char * flags, int size)
{
    int count = 0;

    for (int i = 0; i < size; ++i) flags[i] = true;
    for (int i = 2; i < size; ++i)
    { 
        if (flags[i])
        {
            int prime = i + 1; 
            int k = i + prime; 

            while (k < size)
            { 
                flags[k] = false; 
                k += prime;
            }
            
            ++count;
        }
    }

    return count;
}

static char data[819000];

int main()
{

    bjit::Module module;
    {
        bjit::Proc  pr(0, "ii");

        int _flags = 0;
        int _size = 1;

        // i = 0
        int _i      = pr.env.size(); pr.env.push_back(pr.lci(0));
        // count = 0
        int _count  = pr.env.size(); pr.env.push_back(pr.lci(0));

        auto ls0 = pr.newLabel();
        auto lb0 = pr.newLabel();
        auto le0 = pr.newLabel();

        pr.jmp(ls0);
        
        pr.emitLabel(ls0);
        // while i < size
        pr.jz(pr.ilt(pr.env[_i], pr.env[_size]), le0, lb0);
        pr.emitLabel(lb0);
        
            // *(flags + i) = 1
            pr.si8(pr.iadd(pr.env[_flags], pr.env[_i]), 0, pr.lci(1));
            // ++i
            pr.env[_i] = pr.iadd(pr.env[_i], pr.lci(1));
            pr.jmp(ls0);
            
        pr.emitLabel(le0);

        // i = 2
        pr.env[_i] = pr.lci(2);
        auto ls1 = pr.newLabel();
        auto lb1 = pr.newLabel();
        auto le1 = pr.newLabel();

        pr.jmp(ls1);
        pr.emitLabel(ls1);
        
        // while i < size
        pr.jz(pr.ilt(pr.env[_i], pr.env[_size]), le1, lb1);
        pr.emitLabel(lb1);
            
            auto bt = pr.newLabel();
            auto be = pr.newLabel();
    
            // if flags[i] != 0
            pr.jnz(pr.li8(pr.iadd(pr.env[_flags], pr.env[_i]), 0), bt, be);
            pr.emitLabel(bt);
        
                // prime = i + 1
                int _prime  = pr.env.size();
                pr.env.push_back(pr.iadd(pr.env[_i], pr.lci(1)));
        
                // k = i + prim
                int _k = pr.env.size();
                pr.env.push_back(pr.iadd(pr.env[_i], pr.env[_prime]));
        
                auto ls2 = pr.newLabel();
                auto lb2 = pr.newLabel();
                auto le2 = pr.newLabel();
        
                pr.jmp(ls2);
                pr.emitLabel(ls2);
        
                // while k < size
                pr.jnz(pr.ilt(pr.env[_k], pr.env[_size]), lb2, le2);
                pr.emitLabel(lb2);
        
                    // flags[k] = false;
                    pr.si8(pr.iadd(pr.env[_flags], pr.env[_k]), 0, pr.lci(0));

                    // k = k + prime
                    pr.env[_k] = pr.iadd(pr.env[_k], pr.env[_prime]);
            
                    pr.jmp(ls2);
                    
                pr.emitLabel(le2);
        
                pr.env.pop_back();  // k
                pr.env.pop_back();  // prime
        
                pr.env[_count] = pr.iadd(pr.env[_count], pr.lci(1));
        
                pr.jmp(be);
            
            pr.emitLabel(be);
            
            // ++i
            pr.env[_i] = pr.iadd(pr.env[_i], pr.lci(1));
    
            pr.jmp(ls1);

        pr.emitLabel(le1);

        pr.iret(pr.env[_count]);

        pr.debug();

        module.compile(pr);
    }
    
    auto & codeOut = module.getBytes();
    FILE * f = fopen("out.bin", "wb");
    fwrite(codeOut.data(), 1, codeOut.size(), f);
    fclose(f);
    printf(" - Wrote out.bin\n");
    BJIT_ASSERT(module.load());
    
    auto proc = module.getPointer<int(char*,int)>(0);
    
    printf("C-sieve: %d primes\n", sieve(data, sizeof(data)));
    printf("BJIT-sieve: %d primes\n", proc(data, sizeof(data)));

    BJIT_ASSERT(sieve(data, sizeof(data)) == proc(data, sizeof(data)));

    printf("Iterating 100 times...\n");
    {
        auto start = getTimeMs();

        for(int i = 0; i < 100; ++i)
        {
            sieve(data, sizeof(data));
        }

        printf("C time: %dms\n", getTimeMs() - start);
    }
    
    {
        auto start = getTimeMs();

        for(int i = 0; i < 100; ++i)
        {
            proc(data, sizeof(data));
        }

        printf("BJIT time: %dms\n", getTimeMs() - start);
    }
}