#ifndef VERSION_H
#define VERSION_H

#define AMIDEFRAG_VERNUM   "0.3"
#define AMIDEFRAG_VERDATE  "31.10.2026"      /* dd.mm.yyyy, update with VERNUM */
#define AMIDEFRAG_VERSION  "V" AMIDEFRAG_VERNUM
#define AMIDEFRAG_VERTITLE "AmiDefrag " AMIDEFRAG_VERSION
/* AmigaOS "Version" command string; defined in main.c (kept alive there). */
#define AMIDEFRAG_VERSTRING "$VER: AmiDefrag " AMIDEFRAG_VERNUM " (" AMIDEFRAG_VERDATE ")"

/* Defined in build.c, recompiled on every build (see Makefile). */
extern const char AmiDefrag_BuildStamp[];

#endif /* VERSION_H */
