/*
 * locale_support.h - locale.library catalog support for AmiDefrag.
 *
 * The translatable strings live in catalogs/AmiDefrag.cd; the C ids and
 * built-in English defaults are generated from it into amidefrag_strings.h
 * (support/gencat.py, run by the Makefile).
 *
 * locale.library only exists on AmigaOS 2.1 (Kickstart v38) and later.  On
 * Kickstart 2.04 (v37) LocaleOpen() is a harmless no-op and GS() returns
 * the built-in English strings.  The host builds use host/host_locale.c,
 * which always serves the English defaults.
 */
#ifndef LOCALE_SUPPORT_H
#define LOCALE_SUPPORT_H

#include <exec/types.h>
#include "amidefrag_strings.h"

/* Open locale.library (v38+) and AmiDefrag.catalog for the user's preferred
 * language.  No-op when locale.library is unavailable. */
void LocaleOpen(void);
void LocaleClose(void);

/* Localized string for message id `id`, or the built-in English default when
 * no catalog/locale is available.  Never returns NULL. */
CONST_STRPTR GetDPString(LONG id);

/* Convenience wrapper for call sites.  The cast keeps the many char* /
 * const char* / STRPTR call sites warning-free under this build's flags. */
#define GS(id) ((char *)GetDPString(id))

#endif /* LOCALE_SUPPORT_H */
