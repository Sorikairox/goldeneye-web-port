#!/usr/bin/env python3
"""The retrowser build's copy of port/src/optionsoverlay.c (CMake GE_RTW only; docs/web/RTW.md "Options").

  python3 scripts/rtw/rtw_overlay_src.py <repo root> <out .c>

The shared source is never edited (the web site build must stay byte-identical, and its wasm carries DWARF line
tables, so even a guarded block that shifts lines would change it). This writes a build-dir copy with exact
replacements, each of which must match exactly once (the script stops otherwise, so an upstream change of the
overlay is noticed at build time). A #line directive keeps diagnostics pointing at the original.

What the copy adds, for an online (lockstep) session only; outside one it behaves exactly like the original:
  1. the overlay opens during a session (the original refuses: every option was the room host's);
  2. rows are filtered by port/rtw/rtw.c geRtwOverlayRowKind: this player's own settings (editable), the room host's
     mapped settings (shown, locked, tip "Set by the room host during online play."), navigation (Back, the Player 1
     link of the Controller page); everything else is hidden (Bond-file values, presets, resets, bindings, Restart /
     Quit: they would change the game on one machine only). Pages with nothing left are hidden on the root list;
  3. rowSetCommit and rowAdjust refuse locked and hidden rows (every value change of the overlay passes there);
  4. rowGet / rowSetCommit ask rtw.c first (geRtwOverlayRowGet / Set): during a session "Show crosshair" is kept as the
     crosshair opacity, so the game itself still draws (and loads) the crosshair on every peer.
"""
import sys

root, out = sys.argv[1], sys.argv[2]
src_path = root + '/port/src/optionsoverlay.c'
text = open(src_path, encoding='utf-8', errors='surrogateescape').read()

LOCK = '    if (geRtwOverlayRowLocked(r->key)) return;   /* rtw: online session: the room host\'s value, or hidden */\n'
EXTERNS = (
    '/* rtw (scripts/rtw/rtw_overlay_src.py): online-session row rules from port/rtw/rtw.c */\n'
    'extern int geRtwOverlaySession(void);\n'
    'extern int geRtwOverlayRowKind(const char *key);   /* 0 hidden, 1 own, 2 room host\'s (locked), 3 nav, 4 link */\n'
    'extern int geRtwOverlayRowLocked(const char *key);\n'
    'extern int geRtwOverlayRowGet(const char *key, double *v);   /* 1: rtw.c answers for this row */\n'
    'extern int geRtwOverlayRowSet(const char *key, double v);    /* 1: rtw.c stored it */\n'
    'static void rtwOverlayFilter(void);\n'
)

LOCK_ANCHOR = 'static void rowSetCommit(struct Row *r, double v, int commit)\n{\n' + LOCK
REPLACE = [
    # the quoted relative include does not resolve from the build dir
    ('#include "../fast3d/gfx_api.h"', '#include "%s/port/fast3d/gfx_api.h"' % root),
    # 1. open during a session
    ('    if (geNetDetOn && !s_open) { sysLogPrintf(LOG_NOTE, "options overlay: not during an online session"); return; }',
     '    if (geNetDetOn && !s_open) sysLogPrintf(LOG_NOTE, "options overlay: online session: only this player\'s own settings can change");'),
    # 2. the row filter, at the end of every visible-list rebuild
    ('static void overlayUpdateVisible(void)\n{\n', EXTERNS + 'static void overlayUpdateVisible(void)\n{\n'),
    ('    if (s_visN == 0) {   /* cannot happen (toggles have no hide source) */',
     '    rtwOverlayFilter();\n    if (s_visN == 0) {   /* cannot happen (toggles have no hide source) */'),
    # 3. the locks
    ('static void rowSetCommit(struct Row *r, double v, int commit)\n{\n',
     'static void rowSetCommit(struct Row *r, double v, int commit)\n{\n' + LOCK),
    ('static void rowAdjust(struct Row *r, int dir)\n{\n', 'static void rowAdjust(struct Row *r, int dir)\n{\n' + LOCK),
    # rows whose session value lives elsewhere (Hide crosshair: the crosshair opacity, so the game still draws it)
    ('static double rowGet(const struct Row *r)\n{\n',
     'static double rowGet(const struct Row *r)\n{\n    { double rv; if (geRtwOverlayRowGet(r->key, &rv)) return rv; }   /* rtw */\n'),
    (LOCK_ANCHOR, LOCK_ANCHOR + '    if (geRtwOverlayRowSet(r->key, v)) return;   /* rtw: kept by rtw.c */\n'),
    # the reason, as the focused row's tip
    ('const char *optionsRowHelp(int i)\n{\n    struct Row *r = rowAt(i);\n    if (!r) return NULL;\n',
     'const char *optionsRowHelp(int i)\n{\n    struct Row *r = rowAt(i);\n    if (!r) return NULL;\n'
     '    if (geRtwOverlaySession() && geRtwOverlayRowKind(r->key) == 2) return "Set by the room host during online play.";\n'),
]

for old, new in REPLACE:
    n = text.count(old)
    if n != 1:
        sys.exit('rtw_overlay_src.py: %d matches (want 1) for: %r' % (n, old[:90]))
    text = text.replace(old, new)

text += r'''

/* ---- rtw: the online-session row filter (scripts/rtw/rtw_overlay_src.py) -------------------------------- */
static int rtwPageHasContent(int hdr)
{
    for (int i = hdr + 1; i < NUM_ROWS && rows[i].kind != ROW_HEADER; i++) {
        if (!rows[i].found || rows[i].hub) continue;
        int k = geRtwOverlayRowKind(rows[i].key);
        if (k == 1 || k == 2 || k == 4) return 1;
    }
    return 0;
}

static void rtwOverlayFilter(void)
{
    if (!geRtwOverlaySession()) return;
    int n = 0;
    for (int q = 0; q < s_visN; q++) {
        const int i = s_visIdx[q];
        int keep;
        if (s_section < 0) keep = rows[i].kind == ROW_HEADER && rtwPageHasContent(i);   /* root: pages with content */
        else if (i == s_section) keep = 1;                                             /* page: its title */
        else if (isSepRow(&rows[i])) {   /* a separator only right after a kept setting (no leading / double ones) */
            const int k = n > 0 ? geRtwOverlayRowKind(rows[s_visIdx[n - 1]].key) : 0;
            keep = k == 1 || k == 2 || k == 4;
        } else keep = geRtwOverlayRowKind(rows[i].key) != 0;
        if (keep) s_visIdx[n++] = i;
    }
    s_visN = n;
    if (s_sel >= s_visN) s_sel = s_visN - 1;
    if (s_section >= 0 && s_sel < 1 && s_visN > 1) s_sel = 1;   /* the title is not selectable */
    if (s_sel < 0) s_sel = 0;
}
'''

with open(out, 'w', encoding='utf-8', errors='surrogateescape') as f:
    f.write('#line 1 "%s"\n' % src_path)
    f.write(text)
