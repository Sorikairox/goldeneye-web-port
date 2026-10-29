/*
 * webstubs.c — port-layer services that have no browser equivalent (web build).
 * Native implementations: port/src/crash.c (signal handlers, thread dumps) and
 * port/src/updatecheck.c (HTTPS release check); both are excluded from the web
 * build in CMakeLists.txt. Crashes surface as wasm traps in the browser console.
 */
#include "crash.h"
#include "updatecheck.h"

void crashInit(void) {}
void crashShutdown(void) {}
void crashDumpThreads(const unsigned long *tids, const char **names, int count)
{
    (void)tids; (void)names; (void)count;
}

void updateCheckStart(void) {}
const char *updateCheckAvailable(void) { return 0; }
void updateCheckOpenReleases(void) {}

/* model.c calls the vertex allocator through `struct Vertex *(*vtxallocator)(s32)`, but the
 * function installed there (chr.c get_ptr_allocated_block_for_vertices) is decompiled as
 * `void f(int)` that only calls dynAllocate(n << 4): on MIPS (and x86-64) the caller reads the
 * pointer dynAllocate left in the return register. wasm traps on the type mismatch
 * (doshadow, first shadowed model in the gun-barrel intro), so the web build installs this
 * adapter instead (scripts/web/web_srcfix.py --fn, initweaponanigroups.c): the same call, with
 * the value the register ABI would have returned. */
extern void *dynAllocate(int size);
void *ge_web_vertex_allocator(int count)
{
    return dynAllocate(count << 4);
}
