// console4.h
#include	<windows.h>
#include    <stdlib.h>
#include    <string.h>
#include    <stdint.h>
#include    <time.h>
#include	<stdio.h>

// DCAM-API headers

#ifndef DCAMAPI_VER
#define	DCAMAPI_VER		4000
#endif

#ifndef DCAMAPI_VERMIN
#define	DCAMAPI_VERMIN	4000
#endif

#include "dcamapi4.h"
#include "dcamprop.h"

#pragma comment(lib,"dcamapi.lib")

// ----------------------------------------------------------------

// define common macro

#ifndef ASSERT
#define	ASSERT(c)
#endif
