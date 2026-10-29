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
