/* Shared state of the combined hc47_tweaks.asi (see tweaks.c): the
 * scripts directory, the common log, the feature entry points, and the
 * cross-feature mode-change handshake. */
#ifndef HC47_COMMON_H
#define HC47_COMMON_H

#include <windows.h>
#include <stdarg.h>

/* scripts directory (no trailing backslash) and the shared log writer;
 * both are set up by DllMain in tweaks.c before any feature init runs.
 * Every feature logs through hc47_vlog with its own tag. */
extern char hc47_dir[MAX_PATH];
void hc47_vlog(const char *tag, const char *fmt, va_list ap);

/* the single config file, sections [Widescreen]/[HudScale]/[HudExtras]/
 * [FrameLimit] — keys are unique across features, so each feature's
 * line parser can scan the whole file */
#define HC47_INI "hc47_tweaks.ini"

/* feature inits, called once from DllMain: each reads its config keys
 * and spawns its own worker thread(s) when enabled */
void framelimit_init(void);
void hudscale_init(void);
void hudextras_init(void);
void widescreen_init(void);

/* widescreen -> hudscale handshake: a GetTickCount stamp (|1, so never
 * zero while set), nonzero from the moment an in-game mode change is
 * staged until the renderer has consumed the ZSysInterface layout
 * fields for the new mode. hudscale holds off its +0x19/+0x1d writes
 * while set and treats a stamp older than ~3s as stale. Defined in
 * widescreen.c. */
extern volatile DWORD HC47_ModeChangeTick;

#endif
