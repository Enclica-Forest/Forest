/* =============================================================================
 * diskio.c -- corruption-tolerant raw block-device layer for ForeB.
 *
 * See diskio.h for the contract. The whole point of this module is that a read
 * over failing media degrades gracefully: unreadable sectors are retried, then
 * zero-filled and counted, and the caller still gets a fully populated buffer.
 * Nothing here ever aborts a read because of a bad block (ddrescue-style).
 * ========================================================================== */
#include "diskio.h"

/* ---- optional serial debug (0x3F8), off by default ----------------------- */
#ifndef DISKIO_DEBUG
#define DISKIO_DEBUG 0
#endif
#if DISKIO_DEBUG && (defined(__x86_64__) || defined(_M_X64))
static inline void dio_outb(unsigned short port, unsigned char val)
{ __asm__ __volatile__("outb %0,%1" : : "a"(val), "Nd"(port)); }
static void dlog(const char *s){ if(!s) return; while(*s) dio_outb(0x3F8,(unsigned char)*s++); }
#elif DISKIO_DEBUG
static void dlog(const char *s){ (void)s; }
#else
static void dlog(const char *s){ (void)s; }
#endif

/* ---- module state (single init) ------------------------------------------ */
static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_GUID gBlkGuid  = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID gDiskGuid = EFI_DISK_IO_PROTOCOL_GUID;

/* Fixed scratch block for ragged sub-block reads. Sized to the largest logical
 * block we support. Used only by the BlockIo fallback in diskio_read_bytes. */
static UINT8 g_scratch[DISKIO_MAX_BLOCK];

/* ---- tiny freestanding helpers ------------------------------------------- */
static void mem_zero(void *p, UINTN n)
{
    if(gBS && n){ gBS->SetMem(p, n, 0); return; }
    UINT8 *b=(UINT8*)p; for(UINTN i=0;i<n;i++) b[i]=0;
}
static void mem_copy(void *d, const void *s, UINTN n)
{
    if(gBS && n){ gBS->CopyMem(d, (void*)s, n); return; }
    UINT8 *dp=(UINT8*)d; const UINT8 *sp=(const UINT8*)s;
    for(UINTN i=0;i<n;i++) dp[i]=sp[i];
}
static UINTN str_copy(char *dst, const char *src, UINTN cap)
{
    UINTN i=0; if(!cap) return 0;
    if(src) for(; src[i] && i+1<cap; i++) dst[i]=src[i];
    dst[i]='\0'; return i;
}
/* append unsigned decimal, returns new length */
static UINTN str_append_u64(char *dst, UINTN len, UINTN cap, UINT64 v)
{
    char tmp[24]; int t=0;
    if(v==0) tmp[t++]='0';
    while(v){ tmp[t++]=(char)('0'+(v%10)); v/=10; }
    while(t>0 && len+1<cap) dst[len++]=tmp[--t];
    dst[len]='\0'; return len;
}
static UINTN str_append(char *dst, UINTN len, UINTN cap, const char *s)
{
    if(s) for(; *s && len+1<cap; s++) dst[len++]=*s;
    dst[len]='\0'; return len;
}

/* ---- init ---------------------------------------------------------------- */
void diskio_init(EFI_SYSTEM_TABLE *st)
{
    gST = st;
    gBS = st ? st->BootServices : 0;
}

/* Build a short human label like "disk 476940 MiB" / "part 512 MiB rm". */
static void build_label(struct diskio_dev *d)
{
    UINTN len=0;
    len = str_copy(d->label, d->logical_partition ? "part " : "disk ", DISKIO_LABEL_MAX);
    len = str_append_u64(d->label, len, DISKIO_LABEL_MAX, d->total_bytes >> 20);
    len = str_append(d->label, len, DISKIO_LABEL_MAX, " MiB");
    if(d->removable) len = str_append(d->label, len, DISKIO_LABEL_MAX, " rm");
    (void)len;
}

/* ---- enumeration --------------------------------------------------------- */
int diskio_enumerate(struct diskio_dev *out, int max)
{
    if(!out || max<=0 || !gBS) return 0;

    UINTN n=0; EFI_HANDLE *h=NULL;
    if(EFI_ERROR(gBS->LocateHandleBuffer(ByProtocol,&gBlkGuid,NULL,&n,&h)) || !h)
        return 0;

    int count=0;
    for(UINTN i=0; i<n && count<max; i++){
        EFI_BLOCK_IO_PROTOCOL *b=NULL;
        if(EFI_ERROR(gBS->HandleProtocol(h[i],&gBlkGuid,(VOID**)&b)) || !b || !b->Media)
            continue;
        EFI_BLOCK_IO_MEDIA *m=b->Media;
        if(!m->MediaPresent) continue;              /* skip empty drives */

        struct diskio_dev *d=&out[count];
        mem_zero(d, sizeof(*d));
        d->bio     = b;
        d->handle  = h[i];
        d->media_id= m->MediaId;
        d->block_size = m->BlockSize ? m->BlockSize : 512;
        d->last_lba   = (UINT64)m->LastBlock;
        d->total_bytes= ((UINT64)m->LastBlock + 1) * (UINT64)d->block_size;
        d->removable        = m->RemovableMedia ? 1 : 0;
        d->logical_partition= m->LogicalPartition ? 1 : 0;

        /* DiskIo is optional; bind it if the same handle exposes it. */
        EFI_DISK_IO_PROTOCOL *dio=NULL;
        if(!EFI_ERROR(gBS->HandleProtocol(h[i],&gDiskGuid,(VOID**)&dio)))
            d->dio = dio;

        build_label(d);
        count++;
    }
    gBS->FreePool(h);
    return count;
}

/* ---- read one block into dst, with retries. Returns 1 ok, 0 bad. --------- */
static int read_block_retry(struct diskio_dev *d, UINT64 lba, void *dst)
{
    UINTN bs=d->block_size;
    for(int try=0; try<DISKIO_RETRIES; try++){
        EFI_STATUS s=d->bio->ReadBlocks(d->bio, d->media_id, (EFI_LBA)lba, bs, dst);
        if(!EFI_ERROR(s)) return 1;
    }
    return 0;
}

/* ---- corruption-tolerant block read -------------------------------------- */
int diskio_read(struct diskio_dev *d, UINT64 lba, UINT32 count, void *buf,
                struct diskio_read_stat *st)
{
    if(st){ st->blocks_ok=0; st->blocks_bad=0; st->first_bad_lba=0; }

    if(!d || !d->bio || !d->bio->Media || !buf || count==0) return -1;
    if(!d->bio->Media->MediaPresent) return -1;
    UINTN bs=d->block_size;
    if(bs==0) return -1;

    /* Fast path: attempt the whole span in a single call. */
    UINTN span=(UINTN)count*bs;
    EFI_STATUS s=d->bio->ReadBlocks(d->bio, d->media_id, (EFI_LBA)lba, span, buf);
    if(!EFI_ERROR(s)){
        if(st) st->blocks_ok += count;
        return 0;
    }

    /* Slow path: read block-by-block, retry, zero-fill persistent failures. */
    dlog("diskio: span read failed, per-block fallback\n");
    UINT64 bad=0;
    for(UINT32 i=0;i<count;i++){
        void *dst=(UINT8*)buf + (UINTN)i*bs;
        if(read_block_retry(d, lba+i, dst)){
            if(st) st->blocks_ok++;
        } else {
            mem_zero(dst, bs);
            if(st){
                if(st->blocks_bad==0) st->first_bad_lba=lba+i;
                st->blocks_bad++;
            }
            bad++;
        }
    }
    return (int)bad;
}

/* ---- byte-offset wrapper ------------------------------------------------- */
int diskio_read_bytes(struct diskio_dev *d, UINT64 offset, void *buf,
                      UINTN len, struct diskio_read_stat *st)
{
    if(st){ st->blocks_ok=0; st->blocks_bad=0; st->first_bad_lba=0; }

    if(!d || !d->bio || !d->bio->Media || !buf) return -1;
    if(!d->bio->Media->MediaPresent) return -1;
    if(len==0) return 0;
    UINTN bs=d->block_size;
    if(bs==0) return -1;

    /* Fast path: byte-granular DiskIo, if the device offers it. */
    if(d->dio){
        EFI_STATUS s=d->dio->ReadDisk(d->dio, d->media_id, offset, len, buf);
        if(!EFI_ERROR(s)){
            /* Account touched blocks so stats stay block-oriented. */
            UINT64 first=offset/bs, last=(offset+len-1)/bs;
            if(st) st->blocks_ok += (last-first+1);
            return 0;
        }
        dlog("diskio: ReadDisk failed, BlockIo fallback\n");
    }

    /* Fallback needs a scratch block, so the logical block must fit it. */
    if(bs>DISKIO_MAX_BLOCK) return -1;

    UINT64 bad=0;
    UINT64 cur=offset;
    UINTN  remaining=len;
    UINT8 *dst=(UINT8*)buf;
    while(remaining){
        UINT64 lba=cur/bs;
        UINTN  in =(UINTN)(cur%bs);
        UINTN  chunk=bs-in;
        if(chunk>remaining) chunk=remaining;

        if(in==0 && chunk==bs){
            /* Whole, block-aligned run: try to grab every contiguous full block
             * remaining in a single ReadBlocks, skipping the scratch + copy
             * round-trip and the one-call-per-block firmware overhead. */
            UINTN nfull=remaining/bs;
            EFI_STATUS s=d->bio->ReadBlocks(d->bio, d->media_id, (EFI_LBA)lba,
                                            nfull*bs, dst);
            if(!EFI_ERROR(s)){
                if(st) st->blocks_ok += nfull;
                UINTN got=nfull*bs;
                dst+=got; cur+=got; remaining-=got;
                continue;
            }
            /* Span read failed: degrade to the retry/zero-fill path one block
             * at a time so a single bad sector doesn't sink the whole run. */
            if(read_block_retry(d, lba, dst)){
                if(st) st->blocks_ok++;
            } else {
                mem_zero(dst, chunk);
                if(st){
                    if(st->blocks_bad==0) st->first_bad_lba=lba;
                    st->blocks_bad++;
                }
                bad++;
            }
        } else if(read_block_retry(d, lba, g_scratch)){
            mem_copy(dst, g_scratch+in, chunk);
            if(st) st->blocks_ok++;
        } else {
            mem_zero(dst, chunk);
            if(st){
                if(st->blocks_bad==0) st->first_bad_lba=lba;
                st->blocks_bad++;
            }
            bad++;
        }
        dst+=chunk; cur+=chunk; remaining-=chunk;
    }
    return (int)bad;
}
