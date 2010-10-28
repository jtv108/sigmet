/*
   -	sigmet_raw_start.c --
   -		Callback for "sigmet_raw start ..."
   -
   .	Copyright (c) 2010 Gordon D. Carrie
   .	All rights reserved.
   .
   .	Please send feedback to dev0@trekix.net
   .
   .	$Revision: $ $Date: $
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "sigmet.h"
#include "sigmet_raw.h"

/* Daemon program name */
#define SIGMET_RAWD "sigmet_rawd"

/* Local signal handlers */
static int handle_signals(void);
static void handler(int signum);

/*
   If true, signal the group to stop when handling a term signal.
   This prevents spurious signal loops.
 */
static volatile sig_atomic_t do_term = 1;

/* Size for various strings */
#define LEN 4096

void Sigmet_RawStart(int argc, char *argv[])
{
    char *ucmd;			/* User command to run as a child process */
    char *ddir, ddir_t[LEN];	/* Daemon working directory */
    char sigmet_rawd_in[LEN];	/* Daemon input socket */
    mode_t m;			/* File permissions */
    pid_t spid = getpid();
    pid_t dpid;			/* Id for daemon */
    pid_t upid;			/* Id of user command */
    pid_t pgid = spid;		/* Process group id, for this process,
				   user command, and daemon */
    pid_t chpid;		/* Id of a child process */
    int try;			/* Number of times user command will
				   check for existence of socket */
    int si;			/* Exit information from a user command */
    int status;			/* Exit status from a user command */

    if ( argc == 0 ) {
	fprintf(stderr, "sigmet_raw start: no user command given\n");
	exit(EXIT_FAILURE);
    }
    ucmd = argv[0];

    /* Set up signal handling */
    if ( !handle_signals() ) {
	fprintf(stderr, "sigmet_raw start: could not set up signal "
		"management.");
	exit(EXIT_FAILURE);
    }

    /* Identify and create daemon working directory. */
    if ( !(ddir = getenv("SIGMET_RAWD_DIR")) ) {
	if ( snprintf(ddir_t, LEN, "%s/.sigmet_raw", getenv("HOME"))
		> LEN ) {
	    fprintf(stderr, "sigmet_raw start: could not create name "
		    "for daemon working directory.\n");
	    exit(EXIT_FAILURE);
	}
	ddir = ddir_t;
    }
    m = S_IRUSR | S_IWUSR | S_IXUSR;
    if ( mkdir(ddir, m) == -1 && errno != EEXIST ) {
	perror("Could not create daemon working directory.");
	exit(EXIT_FAILURE);
    }

    /*
       Put daemon working directory into environment. The daemon and
       user command will need it.
     */
    if ( setenv("SIGMET_RAWD_DIR", ddir, 1) == -1 ) {
	perror("sigmet_raw start: could not export name for daemon "
		"working directory.\n");
	exit(EXIT_FAILURE);
    }

    /* Make name of daemon input socket and check if it exists */
    if ( snprintf(sigmet_rawd_in, LEN, "%s/%s", ddir, SIGMET_RAWD_IN)
	    > LEN ) {
	fprintf(stderr, "sigmet_raw start: could not create name "
		"for daemon input socket.\n");
	exit(EXIT_FAILURE);
    }
    if ( access(sigmet_rawd_in, R_OK) == 0 ) {
	fprintf(stderr, "sigmet_raw start: daemon input socket %s "
		"exists. Is %s daemon already running?\n",
		sigmet_rawd_in, SIGMET_RAWD);
	exit(EXIT_FAILURE);
    }

    /* Start the daemon */
    switch (dpid = fork()) {
	case -1:
	    /* Fail */
	    perror("sigmet_raw start: could not fork daemon");
	    break;
	case 0:
	    /* Child process - daemon */
	    if ( setpgid(0, pgid) == -1 ) {
		fprintf(stderr, "%s could not attach to process "
			"group.\n%s\n", SIGMET_RAWD, strerror(errno));
		_exit(EXIT_FAILURE);
	    }
	    if ( chdir(ddir) == -1 ) {
		perror("Could not change to daemon working directory.");
		_exit(EXIT_FAILURE);
	    }
	    execlp(SIGMET_RAWD, SIGMET_RAWD, (char *)NULL);
	    fprintf(stderr, "Could not start %s\n%s\n", SIGMET_RAWD,
		    strerror(errno));
	    _exit(EXIT_FAILURE);
    }

    /* Wait for daemon to make its input socket */
    for (try = 3; try > 0; try--) {
	if ( access(sigmet_rawd_in, R_OK) == 0 ) {
	    break;
	} else {
	    sleep(1);
	}
    }
    if ( try == 0 ) {
	fprintf(stderr, "sigmet_raw start: could not find daemon "
		"input socket %s .\n", sigmet_rawd_in);
	kill(0, SIGTERM);
	_exit(EXIT_FAILURE);
    }

    /* Run the user command */
    switch (upid = fork()) {
	case -1:
	    perror("sigmet_raw start: could not fork user command");
	    break;
	case 0:
	    /* Child process - user command from command line */
	    if ( setpgid(0, pgid) == -1 ) {
		fprintf(stderr, "%s could not attach to process "
			"group.\n%s\n", ucmd, strerror(errno));
		_exit(EXIT_FAILURE);
	    }

	    /* Execute the user command */
	    execvp(ucmd, argv);
	    fprintf(stderr, "Could not start %s\n%s\n",
		    ucmd, strerror(errno));
	    _exit(EXIT_FAILURE);
    }

    /* Wait for a child to exit */
    if ( (chpid = wait(&si)) == -1 ) {
	perror("sigmet_raw start: unable to wait for children");
	kill(0, SIGTERM);
	exit(EXIT_FAILURE);
    }
    if ( chpid == upid ) {
	/*
	   Exiting child is the user command - normal exit. Clean up,
	   stop the daemon, and return the user command's exit status
	   as the status of "sigmet_raw start ..."
	 */

	if ( WIFEXITED(si) ) {
	    status = WEXITSTATUS(si);
	} else if ( WIFSIGNALED(si) ) {
	    fprintf(stderr, "%s: exited on signal %d\n",
		    ucmd, WTERMSIG(si));
	    status = EXIT_FAILURE;
	}
	do_term = 0;
	kill(0, SIGTERM);
	exit(status);
    } else {
	/*
	   Exiting child is the daemon - should not happen.
	 */

	fprintf(stderr, "sigmet_raw start: unexpected exit by %s. ",
		SIGMET_RAWD);
	if ( WIFEXITED(si) ) {
	    fprintf(stderr, "daemon exited with status code %d\n",
		    WEXITSTATUS(si));
	} else if ( WIFSIGNALED(si) ) {
	    fprintf(stderr, "daemon exited on signal %d\n",
		    WTERMSIG(si));
	}
	kill(0, SIGTERM);
	exit(EXIT_FAILURE);
    }
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

    /* Signals to ignore */
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

    /* Generic action for termination signals */
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
   For exit signals, print an error message, and terminate rest of
   process group
 */
void handler(int signum)
{
    char *msg;

    switch (signum) {
	case SIGTERM:
	    if ( do_term ) {
		msg = "sigmet_raw start exiting on termination signal    \n";
		break;
	    } else {
		do_term = 1;
		return;
	    }
	case SIGBUS:
	    msg = "sigmet_raw start exiting on bus error             \n";
	    break;
	case SIGFPE:
	    msg = "sigmet_raw start exiting arithmetic exception     \n";
	    break;
	case SIGILL:
	    msg = "sigmet_raw start exiting illegal instruction      \n";
	    break;
	case SIGSEGV:
	    msg = "sigmet_raw start exiting invalid memory reference \n";
	    break;
	case SIGSYS:
	    msg = "sigmet_raw start exiting on bad system call       \n";
	    break;
	case SIGXCPU:
	    msg = "sigmet_raw start exiting: CPU time limit exceeded \n";
	    break;
	case SIGXFSZ:
	    msg = "sigmet_raw start exiting: file size limit exceeded\n";
	    break;
    }
    write(STDERR_FILENO, msg, 51);
    kill(0, SIGTERM);
    _exit(EXIT_FAILURE);
}
