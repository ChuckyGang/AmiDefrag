/* POSIX bodies for the AmigaOS calls the shared sources use (host build). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include "amiga_compat.h"

static LONG g_ioerr = 0;
void host_set_ioerr(LONG err) { g_ioerr = err; }
LONG IoErr(void) { return g_ioerr; }

APTR AllocVec(ULONG size, ULONG flags)
{
    void *p = malloc(size ? size : 1);
    if (p && (flags & MEMF_CLEAR)) memset(p, 0, size);
    return p;
}
void FreeVec(APTR p) { free(p); }

BPTR Open(CONST_STRPTR name, LONG mode)
{
    FILE *f = fopen(name, mode == MODE_NEWFILE ? "w+b" : "rb");
    if (!f) { g_ioerr = 205; return 0; }
    return (BPTR)(size_t)f;
}
void Close(BPTR fh) { if (fh) fclose((FILE *)(size_t)fh); }
LONG Read(BPTR fh, void *buf, LONG len) { return fh ? (LONG)fread(buf, 1, (size_t)len, (FILE *)(size_t)fh) : -1; }
LONG Seek(BPTR fh, LONG pos, LONG mode)
{
    FILE *f = (FILE *)(size_t)fh;
    long old;
    int whence = (mode == OFFSET_END) ? SEEK_END : (mode == OFFSET_CURRENT) ? SEEK_CUR : SEEK_SET;
    if (!f) return -1;
    old = ftell(f);
    if (fseek(f, pos, whence) != 0) return -1;
    return (LONG)old;
}

LONG HostFilePRead(BPTR fh, void *buf, ULONG len, UQUAD off)
{
    FILE *f = (FILE *)(size_t)fh;
    if (!f) return -1;
    fflush(f);
    return (LONG)pread(fileno(f), buf, (size_t)len, (off_t)off);
}
UQUAD HostFileSize(BPTR fh)
{
    FILE *f = (FILE *)(size_t)fh;
    struct stat st;
    if (!f || fstat(fileno(f), &st) != 0) return 0;
    return (UQUAD)st.st_size;
}

void PutStr(CONST_STRPTR s) { fputs(s, stdout); }
void PrintFault(LONG code, CONST_STRPTR hdr) { fprintf(stderr, "%s: error %ld\n", hdr, (long)code); }

static volatile int g_ctrlc = 0;
static void on_sigint(int s) { (void)s; g_ctrlc = 1; }
ULONG SetSignal(ULONG new_sig, ULONG mask)
{
    static int armed = 0;
    (void)new_sig; (void)mask;
    if (!armed) { signal(SIGINT, on_sigint); armed = 1; }
    return g_ctrlc ? SIGBREAKF_CTRL_C : 0;
}

/* No exec devices on the host: BlockDev_Open fails at the port stage. */
struct MsgPort *CreateMsgPort(void) { return NULL; }
void DeleteMsgPort(struct MsgPort *p) { (void)p; }
LONG OpenDevice(CONST_STRPTR n, ULONG u, struct IORequest *io, ULONG f) { (void)n;(void)u;(void)io;(void)f; return -1; }
void CloseDevice(struct IORequest *io) { (void)io; }
BYTE DoIO(struct IORequest *io) { (void)io; return -1; }
