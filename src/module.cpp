
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
# define MAP_ANONYMOUS MAP_ANON  // the joy of being different
#endif

#include <cstring>

#include "bjit.h"

using namespace bjit;

static void flush_cache(char * exec_mem, uint32_t mmapSize)
{
#if defined(__GNUC__) || defined(__clang__)
    // flush icache with a "portable" builtin
    __builtin___clear_cache(exec_mem, exec_mem + mmapSize);
#elif defined(__aarch64__)
#   warning arm64 i-cache might be left stale
#endif
}

uintptr_t Module::load(unsigned mmapSizeMin)
{
    BJIT_ASSERT(!exec_mem);

#ifndef BJIT_CAN_LOAD
    return 0;
#endif

    // compute sizes
    mmapSize = mmapSizeMin;
    loadSize = bytes.size();
    
    if(mmapSize < loadSize) mmapSize = loadSize;

#ifdef BJIT_USE_MMAP
    // get a block of memory we can mess with, read+write
    exec_mem = mmap(NULL, mmapSize, PROT_READ | PROT_WRITE,
        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if(!exec_mem)
    {
        BJIT_LOG("error: mmap failed in bjit::Module::load()\n");
        return 0;
    }
#endif
#ifdef _WIN32
    exec_mem = VirtualAlloc(0, mmapSize, MEM_COMMIT, PAGE_READWRITE);
    if(!exec_mem)
    {
        BJIT_LOG("error: VirtualAlloc failed in bjit::Module::load()\n");
        return 0;
    }
#endif

    // copy & relocate
    memcpy(exec_mem, bytes.data(), bytes.size());
    for(auto & r : relocs)
    {
        BJIT_ASSERT(r.procIndex < offsets.size());
        arch_patchNear(r.codeOffset+(uint8_t*)exec_mem, offsets[r.procIndex]);
    }

#ifdef BJIT_USE_MMAP
    // return zero on success
    if(mprotect(exec_mem, mmapSize, PROT_READ | PROT_EXEC))
    {
        BJIT_LOG("error: mprotect failed in bjit::Module::load()\n");
        // if we can't set executable, then try to unload
        unload();
        return 0;
    }

    flush_cache((char*)exec_mem, mmapSize);

#endif
#ifdef _WIN32
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    // returns non-zero on success
    DWORD   oldFlags = 0;
    if(!VirtualProtect(exec_mem, mmapSize, PAGE_EXECUTE_READ, &oldFlags))
    {
        BJIT_LOG("error: mprotect failed in bjit::Module::load()\n");
        // if we can't set executable, then try to unload
        unload();
        return 0;
    }
#endif

    return (uintptr_t) exec_mem;
}

bool Module::patch()
{
    BJIT_ASSERT(exec_mem);
    
    // check if patching is going to work?
    if(mmapSize < bytes.size()) return false;

#ifdef BJIT_USE_MMAP
    // return zero on success
    BJIT_ASSERT(!mprotect(exec_mem, mmapSize, PROT_READ | PROT_WRITE));
#endif
#ifdef _WIN32
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    // returns non-zero on success
    DWORD   oldFlags = 0;
    BJIT_ASSERT(VirtualProtect(exec_mem, mmapSize, PAGE_READWRITE, &oldFlags));
#endif

    // copy and relocate, only new ones
    memcpy(loadSize+(uint8_t*)exec_mem, loadSize+bytes.data(),
        bytes.size()-loadSize);
    for(auto & r : relocs)
    {
        if(r.codeOffset < loadSize) continue;
        
        BJIT_ASSERT(r.procIndex < offsets.size());
        arch_patchNear(r.codeOffset+(uint8_t*)exec_mem, offsets[r.procIndex]);
    }
    loadSize = bytes.size();

    // do all pending stub-patches
    for(auto & p : stubPatches)
    {
        arch_patchStub(offsets[p.procIndex] + (uint8_t*)exec_mem, p.newAddress);
    }
    stubPatches.clear();

    // near patches
    for(auto & p : nearPatches)
    {
        uint32_t delta = offsets[p.newTarget] - offsets[p.oldTarget];
        for(auto & r : relocs)
        {
            if(r.codeOffset < p.offsetStart
            || r.codeOffset >= p.offsetEnd) continue;
            
            if(r.procIndex == p.oldTarget)
            {
                r.procIndex = p.newTarget;
                // relocate
                arch_patchNear(r.codeOffset+(uint8_t*)exec_mem, delta);
            }
        }
    }
    nearPatches.clear();

#ifdef BJIT_USE_MMAP
    // return zero on success
    BJIT_ASSERT(!mprotect(exec_mem, mmapSize, PROT_READ | PROT_EXEC));

    flush_cache((char*)exec_mem, mmapSize);
    
#endif
#ifdef _WIN32
    // Note that VirtualProtect REQUIRES oldFlags to be a valid pointer!
    // returns non-zero on success
    BJIT_ASSERT(VirtualProtect(exec_mem, mmapSize, PAGE_EXECUTE_READ, &oldFlags));
#endif    

    return true;
}

uintptr_t Module::unload()
{
    BJIT_ASSERT(exec_mem);

#ifdef BJIT_USE_MMAP
    munmap(exec_mem, mmapSize);
#endif
#ifdef _WIN32
    VirtualFree(exec_mem, 0, MEM_RELEASE);
#endif

    uintptr_t ret = (uintptr_t) exec_mem;

    // do near-patches on unload if any
    for(auto & p : nearPatches)
    {
        for(auto & r : relocs)
        {
            if(r.codeOffset < p.offsetStart
            || r.codeOffset >= p.offsetEnd) continue;
            
            if(r.procIndex == p.oldTarget)
            {
                r.procIndex = p.newTarget;
            }
        }
    }

    // patches are not useful after unload
    stubPatches.clear();
    nearPatches.clear();
    
    exec_mem = 0;
    mmapSize = 0;
    loadSize = 0;

    return ret;
}