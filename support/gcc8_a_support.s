	.cfi_sections .debug_frame
	.section .text.__mulsi3,"ax",@progbits
	.type __mulsi3, function
	.globl	__mulsi3
__mulsi3:
	.cfi_startproc
	movew	sp@(4), d0	/* x0 -> d0 */
	muluw	sp@(10), d0	/* x0*y1 */
	movew	sp@(6), d1	/* x1 -> d1 */
	muluw	sp@(8), d1	/* x1*y0 */
	addw	d1, d0
	swap	d0
	clrw	d0
	movew	sp@(6), d1	/* x1 -> d1 */
	muluw	sp@(10), d1	/* x1*y1 */
	addl	d1, d0
	rts
	.cfi_endproc
	.size __mulsi3, .-__mulsi3

	.section .text.__udivsi3,"ax",@progbits
	.type __udivsi3, function
	.globl	__udivsi3
__udivsi3:
	.cfi_startproc
	movel	d2, sp@-
	.cfi_adjust_cfa_offset 4
	movel	sp@(12), d1	/* d1 = divisor */
	movel	sp@(8), d0	/* d0 = dividend */

	cmpl	#0x10000, d1 /* divisor >= 2 ^ 16 ?   */
	jcc	3f		/* then try next algorithm */
	movel	d0, d2
	clrw	d2
	swap	d2
	divu	d1, d2          /* high quotient in lower word */
	movew	d2, d0		/* save high quotient */
	swap	d0
	movew	sp@(10), d2	/* get low dividend + high rest */
	divu	d1, d2		/* low quotient */
	movew	d2, d0
	jra	6f

3:	movel	d1, d2		/* use d2 as divisor backup */
4:	lsrl	#1, d1	/* shift divisor */
	lsrl	#1, d0	/* shift dividend */
	cmpl	#0x10000, d1 /* still divisor >= 2 ^ 16 ?  */
	jcc	4b
	divu	d1, d0		/* now we have 16-bit divisor */
	andl	#0xffff, d0 /* mask out divisor, ignore remainder */

/* Multiply the 16-bit tentative quotient with the 32-bit divisor.  Because of
   the operand ranges, this might give a 33-bit product.  If this product is
   greater than the dividend, the tentative quotient was too large. */
	movel	d2, d1
	mulu	d0, d1		/* low part, 32 bits */
	swap	d2
	mulu	d0, d2		/* high part, at most 17 bits */
	swap	d2		/* align high part with low part */
	tstw	d2		/* high part 17 bits? */
	jne	5f		/* if 17 bits, quotient was too large */
	addl	d2, d1		/* add parts */
	jcs	5f		/* if sum is 33 bits, quotient was too large */
	cmpl	sp@(8), d1	/* compare the sum with the dividend */
	jls	6f		/* if sum > dividend, quotient was too large */
5:	subql	#1, d0	/* adjust quotient */

6:	movel	sp@+, d2
	.cfi_adjust_cfa_offset -4
	rts
	.cfi_endproc
	.size __udivsi3, .-__udivsi3

	.section .text.__divsi3,"ax",@progbits
	.type __divsi3, function
	.globl	__divsi3
 __divsi3:
 	.cfi_startproc
	movel	d2, sp@-
	.cfi_adjust_cfa_offset 4

	moveq	#1, d2	/* sign of result stored in d2 (=1 or =-1) */
	movel	sp@(12), d1	/* d1 = divisor */
	jpl	1f
	negl	d1
	negb	d2		/* change sign because divisor <0  */
1:	movel	sp@(8), d0	/* d0 = dividend */
	jpl	2f
	negl	d0
	negb	d2

2:	movel	d1, sp@-
	.cfi_adjust_cfa_offset 4
	movel	d0, sp@-
	.cfi_adjust_cfa_offset 4
	jbsr	__udivsi3	/* divide abs(dividend) by abs(divisor) */
	addql	#8, sp
	.cfi_adjust_cfa_offset -8

	tstb	d2
	jpl	3f
	negl	d0

3:	movel	sp@+, d2
	.cfi_adjust_cfa_offset -4
	rts
	.cfi_endproc
	.size __divsi3, .-__divsi3

	.section .text.__modsi3,"ax",@progbits
	.type __modsi3, function
	.globl	__modsi3
__modsi3:
	.cfi_startproc
	movel	sp@(8), d1	/* d1 = divisor */
	movel	sp@(4), d0	/* d0 = dividend */
	movel	d1, sp@-
	.cfi_adjust_cfa_offset 4
	movel	d0, sp@-
	.cfi_adjust_cfa_offset 4
	jbsr	__divsi3
	addql	#8, sp
	.cfi_adjust_cfa_offset -8
	movel	sp@(8), d1	/* d1 = divisor */
	movel	d1, sp@-
	.cfi_adjust_cfa_offset 4
	movel	d0, sp@-
	.cfi_adjust_cfa_offset 4
	jbsr	__mulsi3	/* d0 = (a/b)*b */
	addql	#8, sp
	.cfi_adjust_cfa_offset -8
	movel	sp@(4), d1	/* d1 = dividend */
	subl	d0, d1		/* d1 = a - (a/b)*b */
	movel	d1, d0
	rts
	.cfi_endproc
	.size __modsi3, .-__modsi3

	.section .text.__umodsi3,"ax",@progbits
	.type __umodsi3, function
	.globl	__umodsi3
__umodsi3:
	.cfi_startproc
	movel	sp@(8), d1	/* d1 = divisor */
	movel	sp@(4), d0	/* d0 = dividend */
	movel	d1, sp@-
	.cfi_adjust_cfa_offset 4
	movel	d0, sp@-
	.cfi_adjust_cfa_offset 4
	jbsr	__udivsi3
	addql	#8, sp
	.cfi_adjust_cfa_offset -8
	movel	sp@(8), d1	/* d1 = divisor */
	movel	d1, sp@-
	.cfi_adjust_cfa_offset 4
	movel	d0, sp@-
	.cfi_adjust_cfa_offset 4
	jbsr	__mulsi3	/* d0 = (a/b)*b */
	addql	#8, sp
	.cfi_adjust_cfa_offset -8
	movel	sp@(4), d1	/* d1 = dividend */
	subl	d0, d1		/* d1 = a - (a/b)*b */
	movel	d1, d0
	rts
	.cfi_endproc
	.size __umodsi3, .-__umodsi3

	
	.section .text.KPutCharX,"ax",@progbits
	.type KPutCharX, function
	.globl	KPutCharX

KPutCharX:
	.cfi_startproc
    move.l  a6, -(sp)
	.cfi_adjust_cfa_offset 4
    move.l  4.w, a6
    jsr     -0x204(a6)
    move.l (sp)+, a6
	.cfi_adjust_cfa_offset -4
    rts
	.cfi_endproc
	.size KPutCharX, .-KPutCharX

	.section .text.PutChar,"ax",@progbits
	.type PutChar, function
	.globl	PutChar

PutChar:
	.cfi_startproc
	move.b d0, (a3)+
	rts
	.cfi_endproc
	.size PutChar, .-PutChar

/* LONG RunOnStack(struct StackSwapStruct *sss, LONG (*fn)(void));
 * Swap to the stack described by sss, call fn, swap back, return fn's
 * result.  Done in assembly because C code with -fomit-frame-pointer
 * addresses its locals relative to sp, which is the NEW stack after the
 * first StackSwap: the second StackSwap(&sss) would then read garbage
 * and restore a bogus stack pointer (crash at exit, 2026-10-31). */
	.section .text.RunOnStack,"ax",@progbits
	.type RunOnStack, function
	.globl	RunOnStack
RunOnStack:
	.cfi_startproc
	moveml	d2/a2/a3/a6, sp@-
	movel	sp@(20), a2		/* sss */
	movel	sp@(24), a3		/* fn */
	movel	4.w, a6
	movel	a2, a0
	jsr	a6@(-732)		/* StackSwap: now on the new stack */
	jsr	a3@
	movel	d0, d2
	movel	4.w, a6
	movel	a2, a0
	jsr	a6@(-732)		/* StackSwap back: sp is the old one again */
	movel	d2, d0
	moveml	sp@+, d2/a2/a3/a6
	rts
	.cfi_endproc
	.size RunOnStack, .-RunOnStack
