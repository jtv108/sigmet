/*
 -	sigmet_raw_proj.c --
 -		Manage the geographic projection used in sigmet_raw.
 -
 .	Copyright (c) 2010 Gordon D. Carrie
 .	All rights reserved.
 .
 .	Please send feedback to dev0@trekix.net
 .
 .	$Revision: 1.9 $ $Date: 2011/06/09 16:51:15 $
 */

#include <string.h>
#include <stdio.h>
#include "alloc.h"
#include "err_msg.h"
#include "sigmet_raw.h"

#define DFLT_PROJ_ARGC 5
static char *dflt_proj[DFLT_PROJ_ARGC] = {
    "proj", "-b", "+proj=aeqd", "+ellps=sphere", (char *)NULL
};
static char **proj;
static int init;

static void cleanup(void);
static void cleanup(void)
{
    if ( proj ) {
	FREE(*proj);
    }
    FREE(proj);
    proj = NULL;
}

int SigmetRaw_SetProj(int argc, char *argv[])
{
    char **aa, *a, **pp, *p;
    size_t len;

    if ( !init ) {
	atexit(cleanup);
	init = 1;
    }
    cleanup();
    if ( !(proj = CALLOC(argc + 1, sizeof(char *))) ) {
	Err_Append("Could not allocate space for projection array. ");
	return SIGMET_ALLOC_FAIL;
    }
    for (len = 0, aa = argv; *aa; aa++) {
	len += strlen(*aa) + 1;
    }
    if ( !(*proj = CALLOC(len, 1)) ) {
	FREE(proj);
	proj = NULL;
	Err_Append("Could not allocate space for projection content. ");
	return SIGMET_ALLOC_FAIL;
    }
    for (p = *proj, pp = proj, aa = argv; *aa; aa++) {
	*pp++ = p;
	for (a = *aa; *a; ) {
	    *p++ = *a++;
	}
	*p++ = '\0';
    }
    *pp = (char *)NULL;
    return SIGMET_OK;
}

char **SigmetRaw_GetProj(void)
{
    if ( !init ) {
	atexit(cleanup);
	if ( SigmetRaw_SetProj(DFLT_PROJ_ARGC - 1, dflt_proj) != SIGMET_OK ) {
	    fprintf(stderr, "Could not set default projection.\n");
	    exit(EXIT_FAILURE);
	}
	init = 1;
    }
    return proj;
}
