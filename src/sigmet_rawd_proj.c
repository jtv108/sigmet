/*
 -	sigmet_raw_proj.c --
 -		Manage the geographic projection used in sigmet_raw.
 -
 .	Copyright (c) 2010 Gordon D. Carrie
 .	All rights reserved.
 .
 .	Please send feedback to dev0@trekix.net
 .
 .	$Revision: 1.3 $ $Date: 2010/12/06 20:50:20 $
 */

/*
   This interface uses proj4, originally written by Gerald Evenden of the USGS.
   Ref.
       proj (1)
       http://trac.osgeo.org/proj/
       The package manager for your system
 */

#include "sigmet_raw.h"
#include "err_msg.h"

static projPJ pj;
static char *dflt_proj[] = { "+proj=aeqd", "+ellps=sphere" };

int SigmetRaw_ProjInit(void)
{
    if ( !(pj = pj_init(2, dflt_proj)) ) {
	return 0;
    }
    return 1;
}

int SigmetRaw_SetProj(int argc, char *argv[])
{
    projPJ t_pj;

    if ( !(t_pj = pj_init(argc, argv)) ) {
	int a;

	Err_Append("Unknown projection\n");
	for (a = argc + 2; a < argc; a++) {
	    Err_Append(" ");
	    Err_Append(argv[a]);
	}
	Err_Append("\n");
	return SIGMET_BAD_ARG;
    }
    if ( pj ) {
	pj_free(pj);
    }
    pj = t_pj;
    return SIGMET_OK;
}

projPJ SigmetRaw_GetProj(void)
{
    return pj;
}
