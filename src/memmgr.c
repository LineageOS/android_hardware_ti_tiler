/*
 * memmgr.c
 *
 * Memory Allocator Interface functions for TI OMAP processors.
 *
 * Copyright (C) 2009-2010 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define BUF_ALLOCED 0
#define BUF_MAPPED  1

#include <tiler.h>

typedef struct tiler_block_info tiler_block_info;

#define __DEBUG__
/* #define __DEBUG_ENTRY__ */
#define __DEBUG_ASSERT__

#include "memmgr.h"
#include "utils.h"
#include "tilermem.h"
#include "memmgr_utils.h"

#ifdef __STUB_TILER__
SSPtr TilerMgr_PageModeAlloc(bytes_t l) { return 1; }
SSPtr TilerMgr_Alloc(enum tiler_fmt fmt, pixels_t w, pixels_t h) { return 1; }
SSPtr TilerMgr_Map(void *p, bytes_t l) { return 1; }
int TilerMgr_PageModeFree(SSPtr p) { return 0; }
int TilerMgr_Free(SSPtr p) { return 0; }
int TilerMgr_Unmap(SSPtr p) { return 0; }
#else
#include "tilermgr.h"
#endif

/* list of allocations */
struct _AllocData {
    void     *bufPtr;
    uint32_t  tiler_id;
    int       buf_type;
    struct _AllocList {
        struct _AllocList *next, *last;
        struct _AllocData *me;
    } link;
};
struct _AllocList bufs;
static int bufs_inited = 0;

typedef struct _AllocList _AllocList;
typedef struct _AllocData _AllocData;

static int refCnt = 0;
static int td = -1;

/**
 * Initializes the static structures
 * 
 * @author a0194118 (9/8/2009)
 */
static void init()
{
    if (!bufs_inited)
    {
        DLIST_INIT(bufs);
        bufs_inited = 1;
    }
}

/**
 * Increases the reference count.  Initialized tiler if this was 
 * the first reference 
 * 
 * @author a0194118 (9/2/2009)
 * 
 * @return 0 on success, non-0 error value on failure.
 */
static int inc_ref()
{
    /* initialize tiler on first call */
    /* :TODO: concurrency */
    if (!refCnt++) {
        /* initialize lists */
        init();
#ifndef __STUB_TILER__
        td = open("/dev/tiler", O_RDWR | O_SYNC);
        if (NOT_I(td,>=,0) || NOT_I(TilerMgr_Open(),==,0)) {
            refCnt--;
            return MEMMGR_ERR_GENERIC;
        }
#else
        td = 2;
        return 0;
#endif
    }
    return MEMMGR_ERR_NONE;
}

/**
 * Decreases the reference count.  Deinitialized tiler if this 
 * was the last reference 
 * 
 * @author a0194118 (9/2/2009)
 * 
 * @return 0 on success, non-0 error value on failure.
 */
static int dec_ref()
{
    if (refCnt <= 0) return 1;
    if (!--refCnt) {
#ifndef __STUB_TILER__
        close(td);
        td = -1;
        return A_I(TilerMgr_Close(),==,0);
#else
        return 0;
#endif
    }
    return 0;
}

/**
 * Returns the size of the supplied block
 * 
 * @author a0194118 (9/4/2009)
 * 
 * @param blk    Pointer to the tiler_block_info struct
 * 
 * @return size of the block in bytes 
 */
static bytes_t def_size(tiler_block_info *blk)
{
    return (blk->fmt == PIXEL_FMT_PAGE ?
            blk->dim.len :
            blk->dim.area.height * def_stride(blk->dim.area.width * def_bpp(blk->fmt)));
}

/**
 * Returns the tiler format for an address
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param ssptr   Address
 * 
 * @return The tiler format
 */
static enum tiler_fmt tiler_get_fmt(SSPtr ssptr)
{
#ifndef __STUB_TILER__
    return (ssptr == 0              ? TILFMT_INVALID :
            ssptr < TILER_MEM_8BIT  ? TILFMT_NONE :
            ssptr < TILER_MEM_16BIT ? TILFMT_8BIT :
            ssptr < TILER_MEM_32BIT ? TILFMT_16BIT :
            ssptr < TILER_MEM_PAGED ? TILFMT_32BIT :
            ssptr < TILER_MEM_END   ? TILFMT_PAGE : TILFMT_NONE);
#else
    /* if emulating, we need to get through all allocated memory segments */
    init();
    _AllocData *ad;
    void *ptr = (void *) ssptr;
    if (!ptr) return TILFMT_INVALID;
    /* P("?%p", (void *)ssptr); */
    DLIST_MLOOP(bufs, ad, link) {
        int ix;
        struct tiler_buf_info *buf = (struct tiler_buf_info *) ad->tiler_id;
        /* P("buf[%d]", buf->num_blocks); */
        for (ix = 0; ix < buf->num_blocks; ix++)
        {
            /* P("block[%p-%p]", buf->blocks[ix].ptr, buf->blocks[ix].ptr + def_size(buf->blocks + ix)); */
            if (ptr >= buf->blocks[ix].ptr &&
                ptr < buf->blocks[ix].ptr + def_size(buf->blocks + ix))
                return buf->blocks[ix].fmt;
        }
    }
    return TILFMT_NONE;
#endif
}

/**
 * Allocates a memory block using tiler
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blk    Pointer to the block info
 * 
 * @return ssptr of block allocated, or 0 on error
 */
static SSPtr tiler_alloc(struct tiler_block_info *blk)
{
    blk->ptr = NULL;
    if (blk->fmt == PIXEL_FMT_PAGE)
    {
        blk->ssptr = TilerMgr_PageModeAlloc(blk->dim.len);
    }
    else
    {
        blk->ssptr = TilerMgr_Alloc(blk->fmt, blk->dim.area.width, blk->dim.area.height);
        blk->stride = def_stride(blk->dim.area.width * def_bpp(blk->fmt));
    }
    return blk->ssptr;
}

/**
 * Frees a memory block using tiler
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blk    Pointer to the block info
 * 
 * @return 0 on success, non-0 error value on failure.
 */
static int tiler_free(struct tiler_block_info *blk)
{
    if (blk->fmt == PIXEL_FMT_PAGE)
    {
        return TilerMgr_PageModeFree(blk->ssptr);
    }
    else
    {
        return TilerMgr_Free(blk->ssptr);
    }
}

/**
 * Maps a memory block into tiler using tiler
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blk    Pointer to the block info
 * 
 * @return ssptr of block mapped, or 0 on error
 */
static SSPtr tiler_map(struct tiler_block_info *blk)
{
    if (blk->fmt == PIXEL_FMT_PAGE)
    {
        blk->ssptr = TilerMgr_Map(blk->ptr, blk->dim.len);
    }
    else
    {
        blk->ssptr = 0;
    }
    return blk->ssptr;
}

/**
 * Unmaps a memory block from tiler using tiler
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blk    Pointer to the block info
 * 
 * @return 0 on success, non-0 error value on failure.
 */
static int tiler_unmap(struct tiler_block_info *blk)
{
    if (blk->fmt == PIXEL_FMT_PAGE)
    {
        return TilerMgr_Unmap(blk->ssptr);
    }
    else
    {
        return MEMMGR_ERR_GENERIC;
    }
}

/**
 * Returns the size of a buffer.
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blks        Pointer to array of block info structures
 * @param num_blocks  Number of blocks
 * 
 * @return Size of a resulting buffer
 */
static bytes_t tiler_size(struct tiler_block_info *blks, int num_blocks)
{
    bytes_t size = 0;
    int ix;
    for (ix = 0; ix < num_blocks; ix++)
    {
        size += def_size(blks + ix);
    }
    return size;
}

/**
 * Registers a buffer structure with tiler, and maps the buffer 
 * into memory using tiler. On success, it writes the tiler ID 
 * of the buffer into the area pointed by tiler ID. 
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blks        Pointer to array of block info structures
 * @param num_blocks  Number of blocks
 * @param tiler_id      Pointer to tiler ID.
 * 
 * @return pointer to the mapped buffer.
 */
static void *tiler_mmap(struct tiler_block_info *blks, int num_blocks,
                 uint32_t *tiler_id)
{
    /* get size */
    bytes_t size = tiler_size(blks, num_blocks);
    int ix;

    /* register buffer with tiler */
    struct tiler_buf_info buf;
    buf.num_blocks = num_blocks;
    memcpy(buf.blocks, blks, sizeof(tiler_block_info) * num_blocks);
#ifndef __STUB_TILER__
    int ret = ioctl(td, TILIOC_RBUF, &buf);
    if (NOT_I(ret,==,0)) return NULL;

#else
    /* save buffer in stub */
    struct tiler_buf_info *buf_c = NEW(struct tiler_buf_info);
    buf.offset = (uint32_t) buf_c;
#endif        
    if (NOT_P(buf.offset,!=,0)) return NULL;

    /* map blocks to process space */
#ifndef __STUB_TILER__
    void *bufPtr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        td, buf.offset);
#else
    void *bufPtr = malloc(size);
    /* P("<= [0x%lx]", size); */

    /* fill out pointers - this is needed for caching 1D/2D type */
    for (size = ix = 0; ix < num_blocks; ix++)
    {
        buf.blocks[ix].ptr = bufPtr ? bufPtr + size : bufPtr;
        size += def_size(blks + ix);
    }

    memcpy(buf_c, &buf, sizeof(struct tiler_buf_info));
#endif

    /* if failed to map: unregister buffer */
    if (NOT_P(bufPtr,!=,NULL))
    {
#ifndef __STUB_TILER__
        A_I(ioctl(td, TILIOC_URBUF, &buf),==,0);
#else
        FREE(buf_c);
        buf.offset = 0;
#endif
    }
    /* otherwise, fill out pointers */
    else
    {
        /* fill out pointers */
        for (size = ix = 0; ix < num_blocks; ix++)
        {
            blks[ix].ptr = bufPtr + size;
            /* P("   [0x%p]", blks[ix].ptr); */
            size += def_size(blks + ix);
#ifdef __STUB_TILER__
            blks[ix].ssptr = (uint32_t) blks[ix].ptr;
#endif
        }
    }

    *tiler_id = buf.offset;
    return bufPtr;
}

/**
 * Records a buffer-pointer -- tiler-ID mapping for a specific 
 * buffer type. 
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param bufPtr    Buffer pointer
 * @param tiler_id  Tiler ID
 * @param buf_type  Buffer type: BUF_ALLOCED or BUF_MAPPED
 */
static void buf_cache_add(void *bufPtr, uint32_t tiler_id, int buf_type)
{
    _AllocData *ad = NEW(_AllocData);
    ad->bufPtr = bufPtr;
    ad->tiler_id = tiler_id;
    ad->buf_type = buf_type;
    DLIST_MADD_BEFORE(bufs, ad, link);
}

/**
 * Retrieves the tiler ID for given buffer pointer and buffer 
 * type from the records.  If the tiler ID is found, it is 
 * removed from the records as well. 
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param bufPtr    Buffer pointer
 * @param buf_type  Buffer type: BUF_ALLOCED or BUF_MAPPED
 * 
 * @return uint32_t Tiler ID on success, 0 on failure.
 */
static uint32_t buf_cache_del(void *bufPtr, int buf_type)
{
    _AllocData *ad;
    DLIST_MLOOP(bufs, ad, link) {
        if (ad->bufPtr == bufPtr && ad->buf_type == buf_type) {
            uint32_t tiler_id = ad->tiler_id;
            DLIST_REMOVE(ad->link);
            FREE(ad);
            return tiler_id;
        }
    }
    return 0;
}

/**
 * Checks the consistency of the internal record cache.  The 
 * number of elements in the cache should equal to the number of 
 * references. 
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @return 0 on success, non-0 error value on failure. 
 */
static int cache_check()
{
    int num_bufs = 0;

    init();

    _AllocData *ad;
    DLIST_MLOOP(bufs, ad, link) { num_bufs++; }

    return (num_bufs == refCnt) ? MEMMGR_ERR_NONE : MEMMGR_ERR_GENERIC;
}

/**
 * Checks whether the tiler_block_info is filled in correctly. 
 * Verifies the pixel format, correct length, width and or 
 * height, the length/stride relationship for 1D buffers, and 
 * the correct stride for 2D buffers.  Also verifies block size 
 * to be page sized if desired. 
 * 
 * @author a0194118 (9/4/2009)
 * 
 * @param blk            Pointer to the tiler_block_info struct
 * @param is_page_sized  Whether the block needs to be page 
 *                       sized (fit on whole pages).
 * @return 0 on success, non-0 error value on failure. 
 */
static int check_block(tiler_block_info *blk, bool is_page_sized)
{
    /* check pixelformat */
    if (NOT_I(blk->fmt,>=,PIXEL_FMT_MIN) ||
        NOT_I(blk->fmt,<=,PIXEL_FMT_MAX)) return MEMMGR_ERR_GENERIC;

    
    if (blk->fmt == PIXEL_FMT_PAGE)
    {   /* check 1D buffers */

        /* length must be multiple of stride if stride > 0 */
        if (NOT_I(blk->dim.len,>,0) ||
            (blk->stride && NOT_I(blk->dim.len % blk->stride,==,0)))
            return MEMMGR_ERR_GENERIC;
    }
    else 
    {   /* check 2D buffers */
   
        /* check width, height and stride (must be the default stride or 0) */
        bytes_t stride = def_stride(blk->dim.area.width * def_bpp(blk->fmt));
        if (NOT_I(blk->dim.area.width,>,0) ||
            NOT_I(blk->dim.area.height,>,0) ||
            (blk->stride && NOT_I(blk->stride,==,stride)))
            return MEMMGR_ERR_GENERIC;
    }

    if (is_page_sized && NOT_I(def_size(blk) & (PAGE_SIZE - 1),==,0))
        return MEMMGR_ERR_GENERIC;

    return MEMMGR_ERR_NONE;
}

/**
 * Checks whether the block information is correct for map and 
 * alloc operations.  Checks the number of blocks, and validity 
 * of each block.  Warns if reserved member is not 0. 
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blks                 Pointer to the block info array.
 * @param num_blocks           Number of blocks.
 * @param num_pagesize_blocks  Number of blocks that must be 
 *                             page sized (these must be in
 *                             front)
 * 
 * @return 0 on success, non-0 error value on failure. 
 */
static int check_blocks(struct tiler_block_info *blks, int num_blocks,
                 int num_pagesize_blocks)
{
    /* check arguments */
    if (NOT_I(num_blocks,>,0) ||
        NOT_I(num_blocks,<=,TILER_MAX_NUM_BLOCKS)) return MEMMGR_ERR_GENERIC;

    /* check block allocation params */
    int ix;
    for (ix = 0; ix < num_blocks; ix++)
    {
        struct tiler_block_info *blk = blks + ix;
        CHK_I(blk->ssptr,==,0);
        int ret = check_block(blk, ix < num_pagesize_blocks);
        if (ret)
        {
            DP("for block[%d]", ix);
            return ret;
        }
    }

    return MEMMGR_ERR_NONE;
}

/**
 * Resets the ptr and reserved fields of the block info 
 * structures. 
 * 
 * @author a0194118 (9/7/2009)
 * 
 * @param blks                 Pointer to the block info array.
 * @param num_blocks           Number of blocks.
 */
static void reset_blocks(struct tiler_block_info *blks, int num_blocks)
{
    int ix;
    for (ix = 0; ix < num_blocks; ix++)
    {
        blks[ix].ssptr = 0;
        blks[ix].ptr = NULL;
    }

}

bytes_t MemMgr_PageSize()
{
    return PAGE_SIZE;
}

void *MemMgr_Alloc(MemAllocBlock blocks[], int num_blocks)
{
    IN;
    void *bufPtr = NULL;

    /* need to access ssptrs */
    struct tiler_block_info *blks = (tiler_block_info *) blocks;

    /* check block allocation params, and state */
    if (NOT_I(check_blocks(blks, num_blocks, num_blocks - 1),==,0) ||
        NOT_I(inc_ref(),==,0)) goto DONE;

    /* ----- begin recoverable portion ----- */
    int ix;
    uint32_t tiler_id;

    /* allocate each buffer using tiler driver and initialize block info */
    for (ix = 0; ix < num_blocks; ix++)
    {
        CHK_I(blks[ix].ptr,==,NULL);
        if (NOT_I(tiler_alloc(blks + ix),>,0)) goto FAIL_ALLOC;
    }

    bufPtr = tiler_mmap(blks, num_blocks, &tiler_id);
    if (A_P(bufPtr,!=,0)) {
        /* cache tiler ID for buffer */
        buf_cache_add(bufPtr, tiler_id, BUF_ALLOCED);
        goto DONE;
    }
    
    /* ------ error handling ------ */
FAIL_ALLOC:
    while (ix)
    {
        tiler_free(blks + --ix);
    }

    /* clear ssptr and ptr fields for all blocks */
    reset_blocks(blks, num_blocks);

    A_I(dec_ref(),==,0);
DONE:
    CHK_I(cache_check(),==,0);
    return R_P(bufPtr);
}

int MemMgr_Free(void *bufPtr)
{
    IN;

    int ret = MEMMGR_ERR_GENERIC;
    struct tiler_buf_info buf;

    /* retrieve registered buffers from vsptr */
    /* :NOTE: if this succeeds, Memory Allocator stops tracking this buffer */
    buf.offset = buf_cache_del(bufPtr, BUF_ALLOCED);

    if (A_L(buf.offset,!=,0))
    {
#ifndef __STUB_TILER__
        /* get block information for the buffer */
        ret = A_I(ioctl(td, TILIOC_QBUF, &buf),==,0);

        /* unregister buffer, and free tiler chunks even if there is an
           error */
        if (!ret)
        {
            ret = A_I(ioctl(td, TILIOC_URBUF, &buf),==,0);
    
            /* free each block */
            int ix;
            for (ix = 0; ix < buf.num_blocks; ix++)
            {
                ERR_ADD(ret, tiler_free(buf.blocks + ix));
            }

            /* unmap buffer */
            bytes_t size = tiler_size(buf.blocks, buf.num_blocks);
            ERR_ADD(ret, munmap(bufPtr, size));
        }
#else
        void *ptr = (void *) buf.offset;
        FREE(ptr);
        ret = MEMMGR_ERR_NONE;
#endif
        ERR_ADD(ret, dec_ref());
    }

    CHK_I(cache_check(),==,0);
    return R_I(ret);
}

void *MemMgr_Map(MemAllocBlock blocks[], int num_blocks)
{
    IN;
    void *bufPtr = NULL;

    /* need to access ssptrs */
    struct tiler_block_info *blks = (tiler_block_info *) blocks;

    /* check block params, and state */
    if (check_blocks(blks, num_blocks, num_blocks) ||
        NOT_I(inc_ref(),==,0)) goto DONE;

    /* we only map 1 page aligned 1D buffer for now */
    if (NOT_I(num_blocks,==,1) ||
        NOT_I(blocks[0].pixelFormat,==,PIXEL_FMT_PAGE) ||
        NOT_I(blocks[0].dim.len & (PAGE_SIZE - 1),==,0) ||
#ifdef __STUB_TILER__
        NOT_I(MemMgr_IsMapped(blocks[0].ptr),==,0) ||
#endif
        NOT_I((uint32_t)blocks[0].ptr & (PAGE_SIZE - 1),==,0))
        goto FAIL;

    /* ----- begin recoverable portion ----- */
    int ix;
    uint32_t tiler_id;

    /* allocate each buffer using tiler driver */
    for (ix = 0; ix < num_blocks; ix++)
    {
        if (NOT_I(blks[ix].ptr,!=,NULL) ||
            NOT_I(tiler_map(blks + ix),>,0)) goto FAIL_MAP;
    }

    /* map bufer into tiler space and register with tiler manager */
    bufPtr = tiler_mmap(blks, num_blocks, &tiler_id);
    if (A_P(bufPtr,!=,0)) {
        /* cache tiler ID for buffer */
        buf_cache_add(bufPtr, tiler_id, BUF_MAPPED);
        goto DONE;
    }

    /* ------ error handling ------ */
FAIL_MAP:
    while (ix)
    {
        tiler_unmap(blks + --ix);
    }

FAIL:
    /* clear ssptr and ptr fields for all blocks */
    reset_blocks(blks, num_blocks);

    A_I(dec_ref(),==,0);
DONE:
    CHK_I(cache_check(),==,0);
    return R_P(bufPtr);
}

int MemMgr_UnMap(void *bufPtr)
{
    IN;

    int ret = MEMMGR_ERR_GENERIC;
    struct tiler_buf_info buf;

    /* retrieve registered buffers from vsptr */
    /* :NOTE: if this succeeds, Memory Allocator stops tracking this buffer */
    buf.offset = buf_cache_del(bufPtr, BUF_MAPPED);

    if (A_L(buf.offset,!=,0))
    {
#ifndef __STUB_TILER__
        /* get block information for the buffer */
        ret = A_I(ioctl(td, TILIOC_QBUF, &buf),==,0);

        /* unregister buffer, and free tiler chunks even if there is an
           error */
        if (!ret)
        {
            ret = A_I(ioctl(td, TILIOC_URBUF, &buf),==,0);
    
            /* unmap each block */
            int ix;
            for (ix = 0; ix < buf.num_blocks; ix++)
            {
                ERR_ADD(ret, tiler_unmap(buf.blocks + ix));
            }

            /* unmap buffer */
            bytes_t size = tiler_size(buf.blocks, buf.num_blocks);
            ERR_ADD(ret, munmap(bufPtr, size));
        }
#else
        void *ptr = (void *) buf.offset;
        FREE(ptr);
        ret = MEMMGR_ERR_NONE;
#endif
        ERR_ADD(ret, dec_ref());
    }

    CHK_I(cache_check(),==,0);
    return R_I(ret);
}

bool MemMgr_Is1DBlock(void *ptr)
{
    IN;

    SSPtr ssptr = TilerMem_VirtToPhys(ptr);
    enum tiler_fmt fmt = tiler_get_fmt(ssptr);
    return R_I(fmt == TILFMT_PAGE);
}

bool MemMgr_Is2DBlock(void *ptr)
{
    IN;

    SSPtr ssptr = TilerMem_VirtToPhys(ptr);
    enum tiler_fmt fmt = tiler_get_fmt(ssptr);
    return R_I(fmt == TILFMT_8BIT || fmt == TILFMT_16BIT ||
               fmt == TILFMT_32BIT);
}

bool MemMgr_IsMapped(void *ptr)
{
    IN;

    SSPtr ssptr = TilerMem_VirtToPhys(ptr);
    enum tiler_fmt fmt = tiler_get_fmt(ssptr);
    return R_I(fmt == TILFMT_8BIT || fmt == TILFMT_16BIT ||
               fmt == TILFMT_32BIT || fmt == TILFMT_PAGE);
}

bytes_t MemMgr_GetStride(void *ptr)
{
    IN;
#ifndef __STUB_TILER__
    /* :TODO: */
    return R_UP(PAGE_SIZE);
#else
    /* if emulating, we need to get through all allocated memory segments */
    init();

    _AllocData *ad;
    if (!ptr) return R_UP(0);
    DLIST_MLOOP(bufs, ad, link) {
        int ix;
        struct tiler_buf_info *buf = (struct tiler_buf_info *) ad->tiler_id;
        for (ix = 0; ix < buf->num_blocks; ix++)
        {
            if (ptr >= buf->blocks[ix].ptr &&
                ptr < buf->blocks[ix].ptr + def_size(buf->blocks + ix))
                return R_UP(buf->blocks[ix].stride);
        }
    }
    return R_UP(PAGE_SIZE);
#endif
}

bytes_t TilerMem_GetStride(SSPtr ssptr)
{
    IN;
    switch(tiler_get_fmt(ssptr))
    {
    case TILFMT_8BIT:  return R_UP(TILER_STRIDE_8BIT);
    case TILFMT_16BIT: return R_UP(TILER_STRIDE_16BIT);
    case TILFMT_32BIT: return R_UP(TILER_STRIDE_32BIT);
    case TILFMT_PAGE:  return R_UP(PAGE_SIZE);
    default:           return R_UP(0);
    }
}

SSPtr TilerMem_VirtToPhys(void *ptr)
{
#ifndef __STUB_TILER__
    return TilerMgr_VirtToPhys(ptr);
#else
    return (SSPtr)ptr;
#endif
}

/**
 * Internal Unit Test.  Tests the static methods of this library
 * 
 * @author a0194118 (9/4/2009)
 */
void memmgr_internal_unit_test()
{
    A_I(refCnt,==,0);
    A_I(inc_ref(),==,0);
    A_I(refCnt,==,1);
    A_I(dec_ref(),==,0);
    A_I(refCnt,==,0);

    /* void * arithmetic */
    void *a = (void *)1000, *b = a + 2000, *c = (void *)3000;
    A_P(b,==,c);

    /* def_stride */
    A_I(def_stride(0),==,0);
    A_I(def_stride(1),==,PAGE_SIZE);
    A_I(def_stride(PAGE_SIZE),==,PAGE_SIZE);
    A_I(def_stride(PAGE_SIZE + 1),==,2 * PAGE_SIZE);

    /* def_bpp */
    A_I(def_bpp(PIXEL_FMT_32BIT),==,4);
    A_I(def_bpp(PIXEL_FMT_16BIT),==,2);
    A_I(def_bpp(PIXEL_FMT_8BIT),==,1);

    /* def_size */
    tiler_block_info blk;
    blk.fmt = TILFMT_8BIT;
    blk.dim.area.width = PAGE_SIZE * 8 / 10;
    blk.dim.area.height = 10;
    A_I(def_size(&blk),==,10 * PAGE_SIZE);

    blk.fmt = TILFMT_16BIT;
    blk.dim.area.width = PAGE_SIZE * 7 / 10;
    A_I(def_size(&blk),==,20 * PAGE_SIZE);
    blk.dim.area.width = PAGE_SIZE * 4 / 10;
    A_I(def_size(&blk),==,10 * PAGE_SIZE);

    blk.fmt = TILFMT_32BIT;
    A_I(def_size(&blk),==,20 * PAGE_SIZE);
    blk.dim.area.width = PAGE_SIZE * 6 / 10;
    A_I(def_size(&blk),==,30 * PAGE_SIZE);
}

