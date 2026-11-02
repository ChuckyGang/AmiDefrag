/* host_locale.c - host replacement for src/locale_support.c: no
 * locale.library on Linux, so GS() always serves the built-in English
 * defaults (the same fallback the Amiga build uses on Kickstart 2.04). */
#include "amiga_compat.h"
#define DPSTRINGS_DEFINE_TABLE
#include "amidefrag_strings.h"

void LocaleOpen(void)  {}
void LocaleClose(void) {}

CONST_STRPTR GetDPString(LONG id)
{
    if (id >= 0 && id < MSG_COUNT)
        return (CONST_STRPTR)DPStringDefaults[id];
    return (CONST_STRPTR)"";
}
