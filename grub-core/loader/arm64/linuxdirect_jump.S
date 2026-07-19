/*
 * Direct jump helper for ARM64 Linux.
 *
 * C prototype:
 *   void grub_arm64_linux_jump (grub_addr_t fdt, grub_addr_t kernel_entry);
 */

#include <grub/symbol.h>

	.file	"linuxdirect_jump.S"
	.text

FUNCTION(grub_arm64_linux_jump)
	/* Keep arguments in non-ABI scratch registers while shutting down. */
	mov	x5, x0		/* FDT address */
	mov	x4, x1		/* Kernel entry */

	/* Disable interrupts, debug, SError and IRQ/FIQ delivery. */
	msr	daifset, #0xf
	isb

	/* Clean/invalidate all instruction-side state visible at this level. */
	ic	ialluis
	dsb	ish
	isb

	/* Disable MMU, data cache and instruction cache at EL1. */
	mrs	x6, sctlr_el1
	bic	x6, x6, #(1 << 0)	/* M */
	bic	x6, x6, #(1 << 2)	/* C */
	bic	x6, x6, #(1 << 12)	/* I */
	msr	sctlr_el1, x6
	isb

	tlbi	vmalle1
	dsb	ish
	isb

	/* Linux arm64 boot protocol: x0 = FDT, x1-x3 = 0. */
	mov	x0, x5
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x4
