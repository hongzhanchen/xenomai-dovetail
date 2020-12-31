/**
 *   Arithmetic/conversion routines for x86.
 *
 *   Copyright &copy; 2005 Gilles Chanteperdrix, 32bit version.
 *   Copyright &copy; 2007 Jan Kiszka, 64bit version.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 */
#ifndef _COBALT_X86_ASM_DOVETAIL_UAPI_ARITH_H
#define _COBALT_X86_ASM_DOVETAIL_UAPI_ARITH_H

#include <asm/xenomai/uapi/features.h>

static inline __attribute__((__const__)) long long
mach_x86_64_llimd (long long op, unsigned m, unsigned d)
{
	long long result;

	__asm__ (
		"imul %[m]\n\t"
		"idiv %[d]\n\t"
		: "=a" (result)
		: "a" (op), [m] "r" ((unsigned long long)m),
		  [d] "r" ((unsigned long long)d)
		: "rdx");

	return result;
}
#define xnarch_llimd(ll,m,d) mach_x86_64_llimd((ll),(m),(d))

static inline __attribute__((__const__)) long long
mach_x86_64_llmulshft(long long op, unsigned m, unsigned s)
{
	long long result;

	__asm__ (
		"imulq %[m]\n\t"
		"shrd %%cl,%%rdx,%%rax\n\t"
		: "=a,a" (result)
		: "a,a" (op), [m] "m,r" ((unsigned long long)m),
		  "c,c" (s)
		: "rdx");

	return result;
}
#define xnarch_llmulshft(op, m, s) mach_x86_64_llmulshft((op), (m), (s))

static inline __attribute__((__const__)) unsigned long long
mach_x86_64_nodiv_ullimd(unsigned long long op,
			    unsigned long long frac, unsigned rhs_integ)
{
	register unsigned long long rl __asm__("rax") = frac;
	register unsigned long long rh __asm__("rdx");
	register unsigned long long integ __asm__("rsi") = rhs_integ;
	register unsigned long long t __asm__("r8") = 0x80000000ULL;

	__asm__ ("mulq %[op]\n\t"
		 "addq %[t], %[rl]\n\t"
		 "adcq $0, %[rh]\n\t"
		 "imulq %[op], %[integ]\n\t"
		 "leaq (%[integ], %[rh], 1),%[rl]":
		 [rh]"=&d"(rh), [rl]"+&a"(rl), [integ]"+S"(integ):
		 [op]"D"(op), [t]"r"(t): "cc");

	return rl;
}

#define xnarch_nodiv_ullimd(op, frac, integ) \
	mach_x86_64_nodiv_ullimd((op), (frac), (integ))

#include <cobalt/uapi/asm-generic/arith.h>

#endif /* _COBALT_X86_ASM_DOVETAIL_UAPI_ARITH_H */
