/* $XFree86: xc/programs/Xserver/hw/xfree86/os-support/linux/lnx.h,v 3.3 2002/11/25 14:05:04 eich Exp $ */

#ifndef LNX_H_
# ifdef __alpha__
extern unsigned long _bus_base __P ((void)) __attribute__ ((const));
extern unsigned long _bus_base_sparse __P ((void)) __attribute__ ((const));
extern int iopl __P ((int __level));

/* new pciconfig_iobase syscall added in 2.2.15 and 2.3.99 */
#  include <linux/unistd.h>
/* No longer include asm/pci.h on alpha on Debian as it do not exist in any
 * package (Ola Lundqvist) */
/*  include <asm/pci.h>*/
extern long (*_iobase)(unsigned, int, int, int);

/*
 * _iobase deals with the case the __NR_pciconfig_iobase is either undefined
 * or unsupported by the kernel, but we need to make sure that the `which'
 * argument symbols are defined.
 */
#  ifndef IOBASE_HOSE
#   define IOBASE_HOSE 		0
#  endif
#  ifndef IOBASE_SPARSE_MEM
#   define IOBASE_SPARSE_MEM	1
#  endif
#  ifndef IOBASE_DENSE_MEM
#   define IOBASE_DENSE_MEM	2
#  endif
#  ifndef IOBASE_SPARSE_IO
#   define IOBASE_SPARSE_IO	3
#  endif
#  ifndef IOBASE_DENSE_IO
#   define IOBASE_DENSE_IO	4
#  endif
#  ifndef IOBASE_ROOT_BUS
#   define IOBASE_ROOT_BUS	5
#  endif
#  ifndef IOBASE_FROM_HOSE
#   define IOBASE_FROM_HOSE	0x10000
#  endif
# endif /* __alpha__ */

#define LNX_H_

#endif
