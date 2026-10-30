/* Host (Linux) compatibility shim: the AmigaOS types, constants and
 * prototypes the shared AmiDefrag sources use, so the src/ files compile
 * unmodified with plain gcc.  Only the image-file path is functional;
 * exec device opens fail cleanly. */
#ifndef AMIGA_COMPAT_H
#define AMIGA_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef unsigned char  UBYTE;
typedef signed   char  BYTE;
typedef unsigned short UWORD;
typedef signed   short WORD;
#if ULONG_MAX == 0xFFFFFFFFUL
typedef unsigned long  ULONG;
typedef signed   long  LONG;
#else
typedef unsigned int   ULONG;
typedef signed   int   LONG;
#endif
typedef short          BOOL;
typedef void          *APTR;
typedef char          *STRPTR;
typedef const char    *CONST_STRPTR;
typedef long           BPTR;
typedef unsigned long long UQUAD;
#define AD_UQUAD_DEFINED

#define AD_SIPTR_DEFINED
typedef intptr_t SIPTR;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define MEMF_ANY    0
#define MEMF_PUBLIC (1<<0)
#define MEMF_CLEAR  (1<<16)

#define MODE_OLDFILE   1005
#define MODE_NEWFILE   1006
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT   0
#define OFFSET_END       1

#define RETURN_OK    0
#define RETURN_WARN  5
#define RETURN_ERROR 10
#define RETURN_FAIL  20
#define DOSTRUE  (-1L)
#define DOSFALSE (0L)
#define SIGBREAKF_CTRL_C (1L<<12)
#define NT_REPLYMSG 7

struct MinNode { struct MinNode *mln_Succ, *mln_Pred; };
struct Node { struct Node *ln_Succ, *ln_Pred; UBYTE ln_Type; BYTE ln_Pri; char *ln_Name; };
struct List { struct Node *lh_Head, *lh_Tail, *lh_TailPred; UBYTE lh_Type; UBYTE l_pad; };
struct MsgPort { struct Node mp_Node; UBYTE mp_Flags; UBYTE mp_SigBit; APTR mp_SigTask; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; UWORD mn_Length; };
struct IORequest { struct Message io_Message; APTR io_Device; APTR io_Unit; UWORD io_Command; UBYTE io_Flags; BYTE io_Error; };
struct IOStdReq  { struct Message io_Message; APTR io_Device; APTR io_Unit; UWORD io_Command; UBYTE io_Flags; BYTE io_Error;
                   ULONG io_Actual; ULONG io_Length; APTR io_Data; ULONG io_Offset; };
struct IOExtTD   { struct IOStdReq iotd_Req; ULONG iotd_Count; ULONG iotd_SecLabel; };

#define CMD_READ       2
#define CMD_WRITE      3
#define CMD_UPDATE     4
#define CMD_NONSTD     9
#define TD_GETGEOMETRY 18

struct DriveGeometry {
    ULONG dg_SectorSize, dg_TotalSectors, dg_Cylinders, dg_CylSectors, dg_Heads,
          dg_TrackSectors, dg_BufMemType;
    UBYTE dg_DeviceType, dg_Flags; UWORD dg_Reserved;
};

/* DosEnvec indices (dos/filehandler.h) */
#define DE_TABLESIZE     0
#define DE_SIZEBLOCK     1
#define DE_NUMHEADS      3
#define DE_SECSPERBLK    4
#define DE_BLKSPERTRACK  5
#define DE_RESERVEDBLKS  6
#define DE_LOWCYL        9
#define DE_UPPERCYL      10
#define DE_DOSTYPE       16

/* implemented in amiga_shim.c */
APTR  AllocVec(ULONG size, ULONG flags);
void  FreeVec(APTR p);
BPTR  Open(CONST_STRPTR name, LONG mode);
void  Close(BPTR fh);
LONG  Read(BPTR fh, void *buf, LONG len);
LONG  Seek(BPTR fh, LONG pos, LONG mode);
LONG  IoErr(void);
void  PutStr(CONST_STRPTR s);
BPTR  Input(void);
BPTR  Output(void);
LONG  Flush(BPTR fh);
void  PrintFault(LONG code, CONST_STRPTR hdr);
ULONG SetSignal(ULONG new_sig, ULONG mask);
struct MsgPort *CreateMsgPort(void);
void  DeleteMsgPort(struct MsgPort *p);
LONG  OpenDevice(CONST_STRPTR name, ULONG unit, struct IORequest *io, ULONG flags);
void  CloseDevice(struct IORequest *io);
BYTE  DoIO(struct IORequest *io);
void  host_set_ioerr(LONG err);

#endif /* AMIGA_COMPAT_H */
