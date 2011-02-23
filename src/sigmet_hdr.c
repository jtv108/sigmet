/*
   -	sigmet_hdr.c --
   -		Print volume headers.
   -
   .	Copyright (c) 2011 Gordon D. Carrie
   .	All rights reserved.
   .
   .	Please send feedback to dev0@trekix.net
   .
   .	$Revision: 1.76 $ $Date: 2011/02/22 20:44:51 $
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include "alloc.h"
#include "err_msg.h"
#include "geog_lib.h"
#include "sigmet.h"

/*
   Usage
   sigmet_hdr [-a] [raw_file]
 */

/*
   Local functions
 */

static int handle_signals(void);
static void handler(int signum);
static int print_vol_hdr(struct Sigmet_Vol *vol_p);
static void reg_types(void);

int main(int argc, char *argv[])
{
    char *argv0 = argv[0];
    char *vol_fl_nm = "-";
    FILE *in;
    int abbrv = 0;		/* If true, give abbreviated output */
    pid_t chpid;
    struct Sigmet_Vol vol;

    if ( !handle_signals() ) {
	fprintf(stderr, "%s (%d): could not set up signal management.",
		argv0, getpid());
	return EXIT_FAILURE;
    }

    reg_types();
    Sigmet_Vol_Init(&vol);

    if ( argc == 1 ) {
	in = stdin;
    } else if ( argc == 2 ) {
	if ( strcmp(argv[1], "-a") == 0 ) {
	    abbrv = 1;
	    vol_fl_nm = "-";
	} else {
	    vol_fl_nm = argv[1];
	}
    } else if ( argc == 3 ) {
	if ( strcmp(argv[1], "-a") != 0 ) {
	    fprintf(stderr, "Usage: %s [-a] [raw_file]\n", argv0);
	    exit(EXIT_FAILURE);
	}
	abbrv = 1;
	vol_fl_nm = argv[2];
    } else {
	fprintf(stderr, "Usage: %s [-a] [raw_file]\n", argv0);
	exit(EXIT_FAILURE);
    }
    if ( strcmp(vol_fl_nm, "-") == 0 ) {
	in = stdin;
    } else {
	in = Sigmet_VolOpen(vol_fl_nm, &chpid);
    }
    if ( !in ) {
	fprintf(stderr, "%s: could not open %s for input\n%s\n",
		argv0, vol_fl_nm, Err_Get());
	exit(EXIT_FAILURE);
    }
    if ( Sigmet_Vol_ReadHdr(in, &vol) != SIGMET_OK ) {
	fprintf(stderr, "%s: read failed\n%s\n", argv0, Err_Get());
	exit(EXIT_FAILURE);
    }
    if ( abbrv ) {
	print_vol_hdr(&vol);
    } else {
	Sigmet_Vol_PrintHdr(stdout, &vol);
    }

    return EXIT_SUCCESS;
}

/*
   Register Sigmet data types.
 */

static void reg_types(void)
{
    int sig_type;		/* Loop index */

    for (sig_type = 0; sig_type < SIGMET_NTYPES; sig_type++) {
	int status;

	status = DataType_Add( Sigmet_DataType_Abbrv(sig_type),
		Sigmet_DataType_Descr(sig_type),
		Sigmet_DataType_Unit(sig_type),
		Sigmet_DataType_StorFmt(sig_type),
		Sigmet_DataType_StorToComp(sig_type));
	if ( status != DATATYPE_SUCCESS ) {
	    fprintf(stderr, "could not register data type %s\n%s\n",
		    Sigmet_DataType_Abbrv(sig_type), Err_Get());
	    switch (status) {
		case DATATYPE_INPUT_FAIL:
		    status = SIGMET_IO_FAIL;
		    break;
		case DATATYPE_ALLOC_FAIL:
		    status = SIGMET_ALLOC_FAIL;
		    break;
		case DATATYPE_BAD_ARG:
		    status = SIGMET_BAD_ARG;
		    break;
	    }
	    exit(status);
	}
    }
}

/*
   Print selected headers from vol_p.
 */

static int print_vol_hdr(struct Sigmet_Vol *vol_p)
{
    int y;
    double wavlen, prf, vel_ua;
    enum Sigmet_Multi_PRF mp;
    char *mp_s = "unknown";

    printf("site_name=\"%s\"\n", vol_p->ih.ic.su_site_name);
    printf("radar_lon=%.4lf\n",
	   GeogLonR(Sigmet_Bin4Rad(vol_p->ih.ic.longitude), 0.0) * DEG_PER_RAD);
    printf("radar_lat=%.4lf\n",
	   GeogLonR(Sigmet_Bin4Rad(vol_p->ih.ic.latitude), 0.0) * DEG_PER_RAD);
    printf("task_name=\"%s\"\n", vol_p->ph.pc.task_name);
    printf("types=\"");
    if ( vol_p->dat[0].data_type->abbrv ) {
	printf("%s", vol_p->dat[0].data_type->abbrv);
    }
    for (y = 1; y < vol_p->num_types; y++) {
	if ( vol_p->dat[y].data_type->abbrv ) {
	    printf(" %s", vol_p->dat[y].data_type->abbrv);
	}
    }
    printf("\"\n");
    printf("num_sweeps=%d\n", vol_p->ih.ic.num_sweeps);
    printf("num_rays=%d\n", vol_p->ih.ic.num_rays);
    printf("num_bins=%d\n", vol_p->ih.tc.tri.num_bins_out);
    printf("range_bin0=%d\n", vol_p->ih.tc.tri.rng_1st_bin);
    printf("bin_step=%d\n", vol_p->ih.tc.tri.step_out);
    wavlen = 0.01 * 0.01 * vol_p->ih.tc.tmi.wave_len; 	/* convert -> cm > m */
    prf = vol_p->ih.tc.tdi.prf;
    mp = vol_p->ih.tc.tdi.m_prf_mode;
    vel_ua = -1.0;
    switch (mp) {
	case ONE_ONE:
	    mp_s = "1:1";
	    vel_ua = 0.25 * wavlen * prf;
	    break;
	case TWO_THREE:
	    mp_s = "2:3";
	    vel_ua = 2 * 0.25 * wavlen * prf;
	    break;
	case THREE_FOUR:
	    mp_s = "3:4";
	    vel_ua = 3 * 0.25 * wavlen * prf;
	    break;
	case FOUR_FIVE:
	    mp_s = "4:5";
	    vel_ua = 3 * 0.25 * wavlen * prf;
	    break;
    }
    printf("prf=%.2lf\n", prf);
    printf("prf_mode=%s\n", mp_s);
    printf("vel_ua=%.3lf\n", vel_ua);
    return SIGMET_OK;
}

/*
   Basic signal management.

   Reference --
   Rochkind, Marc J., "Advanced UNIX Programming, Second Edition",
   2004, Addison-Wesley, Boston.
 */

int handle_signals(void)
{
    sigset_t set;
    struct sigaction act;

    if ( sigfillset(&set) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigprocmask(SIG_SETMASK, &set, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    memset(&act, 0, sizeof(struct sigaction));
    if ( sigfillset(&act.sa_mask) == -1 ) {
	perror(NULL);
	return 0;
    }

    /*
       Signals to ignore
     */

    act.sa_handler = SIG_IGN;
    if ( sigaction(SIGHUP, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGINT, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGQUIT, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGPIPE, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }

    /*
       Generic action for termination signals
     */

    act.sa_handler = handler;
    if ( sigaction(SIGTERM, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGBUS, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGFPE, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGILL, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGSEGV, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGSYS, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGXCPU, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGXFSZ, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigemptyset(&set) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigprocmask(SIG_SETMASK, &set, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }

    return 1;
}

/*
   For exit signals, close fifo's and print an error message if possible.
 */

void handler(int signum)
{
    char *msg;
    ssize_t dum;

    msg = "sigmet_raw command exiting                          \n";
    switch (signum) {
	case SIGTERM:
	    msg = "sigmet_raw command exiting on termination signal    \n";
	    break;
	case SIGBUS:
	    msg = "sigmet_raw command exiting on bus error             \n";
	    break;
	case SIGFPE:
	    msg = "sigmet_raw command exiting arithmetic exception     \n";
	    break;
	case SIGILL:
	    msg = "sigmet_raw command exiting illegal instruction      \n";
	    break;
	case SIGSEGV:
	    msg = "sigmet_raw command exiting invalid memory reference \n";
	    break;
	case SIGSYS:
	    msg = "sigmet_raw command exiting on bad system call       \n";
	    break;
	case SIGXCPU:
	    msg = "sigmet_raw command exiting: CPU time limit exceeded \n";
	    break;
	case SIGXFSZ:
	    msg = "sigmet_raw command exiting: file size limit exceeded\n";
	    break;
    }
    dum = write(STDERR_FILENO, msg, 53);
    _exit(EXIT_FAILURE);
}