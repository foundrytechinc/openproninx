/*	$OpenBSD: difftime.c,v 1.14 2026/04/23 01:08:47 millert Exp $ */
/* This file is placed in the public domain by Matthew Dempsky. */

#include "private.h"

#define HI(t) ((double)(time_t)(t & 0xffffffff00000000LL))
#define LO(t) ((double)(time_t)(t & 0x00000000ffffffffLL))

double __pure
difftime(time_t t1, time_t t0)
{
	return (HI(t1) - HI(t0)) + (LO(t1) - LO(t0));
}
