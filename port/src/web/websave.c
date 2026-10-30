/*
 * websave.c — saves that persist in the browser (GE_WEB; page side: port/web/saves.js).
 *
 * The game keeps its two files where the native port keeps them, in the Emscripten MEMFS under /data:
 * the 2048-byte EEPROM image ge007.eep (libultra.c geEepromStore, rewritten on every game save) and the
 * config ge007.ini (config.c configSave). MEMFS is lost when the page closes, so after each write the C
 * side calls webSaveWritten(); the page marks the file dirty and copies it to IndexedDB after a short
 * debounce, and at once when the tab is hidden or the page is closed (beforeunload's configSave
 * included). Before main() the page writes the stored copies back into /data, so the game's first
 * EEPROM read and configLoad see them.
 *
 * Online-session seam (multiplayer playbook 10.2 item 8: the host's EEPROM image, sandboxed, never
 * persisted): ge_web_save_sandbox(1) makes geEepromStore keep the image in RAM only (no /data write, no
 * notification) and stops the config notifications; the page loads the host's bytes with
 * ge_web_eeprom_load() (libultra.c) and turns its own persistence off. See saves.js GESaves.sandbox().
 */
#include <stddef.h>  /* port/shim/stddef.h: size_t before the Emscripten headers */
#include <emscripten.h>

static int s_sandbox = 0;

int webSaveSandboxed(void) { return s_sandbox; }

/* what: 1 = /data/ge007.eep, 2 = /data/ge007.ini */
void webSaveWritten(int what)
{
    if (s_sandbox) return;
    EM_ASM({ if (Module.geSaveWritten) Module.geSaveWritten($0); }, what);
}

EMSCRIPTEN_KEEPALIVE void ge_web_save_sandbox(int on) { s_sandbox = on ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE int ge_web_save_is_sandboxed(void) { return s_sandbox; }
