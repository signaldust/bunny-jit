
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

#include <cstring>

#include "bjit.h"

using namespace bjit;

uintptr_t Module::load(unsigned mmapSizeMin)
{
    assert(!exec_mem);

#ifndef BJIT_CAN_LOAD
    return 0;
#endif

    // compute sizes
    mmapSize = mmapSizeMin;
    loadSize = bytes.size();
    
    if(mmapSize < loadSize) mmapSize = loadSize;

    // relocations
    for(auto & r : relocs)
    {
        assert(r.symbolIndex < offsets.size());
        ((uint32_t*)(bytes.data()+r.codeOffset))[0] += offsets[r.symbolIndex];
    }

#ifdef BJIT_USE_MMAP
    // get a block of memory we can mess with, read+write
    exec_mem = mmap(NULL, mmapSize, PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if(!exec_mem)
    {
        fprintf(stderr, "warning: mmap failed in bjit::Module::load()\n");
        return 0;
    }
    memcpy(exec_mem, bytes.data(), bytes.size());
    // return zero on success
    if(mprotect(exec_mem, mmapSize, PROT_READ | PROT_EXEC))
    {
        fprintf(stderr, "warning: mprotect failed in bjit::Module::load()\n");
        // if we can't set executable, then try to unload
        unload();
        return 0;
    }
    return (uintptr_t) exec_mem;
#endif
#ifdef _WIN32
    exec_mem = VirtualAlloc(0, mmapSize, MEM_COMMIT, PAGE_READWRITE);
    if(!exec_mem)
    {
        fprintf(stderr, "warning: VirtualAlloc failed in bjit::Module::load()\n");
        return 0;
    }
    memcpy(exec_mem, bytes.data(), bytes.size());
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    // returns non-zero on success
    DWORD   oldFlags = 0;
    if(!VirtualProtect(exec_mem, mmapSize, PAGE_EXECUTE_READ, &oldFlags))
    {
        fprintf(stderr, "warning: mprotect failed in bjit::Module::load()\n");
        // if we can't set executable, then try to unload
        unload();
        return 0;
    }
    return (uintptr_t) exec_mem;
#endif
}

bool Module::patch()
{
    assert(exec_mem);
    
    // check if patching is going to work?
    if(mmapSize < bytes.size()) return false;

#ifdef BJIT_USE_MMAP
    // return zero on success
    assert(!mprotect(exec_mem, mmapSize, PROT_READ | PROT_WRITE));
#endif
#ifdef _WIN32
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    // returns non-zero on success
    DWORD   oldFlags = 0;
    assert(VirtualProtect(exec_mem, mmapSize, PAGE_READWRITE, &oldFlags));
#endif

    // relocations, only do new ones
    for(auto & r : relocs)
    {
        if(r.codeOffset < loadSize) continue;
        
        assert(r.symbolIndex < offsets.size());
        ((uint32_t*)(bytes.data()+r.codeOffset))[0] += offsets[r.symbolIndex];
    }

    memcpy(loadSize+(uint8_t*)exec_mem, loadSize+bytes.data(),
        bytes.size()-loadSize);
    loadSize = bytes.size();

#ifdef BJIT_USE_MMAP
    // return zero on success
    assert(!mprotect(exec_mem, mmapSize, PROT_READ | PROT_EXEC));
#endif
#ifdef _WIN32
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    // returns non-zero on success
    assert(VirtualProtect(exec_mem, mmapSize, PAGE_EXECUTE_READ, &oldFlags));
#endif    

    return true;
}

uintptr_t Module::unload()
{
    assert(exec_mem);

    // undo relocations so we can reload
    for(auto & r : relocs)
    {
        if(r.codeOffset >= loadSize) break;
        ((uint32_t*)(bytes.data()+r.codeOffset))[0] -= offsets[r.symbolIndex];
    }

#ifdef BJIT_USE_MMAP
    munmap(exec_mem, mmapSize);
#endif
#ifdef _WIN32
    VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    uintptr_t ret = (uintptr_t) exec_mem;
    
    exec_mem = 0;
    mmapSize = 0;
    loadSize = 0;

    return ret;
}