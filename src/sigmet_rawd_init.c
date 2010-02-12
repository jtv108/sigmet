/*
 -	sigmet_rawd.c --
 -		This file defines data structures and functions for a
 -		daemon that reads, manipulates data from, and provides
 -		access to Sigmet raw volumes. See sigmet_rawd (1).
 -
 .	Copyright (c) 2009 Gordon D. Carrie
 .	All rights reserved.
 .
 .	Please send feedback to dev0@trekix.net
 .
 .	$Revision: 1.119 $ $Date: 2010/02/12 22:17:11 $
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include "alloc.h"
#include "str.h"
#include "err_msg.h"
#include "tm_calc_lib.h"
#include "geog_lib.h"
#include "sigmet.h"

/* Size for various strings */
#define LEN 1024

/* Maximum number of arguments in input command */
#define ARGCX 512

/* Shell type determines how environment variables and commands are printed */
enum SHELL_TYPE {C, SH};

/* If true, send full error messages. */
static int verbose = 0;

/* If true, use degrees instead of radians */
static int use_deg = 0;

/* Sigmet volumes */
struct sig_vol {
    int v;			/* If true, this struct contains a volume */
    struct Sigmet_Vol vol;	/* Sigmet volume */
    dev_t st_dev;		/* Device that provided vol */
    ino_t st_ino;		/* I-number of file that provided vol */
    int users;			/* Number of client sessions using vol */
};
static struct sig_vol *vols;	/* Available volumes */
static size_t n_vols = 12;	/* Number of volumes can be stored at vols */
static int get_vol_i(char *);	/* Find member of vols given file name */

/* Process streams and files */
static char *ddir;		/* Working directory for server */
static int i_cmd0;		/* Where to get commands */
static int i_cmd1;		/* Unused outptut (see explanation below). */
static int idlog = -1;		/* Error log */
static char log_nm[LEN];	/* Name of log file */
static FILE *dlog;		/* Error log */
static FILE *rslt1;		/* Where to send standard output */
static FILE *rslt2;		/* Where to send standard error */
pid_t client_pid;		/* Process id of client */

/* These variables determine whether and when a slow client will be killed. */
static int tmgout = 1;		/* If true, time out blocking clients. */
static int tmoadj = 1;		/* If true, adjust timeout periodically. */
static unsigned tmout = 60;	/* Max seconds client can block daemon */

/* Input line - has commands for the daemon */
#define BUF_L 512
static char buf[BUF_L];
static char *buf_e = buf + BUF_L;

/* Application and subcommand name */
static char *cmd;
static char *cmd1;

/* Subcommands */
#define NCMD 15
static char *cmd1v[NCMD] = {
    "cmd_len", "verbose", "log", "timeout", "types", "good", "hread","read",
    "release", "volume_headers", "ray_headers", "data", "bin_outline", "bintvls",
    "stop"
};
typedef int (callback)(int , char **);
static callback cmd_len_cb;
static callback verbose_cb;
static callback log_cb;
static callback timeout_cb;
static callback types_cb;
static callback good_cb;
static callback hread_cb;
static callback read_cb;
static callback release_cb;
static callback volume_headers_cb;
static callback ray_headers_cb;
static callback data_cb;
static callback bin_outline_cb;
static callback bintvls_cb;
static callback stop_cb;
static callback *cb1v[NCMD] = {
    cmd_len_cb, verbose_cb, log_cb, timeout_cb, types_cb, good_cb, hread_cb,
    read_cb, release_cb, volume_headers_cb, ray_headers_cb, data_cb,
    bin_outline_cb, bintvls_cb, stop_cb
};

/* Convenience functions */
static char *time_stamp(void);
static FILE *vol_open(const char *, pid_t *);
static unsigned new_dchk(void);

/* Signal handling functions */
static int handle_signals(void);
static void alarm_handler(int);
static void handler(int);

int main(int argc, char *argv[])
{
    int bg = 1;			/* If true, run in foreground (do not fork) */
    char dtmpl[] = "/tmp/sigmet_rawd.XXXXXX"; /* Work directory */
    enum SHELL_TYPE shtyp = SH;	/* Control how environment variables are printed */
    char cmd_in_nm[LEN];	/* Name of fifo from which to read commands */
    int a;			/* Index into argv */
    pid_t pid;			/* Process id for this process */
    struct sig_vol *sv_p;	/* Member of vols */
    char *ang_u;		/* Angle unit */
    char *b;			/* Point into buf */
    ssize_t r;			/* Return value from read */
    size_t l;			/* Number of bytes to read from command buffer */
    unsigned dchk;		/* After dchk clients, time some, adjust tmout */
    time_t start, end;		/* When a client session starts, ends */
    double dt;			/* Time taken by a client */
    double dtx;			/* Time taken by slowest client in a set */

    /* Set up signal handling */
    if ( !handle_signals() ) {
	fprintf(stderr, "%s: could not set up signal management.", argv[0]);
	exit(EXIT_FAILURE);
    }

    if ( (cmd = strrchr(argv[0], '/')) ) {
	cmd++;
    } else {
	cmd = argv[0];
    }

    /* Usage: sigmet_rawd [-c] [-f] [-n integer] */
    for (a = 1; a < argc; a++) {
	if (strcmp(argv[a], "-c") == 0) {
	    shtyp = C;
	} else if (strcmp(argv[a], "-f") == 0) {
	    bg = 0;
	} else if (strcmp(argv[a], "-n") == 0) {
	    if ( ++a == argc ) {
		fprintf(stderr, "%s: -n requires an value\n", cmd);
		exit(EXIT_FAILURE);
	    }
	    if ( sscanf(argv[a], "%lu", &n_vols) != 1 ) {
		fprintf(stderr, "%s: expected integer for volume count, got %s\n",
			cmd, argv[a]);
		exit(EXIT_FAILURE);
	    }
	} else {
	    fprintf(stderr, "%s: unknown option \"%s\"\n", cmd, argv[a]);
	    exit(EXIT_FAILURE);
	}
    }

    /* Create vols array */
    if ( !(vols = CALLOC(n_vols, sizeof(struct sig_vol))) ) {
	fprintf(stderr, "%s: Could not allocate vols array.\n", cmd);
	exit(EXIT_FAILURE);
    }
    for (sv_p = vols; sv_p < vols + n_vols; sv_p++) {
	sv_p->v = 0;
	Sigmet_InitVol(&sv_p->vol);
	sv_p->st_dev = 0;
	sv_p->st_ino = 0;
	sv_p->users = 0;
    }

    /* Check for angle unit */
    if ((ang_u = getenv("ANGLE_UNIT")) != NULL) {
	if (strcmp(ang_u, "DEGREE") == 0) {
	    use_deg = 1;
	} else if (strcmp(ang_u, "RADIAN") == 0) {
	    use_deg = 0;
	} else {
	    fprintf(stderr, "%s: Unknown angle unit %s.\n", cmd, ang_u);
	    exit(EXIT_FAILURE);
	}
    }

    /* Create working directory */
    if ( (ddir = getenv("SIGMET_RAWD_DIR"))
	    && (mkdir(ddir, S_IRWXU | S_IRWXG ) == -1) ) {
	fprintf(stderr, "%s: could not create directory %s\n%s\n",
		cmd, ddir, strerror(errno));
	exit(EXIT_FAILURE);
    } else if ( !(ddir = mkdtemp(dtmpl)) ) {
	perror("could not create daemon work directory");
	exit(EXIT_FAILURE);
    }
    if ( (chmod(ddir, S_IRWXU | S_IRWXG ) == -1) ) {
	perror("could not set permissions for work directory");
	exit(EXIT_FAILURE);
    }

    /* Create named pipe for command input */
    if ( snprintf(cmd_in_nm, LEN, "%s/%s", ddir, "sigmet.in") >= LEN ) {
	fprintf(stderr, "%s: could not create name for server input pipe.\n", cmd);
	exit(EXIT_FAILURE);
    }
    if ( (mkfifo(cmd_in_nm, S_IRUSR | S_IWUSR) == -1) ) {
	fprintf(stderr, "%s: sigmet_rawd could not create input pipe.\n%s\n",
		cmd, strerror(errno));
	exit(EXIT_FAILURE);
    }

    /* Open log file */
    if ( snprintf(log_nm, LEN, "%s/%s", ddir, "sigmet.log") >= LEN ) {
	fprintf(stderr, "%s: could not create name for log file.\n", cmd);
	exit(EXIT_FAILURE);
    }
    idlog = open(log_nm, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if ( idlog == -1 || !(dlog = fdopen(idlog, "w"))
	    || setvbuf(dlog, NULL, _IOLBF, 0) != 0 ) {
	fprintf(stderr, "%s: could not create log file.\n", cmd);
	exit(EXIT_FAILURE);
    }

    if ( bg ) {
	/* Put server in background */
	switch (pid = fork()) {
	    case -1:
		Err_Append(strerror(errno));
		Err_Append("\nCould not spawn sigmet daemon in background.  ");
		return 0;
	    case 0:
		/* Child = daemon process. */

		if ( !handle_signals() ) {
		    fprintf(stderr, "%s: could not set up signal management "
			    "in daemon.", cmd);
		    exit(EXIT_FAILURE);
		}

		/* Go to background. */
		fclose(stdin);
		fclose(stdout);
		fclose(stderr);
		break;
	    default:
		/* Parent. Print information and exit. */
		if (shtyp == SH) {
		    printf("SIGMET_RAWD_PID=%d; export SIGMET_RAWD_PID;\n", pid);
		    printf("SIGMET_RAWD_DIR=%s; export SIGMET_RAWD_DIR;\n", ddir);
		    printf("SIGMET_RAWD_IN=%s; export SIGMET_RAWD_IN;\n",
			    cmd_in_nm);
		} else {
		    printf("setenv SIGMET_RAWD_PID %d;\n", pid);
		    printf("setenv SIGMET_RAWD_DIR %s;\n", ddir);
		    printf("setenv SIGMET_RAWD_IN=%s;\n", cmd_in_nm);
		}
		printf("echo Starting sigmet_rawd. Process id = %d.;\n", pid);
		printf("echo Working directory = %s;\n", ddir);
		printf("echo Log file = %s;\n", log_nm);
		exit(EXIT_SUCCESS);
	}
    }

    /* Initialize log */
    pid = getpid();
    fprintf(dlog, "%s: sigmet_rawd started. pid=%d\n", time_stamp(), pid);
    fprintf(dlog, "Set environment as follows:\n");
    if (shtyp == SH) {
	fprintf(dlog, "SIGMET_RAWD_PID=%d; export SIGMET_RAWD_PID;\n", pid);
	fprintf(dlog, "SIGMET_RAWD_DIR=%s; export SIGMET_RAWD_DIR;\n", ddir);
	fprintf(dlog, "SIGMET_RAWD_IN=%s; export SIGMET_RAWD_IN;\n", cmd_in_nm);
    } else {
	fprintf(dlog, "setenv SIGMET_RAWD_PID %d;\n", pid);
	fprintf(dlog, "setenv SIGMET_RAWD_DIR %s;\n", ddir);
	fprintf(dlog, "setenv SIGMET_RAWD_IN=%s;\n", cmd_in_nm);
    }
    fprintf(dlog, "\n");

    tmout = 20;
    dchk = new_dchk();
    fprintf(dlog, "Initializing client timeout. Clients that block the daemon"
	    " for more %d seconds will be killed.\n", tmout);
    if (tmoadj) {
	fprintf(dlog, "Will assess timeout after %d iterations\n", dchk );
    }

    /* Open command input stream. */
    if ( (i_cmd0 = open(cmd_in_nm, O_RDONLY)) == -1 ) {
	fprintf(stderr, "%s: Could not open %s for input.\n%s\n",
		cmd, cmd_in_nm, strerror(errno));
	exit(EXIT_FAILURE);
    }
    if ( (i_cmd1 = open(cmd_in_nm, O_WRONLY)) == -1 ) {
	fprintf(stderr, "%s: Could not open %s for output.\n%s\n",
		cmd, cmd_in_nm, strerror(errno));
	exit(EXIT_FAILURE);
    }

    /* Read commands from i_cmd0 into buf and execute them.  */
    while ( read(i_cmd0, &l, sizeof(size_t)) != -1 ) {
	if ( l > BUF_L || (r = read(i_cmd0, buf, l)) != l ) {
	    fprintf(dlog, "%s: Failed to read command of %ld bytes. ",
		    time_stamp(), l);
	    if ( r == -1 ) {
		fprintf(dlog, "%s\n", strerror(errno));
	    }
	} else {
	    int argc1;		/* Number of arguments in received command line */
	    char *argv1[ARGCX];	/* Arguments from received command line */
	    int a;		/* Index into argv1 */
	    char rslt1_nm[LEN];	/* Name of file for standard output */
	    char rslt2_nm[LEN];	/* Name of file for error output */
	    int status;		/* Result of callback */
	    int i;		/* Loop index */
	    int tmx;		/* If true, time this session */

	    /*
	       If imposing timeouts (tmgout is set), set alarm.
	       If adjusting timeouts store start time for this session.
	     */

	    if ( tmgout ) {
		alarm(tmout);
		if ( tmoadj && --dchk < 8 ) {
		    tmx = 1;
		    start = time(NULL);
		} else {
		    tmx = 0;
		}
	    }

	    /* Break command line into arguments */
	    b = buf;
	    client_pid = *(pid_t *)b;
	    b += sizeof(client_pid);
	    argc1 = *(int *)b;
	    b += sizeof(argc1);
	    if (argc1 > ARGCX) {
		fprintf(dlog, "%s: Unable to parse command with %d arguments. "
			"Limit is %d\n", time_stamp(), argc1, ARGCX);
		continue;
	    }
	    for (a = 0, argv1[a] = b; b < buf_e && a < argc1; b++) {
		if ( *b == '\0' ) {
		    argv1[++a] = b + 1;
		}
	    }
	    if ( b == buf_e ) {
		fprintf(dlog, "%s: Command line gives no destination.\n",
			time_stamp());
		continue;
	    }

	    /* Create and open "standard output" file */
	    rslt1 = NULL;
	    if (snprintf(rslt1_nm, LEN, "%s/%d.1", ddir, client_pid) > LEN) {
		fprintf(dlog, "%s: Could not create file name for client %d.\n",
			time_stamp(), client_pid);
		continue;
	    }
	    if ( !(rslt1 = fopen(rslt1_nm, "w")) ) {
		fprintf(dlog, "%s: Could not open %s for output.\n%s\n",
			time_stamp(), rslt1_nm, strerror(errno));
		continue;
	    }

	    /* Create and open error file */
	    rslt2 = NULL;
	    if (snprintf(rslt2_nm, LEN, "%s/%d.2", ddir, client_pid) > LEN) {
		fprintf(dlog, "%s: Could not create file name for client %d.\n",
			time_stamp(), client_pid);
		continue;
	    }
	    if ( !(rslt2 = fopen(rslt2_nm, "w")) ) {
		fprintf(dlog, "%s: Could not open %s for output.\n%s\n",
			time_stamp(), rslt2_nm, strerror(errno));
		continue;
	    }

	    /* Identify command */
	    cmd1 = argv1[0];
	    if ( (i = Sigmet_RawCmd(cmd1)) == -1) {
		fputc(EXIT_FAILURE, rslt2);
		if ( verbose ) {
		    fprintf(rslt2, "No option or subcommand named \"%s\"\n", cmd1);
		    fprintf(rslt2, "Subcommand must be one of: ");
		    for (i = 0; i < NCMD; i++) {
			fprintf(rslt2, "%s ", cmd1v[i]);
		    }
		    fprintf(rslt2, "\n");
		}
		if ( (fclose(rslt1) == EOF) ) {
		    fprintf(dlog, "%s: Could not close %s.\n%s\n",
			    time_stamp(), rslt1_nm, strerror(errno));
		}
		if ( (fclose(rslt2) == EOF) ) {
		    fprintf(dlog, "%s: Could not close %s.\n%s\n",
			    time_stamp(), rslt2_nm, strerror(errno));
		}
		continue;
	    }

	    /* Run command. Send "standard output" to rslt1. */
	    status = (cb1v[i])(argc1, argv1);
	    if ( (fclose(rslt1) == EOF) ) {
		fprintf(dlog, "%s: Could not close %s.\n%s\n",
			time_stamp(), rslt1_nm, strerror(errno));
	    }

	    /* Send status and error messages, if any, to rslt2 and log */
	    if ( status ) {
		fputc(EXIT_SUCCESS, rslt2);
	    } else {
		fputc(EXIT_FAILURE, rslt2);
		if ( verbose ) {
		    fprintf(rslt2, "%s: %s failed.\n%s\n", cmd, cmd1, Err_Get());
		    fprintf(dlog, "%s: %s failed.\n%s\n",
			    time_stamp(), cmd1, Err_Get());
		}
	    }
	    if ( (fclose(rslt2) == EOF) ) {
		fprintf(dlog, "%s: Could not close %s.\n%s\n",
			time_stamp(), rslt2_nm, strerror(errno));
	    }

	    /*
	       If imposing timeouts, clear alarm.
	       Adjust timeout if desired and necessary.
	     */
	    if ( tmgout ) {
		alarm(0);
		if ( tmx ) {
		    end = time(NULL);
		    dt = difftime(end, start);
		    if (verbose) {
			fprintf(dlog, "%s: Session %d took ", time_stamp(), dchk);
			if ( dt == 0.0 ) {
			    fprintf(dlog, "< 1.0 sec.\n");
			} else {
			    fprintf(dlog, "%lf sec.\n", dt);
			}
		    }
		    if (dt > dtx) {
			dtx = dt;
			if (verbose) {
			    fprintf(dlog, "%s: Slowest client in this set so "
				    "far took ", time_stamp());
			    if ( dtx == 0.0 ) {
				fprintf(dlog, "< 1.0 sec.\n");
			    } else {
				fprintf(dlog, "%lf sec.\n", dtx);
			    }
			}
		    }
		    if ( dchk == 0 ) {
			if ( dtx == 0.0 ) {
			    dtx = 1.0;
			}
			if ( dtx < tmout / 2 ) {
			    tmout = tmout / 2 + 1;
			    if (verbose) {
				fprintf(dlog, "%s: Setting timeout to %u sec.\n",
					time_stamp(), tmout);
			    }
			}
			dchk = new_dchk();
			if (verbose) {
			    fprintf(dlog, "%s: Will assess timeout after %d "
				    "iterations\n", time_stamp(), dchk);
			}
			dtx = -1.0;
		    }
		}
	    }
	}
    }

    /* Should not end up here. Process should exit with "stop" command. */
    fprintf(dlog, "%s: unexpected exit.  %s\n", time_stamp(), strerror(errno));
    if ( close(i_cmd0) == -1 ) {
	fprintf(dlog, "%s: could not close command stream.\n%s\n",
		time_stamp(), strerror(errno));
    }
    for (sv_p = vols; sv_p < vols + n_vols; sv_p++) {
	Sigmet_FreeVol(&sv_p->vol);
    }
    FREE(vols);
    fclose(dlog);

    return 0;
}

/* Get a character string with the current time */
static char *time_stamp(void)
{
    static char ts[LEN];
    time_t now;

    now = time(NULL);
    if (strftime(ts, LEN, "%Y/%m/%d %H:%M:%S %Z", localtime(&now))) {
	return ts;
    } else {
	return "";
    }
}

/*
   Daemon times a few clients after serving dchk of them.  This function
   computes a new dchk so that the sampling intervals are random.
 */
static unsigned new_dchk(void)
{
    return 100 + 100 * random() / 0x7fffffff;
}

static int cmd_len_cb(int argc, char *argv[])
{
    if (argc != 1) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	return 0;
    }
    fprintf(rslt1, "%d\n", BUF_L);
    return 1;
}

static int verbose_cb(int argc, char *argv[])
{
    char *val;

    if (argc != 2) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" true|false");
	return 0;
    }
    val = argv[1];
    if ( strcmp(val, "true") == 0 ) {
	verbose = 1;
    } else if ( strcmp(val, "false") == 0 ) {
	verbose = 0;
    } else {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" true|false");
	return 0;
    }
    return 1;
}

static int log_cb(int argc, char *argv[])
{
    FILE *l;
    int c;

    if (argc != 1) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	return 0;
    }
    if ( !(l = fopen(log_nm, "r")) ) {
	Err_Append("Could not open log file.\n");
	return 0;
    }
    while ( (c = fgetc(l)) != EOF && fputc(c, rslt1) != EOF ) {
	continue;
    }
    if ( ferror(l) || ferror(rslt1) ) {
	Err_Append("Error writing log file.  ");
	Err_Append(strerror(errno));
    }
    if ( fclose(l) == EOF ) {
	Err_Append("Could not close log file.\n");
	return 0;
    }
    return 1;
}

static int timeout_cb(int argc, char *argv[])
{
    char *a, *m;
    unsigned to;

    if (argc == 1) {
	if (tmgout) {
	    fprintf(rslt1, "timeout = %d sec %s\n",
		    tmout, tmoadj ? "adjustable" : "fixed");
	} else {
	    fprintf(rslt1, "None\n");
	}
	return 1;
    } else if (argc == 2) {
	a = argv[1];
	if (strcmp(a, "none") == 0) {
	    tmgout = 0;
	    return 1;
	} else if (sscanf(a, "%d", &tmout) == 1) {
	    tmgout = 1;
	    return 1;
	} else {
	    Err_Append(cmd1);
	    Err_Append(" expected integer or \"none\" for timeout, got ");
	    Err_Append(a);
	    Err_Append(".  ");
	    return 0;
	}
    } else if (argc == 3) {
	a = argv[1];
	m = argv[2];
	if (sscanf(a, "%d", &to) != 1) {
	    Err_Append(cmd1);
	    Err_Append(" expected integer or \"none\" for timeout, got ");
	    Err_Append(a);
	    Err_Append(".  ");
	    return 0;
	}
	if (strcmp(m, "adjustable") == 0) {
	    tmoadj = 1;
	} else if (strcmp(m, "fixed") == 0) {
	    tmoadj = 0;
	} else {
	    Err_Append(cmd1);
	    Err_Append(" expected \"adjustable\" or \"fixed\" for modifier, got ");
	    Err_Append(m);
	    Err_Append(".  ");
	    return 0;
	}
	tmgout = 1;
	tmout = to;
	return 1;
    } else {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" \"none\"|integer \"adjustable\"|\"fixed\"");
	return 0;
    }

    return 1;
}

static int types_cb(int argc, char *argv[])
{
    int y;

    if (argc != 1) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	return 0;
    }
    for (y = 0; y < SIGMET_NTYPES; y++) {
	fprintf(rslt1, "%s | %s\n",
		Sigmet_DataType_Abbrv(y), Sigmet_DataType_Descr(y));
    }
    return 1;
}

/*
   Open volume file vol_nm, possibly via a pipe.
   Return file handle, or NULL if failure.
   If file is actually a process, pidp gets the child's process id.
   Otherwise, pidp is left alone.
 */
static FILE *vol_open(const char *vol_nm, pid_t *pidp)
{
    FILE *in;		/* Return value */
    pid_t pid = 0;	/* Process identifier, for fork */
    char *sfx;		/* Filename suffix */
    int pfd[2];		/* Pipe for data */

    sfx = strrchr(vol_nm , '.');
    if ( sfx && strcmp(sfx, ".gz") == 0 ) {
	/* If filename ends with ".gz", read from gunzip pipe */
	if ( pipe(pfd) == -1 ) {
	    Err_Append(strerror(errno));
	    Err_Append("\nCould not create pipe for gzip.  ");
	    return NULL;
	}
	pid = fork();
	switch (pid) {
	    case -1:
		Err_Append(strerror(errno));
		Err_Append("\nCould not spawn gzip process.  ");
		return NULL;
	    case 0:
		/* Child process - gzip.  Send child stdout to pipe. */
		if ( close(i_cmd0) == -1 || close(i_cmd1) == -1
			|| fclose(rslt1) == EOF) {
		    fprintf(dlog, "%s: gzip child could not close"
			    " server streams", time_stamp());
		    _exit(EXIT_FAILURE);
		}
		if ( dup2(pfd[1], STDOUT_FILENO) == -1
			|| close(pfd[1]) == -1 ) {
		    fprintf(dlog, "%s: could not set up gzip process",
			    time_stamp());
		    _exit(EXIT_FAILURE);
		}
		if ( dup2(idlog, STDERR_FILENO) == -1 || close(idlog) == -1 ) {
		    _exit(EXIT_FAILURE);
		}
		execlp("gunzip", "gunzip", "-c", vol_nm, (char *)NULL);
		_exit(EXIT_FAILURE);
	    default:
		/* This process.  Read output from gzip. */
		if ( close(pfd[1]) == -1 || !(in = fdopen(pfd[0], "r"))) {
		    Err_Append(strerror(errno));
		    Err_Append("\nCould not read gzip process.  ");
		    return NULL;
		} else {
		    *pidp = pid;
		    return in;
		}
	}
    } else if ( sfx && strcmp(sfx, ".bz2") == 0 ) {
	/* If filename ends with ".bz2", read from bunzip2 pipe */
	if ( pipe(pfd) == -1 ) {
	    Err_Append(strerror(errno));
	    Err_Append("\nCould not create pipe for gzip.  ");
	    return NULL;
	}
	pid = fork();
	switch (pid) {
	    case -1:
		Err_Append(strerror(errno));
		Err_Append("\nCould not spawn gzip process.  ");
		return NULL;
	    case 0:
		/* Child process - gzip.  Send child stdout to pipe. */
		if ( close(i_cmd0) == -1 || close(i_cmd1) == -1
			|| fclose(rslt1) == EOF) {
		    fprintf(dlog, "%s: gzip child could not close"
			    " server streams", time_stamp());
		    _exit(EXIT_FAILURE);
		}
		if ( dup2(pfd[1], STDOUT_FILENO) == -1
			|| close(pfd[1]) == -1 ) {
		    fprintf(dlog, "%s: could not set up gzip process",
			    time_stamp());
		    _exit(EXIT_FAILURE);
		}
		if ( dup2(idlog, STDERR_FILENO) == -1 || close(idlog) == -1 ) {
		    _exit(EXIT_FAILURE);
		}
		execlp("bunzip2", "bunzip2", "-c", vol_nm, (char *)NULL);
		_exit(EXIT_FAILURE);
	    default:
		/* This process.  Read output from gzip. */
		if ( close(pfd[1]) == -1 || !(in = fdopen(pfd[0], "r"))) {
		    Err_Append(strerror(errno));
		    Err_Append("\nCould not read gzip process.  ");
		    return NULL;
		} else {
		    *pidp = pid;
		    return in;
		}
	}
    } else if ( !(in = fopen(vol_nm, "r")) ) {
	/* Uncompressed file */
	Err_Append("Could not open ");
	Err_Append(vol_nm);
	Err_Append(" for input.\n");
	return NULL;
    }
    return in;
}

static int good_cb(int argc, char *argv[])
{
    FILE *in;
    int rslt;
    pid_t pid = -1;

    if ( argc != 2 ) {
	return 0;
    }
    if ( !(in = vol_open(argv[1], &pid)) ) {
	return 0;
    }
    rslt = Sigmet_GoodVol(in);
    if ( pid != -1 ) {
	kill(pid, SIGKILL);
    }
    fclose(in);
    return rslt;
}

static int hread_cb(int argc, char *argv[])
{
    return 1;
}

static int read_cb(int argc, char *argv[])
{
    struct stat sbuf;		/* Information about file to read */
    int i;			/* Index into vols */
    char *vol_nm;		/* Sigmet raw file */
    FILE *in;			/* Stream from Sigmet raw file */
    pid_t pid;			/* Decompressing child */

    if ( argc == 2 ) {
	vol_nm = argv[1];
    } else {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" sigmet_volume");
	return 0;
    }

    /* Check if volume from this file already loaded */
    if ( stat(vol_nm, &sbuf) == -1 ) {
	Err_Append("Could not get information about volume file ");
	Err_Append(vol_nm);
	Err_Append("\n");
	Err_Append(strerror(errno));
	return 0;
    }
    for (i = 0; i < n_vols; i++) {
	if ( vols[i].v
		&& vols[i].st_dev == sbuf.st_dev
		&& vols[i].st_ino == sbuf.st_ino) {
	    break;
	}
    }
    if ( i == n_vols ) {
	/* Volume is not loaded. Try to find a slot in which to load it. */
	for (i = 0; i < n_vols; i++) {
	    if ( vols[i].users <= 0 ) {
		Sigmet_FreeVol(&vols[i].vol);
		vols[i].v = 0;
		vols[i].st_dev = 0;
		vols[i].st_ino = 0;
		vols[i].users = 0;
		break;
	    }
	}
	if ( i == n_vols ) {
	    Err_Append("Could not find a slot while attempting to (re)load ");
	    Err_Append(vol_nm);
	    Err_Append(". ");
	    return 0;
	}
    } else if ( i >= 0 && vols[i].vol.truncated ) {
	/* Volume truncated. Will attempt reload. Preserve vols[i]. */
	Sigmet_FreeVol(&vols[i].vol);
    } else {
	/* Volume is loaded and complete, return. */
	vols[i].users++;
	fprintf(rslt1, "%s loaded.\n", vol_nm);
	return 1;
    }

    /* Read volume */
    pid = -1;
    if ( !(in = vol_open(vol_nm, &pid)) ) {
	Err_Append("Could not open ");
	Err_Append(vol_nm);
	Err_Append(" for input. ");
	return 0;
    }
    /* Read volume */
    if ( !Sigmet_ReadVol(in, &vols[i].vol) ) {
	Err_Append("Could not read volume from ");
	Err_Append(vol_nm);
	Err_Append(".\n");
	fclose(in);
	if (pid != -1) {
	    kill(pid, SIGKILL);
	}

	/* Volume is broken. Deny it to clients. */
	vols[i].v = 0;
	vols[i].st_dev = 0;
	vols[i].st_ino = 0;
	vols[i].users = 0;
	return 0;
    }
    fclose(in);
    if (pid != -1) {
	kill(pid, SIGKILL);
    }
    vols[i].v = 1;
    vols[i].st_dev = sbuf.st_dev;
    vols[i].st_ino = sbuf.st_ino;
    vols[i].users++;
    fprintf(rslt1, "%s loaded%s.\n",
	    vol_nm, vols[i].vol.truncated ? " (truncated)" : "");
    return 1;
}

/*
   This function returns the index from vols of the volume obtained
   from Sigmet raw product file vol_nm, or -1 is the volume is not
   loaded. Note: this function will return a non-negative offset to
   a loaded truncated volume.
 */
static int get_vol_i(char *vol_nm)
{
    struct stat sbuf;		/* Information about file to read */
    int i;			/* Index into vols */

    if ( stat(vol_nm, &sbuf) == -1 ) {
	Err_Append("Could not get information about volume file ");
	Err_Append(vol_nm);
	Err_Append("\n");
	Err_Append(strerror(errno));
	return 0;
    }
    for (i = 0; i < n_vols; i++) {
	if ( vols[i].v
		&& vols[i].st_dev == sbuf.st_dev
		&& vols[i].st_ino == sbuf.st_ino) {
	    return i;
	}
    }
    return -1;
}

static int release_cb(int argc, char *argv[])
{
    char *vol_nm;
    int i;

    if ( argc != 2 ) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" sigmet_volume");
	return 0;
    }
    vol_nm = argv[1];
    i = get_vol_i(vol_nm);
    if ( i == -1 ) {
	Err_Append(vol_nm);
	Err_Append(" not loaded or was unloaded due to being truncated."
		" Please (re)load with read command. ");
	return 0;
    }
    if ( vols[i].users > 0 ) {
	vols[i].users--;
    }
    return 1;
}

static int volume_headers_cb(int argc, char *argv[])
{
    char *vol_nm;
    int i;

    if ( argc != 2 ) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" sigmet_volume");
	return 0;
    }
    vol_nm = argv[1];
    i = get_vol_i(vol_nm);
    if ( i == -1 ) {
	Err_Append(vol_nm);
	Err_Append(" not loaded or was unloaded due to being truncated."
		" Please (re)load with read command. ");
	return 0;
    }
    Sigmet_PrintHdr(rslt1, vols[i].vol);
    return 1;
}

static int ray_headers_cb(int argc, char *argv[])
{
    char *vol_nm;
    int i;
    struct Sigmet_Vol vol;
    int s, r;

    if ( argc != 2 ) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" sigmet_volume");
	return 0;
    }
    vol_nm = argv[1];
    i = get_vol_i(vol_nm);
    if ( i == -1 ) {
	Err_Append(vol_nm);
	Err_Append(" not loaded or was unloaded due to being truncated."
		" Please (re)load with read command. ");
	return 0;
    }
    vol = vols[i].vol;
    for (s = 0; s < vol.ih.tc.tni.num_sweeps; s++) {
	for (r = 0; r < (int)vol.ih.ic.num_rays; r++) {
	    int yr, mon, da, hr, min;
	    double sec;

	    if ( !vol.ray_ok[s][r] ) {
		continue;
	    }
	    fprintf(rslt1, "sweep %3d ray %4d | ", s, r);
	    if ( !Tm_JulToCal(vol.ray_time[s][r],
			&yr, &mon, &da, &hr, &min, &sec) ) {
		Err_Append("Bad ray time.  ");
		return 0;
	    }
	    fprintf(rslt1, "%04d/%02d/%02d %02d:%02d:%04.1f | ",
		    yr, mon, da, hr, min, sec);
	    fprintf(rslt1, "az %7.3f %7.3f | ",
		    vol.ray_az0[s][r] * DEG_PER_RAD,
		    vol.ray_az1[s][r] * DEG_PER_RAD);
	    fprintf(rslt1, "tilt %6.3f %6.3f\n",
		    vol.ray_tilt0[s][r] * DEG_PER_RAD,
		    vol.ray_tilt1[s][r] * DEG_PER_RAD);
	}
    }
    return 1;
}

static int data_cb(int argc, char *argv[])
{
    char *vol_nm;
    int i;
    struct Sigmet_Vol vol;
    int s, y, r, b;
    char *abbrv;
    float d;
    enum Sigmet_DataType type;
    int all = -1;

    /*
       Identify input and desired output
       Possible forms:
	   data	vol_nm		(argc = 2)
	   data y vol_nm	(argc = 3)
	   data y s vol_nm	(argc = 4)
	   data y s r vol_nm	(argc = 5)
	   data y s r b vol_nm	(argc = 6)
     */

    y = s = r = b = all;
    type = DB_ERROR;
    if (argc > 2 && (type = Sigmet_DataType(argv[1])) == DB_ERROR) {
	Err_Append("No data type named ");
	Err_Append(argv[1]);
	Err_Append(".  ");
	return 0;
    }
    if (argc > 3 && sscanf(argv[2], "%d", &s) != 1) {
	Err_Append("Sweep index must be an integer.  ");
	return 0;
    }
    if (argc > 4 && sscanf(argv[3], "%d", &r) != 1) {
	Err_Append("Ray index must be an integer.  ");
	return 0;
    }
    if (argc > 5 && sscanf(argv[4], "%d", &b) != 1) {
	Err_Append("Bin index must be an integer.  ");
	return 0;
    }
    if (argc > 6) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" [type] [sweep] [ray] [bin] sigmet_volume");
	return 0;
    }
    vol_nm = argv[argc - 1];
    i = get_vol_i(vol_nm);
    if ( i == -1 ) {
	Err_Append(vol_nm);
	Err_Append(" not loaded or was unloaded due to being truncated."
		" Please (re)load with read command. ");
	return 0;
    }
    vol = vols[i].vol;

    if (type != DB_ERROR) {
	/*
	   User has specified a data type.  Search for it in the volume,
	   and set y to the specified type (instead of all).
	 */
	abbrv = Sigmet_DataType_Abbrv(type);
	for (y = 0; y < vol.num_types; y++) {
	    if (type == vol.types[y]) {
		break;
	    }
	}
	if (y == vol.num_types) {
	    Err_Append("Data type ");
	    Err_Append(abbrv);
	    Err_Append(" not in volume.\n");
	    return 0;
	}
    }
    if (s != all && s >= vol.ih.ic.num_sweeps) {
	Err_Append("Sweep index greater than number of sweeps.  ");
	return 0;
    }
    if (r != all && r >= (int)vol.ih.ic.num_rays) {
	Err_Append("Ray index greater than number of rays.  ");
	return 0;
    }
    if (b != all && b >= vol.ih.tc.tri.num_bins_out) {
	Err_Append("Ray index greater than number of rays.  ");
	return 0;
    }

    /* Write */
    if (y == all && s == all && r == all && b == all) {
	for (y = 0; y < vol.num_types; y++) {
	    type = vol.types[y];
	    abbrv = Sigmet_DataType_Abbrv(type);
	    for (s = 0; s < vol.ih.ic.num_sweeps; s++) {
		fprintf(rslt1, "%s. sweep %d\n", abbrv, s);
		for (r = 0; r < (int)vol.ih.ic.num_rays; r++) {
		    if ( !vol.ray_ok[s][r] ) {
			continue;
		    }
		    fprintf(rslt1, "ray %d: ", r);
		    for (b = 0; b < vol.ray_num_bins[s][r]; b++) {
			d = Sigmet_DataType_ItoF(type, vol, vol.dat[y][s][r][b]);
			if (Sigmet_IsData(d)) {
			    fprintf(rslt1, "%f ", d);
			} else {
			    fprintf(rslt1, "nodat ");
			}
		    }
		    fprintf(rslt1, "\n");
		}
	    }
	}
    } else if (s == all && r == all && b == all) {
	for (s = 0; s < vol.ih.ic.num_sweeps; s++) {
	    fprintf(rslt1, "%s. sweep %d\n", abbrv, s);
	    for (r = 0; r < vol.ih.ic.num_rays; r++) {
		    if ( !vol.ray_ok[s][r] ) {
			continue;
		    }
		fprintf(rslt1, "ray %d: ", r);
		for (b = 0; b < vol.ray_num_bins[s][r]; b++) {
		    d = Sigmet_DataType_ItoF(type, vol, vol.dat[y][s][r][b]);
		    if (Sigmet_IsData(d)) {
			fprintf(rslt1, "%f ", d);
		    } else {
			fprintf(rslt1, "nodat ");
		    }
		}
		fprintf(rslt1, "\n");
	    }
	}
    } else if (r == all && b == all) {
	fprintf(rslt1, "%s. sweep %d\n", abbrv, s);
	for (r = 0; r < vol.ih.ic.num_rays; r++) {
	    if ( !vol.ray_ok[s][r] ) {
		continue;
	    }
	    fprintf(rslt1, "ray %d: ", r);
	    for (b = 0; b < vol.ray_num_bins[s][r]; b++) {
		d = Sigmet_DataType_ItoF(type, vol, vol.dat[y][s][r][b]);
		if (Sigmet_IsData(d)) {
		    fprintf(rslt1, "%f ", d);
		} else {
		    fprintf(rslt1, "nodat ");
		}
	    }
	    fprintf(rslt1, "\n");
	}
    } else if (b == all) {
	if (vol.ray_ok[s][r]) {
	    fprintf(rslt1, "%s. sweep %d, ray %d: ", abbrv, s, r);
	    for (b = 0; b < vol.ray_num_bins[s][r]; b++) {
		d = Sigmet_DataType_ItoF(type, vol, vol.dat[y][s][r][b]);
		if (Sigmet_IsData(d)) {
		    fprintf(rslt1, "%f ", d);
		} else {
		    fprintf(rslt1, "nodat ");
		}
	    }
	    fprintf(rslt1, "\n");
	}
    } else {
	if (vol.ray_ok[s][r]) {
	    fprintf(rslt1, "%s. sweep %d, ray %d, bin %d: ", abbrv, s, r, b);
	    d = Sigmet_DataType_ItoF(type, vol, vol.dat[y][s][r][b]);
	    if (Sigmet_IsData(d)) {
		fprintf(rslt1, "%f ", d);
	    } else {
		fprintf(rslt1, "nodat ");
	    }
	    fprintf(rslt1, "\n");
	}
    }
    return 1;
}

static int bin_outline_cb(int argc, char *argv[])
{
    struct Sigmet_Vol vol;
    char *s_s, *r_s, *b_s;
    int s, r, b;
    double corners[8];
    double c;

    if (argc != 4) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" sweep ray bin");
	return 0;
    }
    s_s = argv[1];
    r_s = argv[2];
    b_s = argv[3];

    if (sscanf(s_s, "%d", &s) != 1) {
	Err_Append("Sweep index must be an integer.  ");
	return 0;
    }
    if (sscanf(r_s, "%d", &r) != 1) {
	Err_Append("Ray index must be an integer.  ");
	return 0;
    }
    if (sscanf(b_s, "%d", &b) != 1) {
	Err_Append("Bin index must be an integer.  ");
	return 0;
    }
    if (s >= vol.ih.ic.num_sweeps) {
	Err_Append("Sweep index greater than number of sweeps.  ");
	return 0;
    }
    if (r >= vol.ih.ic.num_rays) {
	Err_Append("Ray index greater than number of rays.  ");
	return 0;
    }
    if (b >= vol.ih.tc.tri.num_bins_out) {
	Err_Append("Ray index greater than number of rays.  ");
	return 0;
    }
    if ( !Sigmet_BinOutl(&vol, s, r, b, corners) ) {
	Err_Append("Could not compute bin outlines.  ");
	return 0;
    }
    Sigmet_FreeVol(&vol);
    c = (use_deg ? DEG_RAD : 1.0);
    fprintf(rslt1, "%f %f %f %f %f %f %f %f\n",
	    corners[0] * c, corners[1] * c, corners[2] * c, corners[3] * c,
	    corners[4] * c, corners[5] * c, corners[6] * c, corners[7] * c);

    return 1;
}

/* Usage: sigmet_raw bintvls type s bounds raw_vol */
static int bintvls_cb(int argc, char *argv[])
{
    struct Sigmet_Vol vol;
    char *s_s;
    int s, y, r, b;
    char *abbrv;
    double d;
    enum Sigmet_DataType type_t;

    if (argc != 4) {
	Err_Append("Usage: ");
	Err_Append(cmd1);
	Err_Append(" type sweep bounds");
	return 0;
    }
    abbrv = argv[1];
    if ((type_t = Sigmet_DataType(abbrv)) == DB_ERROR) {
	Err_Append("No data type named ");
	Err_Append(abbrv);
	Err_Append(".  ");
    }
    s_s = argv[2];
    if (sscanf(s_s, "%d", &s) != 1) {
	Err_Append("Sweep index must be an integer.  ");
	return 0;
    }

    for (y = 0; y < vol.num_types; y++) {
	if (type_t == vol.types[y]) {
	    break;
	}
    }
    if (y == vol.num_types) {
	Err_Append("Data type ");
	Err_Append(abbrv);
	Err_Append(" not in volume.\n");
	return 0;
    }

    if (s >= vol.ih.ic.num_sweeps) {
	Err_Append("Sweep index greater than number of sweeps.  ");
	return 0;
    }
    if ( !vol.sweep_ok[s] ) {
	Err_Append("Sweep not valid in this volume.  ");
	return 0;
    }

    for (r = 0; r < vol.ih.ic.num_rays; r++) {
	if ( !vol.ray_ok[s][r] ) {
	    continue;
	}
	for (b = 0; b < vol.ray_num_bins[s][r]; b++) {
	    d = Sigmet_DataType_ItoF(type_t, vol, vol.dat[y][s][r][b]);
	}
	fprintf(rslt1, "\n");
    }

    return 1;
}

static int stop_cb(int argc, char *argv[])
{
    char rm[LEN];
    struct sig_vol *sv_p;

    for (sv_p = vols; sv_p < vols + n_vols; sv_p++) {
	Sigmet_FreeVol(&sv_p->vol);
    }
    FREE(vols);
    if (snprintf(rm, LEN, "rm -r %s", ddir) < LEN) {
	system(rm);
    } else {
	fprintf(dlog, "%s: could not delete working directory.\n", time_stamp());
    }
    fclose(dlog);
    fprintf(rslt1, "unset SIGMET_RAWD_PID SIGMET_RAWD_DIR SIGMET_RAWD_IN\n");
    exit(EXIT_SUCCESS);
    return 0;
}

/*
   Basic signal management.

   Reference --
       Rochkind, Marc J., "Advanced UNIX Programming, Second Edition",
       2004, Addison-Wesley, Boston.
 */
static int handle_signals(void)
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
    if ( sigaction(SIGCHLD, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }
    if ( sigaction(SIGPIPE, &act, NULL) == -1 ) {
	perror(NULL);
	return 0;
    }

    /* Call special handler for alarm signals */
    act.sa_handler = alarm_handler;
    if ( sigaction(SIGALRM, &act, NULL) == -1 ) {
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
   Assume alarm signals are due to a blocking client. Attempt to kill client.
   POSSIBLE BUG. This assumes client_pid identifies a blocking client, and not
   an unrelated process that replaced the (possibly long dead) client.
 */
static void alarm_handler(int signum)
{
    write(idlog, "Client timed out\n", 17);
    kill(client_pid, SIGTERM);
    tmout *= 2;
}

/* For exit signals, print an error message if possible */
static void handler(int signum)
{
    if ( idlog != -1 ) {
	switch (signum) {
	    case SIGTERM:
		write(idlog, "Exiting on termination signal    \n", 34);
		break;
	    case SIGBUS:
		write(idlog, "Exiting on bus error             \n", 34);
		break;
	    case SIGFPE:
		write(idlog, "Exiting arithmetic exception     \n", 34);
		break;
	    case SIGILL:
		write(idlog, "Exiting illegal instruction      \n", 34);
		break;
	    case SIGSEGV:
		write(idlog, "Exiting invalid memory reference \n", 34);
		break;
	    case SIGSYS:
		write(idlog, "Exiting on bad system call       \n", 34);
		break;
	    case SIGXCPU:
		write(idlog, "Exiting: CPU time limit exceeded \n", 34);
		break;
	    case SIGXFSZ:
		write(idlog, "Exiting: file size limit exceeded\n", 34);
		break;
	}
    }
    _exit(EXIT_FAILURE);
}
