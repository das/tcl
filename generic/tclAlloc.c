/* 
 * tclAlloc.c --
 *
 *	This is a very fast storage allocator.  It allocates blocks of a
 *	small number of different sizes, and keeps free lists of each size.
 *	Blocks that don't exactly fit are passed up to the next larger size.
 *	Blocks over a certain size are directly allocated from the system.
 *
 * Copyright (c) 1983 Regents of the University of California.
 * Copyright (c) 1996-1997 Sun Microsystems, Inc.
 *
 * Portions contributed by Chris Kingsley, Jack Jansen and Ray Johnson.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclPort.h"

#if USE_TCLALLOC

#ifdef TCL_DEBUG
#   define DEBUG
/* #define MSTATS */
#   define RCHECK
#endif

typedef unsigned long caddr_t;

/*
 * The overhead on a block is at least 8 bytes.  When free, this space
 * contains a pointer to the next free block, and the bottom two bits must
 * be zero.  When in use, the first byte is set to MAGIC, and the second
 * byte is the size index.  The remaining bytes are for alignment.
 * If range checking is enabled then a second word holds the size of the
 * requested block, less 1, rounded up to a multiple of sizeof(RMAGIC).
 * The order of elements is critical: ov_magic must overlay the low order
 * bits of ov_next, and ov_magic can not be a valid ov_next bit pattern.
 */

union overhead {
    union overhead *ov_next;	/* when free */
    unsigned char ov_padding[8]; /* Ensure the structure is 8-byte aligned. */
    struct {
	unsigned char	ovu_magic0;	/* magic number */
	unsigned char	ovu_index;	/* bucket # */
	unsigned char	ovu_unused;	/* unused */
	unsigned char	ovu_magic1;	/* other magic number */
#ifdef RCHECK
	unsigned short	ovu_rmagic;	/* range magic number */
	unsigned long	ovu_size;	/* actual block size */
	unsigned short  ovu_unused2;    /* padding to 8-byte align */
#endif
    } ovu;
#define ov_magic0	ovu.ovu_magic0
#define ov_magic1	ovu.ovu_magic1
#define ov_index	ovu.ovu_index
#define ov_rmagic	ovu.ovu_rmagic
#define ov_size		ovu.ovu_size
};


#define MAGIC		0xef		/* magic # on accounting info */
#define RMAGIC		0x5555		/* magic # on range info */

#ifdef RCHECK
#define	RSLOP		sizeof (unsigned short)
#else
#define	RSLOP		0
#endif

#define OVERHEAD (sizeof(union overhead) + RSLOP)

/*
 * nextf[i] is the pointer to the next free block of size 2^(i+3).  The
 * smallest allocatable block is 8 bytes.  The overhead information
 * precedes the data area returned to the user.
 */

#define NBUCKETS	13
#define MAXMALLOC	(1<<(NBUCKETS+2))
static	union overhead *nextf[NBUCKETS];

/* 
 * The following structure is used to keep track of all system memory 
 * currently owned by Tcl.  When finalizing, all this memory will
 * be returned to the system.
 */

struct block {
    struct block *nextPtr;	/* Linked list. */
    struct block *prevPtr;	/* Linked list for big blocks, ensures 8-byte 
				 * alignment for suballocated blocks. */
};

static struct block *blockList;		/* Tracks the suballocated blocks. */
static struct block bigBlocks = {	/* Big blocks aren't suballocated. */
    &bigBlocks, &bigBlocks
};

/*
 * The allocator is protected by a special mutex that must be
 * explicitly initialized.  Futhermore, because Tcl_Alloc may be
 * used before anything else in Tcl, we make this module self-initializing
 * after all with the allocInit variable.
 */

#ifdef TCL_THREADS
static TclpMutex allocMutex;
#endif
static int allocInit = 0;


#ifdef MSTATS

/*
 * nmalloc[i] is the difference between the number of mallocs and frees
 * for a given block size.
 */

static	unsigned int nmalloc[NBUCKETS+1];
#include <stdio.h>
#endif

#if defined(DEBUG) || defined(RCHECK)
#define	ASSERT(p)   if (!(p)) panic(# p)
#define RANGE_ASSERT(p) if (!(p)) panic(# p)
#else
#define	ASSERT(p)
#define RANGE_ASSERT(p)
#endif

/*
 * Prototypes for functions used only in this file.
 */

static void 		MoreCore _ANSI_ARGS_((int bucket));


/*
 *-------------------------------------------------------------------------
 *
 * TclInitAlloc --
 *
 *	Initialize the memory system.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Initialize the mutex used to serialize allocations.
 *
 *-------------------------------------------------------------------------
 */

void
TclInitAlloc()
{
    if (!allocInit) {
	allocInit = 1;
	TclpMutexInit(&allocMutex);
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * TclFinalizeAllocSubsystem --
 *
 *	Release all resources being used by this subsystem, including 
 *	aggressively freeing all memory allocated by TclpAlloc() that 
 *	has not yet been released with TclpFree().
 *	
 *	After this function is called, all memory allocated with 
 *	TclpAlloc() should be considered unusable.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	This subsystem is self-initializing, since memory can be 
 *	allocated before Tcl is formally initialized.  After this call,
 *	this subsystem has been reset to its initial state and is 
 *	usable again.
 *
 *-------------------------------------------------------------------------
 */

void
TclFinalizeAllocSubsystem()
{
    int i;
    struct block *blockPtr, *nextPtr;

    TclpMutexLock(&allocMutex);
    for (blockPtr = blockList; blockPtr != NULL; blockPtr = nextPtr) {
	nextPtr = blockPtr->nextPtr;
	TclpSysFree(blockPtr);
    }
    blockList = NULL;

    for (blockPtr = bigBlocks.nextPtr; blockPtr != &bigBlocks; ) {
	nextPtr = blockPtr->nextPtr;
	TclpSysFree(blockPtr);
	blockPtr = nextPtr;
    }
    bigBlocks.nextPtr = &bigBlocks;
    bigBlocks.prevPtr = &bigBlocks;

    for (i = 0; i < NBUCKETS; i++) {
	nextf[i] = NULL;
#ifdef MSTATS
	nmalloc[i] = 0;
#endif
    }
#ifdef MSTATS
    nmalloc[i] = 0;
#endif
    TclpMutexUnlock(&allocMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * TclpAlloc --
 *
 *	Allocate more memory.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
TclpAlloc(
    unsigned int nbytes)	/* Number of bytes to allocate. */
{
    register union overhead *op;
    register long bucket;
    register unsigned amt;
    struct block *bigBlockPtr;

    if (!allocInit) {
	/*
	 * We have to make the "self initializing" because Tcl_Alloc
	 * may be used before any other part of Tcl.  E.g., see
	 * main() for tclsh!
	 */

	allocInit = 1;
	TclpMutexInit(&allocMutex);
    }
    TclpMutexLock(&allocMutex);
    /*
     * First the simple case: we simple allocate big blocks directly
     */
    if (nbytes + OVERHEAD >= MAXMALLOC) {
	bigBlockPtr = (struct block *) TclpSysAlloc((unsigned) 
		(sizeof(struct block) + OVERHEAD + nbytes), 0);
	if (bigBlockPtr == NULL) {
	    TclpMutexUnlock(&allocMutex);
	    return NULL;
	}
	bigBlockPtr->nextPtr = bigBlocks.nextPtr;
	bigBlocks.nextPtr = bigBlockPtr;
	bigBlockPtr->prevPtr = &bigBlocks;
	bigBlockPtr->nextPtr->prevPtr = bigBlockPtr;

	op = (union overhead *) (bigBlockPtr + 1);
	op->ov_magic0 = op->ov_magic1 = MAGIC;
	op->ov_index = 0xff;
#ifdef MSTATS
	nmalloc[NBUCKETS]++;
#endif
#ifdef RCHECK
	/*
	 * Record allocated size of block and
	 * bound space with magic numbers.
	 */
	op->ov_size = (nbytes + RSLOP - 1) & ~(RSLOP - 1);
	op->ov_rmagic = RMAGIC;
	*(unsigned short *)((caddr_t)(op + 1) + op->ov_size) = RMAGIC;
#endif
	TclpMutexUnlock(&allocMutex);
	return (void *)(op+1);
    }
    /*
     * Convert amount of memory requested into closest block size
     * stored in hash buckets which satisfies request.
     * Account for space used per block for accounting.
     */
#ifndef RCHECK
    amt = 8;	/* size of first bucket */
    bucket = 0;
#else
    amt = 16;	/* size of first bucket */
    bucket = 1;
#endif
    while (nbytes + OVERHEAD > amt) {
	amt <<= 1;
	if (amt == 0) {
	    TclpMutexUnlock(&allocMutex);
	    return (NULL);
	}
	bucket++;
    }
    ASSERT( bucket < NBUCKETS );

    /*
     * If nothing in hash bucket right now,
     * request more memory from the system.
     */
    if ((op = nextf[bucket]) == NULL) {
	MoreCore(bucket);
	if ((op = nextf[bucket]) == NULL) {
	    TclpMutexUnlock(&allocMutex);
	    return (NULL);
	}
    }
    /*
     * Remove from linked list
     */
    nextf[bucket] = op->ov_next;
    op->ov_magic0 = op->ov_magic1 = MAGIC;
    op->ov_index = (unsigned char) bucket;
#ifdef MSTATS
    nmalloc[bucket]++;
#endif
#ifdef RCHECK
    /*
     * Record allocated size of block and
     * bound space with magic numbers.
     */
    op->ov_size = (nbytes + RSLOP - 1) & ~(RSLOP - 1);
    op->ov_rmagic = RMAGIC;
    *(unsigned short *)((caddr_t)(op + 1) + op->ov_size) = RMAGIC;
#endif
    TclpMutexUnlock(&allocMutex);
    return ((char *)(op + 1));
}

/*
 *----------------------------------------------------------------------
 *
 * MoreCore --
 *
 *	Allocate more memory to the indicated bucket.
 *
 *	Assumes Mutex is already held.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Attempts to get more memory from the system.
 *
 *----------------------------------------------------------------------
 */

static void
MoreCore(
    int bucket)		/* What bucket to allocat to. */
{
    register union overhead *op;
    register long sz;		/* size of desired block */
    long amt;			/* amount to allocate */
    int nblks;			/* how many blocks we get */
    struct block *blockPtr;

    /*
     * sbrk_size <= 0 only for big, FLUFFY, requests (about
     * 2^30 bytes on a VAX, I think) or for a negative arg.
     */
    sz = 1 << (bucket + 3);
    ASSERT(sz > 0);

    amt = MAXMALLOC;
    nblks = amt / sz;
    ASSERT(nblks*sz == amt);

    blockPtr = (struct block *) TclpSysAlloc((unsigned) 
	    (sizeof(struct block) + amt), 1);
    /* no more room! */
    if (blockPtr == NULL) {
	return;
    }
    blockPtr->nextPtr = blockList;
    blockList = blockPtr;

    op = (union overhead *) (blockPtr + 1);
    
    /*
     * Add new memory allocated to that on
     * free list for this hash bucket.
     */
    nextf[bucket] = op;
    while (--nblks > 0) {
	op->ov_next = (union overhead *)((caddr_t)op + sz);
	op = (union overhead *)((caddr_t)op + sz);
    }
    op->ov_next = (union overhead *)NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TclpFree --
 *
 *	Free memory.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TclpFree(
    char *cp)		/* Pointer to memory to free. */
{   
    register long size;
    register union overhead *op;
    struct block *bigBlockPtr;

    if (cp == NULL) {
	return;
    }

    TclpMutexLock(&allocMutex);
    op = (union overhead *)((caddr_t)cp - sizeof (union overhead));

    ASSERT(op->ov_magic0 == MAGIC);		/* make sure it was in use */
    ASSERT(op->ov_magic1 == MAGIC);
    if (op->ov_magic0 != MAGIC || op->ov_magic1 != MAGIC) {
	TclpMutexUnlock(&allocMutex);
	return;
    }

    RANGE_ASSERT(op->ov_rmagic == RMAGIC);
    RANGE_ASSERT(*(unsigned short *)((caddr_t)(op + 1) + op->ov_size) == RMAGIC);
    size = op->ov_index;
    if ( size == 0xff ) {
#ifdef MSTATS
	nmalloc[NBUCKETS]--;
#endif
	bigBlockPtr = (struct block *) op - 1;
	bigBlockPtr->prevPtr->nextPtr = bigBlockPtr->nextPtr;
	bigBlockPtr->nextPtr->prevPtr = bigBlockPtr->prevPtr;
	TclpSysFree(bigBlockPtr);
	TclpMutexUnlock(&allocMutex);
	return;
    }
    ASSERT(size < NBUCKETS);
    op->ov_next = nextf[size];	/* also clobbers ov_magic */
    nextf[size] = op;
#ifdef MSTATS
    nmalloc[size]--;
#endif
    TclpMutexUnlock(&allocMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * TclpRealloc --
 *
 *	Reallocate memory.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
TclpRealloc(
    char *cp,			/* Pointer to alloced block. */
    unsigned int nbytes)	/* New size of memory. */
{   
    int i;
    union overhead *op;
    struct block *bigBlockPtr;
    int expensive;
    unsigned long maxsize;

    if (cp == NULL) {
	return (TclpAlloc(nbytes));
    }

    TclpMutexLock(&allocMutex);

    op = (union overhead *)((caddr_t)cp - sizeof (union overhead));

    ASSERT(op->ov_magic0 == MAGIC);		/* make sure it was in use */
    ASSERT(op->ov_magic1 == MAGIC);
    if (op->ov_magic0 != MAGIC || op->ov_magic1 != MAGIC) {
	TclpMutexUnlock(&allocMutex);
	return NULL;
    }

    RANGE_ASSERT(op->ov_rmagic == RMAGIC);
    RANGE_ASSERT(*(unsigned short *)((caddr_t)(op + 1) + op->ov_size) == RMAGIC);
    i = op->ov_index;

    /*
     * If the block isn't in a bin, just realloc it.
     */

    if (i == 0xff) {
	bigBlockPtr = (struct block *) op - 1;
	bigBlockPtr = (struct block *) TclpSysRealloc(bigBlockPtr, 
		sizeof(struct block) + OVERHEAD + nbytes);
	if (bigBlockPtr == NULL) {
	    TclpMutexUnlock(&allocMutex);
	    return NULL;
	}
	op = (union overhead *) (bigBlockPtr + 1);
#ifdef MSTATS
	nmalloc[NBUCKETS]++;
#endif
#ifdef RCHECK
	/*
	 * Record allocated size of block and update magic number bounds.
	 */

	op->ov_size = (nbytes + RSLOP - 1) & ~(RSLOP - 1);
	*(unsigned short *)((caddr_t)(op + 1) + op->ov_size) = RMAGIC;
#endif
	TclpMutexUnlock(&allocMutex);
	return (char *)(op+1);
    }
    maxsize = 1 << (i+3);
    expensive = 0;
    if ( nbytes + OVERHEAD > maxsize ) {
	expensive = 1;
    } else if ( i > 0 && nbytes + OVERHEAD < (maxsize/2) ) {
	expensive = 1;
    }

    if (expensive) {
	void *newp;

	TclpMutexUnlock(&allocMutex);

	newp = TclpAlloc(nbytes);
	if ( newp == NULL ) {
	    return NULL;
	}
	maxsize -= OVERHEAD;
	if ( maxsize < nbytes )
	    nbytes = maxsize;
	memcpy((VOID *) newp, (VOID *) cp, (size_t) nbytes);
	TclpFree(cp);
	return newp;
    }
    
    /*
     * Ok, we don't have to copy, it fits as-is
     */
#ifdef RCHECK
    op->ov_size = (nbytes + RSLOP - 1) & ~(RSLOP - 1);
    *(unsigned short *)((caddr_t)(op + 1) + op->ov_size) = RMAGIC;
#endif
    TclpMutexUnlock(&allocMutex);
    return(cp);
}

/*
 *----------------------------------------------------------------------
 *
 * mstats --
 *
 *	Prints two lines of numbers, one showing the length of the 
 *	free list for each size category, the second showing the 
 *	number of mallocs - frees for each size category.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

#ifdef MSTATS
void
mstats(
    char *s)	/* Where to write info. */
{
    register int i, j;
    register union overhead *p;
    int totfree = 0,
	totused = 0;

    TclpMutexLock(&allocMutex);
    fprintf(stderr, "Memory allocation statistics %s\nTclpFree:\t", s);
    for (i = 0; i < NBUCKETS; i++) {
	for (j = 0, p = nextf[i]; p; p = p->ov_next, j++)
	    fprintf(stderr, " %d", j);
	totfree += j * (1 << (i + 3));
    }
    fprintf(stderr, "\nused:\t");
    for (i = 0; i < NBUCKETS; i++) {
	fprintf(stderr, " %d", nmalloc[i]);
	totused += nmalloc[i] * (1 << (i + 3));
    }
    fprintf(stderr, "\n\tTotal small in use: %d, total free: %d\n",
	    totused, totfree);
    fprintf(stderr, "\n\tNumber of big (>%d) blocks in use: %d\n", 
	    MAXMALLOC, nmalloc[NBUCKETS]);
    TclpMutexUnlock(&allocMutex);
}
#endif

#else	/* USE_TCLALLOC */

/* 
 * Put something in here so compiler and/or linker doesn't complain about 
 * empty file.
 */

static int i;

#endif
