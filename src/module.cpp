
// We need separate logic for Unix vs. Windows for loading code.
// Define BJIT_USE_MMAP on platforms where we can use the Unix version.
#if defined(__unix__) || defined(__LINUX__) || defined(__APPLE__)
#  define BJIT_USE_MMAP
#  define BJIT_CAN_LOAD
#  include <sys/mman.h>
#endif

#ifdef __APPLE__
#define MAP_ANONYMOUS MAP_ANON  // the joy of being different
#endif

#include "bjit.h"

using namespace bjit;

bool Module::load()
{
    assert(!exec_mem);

#ifdef BJIT_USE_MMAP

    // get a block of memory we can mess with, read+write
    exec_mem = mmap(NULL, bytes.size(), PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if(!exec_mem)
    {
        fprintf(stderr, "warning: mmap failed in bjit::Module::load()\n");
        return false;
    }

    // copy the code into this block
    memcpy(exec_mem, bytes.data(), bytes.size());

    if (mprotect(exec_mem, bytes.size(), PROT_READ | PROT_EXEC) == -1)
    {
        fprintf(stderr, "warning: mprotect failed in bjit::Module::load()\n");
        // if we can't mprotect, try to unmap
        // if that fails too, there's not much we can do
        munmap(exec_mem, bytes.size());
        return false;
    }

    return true;
    
#endif

#ifndef BJIT_CAN_LOAD
    return false;
#endif
}

void Module::unload()
{
    assert(exec_mem);

#ifdef BJIT_USE_MMAP
    munmap(exec_mem, bytes.size());
    exec_mem = 0;
#endif

#ifndef BJIT_CAN_LOAD
    assert(false);
#endif
}