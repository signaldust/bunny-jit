
// We need separate logic for Unix vs. Windows for loading code.
// Define BJIT_USE_MMAP on platforms where we can use the Unix version.
#if defined(__unix__) || defined(__LINUX__) || defined(__APPLE__)
#  define BJIT_USE_MMAP
#  define BJIT_CAN_LOAD
#  include <sys/mman.h>
#endif

#if defined(_WIN32)
#  define BJIT_CAN_LOAD
#  include <windows.h>
#endif

#ifdef __APPLE__
#define MAP_ANONYMOUS MAP_ANON  // the joy of being different
#endif

#include "bjit.h"

using namespace bjit;

bool Module::load()
{
    assert(!exec_mem);

    // relocations
    for(auto & r : relocs)
    {
        assert(r.symbolIndex < offsets.size());
        ((uint32_t*)(bytes.data()+r.codeOffset))[0] += offsets[r.symbolIndex];
    }

#ifdef BJIT_USE_MMAP
    // get a block of memory we can mess with, read+write
    exec_mem = mmap(NULL, bytes.size(), PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if(!exec_mem)
    {
        fprintf(stderr, "warning: mmap failed in bjit::Module::load()\n");
        return false;
    }
    memcpy(exec_mem, bytes.data(), bytes.size());
    // return zero on success
    if(mprotect(exec_mem, bytes.size(), PROT_READ | PROT_EXEC))
    {
        fprintf(stderr, "warning: mprotect failed in bjit::Module::load()\n");
        // if we can't set executable, then try to unload
        unload();
        return false;
    }
    return true;
#endif

#ifdef _WIN32
    exec_mem = VirtualAlloc(0, bytes.size(), MEM_COMMIT, PAGE_READWRITE);
    if(!exec_mem)
    {
        fprintf(stderr, "warning: VirtualAlloc failed in bjit::Module::load()\n");
        return false;
    }
    memcpy(exec_mem, bytes.data(), bytes.size());
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    DWORD   oldFlags = 0;
    if(!VirtualProtect(exec_mem, bytes.size(), PAGE_EXECUTE_READ, &oldFlags))
    {
        fprintf(stderr, "warning: mprotect failed in bjit::Module::load()\n");
        // if we can't set executable, then try to unload
        unload();
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

    // undo relocations so we can reload
    for(auto & r : relocs)
    {
        ((uint32_t*)(bytes.data()+r.codeOffset))[0] -= offsets[r.symbolIndex];
    }

#ifdef BJIT_USE_MMAP
    munmap(exec_mem, bytes.size());
    exec_mem = 0;
#endif

#ifdef _WIN32
    VirtualFree(exec_mem, 0, MEM_RELEASE);
    exec_mem = 0;
#endif

#ifndef BJIT_CAN_LOAD
    assert(false);
#endif
}