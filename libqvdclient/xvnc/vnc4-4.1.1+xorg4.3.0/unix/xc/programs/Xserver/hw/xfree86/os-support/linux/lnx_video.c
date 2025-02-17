/* $XFree86: xc/programs/Xserver/hw/xfree86/os-support/linux/lnx_video.c,v 3.64 2003/02/17 15:29:22 dawes Exp $ */
/*
 * Copyright 1992 by Orest Zborowski <obz@Kodak.com>
 * Copyright 1993 by David Wexelblat <dwex@goblin.org>
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the names of Orest Zborowski and David Wexelblat
 * not be used in advertising or publicity pertaining to distribution of
 * the software without specific, written prior permission.  Orest Zborowski
 * and David Wexelblat make no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without express or
 * implied warranty.
 *
 * OREST ZBOROWSKI AND DAVID WEXELBLAT DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL OREST ZBOROWSKI OR DAVID WEXELBLAT BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
/* $XConsortium: lnx_video.c /main/9 1996/10/19 18:06:34 kaleb $ */

#ifdef __x86_64__
#ifndef BITS_PER_LONG
#define BITS_PER_LONG 64
#endif
#endif

#include "X.h"
#include "input.h"
#include "scrnintstr.h"

#include "xf86.h"
#include "xf86Priv.h"
#include "xf86_OSlib.h"
#include "xf86OSpriv.h"
#include "lnx.h"
#ifdef __alpha__
#include "xf86Axp.h"
#endif

#ifdef HAS_MTRR_SUPPORT
#include <asm/mtrr.h>
#endif

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

static Bool ExtendedEnabled = FALSE;

#ifdef __ia64__

#include "compiler.h"
#include <sys/io.h>

#elif !defined(__powerpc__) && \
      !defined(__mc68000__) && \
      !defined(__sparc__) && \
      !defined(__mips__)

/*
 * Due to conflicts with "compiler.h", don't rely on <sys/io.h> to declare
 * these.
 */
extern int ioperm(unsigned long __from, unsigned long __num, int __turn_on);
extern int iopl(int __level);

#endif

#ifdef __alpha__

# ifdef LIBC_IS_FIXED
extern void sethae(unsigned long hae);
# else
#  include <unistd.h>
#  define sethae(x) syscall(301,x);
# endif

/* define to test the Sparse addressing on a non-Jensen */
# ifdef TEST_JENSEN_CODE 
#  define isJensen (1)
# else
#  define isJensen (axpSystem == JENSEN)
# endif

# define BUS_BASE bus_base

#else 

#define BUS_BASE (0)

#endif /*  __alpha__ */

/***************************************************************************/
/* Video Memory Mapping section                                            */
/***************************************************************************/

static pointer mapVidMem(int, unsigned long, unsigned long, int);
static void unmapVidMem(int, pointer, unsigned long);
#if defined (__alpha__) 
static pointer mapVidMemSparse(int, unsigned long, unsigned long, int);
extern axpDevice lnxGetAXP(void);
static void unmapVidMemSparse(int, pointer, unsigned long);
# if defined(JENSEN_SUPPORT)
static pointer mapVidMemJensen(int, unsigned long, unsigned long, int);
static void unmapVidMemJensen(int, pointer, unsigned long);
# endif
static axpDevice axpSystem = -1;
static Bool needSparse;
static unsigned long hae_thresh;
static unsigned long hae_mask;
static unsigned long bus_base;
static unsigned long sparse_size;
#endif

#ifdef HAS_MTRR_SUPPORT

#define SPLIT_WC_REGIONS 1

static pointer setWC(int, unsigned long, unsigned long, Bool, MessageType);
static void undoWC(int, pointer);

/* The file desc for /proc/mtrr. Once opened, left opened, and the mtrr
   driver will clean up when we exit. */
#define MTRR_FD_UNOPENED (-1)	/* We have yet to open /proc/mtrr */
#define MTRR_FD_PROBLEM (-2)	/* We tried to open /proc/mtrr, but had
				   a problem. */
static int mtrr_fd = MTRR_FD_UNOPENED;

/* Open /proc/mtrr. FALSE on failure. Will always fail on Linux 2.0, 
   and will fail on Linux 2.2 with MTRR support configured out,
   so verbosity should be chosen appropriately. */
static Bool
mtrr_open(int verbosity)
{
	/* Only report absence of /proc/mtrr once. */
	static Bool warned = FALSE;

	char **fn;
	static char *mtrr_files[] = {
		"/dev/cpu/mtrr",	/* Possible future name */
		"/proc/mtrr",		/* Current name */
		NULL
	};

	if (mtrr_fd == MTRR_FD_UNOPENED) { 
		/* So open it. */
		for (fn = mtrr_files; mtrr_fd < 0 && *fn; fn++)
			mtrr_fd = open(*fn, O_WRONLY);

		if (mtrr_fd < 0)
			mtrr_fd = MTRR_FD_PROBLEM;
	}

	if (mtrr_fd == MTRR_FD_PROBLEM) {
		/* To make sure we only ever warn once, need to check
		   verbosity outside xf86MsgVerb */
		if (!warned && verbosity <= xf86GetVerbosity()) {
			xf86MsgVerb(X_WARNING, verbosity,
				  "System lacks support for changing MTRRs\n");
			warned = TRUE;
		}

		return FALSE;
	}
	else
		return TRUE;
}

/*
 * We maintain a list of WC regions for each physical mapping so they can
 * be undone when unmapping.
 */

struct mtrr_wc_region {
	struct mtrr_sentry	sentry;
	Bool			added;		/* added WC or removed it */
	struct mtrr_wc_region *	next;
};

static struct mtrr_wc_region *
mtrr_cull_wc_region(int screenNum, unsigned long base, unsigned long size,
		      MessageType from)
{
	/* Some BIOS writers thought that setting wc over the mmio
	   region of a graphics devices was a good idea. Try to fix
	   it. */

	struct mtrr_gentry gent;
	char buf[20];
	struct mtrr_wc_region *wcreturn = NULL, *wcr;

	/* Linux 2.0 users should not get a warning without -verbose */
	if (!mtrr_open(2))
		return NULL;

	for (gent.regnum = 0; 
	     ioctl(mtrr_fd, MTRRIOC_GET_ENTRY, &gent) >= 0;
	     gent.regnum++) {
		if (gent.type != MTRR_TYPE_WRCOMB
		    || gent.base + gent.size <= base
		    || base + size <= gent.base)
			continue;

		/* Found an overlapping region. Delete it. */
		
		wcr = xalloc(sizeof(*wcr));
		if (!wcr)
			return NULL;
		wcr->sentry.base = gent.base;
		wcr->sentry.size = gent.size;
		wcr->sentry.type = MTRR_TYPE_WRCOMB;
		wcr->added = FALSE;
		
		/* There is now a nicer ioctl-based way to do this,
		   but it isn't in current kernels. */
		snprintf(buf, sizeof(buf), "disable=%u\n", gent.regnum);

		if (write(mtrr_fd, buf, strlen(buf)) >= 0) {
			xf86DrvMsg(screenNum, from,
				   "Removed MMIO write-combining range "
				   "(0x%lx,0x%lx)\n",
				   gent.base, gent.size);
			wcr->next = wcreturn;
			wcreturn = wcr;
		} else {
			xfree(wcr);
			xf86DrvMsgVerb(screenNum, X_WARNING, 0,
				   "Failed to remove MMIO "
				   "write-combining range (0x%lx,0x%lx)\n",
				   gent.base, gent.size);
		}
	}
	return wcreturn;
}


static struct mtrr_wc_region *
mtrr_add_wc_region(int screenNum, unsigned long base, unsigned long size,
		   MessageType from)
{
	struct mtrr_wc_region *wcr;

	/* Linux 2.0 should not warn, unless the user explicitly asks for
	   WC. */
	if (!mtrr_open(from == X_CONFIG ? 0 : 2))
		return NULL;

	wcr = xalloc(sizeof(*wcr));
	if (!wcr)
		return NULL;

	wcr->sentry.base = base;
	wcr->sentry.size = size;
	wcr->sentry.type = MTRR_TYPE_WRCOMB;
	wcr->added = TRUE;
	wcr->next = NULL;

#if SPLIT_WC_REGIONS
	/*
	 * Splits up the write-combining region if it is not aligned on a
 	 * size boundary.
	 */

	{
	    unsigned long lbase, d_size = 1;
	    unsigned long n_size = size;
	    unsigned long n_base = base;

	    for (lbase = n_base, d_size = 1; !(lbase & 1);
		 lbase = lbase >> 1, d_size <<= 1);
	    while (d_size > n_size)
		d_size = d_size >> 1;
#ifdef DEBUG
	    ErrorF("WC_BASE: 0x%lx WC_END: 0x%lx\n",base,base+d_size-1);
#endif
	    n_base += d_size;
	    n_size -= d_size;
	    if (n_size) {
		xf86DrvMsgVerb(screenNum,X_INFO,3,"Splitting WC range: "
			       "base: 0x%lx, size: 0x%lx\n",base,size);
		wcr->next = mtrr_add_wc_region(screenNum, n_base, n_size,from);
	    }
	    wcr->sentry.size = d_size;
	} 
	
	/*****************************************************************/
#endif /* SPLIT_WC_REGIONS */

	if (ioctl(mtrr_fd, MTRRIOC_ADD_ENTRY, &wcr->sentry) >= 0) {
		/* Avoid printing on every VT switch */
		if (xf86ServerIsInitialising()) {
			xf86DrvMsg(screenNum, from,
				   "Write-combining range (0x%lx,0x%lx)\n",
				   base, size);
		}
		return wcr;
	}
	else {
		xfree(wcr);
		
		/* Don't complain about the VGA region: MTRR fixed
		   regions aren't currently supported, but might be in
		   the future. */
		if ((unsigned long)base >= 0x100000) {
			xf86DrvMsgVerb(screenNum, X_WARNING, 0,
				"Failed to set up write-combining range "
				"(0x%lx,0x%lx)\n", base, size);
		}
		return NULL;
	}
}

static void
mtrr_undo_wc_region(int screenNum, struct mtrr_wc_region *wcr)
{
	struct mtrr_wc_region *p, *prev;

	if (mtrr_fd > 0) {
		p = wcr;
		while (p) {
			if (p->added)
				ioctl(mtrr_fd, MTRRIOC_DEL_ENTRY, &p->sentry);
			prev = p;
			p = p->next;
			xfree(prev);
		}
	}
}

static pointer
setWC(int screenNum, unsigned long base, unsigned long size, Bool enable,
      MessageType from)
{
	if (enable)
		return mtrr_add_wc_region(screenNum, base, size, from);
	else
		return mtrr_cull_wc_region(screenNum, base, size, from);
}

static void
undoWC(int screenNum, pointer regioninfo)
{
	mtrr_undo_wc_region(screenNum, regioninfo);
}

#endif /* HAS_MTRR_SUPPORT */

void
xf86OSInitVidMem(VidMemInfoPtr pVidMem)
{
	pVidMem->linearSupported = TRUE;
#ifdef __alpha__
	if (axpSystem == -1) {
	  axpSystem = lnxGetAXP();
	  if ((needSparse = (_bus_base_sparse() > 0))) {
	    hae_thresh = xf86AXPParams[axpSystem].hae_thresh;
	    hae_mask = xf86AXPParams[axpSystem].hae_mask;
	    sparse_size = xf86AXPParams[axpSystem].size;
	  }
	  bus_base = _bus_base();
	}
	if (isJensen) {
# ifndef JENSEN_SUPPORT
	  FatalError("Jensen is not supported any more\n"
		     "If you are intereseted in fixing Jensen support\n"
		     "please contact xfree86@xfree86.org\n");
# else
	  xf86Msg(X_INFO,"Machine type is Jensen\n");
	  pVidMem->mapMem = mapVidMemJensen;
	  pVidMem->unmapMem = unmapVidMemJensen;
# endif /* JENSEN_SUPPORT */
	} else if (needSparse) {
	  xf86Msg(X_INFO,"Machine needs sparse mapping\n");
	  pVidMem->mapMem = mapVidMemSparse;
	  pVidMem->unmapMem = unmapVidMemSparse;
	} else {
	  xf86Msg(X_INFO,"Machine type has 8/16 bit access\n");
	  pVidMem->mapMem = mapVidMem;
	  pVidMem->unmapMem = unmapVidMem;
	}	
#else
	pVidMem->mapMem = mapVidMem;
	pVidMem->unmapMem = unmapVidMem;
#endif /* __alpha__ */


#ifdef HAS_MTRR_SUPPORT
	pVidMem->setWC = setWC;
	pVidMem->undoWC = undoWC;
#endif
	pVidMem->initialised = TRUE;
}

#ifdef __sparc__
/* Basically, you simply cannot do this on Sparc.  You have to do something portable
 * like use /dev/fb* or mmap() on /proc/bus/pci/X/Y nodes. -DaveM
 */
static pointer mapVidMem(int ScreenNum, unsigned long Base, unsigned long Size, int flags)
{
	return NULL;
}
#else
static pointer
mapVidMem(int ScreenNum, unsigned long Base, unsigned long Size, int flags)
{
    pointer base;
    int fd;
    int mapflags = MAP_SHARED; 
    memType realBase, alignOff;

    realBase = Base & ~(getpagesize() - 1);
    alignOff = Base - realBase;
#ifdef DEBUG
    ErrorF("base: %lx, realBase: %lx, alignOff: %lx \n",
	   Base,realBase,alignOff);
#endif
    
#if defined(__ia64__)
#ifndef MAP_WRITECOMBINED
#define MAP_WRITECOMBINED 0x00010000
#endif
#ifndef MAP_NONCACHED
#define MAP_NONCACHED 0x00020000
#endif
    if(flags & VIDMEM_FRAMEBUFFER) 
        mapflags |= MAP_WRITECOMBINED; 
    else
        mapflags |= MAP_NONCACHED; 
#endif

#if defined(__ia64__)
    /* this will disappear when people upgrade their kernels */
    if ((fd = open(DEV_MEM, O_RDWR|O_SYNC)) < 0) 
#else
    if ((fd = open(DEV_MEM, O_RDWR)) < 0)
#endif
    {
	FatalError("xf86MapVidMem: failed to open " DEV_MEM " (%s)\n",
		   strerror(errno));
    }
    /* This requires linux-0.99.pl10 or above */
    base = mmap((caddr_t)0, Size + alignOff,
  		PROT_READ|PROT_WRITE,
  		mapflags, fd,
 		(off_t)(off_t)realBase  + BUS_BASE);
    close(fd);
    if (base == MAP_FAILED) {
        FatalError("xf86MapVidMem: Could not mmap framebuffer"
		   " (0x%08x,0x%x) (%s)\n", Base, Size,
		   strerror(errno));
    }
#ifdef DEBUG
    ErrorF("base: %lx aligned base: %lx\n",base, base + alignOff);
#endif
    return (char *)base + alignOff;
}
#endif /* !(__sparc__) */
    
static void
unmapVidMem(int ScreenNum, pointer Base, unsigned long Size)
{
    memType alignOff = (memType)Base 
	- ((memType)Base & ~(getpagesize() - 1));
    
#ifdef DEBUG
    ErrorF("alignment offset: %lx\n",alignOff);
#endif
    munmap((caddr_t)((memType)Base - alignOff), (Size + alignOff));
}


/***************************************************************************/
/* I/O Permissions section                                                 */
/***************************************************************************/

#if defined(__powerpc__)
volatile unsigned char *ioBase = NULL;

#ifndef __NR_pciconfig_iobase
#define __NR_pciconfig_iobase	200
#endif

#endif

void
xf86EnableIO(void)
{
#if defined(__powerpc__)
	int fd;
	unsigned int ioBase_phys;
#endif

	if (ExtendedEnabled)
		return;

#if defined(__powerpc__)
	ioBase_phys = syscall(__NR_pciconfig_iobase, 2, 0, 0);

	fd = open("/dev/mem", O_RDWR);
	if (ioBase == NULL) {
		ioBase = (volatile unsigned char *)mmap(0, 0x20000,
				PROT_READ|PROT_WRITE, MAP_SHARED, fd,
				ioBase_phys);
/* Should this be fatal or just a warning? */
#if 0
		if (ioBase == MAP_FAILED) {
			FatalError(
			    "xf86EnableIOPorts: Failed to map iobase (%s)\n",
			    strerror(errno));
		}
#endif
	}
	close(fd);
#elif !defined(__mc68000__) && !defined(__sparc__) && !defined(__mips__) && !defined(__sh__) && !defined(__hppa__)
	if (ioperm(0, 1024, 1) || iopl(3))
		FatalError("xf86EnableIOPorts: Failed to set IOPL for I/O\n");
# if !defined(__alpha__)
	ioperm(0x40,4,0); /* trap access to the timer chip */
	ioperm(0x60,4,0); /* trap access to the keyboard controller */
# endif
#endif
	ExtendedEnabled = TRUE;

	return;
}

void
xf86DisableIO(void)
{
	if (!ExtendedEnabled)
		return;
#if defined(__powerpc__)
	munmap(ioBase, 0x20000);
	ioBase = NULL;
#elif !defined(__mc68000__) && !defined(__sparc__) && !defined(__mips__) && !defined(__sh__) && !defined(__hppa__)
	iopl(0);
	ioperm(0, 1024, 0);
#endif
	ExtendedEnabled = FALSE;

	return;
}


/***************************************************************************/
/* Interrupt Handling section                                              */
/***************************************************************************/

/* XXX The #ifdefs should be made simpler. */

Bool
xf86DisableInterrupts()
{
#if !defined(__mc68000__) && !defined(__powerpc__) && !defined(__sparc__) && !defined(__mips__) && !defined(__ia64__) && !defined(__sh__) && !defined(__hppa__)
	if (!ExtendedEnabled)
	    if (iopl(3) || ioperm(0, 1024, 1))
			return (FALSE);
#endif
#if defined(__alpha__) || defined(__mc68000__) || defined(__powerpc__) || defined(__sparc__) || defined(__mips__) || defined(__arm__) || defined(__arm64__) || defined(__sh__) || defined(__ia64__) || defined(__hppa__)
#else
# ifdef __GNUC__
#  if defined(__ia64__)
#   if 0
	__asm__ __volatile__ (";; rsm psr.i;; srlz.d" ::: "memory");
#   endif
#  else
      __asm__ __volatile__("cli");
#  endif
# else
	asm("cli");
# endif
#endif
#if !defined(__mc68000__) && !defined(__powerpc__) && !defined(__sparc__) && !defined(__mips__) && !defined(__sh__) && !defined(__ia64__) && !defined(__hppa__)
	if (!ExtendedEnabled) {
	    iopl(0);
	    ioperm(0, 1024, 0);
	}
	
#endif
	return (TRUE);
}

void
xf86EnableInterrupts()
{
#if !defined(__mc68000__) && !defined(__powerpc__) && !defined(__sparc__) && !defined(__mips__) && !defined(__ia64__) && !defined(__sh__) && !defined(__hppa__)
	if (!ExtendedEnabled)
	    if (iopl(3) || ioperm(0, 1024, 1))
			return;
#endif
#if defined(__alpha__) || defined(__mc68000__) || defined(__powerpc__) || defined(__sparc__) || defined(__mips__) || defined(__arm__) || defined(__arm64__) || defined(__sh__) || defined(__ia64__) || defined(__hppa__)
#else
# ifdef __GNUC__
#  if defined(__ia64__)
#   if 0
	__asm__ __volatile__ (";; ssm psr.i;; srlz.d" ::: "memory");
#   endif
#  else
      __asm__ __volatile__("sti");
#  endif
# else
	asm("sti");
# endif
#endif
#if !defined(__mc68000__) && !defined(__powerpc__) && !defined(__sparc__) && !defined(__mips__) && !defined(__sh__) && !defined(__ia64__) && !defined(__hppa__)
	if (!ExtendedEnabled) {
	    iopl(0);
	    ioperm(0, 1024, 0);
	}
#endif
	return;
}

#if defined (__alpha__)

#define vuip    volatile unsigned int *

extern int readDense8(pointer Base, register unsigned long Offset);
extern int readDense16(pointer Base, register unsigned long Offset);
extern int readDense32(pointer Base, register unsigned long Offset);
extern void
writeDenseNB8(int Value, pointer Base, register unsigned long Offset);
extern void
writeDenseNB16(int Value, pointer Base, register unsigned long Offset);
extern void
writeDenseNB32(int Value, pointer Base, register unsigned long Offset);
extern void
writeDense8(int Value, pointer Base, register unsigned long Offset);
extern void
writeDense16(int Value, pointer Base, register unsigned long Offset);
extern void
writeDense32(int Value, pointer Base, register unsigned long Offset);

static int readSparse8(pointer Base, register unsigned long Offset);
static int readSparse16(pointer Base, register unsigned long Offset);
static int readSparse32(pointer Base, register unsigned long Offset);
static void
writeSparseNB8(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseNB16(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseNB32(int Value, pointer Base, register unsigned long Offset);
static void
writeSparse8(int Value, pointer Base, register unsigned long Offset);
static void
writeSparse16(int Value, pointer Base, register unsigned long Offset);
static void
writeSparse32(int Value, pointer Base, register unsigned long Offset);

#define DENSE_BASE	0x2ff00000000UL
#define SPARSE_BASE	0x30000000000UL

static unsigned long msb_set = 0;

static pointer
mapVidMemSparse(int ScreenNum, unsigned long Base, unsigned long Size, int flags)
{
    int fd;
    unsigned long ret, rets = 0;

    static Bool was_here = FALSE;

    if (!was_here) {
      was_here = TRUE;

      xf86WriteMmio8 = writeSparse8;
      xf86WriteMmio16 = writeSparse16;
      xf86WriteMmio32 = writeSparse32;
      xf86WriteMmioNB8 = writeSparseNB8;
      xf86WriteMmioNB16 = writeSparseNB16;
      xf86WriteMmioNB32 = writeSparseNB32;
      xf86ReadMmio8 = readSparse8;
      xf86ReadMmio16 = readSparse16;
      xf86ReadMmio32 = readSparse32;
    }
	
    if ((fd = open(DEV_MEM, O_RDWR)) < 0) {
        FatalError("xf86MapVidMem: failed to open " DEV_MEM " (%s)\n",
		   strerror(errno));
    }

#if 0
    xf86Msg(X_INFO,"mapVidMemSparse: try Base 0x%lx size 0x%lx flags 0x%x\n",
	    Base, Size, flags);
#endif

    /* This requirers linux-0.99.pl10 or above */

    /*
     * Always do DENSE mmap, since read32/write32 currently require it.
     */
    ret = (unsigned long)mmap((caddr_t)(DENSE_BASE + Base), Size,
		   PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd,
		   (off_t) (bus_base + Base));

    /*
     * Do SPARSE mmap only when MMIO and not MMIO_32BIT, or FRAMEBUFFER
     * and SPARSE (which should require the use of read/write macros).
     *
     * By not SPARSE mmapping an 8MB framebuffer, we can save approx. 256K
     * bytes worth of pagetable (32 pages).
     */
    if (((flags & VIDMEM_MMIO) && !(flags & VIDMEM_MMIO_32BIT)) ||
	((flags & VIDMEM_FRAMEBUFFER) && (flags & VIDMEM_SPARSE)))
    {
        rets = (unsigned long)mmap((caddr_t)(SPARSE_BASE + (Base << 5)),
				   Size << 5, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd,
				   (off_t) _bus_base_sparse() + (Base << 5));
    }

    close(fd);
      
    if (ret == (unsigned long)MAP_FAILED) {
        FatalError("xf86MapVidMemSparse: Could not (dense) mmap fb (%s)\n",
		   strerror(errno));
    }

    if (((flags & VIDMEM_MMIO) && !(flags & VIDMEM_MMIO_32BIT)) ||
	((flags & VIDMEM_FRAMEBUFFER) && (flags & VIDMEM_SPARSE)))
    {
        if (rets == (unsigned long)MAP_FAILED ||
	    rets != (SPARSE_BASE + (Base << 5)))
	{
	    FatalError("mapVidMemSparse: Could not (sparse) mmap fb (%s)\n",
		       strerror(errno));
	}
    }

#if 1
    if (rets)
        xf86Msg(X_INFO,"mapVidMemSparse: mapped Base 0x%lx size 0x%lx"
		" to DENSE at 0x%lx and SPARSE at 0x%lx\n",
		Base, Size, ret, rets);
    else
        xf86Msg(X_INFO,"mapVidMemSparse: mapped Base 0x%lx size 0x%lx"
		" to DENSE only at 0x%lx\n",
		Base, Size, ret);

#endif
    return (pointer) ret;
}

static void
unmapVidMemSparse(int ScreenNum, pointer Base, unsigned long Size)
{
    unsigned long Offset = (unsigned long)Base - DENSE_BASE;
#if 1
    xf86Msg(X_INFO,"unmapVidMemSparse: unmapping Base 0x%lx Size 0x%lx\n",
	    Base, Size);
#endif
    /* Unmap DENSE always. */
    munmap((caddr_t)Base, Size);

    /* Unmap SPARSE always, and ignore error in case we did not map it. */
    munmap((caddr_t)(SPARSE_BASE + (Offset << 5)), Size << 5);
}

static int
readSparse8(pointer Base, register unsigned long Offset)
{
    register unsigned long result, shift;
    register unsigned long msb;

    mem_barrier();
    Offset += (unsigned long)Base - DENSE_BASE;
    shift = (Offset & 0x3) << 3;
    if (Offset >= (hae_thresh)) {
        msb = Offset & hae_mask;
        Offset -= msb;
	if (msb_set != msb) {
	    sethae(msb);
	    msb_set = msb;
	}
    }

    mem_barrier();
    result = *(vuip) (SPARSE_BASE + (Offset << 5));
    result >>= shift;
    return 0xffUL & result;
}

static int
readSparse16(pointer Base, register unsigned long Offset)
{
    register unsigned long result, shift;
    register unsigned long msb;

    mem_barrier();
    Offset += (unsigned long)Base - DENSE_BASE;
    shift = (Offset & 0x2) << 3;
    if (Offset >= hae_thresh) {
        msb = Offset & hae_mask;
        Offset -= msb;
	if (msb_set != msb) {
	    sethae(msb);
	    msb_set = msb;
	}
    }

    mem_barrier();
    result = *(vuip)(SPARSE_BASE + (Offset<<5) + (1<<(5-2)));
    result >>= shift;
    return 0xffffUL & result;
}

static int
readSparse32(pointer Base, register unsigned long Offset)
{
    /* NOTE: this is really using DENSE. */
    mem_barrier();
    return *(vuip)((unsigned long)Base+(Offset));
}

static void
writeSparse8(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned long msb;
    register unsigned int b = Value & 0xffU;

    write_mem_barrier();
    Offset += (unsigned long)Base - DENSE_BASE;
    if (Offset >= hae_thresh) {
        msb = Offset & hae_mask;
	Offset -= msb;
	if (msb_set != msb) {
	    sethae(msb); 
	    msb_set = msb;
	}
    }

    write_mem_barrier();
    *(vuip) (SPARSE_BASE + (Offset << 5)) = b * 0x01010101;
}

static void
writeSparse16(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned long msb;
    register unsigned int w = Value & 0xffffU;

    write_mem_barrier();
    Offset += (unsigned long)Base - DENSE_BASE;
    if (Offset >= hae_thresh) {
        msb = Offset & hae_mask;
	Offset -= msb;
	if (msb_set != msb) {
	    sethae(msb);
	    msb_set = msb;
	}
    }

    write_mem_barrier();
    *(vuip)(SPARSE_BASE + (Offset<<5) + (1<<(5-2))) = w * 0x00010001;
}

static void
writeSparse32(int Value, pointer Base, register unsigned long Offset)
{
    /* NOTE: this is really using DENSE. */
    write_mem_barrier();
    *(vuip)((unsigned long)Base + (Offset)) = Value;
    return;
}

static void
writeSparseNB8(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned long msb;
    register unsigned int b = Value & 0xffU;

    Offset += (unsigned long)Base - DENSE_BASE;
    if (Offset >= hae_thresh) {
        msb = Offset & hae_mask;
	Offset -= msb;
	if (msb_set != msb) {
	    sethae(msb);
	    msb_set = msb;
	}
    }
    *(vuip) (SPARSE_BASE + (Offset << 5)) = b * 0x01010101;
}

static void
writeSparseNB16(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned long msb;
    register unsigned int w = Value & 0xffffU;

    Offset += (unsigned long)Base - DENSE_BASE;
    if (Offset >= hae_thresh) {
        msb = Offset & hae_mask;
	Offset -= msb;
	if (msb_set != msb) {
	    sethae(msb);
	    msb_set = msb;
	}
    }
    *(vuip)(SPARSE_BASE+(Offset<<5)+(1<<(5-2))) = w * 0x00010001;
}

static void
writeSparseNB32(int Value, pointer Base, register unsigned long Offset)
{
    /* NOTE: this is really using DENSE. */
    *(vuip)((unsigned long)Base + (Offset)) = Value;
    return;
}

void (*xf86WriteMmio8)(int Value, pointer Base, unsigned long Offset) 
     = writeDense8;
void (*xf86WriteMmio16)(int Value, pointer Base, unsigned long Offset)
     = writeDense16;
void (*xf86WriteMmio32)(int Value, pointer Base, unsigned long Offset)
     = writeDense32;
void (*xf86WriteMmioNB8)(int Value, pointer Base, unsigned long Offset) 
     = writeDenseNB8;
void (*xf86WriteMmioNB16)(int Value, pointer Base, unsigned long Offset)
     = writeDenseNB16;
void (*xf86WriteMmioNB32)(int Value, pointer Base, unsigned long Offset)
     = writeDenseNB32;
int  (*xf86ReadMmio8)(pointer Base, unsigned long Offset) 
     = readDense8;
int  (*xf86ReadMmio16)(pointer Base, unsigned long Offset)
     = readDense16;
int  (*xf86ReadMmio32)(pointer Base, unsigned long Offset)
     = readDense32;

#ifdef JENSEN_SUPPORT

static int
readSparseJensen8(pointer Base, register unsigned long Offset);
static int
readSparseJensen16(pointer Base, register unsigned long Offset);
static int
readSparseJensen32(pointer Base, register unsigned long Offset);
static void
writeSparseJensen8(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseJensen16(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseJensen32(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseJensenNB8(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseJensenNB16(int Value, pointer Base, register unsigned long Offset);
static void
writeSparseJensenNB32(int Value, pointer Base, register unsigned long Offset);

/*
 * The Jensen lacks dense memory, thus we have to address the bus via
 * the sparse addressing scheme.
 *
 * Martin Ostermann (ost@comnets.rwth-aachen.de) - Apr.-Sep. 1996
 */

#ifdef TEST_JENSEN_CODE 
#define SPARSE (5)
#else
#define SPARSE (7)
#endif

#define JENSEN_SHIFT(x) ((long)x<<SPARSE)

static pointer
mapVidMemJensen(int ScreenNum, unsigned long Base, unsigned long Size, int flags)
{
  pointer base;
  int fd;

  xf86WriteMmio8 = writeSparseJensen8;
  xf86WriteMmio16 = writeSparseJensen16;
  xf86WriteMmio32 = writeSparseJensen32;
  xf86WriteMmioNB8 = writeSparseJensenNB8;
  xf86WriteMmioNB16 = writeSparseJensenNB16;
  xf86WriteMmioNB32 = writeSparseJensenNB32;
  xf86ReadMmio8 = readSparseJensen8;
  xf86ReadMmio16 = readSparseJensen16;
  xf86ReadMmio32 = readSparseJensen32;

  if ((fd = open(DEV_MEM, O_RDWR)) < 0) {
    FatalError("xf86MapVidMem: failed to open " DEV_MEM " (%s)\n",
	       strerror(errno));
  }
  /* This requires linux-0.99.pl10 or above */
  base = mmap((caddr_t)0, JENSEN_SHIFT(Size),
	      PROT_READ|PROT_WRITE,
	      MAP_SHARED, fd,
	      (off_t)(JENSEN_SHIFT((off_t)Base) + _bus_base_sparse()));
  close(fd);
  if (base == MAP_FAILED) {
    FatalError("xf86MapVidMem: Could not mmap framebuffer"
	       " (0x%08x,0x%x) (%s)\n", Base, Size,
	       strerror(errno));
  }
  return base;
}

static void
unmapVidMemJensen(int ScreenNum, pointer Base, unsigned long Size)
{
  munmap((caddr_t)Base, JENSEN_SHIFT(Size));
}

static int
readSparseJensen8(pointer Base, register unsigned long Offset)
{
    register unsigned long result, shift;

    mem_barrier();
    shift = (Offset & 0x3) << 3;

    result = *(vuip) ((unsigned long)Base + (Offset << SPARSE));

    result >>= shift;
    return 0xffUL & result;
}

static int
readSparseJensen16(pointer Base, register unsigned long Offset)
{
    register unsigned long result, shift;

    mem_barrier();
    shift = (Offset & 0x2) << 3;

    result = *(vuip)((unsigned long)Base+(Offset<<SPARSE)+(1<<(SPARSE-2)));

    result >>= shift;
    return 0xffffUL & result;
}

static int
readSparseJensen32(pointer Base, register unsigned long Offset)
{
    register unsigned long result;

    mem_barrier();
    result = *(vuip)((unsigned long)Base+(Offset<<SPARSE)+(3<<(SPARSE-2)));

    return result;
}

static void
writeSparseJensen8(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned int b = Value & 0xffU;

    write_mem_barrier();
    *(vuip) ((unsigned long)Base + (Offset << SPARSE)) = b * 0x01010101;
}

static void
writeSparseJensen16(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned int w = Value & 0xffffU;

    write_mem_barrier();
    *(vuip)((unsigned long)Base+(Offset<<SPARSE)+(1<<(SPARSE-2))) =
      w * 0x00010001;
}

static void
writeSparseJensen32(int Value, pointer Base, register unsigned long Offset)
{
    write_mem_barrier();
    *(vuip)((unsigned long)Base+(Offset<<SPARSE)+(3<<(SPARSE-2))) = Value;
}

static void
writeSparseJensenNB8(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned int b = Value & 0xffU;

    *(vuip) ((unsigned long)Base + (Offset << SPARSE)) = b * 0x01010101;
}

static void
writeSparseJensenNB16(int Value, pointer Base, register unsigned long Offset)
{
    register unsigned int w = Value & 0xffffU;

    *(vuip)((unsigned long)Base+(Offset<<SPARSE)+(1<<(SPARSE-2))) =
      w * 0x00010001;
}

static void
writeSparseJensenNB32(int Value, pointer Base, register unsigned long Offset)
{
    *(vuip)((unsigned long)Base+(Offset<<SPARSE)+(3<<(SPARSE-2))) = Value;
}
#endif /* JENSEN_SUPPORT */

#endif /* __alpha__ */
