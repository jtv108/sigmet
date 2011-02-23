/*
   -	sigmet_raw.c --
   -		Command line access to sigmet raw product volumes.
   -		See sigmet_raw (1).
   -
   .	Copyright (c) 2011 Gordon D. Carrie
   .	All rights reserved.
   .
   .	Please send feedback to dev0@trekix.net
   .
   .	$Revision: 1.77 $ $Date: 2011/02/23 14:03:09 $
 */

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/select.h>
#include "alloc.h"
#include "err_msg.h"
#include "strlcpy.h"
#include "geog_lib.h"
#include "sigmet_raw.h"

/*
   Size of various objects
 */

#define SA_UN_SZ (sizeof(struct sockaddr_un))
#define SA_PLEN (sizeof(sa.sun_path))
#define LEN 4096

/*
   Names of fifos that will receive standard output and error output from
   the sigmet_rawd daemon. (These variables are global for cleanup).
 */

static char out_nm[LEN];
static char err_nm[LEN];

/*
   Local functions
 */

static int handle_signals(void);
static void handler(int signum);
static void cleanup(void);
static int daemon_task(int argc, char *argv[]);

int main(int argc, char *argv[])
{
    char *argv0 = argv[0];
    char *argv1;
    char *vol_fl_nm;		/* Name of Sigmet raw product file */
    FILE *in;
    pid_t chpid;

    if ( !handle_signals() ) {
	fprintf(stderr, "%s (%d): could not set up signal management.",
		argv0, getpid());
	return EXIT_FAILURE;
    }
    if ( argc < 2 ) {
	fprintf(stderr, "Usage: %s command\n", argv0);
	exit(EXIT_FAILURE);
    }
    argv1 = argv[1];

    /*
       Branch according to subcommand - second word of command line.
     */

    if ( strcmp(argv1, "data_types") == 0 ) {
	enum Sigmet_DataTypeN y;

	if ( argc != 2 ) {
	    fprintf(stderr, "Usage: %s data_types\n", argv0);
	    exit(EXIT_FAILURE);
	}

	/*
	   If current working directory contains a daemon socket, send this
	   command to it. The daemon might know of data types besides the
	   basic Sigmet ones.  Otherwise, just list data types from the
	   Sigmet programmers manual.
	 */

	if ( access(SIGMET_RAWD_IN, F_OK) == 0 ) {
	    return daemon_task(argc, argv);
	} else {
	    for (y = 0; y < SIGMET_NTYPES; y++) {
		printf("%s | %s | %s\n", Sigmet_DataType_Abbrv(y),
			Sigmet_DataType_Descr(y), Sigmet_DataType_Unit(y));
	    }
	}
    } else if ( strcmp(argv1, "load") == 0 ) {
	/*
	   Start a raw volume daemon.
	 */

	if ( argc != 3 ) {
	    fprintf(stderr, "Usage: %s load raw_file\n", argv0);
	    exit(EXIT_FAILURE);
	}
	vol_fl_nm = argv[2];
	SigmetRaw_Load(vol_fl_nm);
    } else if ( strcmp(argv1, "good") == 0 ) {
	/*
	   Determine if file given as stdin is navigable Sigmet volume.
	 */

	if ( argc == 2 ) {
	    return Sigmet_Vol_Read(stdin, NULL);
	} else if ( argc == 3 ) {
	    vol_fl_nm = argv[2];
	    if ( (in = Sigmet_VolOpen(vol_fl_nm, &chpid)) ) {
		return Sigmet_Vol_Read(in, NULL);
	    } else {
		return SIGMET_IO_FAIL;
	    }
	} else {
	    fprintf(stderr, "Usage: %s %s [raw_file]\n", argv0, argv1);
	    exit(EXIT_FAILURE);
	}
    } else {
	return daemon_task(argc, argv);
    }

    return EXIT_SUCCESS;
}

/*
   Send a command to raw product daemon.
 */

static int daemon_task(int argc, char *argv[])
{
    char *argv0 = argv[0];
    mode_t m;			/* File permissions */
    struct sockaddr_un sa;	/* Address of socket that connects with daemon */
    int i_dmn = -1;		/* File descriptor associated with sa */
    FILE *dmn;			/* File associated with sa */
    pid_t pid = getpid();	/* Id for this process */
    char buf[LEN];		/* Line sent to or received from daemon */
    char **a;			/* Pointer into argv */
    size_t cmd_ln_l;		/* Command line length */
    int i_out = -1;		/* File descriptor for standard output from daemon*/
    int i_err = -1;		/* File descriptors error output from daemon */
    fd_set set, read_set;	/* Give i_dmn, i_out, and i_err to select */
    int fd_hwm = 0;		/* Highest file descriptor */
    ssize_t ll;			/* Number of bytes read from server */
    int sstatus;		/* Result of callback */

    if ( argc > SIGMET_RAWD_ARGCX ) {
	fprintf(stderr, "%s: cannot parse %d arguments. Maximum argument "
		"count is %d\n", argv0, argc, SIGMET_RAWD_ARGCX);
	goto error;
    }
    *out_nm = '\0';
    *err_nm = '\0';
    atexit(cleanup);

    /*
       Create input and output fifo's in daemon working directory.
     */

    m = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    if ( snprintf(out_nm, LEN, "%d.1", pid) > LEN ) {
	fprintf(stderr, "%s (%d): could not create name for result pipe.\n",
		argv0, pid);
	goto error;
    }
    if ( mkfifo(out_nm, m) == -1 ) {
	fprintf(stderr, "%s (%d): could not create pipe for standard output\n"
		"%s\n", argv0, pid, strerror(errno));
	goto error;
    }
    if ( snprintf(err_nm, LEN, "%d.2", pid) > LEN ) {
	fprintf(stderr, "%s (%d): could not create name for error pipe.\n",
		argv0, pid);
	goto error;
    }
    if ( mkfifo(err_nm, m) == -1 ) {
	fprintf(stderr, "%s (%d): could not create pipe for error messages\n"
		"%s\n", argv0, pid, strerror(errno));
	goto error;
    }

    /*
       Connect to daemon via socket in daemon directory
     */

    memset(&sa, '\0', SA_UN_SZ);
    sa.sun_family = AF_UNIX;
    strlcpy(sa.sun_path, SIGMET_RAWD_IN, SA_PLEN);
    if ( (i_dmn = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 ) {
	fprintf(stderr, "%s (%d): could not create socket to connect with daemon\n"
		"%s\n", argv0, pid, strerror(errno));
	goto error;
    }
    if ( connect(i_dmn, (struct sockaddr *)&sa, SA_UN_SZ) == -1
	    || !(dmn = fdopen(i_dmn, "w")) ) {
	fprintf(stderr, "%s (%d): could not connect to daemon\nError %d: %s\n",
		argv0, pid, errno, strerror(errno));
	goto error;
    }

    /*
       Send to input socket of volume daemon:
	   Id of this process.
	   Argument count.
	   Command line length.
	   Arguments.
     */

    for (cmd_ln_l = 0, a = argv; *a; a++) {
	cmd_ln_l += strlen(*a) + 1;
    }
    if ( fwrite(&pid, sizeof(pid_t), 1, dmn) != 1 ) {
	fprintf(stderr, "%s (%d): could not send process id to daemon\n%s\n",
		argv0, pid, strerror(errno));
	goto error;
    }
    if ( fwrite(&argc, sizeof(int), 1, dmn) != 1 ) {
	fprintf(stderr, "%s (%d): could not send argument count to daemon\n%s\n",
		argv0, pid, strerror(errno));
	goto error;
    }
    if ( fwrite(&cmd_ln_l, sizeof(size_t), 1, dmn) != 1 ) {
	fprintf(stderr, "%s (%d): could not send command line length to "
		"daemon\n%s\n", argv0, pid, strerror(errno));
	goto error;
    }
    for (a = argv; *a; a++) {
	size_t a_l;

	a_l = strlen(*a);
	if ( fwrite(*a, 1, a_l, dmn) != a_l || fputc('\0', dmn) == EOF ) {
	    fprintf(stderr, "%s (%d): could not send command to daemon\n%s\n",
		    argv0, pid, strerror(errno));
	    goto error;
	}
    }
    fflush(dmn);

    /*
       Get standard output and errors from fifos.
       Get exit status (single byte 0 or 1) from i_dmn.
     */

    if ( (i_out = open(out_nm, O_RDONLY)) == -1 ) {
	fprintf(stderr, "%s (%d): could not open pipe for standard output\n%s\n",
		argv0, pid, strerror(errno));
	goto error;
    }
    if ( (i_err = open(err_nm, O_RDONLY)) == -1 ) {
	fprintf(stderr, "%s (%d): could not open pipe for error messages\n%s\n",
		argv0, pid, strerror(errno));
	goto error;
    }
    if ( i_dmn > fd_hwm ) {
	fd_hwm = i_dmn;
    }
    if ( i_out > fd_hwm ) {
	fd_hwm = i_out;
    }
    if ( i_err > fd_hwm ) {
	fd_hwm = i_err;
    }
    FD_ZERO(&set);
    FD_SET(i_dmn, &set);
    FD_SET(i_out, &set);
    FD_SET(i_err, &set);
    while ( 1 ) {
	read_set = set;
	if ( select(fd_hwm + 1, &read_set, NULL, NULL, NULL) == -1 ) {
	    fprintf(stderr, "%s (%d): could not get output from daemon\n%s\n",
		    argv0, pid, strerror(errno));
	    goto error;
	}
	if ( i_out != -1 && FD_ISSET(i_out, &read_set) ) {
	    /*
	       Daemon has sent standard output
	     */

	    if ( (ll = read(i_out, buf, LEN)) == -1 ) {
		fprintf(stderr, "%s (%d): could not get standard output from "
			"daemon\n%s\n", argv0, pid, strerror(errno));
		goto error;
	    }
	    if ( ll == 0 ) {
		/*
		   Zero bytes read => no more standard output from daemon.
		 */

		FD_CLR(i_out, &set);
		if ( i_out == fd_hwm ) {
		    fd_hwm--;
		}
		if ( close(i_out) == -1 ) {
		    fprintf(stderr, "%s (%d): could not close standard output "
			    "stream from daemon\n%s\n",
			    argv0, pid, strerror(errno));
		    goto error;
		}
		i_out = -1;
		fflush(stdout);
	    } else {
		/*
		   Non-empty standard output from daemon. Forward to stdout
		   for this process.
		 */

		if ( (fwrite(buf, 1, ll, stdout)) != ll ) {
		    fprintf(stderr, "%s (%d): failed to write standard output.\n",
			    argv0, pid);
		    goto error;
		}
	    }
	} else if ( i_err != -1 && FD_ISSET(i_err, &read_set) ) {
	    /*
	       Daemon has sent error output
	     */

	    if ( (ll = read(i_err, buf, LEN)) == -1 ) {
		fprintf(stderr, "%s (%d): could not get error output from "
			"daemon\n%s\n", argv0, pid, strerror(errno));
		goto error;
	    }
	    if ( ll == 0 ) {
		/*
		   Zero bytes read => no more error output from daemon.
		 */

		FD_CLR(i_err, &set);
		if ( i_err == fd_hwm ) {
		    fd_hwm--;
		}
		if ( close(i_err) == -1 ) {
		    fprintf(stderr, "%s (%d): could not close error output stream "
			    "from daemon\n%s\n", argv0, pid, strerror(errno));
		    goto error;
		}
		i_err = -1;
		fflush(stderr);
	    } else {
		/* 
		   Non-empty error output from daemon. Forward to stderr
		   for this process.
		 */

		if ( (fwrite(buf, 1, ll, stderr)) != ll ) {
		    fprintf(stderr, "%s (%d): failed to write error output.\n",
			    argv0, pid);
		    goto error;
		}
	    }
	} else if ( i_dmn != -1 && FD_ISSET(i_dmn, &read_set) ) {
	    /*
	       Daemon is done with this command and has sent return status.
	       Clean up and return the status.
	     */

	    if ( (ll = read(i_dmn, &sstatus, sizeof(int))) == -1 || ll == 0 ) {
		fprintf(stderr, "%s (%d): could not get exit status from "
			"daemon\n%s\n", argv0, pid,
			(ll == -1) ? strerror(errno) : "nothing to read");
		goto error;
	    }
	    unlink(out_nm);
	    unlink(err_nm);
	    if ( i_dmn != -1 ) {
		close(i_dmn);
	    }
	    if ( i_out != -1 ) {
		close(i_out);
	    }
	    if ( i_err != -1 ) {
		close(i_err);
	    }
	    return sstatus;
	}
    }

error:
    if ( strcmp(out_nm, "") != 0 ) {
	unlink(out_nm);
    }
    if ( strcmp(err_nm, "") != 0 ) {
	unlink(err_nm);
    }
    if ( i_dmn != -1 ) {
	close(i_dmn);
    }
    if ( i_out != -1 ) {
	close(i_out);
    }
    if ( i_err != -1 ) {
	close(i_err);
    }
    return EXIT_FAILURE;
}

static void cleanup(void)
{
    unlink(out_nm);
    unlink(err_nm);
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

    unlink(out_nm);
    unlink(err_nm);
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
