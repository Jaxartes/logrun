/* logrun.c - Jeremy Dilatush
 *
 * Copyright (c) 2017 Jeremy Dilatush
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY JEREMY DILATUSH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL JEREMY DILATUSH OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * Logrun - This program runs another program while saving its output to
 * a file.  Sort of like 'script', but without the PTY and with some additional
 * information and output file management.
 *
 * I've written 'logrun' scripts a number of times over the years.  Until now
 * they've been scripts (shell or perl) but this time I'm going to make a C
 * program and hopefully have something I can use wherever I have a compiler.
 * 
 * According to my notes, development on this program started 24 October 2015.
 */

typedef long long ustime_t;

#include "logrun_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#ifdef HAVE_GETRUSAGE
#include <sys/resource.h>
#endif
#include <libgen.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <dirent.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/wait.h>

/* some hard coded values */
static const char *progname = "logrun"; /* program name, for messages */
static const char *dir1 = "LOGRUN_DIR"; /* env variable holding directory */
static const char *dir2 = "logs"; /* under $HOME if that's not specified */
static const char *opfx = "Out_"; /* prefix for output file names */
static const char *bar = "===================================="
                         "====================================";
static const char *shell = "/bin/sh"; /* shell to run commands */
static const int wait_reap = 50000; /* microseconds to wait for children */

/* help message */
static void
usage(void)
{
    fprintf(stderr,
        "Usage: %s [options] command\n"
        "Options:\n"
        "\t-d dir -- place output files in this directory; if not set,\n"
        "\t          this program uses $LOGRUN_DIR, or failing that\n"
        "\t          $HOME/logs/, or failing that the current directory.\n"
        "\t-g -- every 5 minutes print time statistics; -gg for more frequent\n"
        "\t-x -- instead of passing 'command' through the shell (%s),\n"
        "\t      treat it as an executable file name and arguments\n"
        "Version: %s\n",
        progname, shell,
        LOGRUN_VERSION);
#ifdef LOGRUN_SRC_HASH
#ifdef LOGRUN_SRC_HASH_ALGO
    fprintf(stderr,
            "(Source code hash (%s) is %.40s)\n",
            LOGRUN_SRC_HASH_ALGO, LOGRUN_SRC_HASH);
#endif
#endif
    exit(1);
}

/* utility functions */

/* dirok(): Check to see that a given directory exists.  Returns 1 if it
 * does, 0 if it doesn't or can't be used.  If the directory
 * name is NULL or empty string, returns 0.
 */
static int
dirok(const char *path)
{
    struct stat sb;

    if (!path) return(0);
    if (!*path) return(0);
    memset(&sb, 0, sizeof sb);
    if (stat(path, &sb) < 0) {
        perror(path);
        return(0);
    }
    if ((sb.st_mode & S_IFMT) != S_IFDIR) {
        fprintf(stderr, "%s: not a directory\n", path);
        return(0);
    }
    if (access(path, X_OK) < 0) {
        perror(path);
        return(0);
    }
    return(1);
}

/* mkfile(): Pick an unused file name and create that file for writing.
 * Filename takes the following form:
 *      Out_YYMMDD_NN
 * where N is a number that keeps incrementing.  It picks the next value
 * after the highest present for any file currently, including suffixes.
 * On success returns >= 0; on failure < 0.
 */
static int
mkfile(const char *dir, char **path, FILE **fp)
{
    char buf[2048];
    unsigned long n = 1, nf;
    DIR *d;
    struct dirent *e;
    char pfx[32];
    int pfxlen, rv;
    time_t t;
    struct tm *tm;

    /* figure out the prefix of the file names including the current date */
    t = time(NULL);
    tm = localtime(&t);
    if (!tm) {
        perror("error computing current date");
        return(-1);
    }
    pfxlen = snprintf(pfx, sizeof pfx, "%s%02u%02u%02u_",
                      opfx, (unsigned)(tm->tm_year % 100),
                      (unsigned)(tm->tm_mon + 1), (unsigned)(tm->tm_mday));
    if (pfxlen < 0 || pfxlen >= sizeof(pfx)) {
        /* shouldn't happen */
        fprintf(stderr, "unexpected error computing file name\n");
        return(-1);
    }

    /* read entries in the directory, to find the right index number */
    d = opendir(dir);
    if (!d) {
        perror(dir);
        return(-1);
    }
    while ((e = readdir(d)) != NULL) {
        if (!strncmp(pfx, e->d_name, pfxlen)) {
            nf = strtoul(e->d_name + pfxlen, NULL, 10);
            if (nf != ULONG_MAX && nf >= n) {
                n = nf + 1;
            }
        }
    }
    closedir(d);

    /* build a filename out of it */
    rv = snprintf(buf, sizeof buf, "%s/%s%02lu", dir, pfx, n);
    if (rv < 0 || rv >= sizeof(buf)) {
        /* should be very rare */
        fprintf(stderr, "unexpected error computing file name\n");
        return(-1);
    }
    *path = strdup(buf);

    /* Open & create a file of that name.  There shouldn't ever already be one.
     * Except if one got created while we were in the middle of this present
     * function.  That can be prevented with O_EXCL, only it's not the
     * most easily portable thing ever.  Sigh.
     */
#ifdef HAVE_FOPEN_X
    *fp = fopen(*path, "wx");
    if (!*fp) {
        perror(*path);
        return(-1);
    }
#elif defined(HAVE_FDOPEN)
    {
        int fd;
        fd = open(*path, O_WRONLY | O_CREAT | O_EXCL, 0660);
        if (fd < 0) {
            perror(*path);
            return(-1);
        }
        *fp = fdopen(fd, "w");
        if (!*fp) {
            perror(*path);
            close(fd);
            return(-1);
        }
    }
#else /* HAVE_FDOPEN */
#error "need to code an alternative for fopen('x')/fdopen()"
#endif

    return(0);
}

/* demit() - write the same formatted values in two places
 */
void demit(FILE *f1, FILE *f2, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f1, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(f2, fmt, ap);
    va_end(ap);
}

/* ustime() - get time in microseconds (and maybe in seconds too) */
inline static ustime_t ustime(time_t *tsecp)
{
    struct timeval tv;
    ustime_t a;
    if (gettimeofday(&tv, NULL) < 0) {
        /* this should not have failed */
        perror("gettimeofday");
        exit(1);
    }
    a = tv.tv_sec;
    a *= 1000000;
    a += tv.tv_usec;
    if (tsecp) *tsecp = tv.tv_sec;
    return(a);
}

/* time_emit() - emit current time; and resource usage except the first time.
 * Parameters:
 *      f1 - first of two places it should go
 *      f2 - second of two places it should go
 *      first - if this is the first such time update
 *      norusage - if this time update should not include resource usage
 *      eol - character sequence for end of line; use "\n" normally
 */
static void time_emit(FILE *f1, FILE *f2, int first, int norusage, char *eol)
{
#ifdef HAVE_GETRUSAGE
    struct rusage ru;
#endif
    ustime_t t, dt;
    static ustime_t tstart;
    time_t tsec;
    char buf[64];
    struct tm *tm;

    t = ustime(&tsec);
    if (first) tstart = t;
    tm = localtime(&tsec);
    if (strftime(buf, sizeof buf, "%c (%Z)", tm) <= 0) {
        /* should never happen */
        snprintf(buf, sizeof buf, "??? unable to discover time ???");
    }
    demit(f1, f2, "TIME: %s%s", buf, eol);

    if (!first) {
        dt = (t < tstart) ? 0 : (t - tstart);
        demit(f1, f2,
              "ELAPSED TIME:  %u.%03u sec%s",
              (unsigned)(dt / 1000000),
              (unsigned)(((dt % 1000000) + 500) / 1000),
              eol);
        if (!norusage) {
#ifdef HAVE_GETRUSAGE
            /* Get resource usage by this process & its children */
            memset(&ru, 0, sizeof ru);
            if (getrusage(RUSAGE_CHILDREN, &ru) < 0) {
                /* really shouldn't happen */
                demit(f1, f2, "getrusage failed: %s", strerror(errno));
                exit(1);
            }
            demit(f1, f2,
                  "USER CPU TIME: %u.%03u sec%s"
                  "SYS CPU TIME:  %u.%03u sec%s",
                  (unsigned)ru.ru_utime.tv_sec,
                  (unsigned)((ru.ru_utime.tv_usec + 500) / 1000),
                  eol,
                  (unsigned)ru.ru_stime.tv_sec,
                  (unsigned)((ru.ru_stime.tv_usec + 500) / 1000),
                  eol);
#endif /* HAVE_GETRUSAGE */
        }
    }
}

/* spacepaste(): Create a string in buf[] (a buffer, 'buflen' bytes
 * long) by concatenating the 'argc' strings in argv[] with spaces
 * between them.  Returns length on success, or negative on failure.
 */
int spacepaste(char *buf, int buflen, char **argv, int argc)
{
    int pos = 0, a, l;

    for (a = 0; a < argc && argv && argv[a]; ++a) {
        /* put a space before all but the first */
        if (pos > 0) {
            if (pos < buflen) buf[pos] = ' ';
            ++pos;
        }

        /* put in the string */
        l = strlen(argv[a]);
        if (pos + l < buflen) {
            memcpy(buf + pos, argv[a], l);
        }
        pos += l;
    }

    if (pos < buflen) {
        /* success */
        buf[pos] = '\0';
        return(pos);
    } else {
        /* out of space */
        return(-1);
    }
}

/* main program */
int
main(int argc, char **argv)
{
    int execit = 0; /* -x option to bypass shell */
    int doclock = 0; /* -k for time updates every 5 minutes */
    int oc, i;
    char *dir = NULL, *path = NULL;
    char buf[4096];
    FILE *fp, *o;
    int xstatus = 0, xstatus1 = 0, xstatus2 = 0;
    pid_t child, reaped;
    int pout[2]; /* child's stdout in [1], parent's end in [0] */
    int perr[2]; /* child's stderr in [1], parent's end in [0] */
    int waited;
    fd_set rfds;
    ustime_t tclocklast, tnow, dt;
    struct timeval tvto;

    tvto.tv_sec = tvto.tv_usec = 0;

    /* parse command line options */
    if (argc > 0) progname = strdup(basename(argv[0]));
    while ((oc = getopt(argc, argv,
#ifdef USE_GETOPT_PLUS
                        "+" /* stop option parsing with the first non-option */
#endif
                        "d:xg")) >= 0) {
        switch (oc) {
        case 'd': dir = optarg; break;
        case 'g': doclock++; break;
        case 'x': execit = 1; break;
        default: case '?': usage();
        }
    }
    if (optind >= argc) usage();

    /* figure out where to put the output file */
    if (!dirok(dir)) {
        dir = getenv(dir1);
    }
    if (!dirok(dir)) {
        dir = getenv("HOME");
        if (dir) {
            snprintf(buf, sizeof buf, "%s/%s", dir, dir2);
            dir = strdup(buf);
        }
    }
    if (!dirok(dir)) {
        dir = ".";
    }

    /* and figure out the actual file name & create it */
    if (mkfile(dir, &path, &fp) < 0) exit(2);

    /* write initial "header" information */
    fprintf(stderr, "(This output saved to file: %s)\n", path);
    demit(stderr, fp, "%s\n", bar);
    time_emit(stderr, fp, 1, 0, "\n");
    if (execit) {
        demit(stderr, fp, "EXECUTABLE: %s\n", argv[optind]);
        demit(stderr, fp, "COMMAND LINE:");
        for (i = optind; i < argc; ++i) {
            demit(stderr, fp, " %s", argv[i]);
        }
        demit(stderr, fp, "\nCOMMAND LINE (QUOTED):");
        for (i = optind; i < argc; ++i) {
            demit(stderr, fp, " \"%s\"", argv[i]);
        }
        demit(stderr, fp, "\n");
    } else {
        demit(stderr, fp, "SHELL COMMAND:");
        for (i = optind; i < argc; ++i) {
            demit(stderr, fp, " %s", argv[i]);
        }
        demit(stderr, fp, "\n");
    }
    if (!getcwd(buf, sizeof buf)) {
        snprintf(buf, sizeof buf, "Unable to find out: %s",
                 strerror(errno));
    }
    demit(stderr, fp,
          "WORKING DIRECTORY: %s\n"
          "EFFECTIVE USER ID: %u\n%s\n", buf, (unsigned)geteuid(), bar);
    fflush(fp);

    /* open pipes for the command's stdout and stderr */
    pout[0] = pout[1] = perr[0] = perr[1] = -1;
    if ((i = pipe(pout) < 0) && (pout[0] < 0 || pout[1] < 0)) {
        demit(stderr, fp, "ERROR: stdout pipe creation failed: %s\n",
              (i >= 0) ? strerror(errno) : "unknown reason");
        fclose(fp);
        exit(1);
    }
    if ((i = pipe(perr) < 0) && (perr[0] < 0 || perr[1] < 0)) {
        demit(stderr, fp, "ERROR: stderr pipe creation failed: %s\n",
              (i >= 0) ? strerror(errno) : "unknown reason");
        fclose(fp);
        exit(1);
    }

    /* if running command in a shell: format it into a string */
    if (spacepaste(buf, sizeof buf, argv + optind, argc - optind) < 0) {
        demit(stderr, fp, "ERROR: command too long for buffer\n");
        fclose(fp);
        exit(1);
    }

    /* in case we're doing 'clock' updates, prepare for them */
    tclocklast = ustime(NULL);

    /* fork a child process in which to run the command */
    child = fork();
    if (child < 0) {
        /* should be uncommon */
        demit(stderr, fp, "fork failed: %s\n", strerror(errno));
        exit(1);
    } else if (child == 0) {
        /* child process */
        /* hook up the pipes */
        close(pout[0]); /* parent side of stdout pipe */
        close(perr[0]); /* parent side of stderr pipe */
        dup2(pout[1], STDOUT_FILENO);
        dup2(perr[1], STDERR_FILENO);
        close(pout[1]); /* don't know it by two file descriptors, just one */
        close(perr[1]); /* ditto */

        /* run the command */
        if (execit) {
            /* run the command directly; execp() will find the command
             * in $PATH
             */
            execvp(argv[optind], argv + optind);
            fprintf(stderr, "execvp(%s) failed: %s\n",
                    argv[optind], strerror(errno));
        } else {
            /* build a shell command line string and run the shell on it */
            execl(shell, shell, "-c", buf, NULL);
            fprintf(stderr, "execl(%s -c '%s') failed: %s\n",
                    shell, buf, strerror(errno));
        }
        _exit(1);
    } else {
        /* parent process */
        close(pout[1]); /* child side of stdout pipe */
        close(perr[1]); /* child side of stderr pipe */
    }

    /* wait for the command to exit; collecting its stdout and stderr
     * and copying them to both our own stdout/stderr, and the file.
     */
    for (;;) {
        fflush(fp);
        if (doclock) {
            /* figure out if it's time for a "clock" message or how long
             * to wait until it is
             */
            tnow = ustime(NULL);
            if (tclocklast > tnow) {
                /* time went backwards */
                tclocklast = tnow;
            }
            switch (doclock) {
                case 1: dt = 300; break; /* 5 min */
                case 2: dt = 60; break; /* 1 min */
                default:dt = 20; break; /* 20 sec */
            }
            dt = dt * 1000000 + tclocklast - tnow;
            if (dt <= 0) {
                /* Time for a clock message.  End of line is marked with
                 * "\r\n" instead of the more usual "\n" because some
                 * programs change the terminal settings in ways that
                 * cause "\n" alone not to work right.
                 */
                tclocklast = tnow;
                demit(stderr, fp, "\r\n%s\r\n", bar);
                time_emit(stderr, fp, 0, 1, "\r\n");
                demit(stderr, fp, "%s\r\n", bar);
                continue;
            }

            /* not time yet */
            tvto.tv_sec = dt / 1000000;
            tvto.tv_usec = dt % 1000000;
        }

        /* use select() to find out what happens */
        FD_ZERO(&rfds);
        if (pout[0] >= 0) { FD_SET(pout[0], &rfds); }
        if (perr[0] >= 0) { FD_SET(perr[0], &rfds); }
        i = select(((pout[0] > perr[0]) ? pout[0] : perr[0]) + 1,
                   &rfds, NULL, NULL,
                   doclock ? &tvto : NULL);
        if (i < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                /* These are temporary problems not real errors. */
            } else {
                /* This really shouldn't happen */
                demit(stderr, fp, "select() failed: %s\r\n", strerror(errno));
            }
            /* Put in a little delay and try again */
            usleep(250000); /* 1/4 second */
            continue;
        } else if (i > 0) {
            /* There's something to do */
            if (pout[0] >= 0 && FD_ISSET(pout[0], &rfds)) {
                /* Read from the child's stdout */
                i = read(pout[0], buf, sizeof buf);
                o = stdout;
            } else if (perr[0] >= 0 && FD_ISSET(perr[0], &rfds)) {
                /* Read from the child's stderr */
                i = read(perr[0], buf, sizeof buf);
                o = stderr;
            }
            if (i < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    /* shouldn't have happened, but these are transient errors
                     * and we shouldn't stop for them.  Put in a little delay
                     * and try again.
                     */
                    usleep(250000); /* 1/4 second */
                    continue;
                } else {
                    /* this shouldn't have happened */
                    i = snprintf(buf, sizeof buf, "read failed: %s\r\n",
                                 strerror(errno));
                    o = stderr;
                    usleep(250000); /* 1/4 second */
                }
            }
            if (i == 0) {
                /* end of file! */
                if (o == stderr) {
                    close(perr[0]);
                    perr[0] = -1;
                } else if (o == stdout) {
                    close(pout[0]);
                    pout[0] = -1;
                }
            }
            if (i > 0) {
                /* Got something, in the buffer!  Pass it along. */
                fwrite(buf, 1, i, o);
                fwrite(buf, 1, i, fp);
                if (o == stdout) fflush(stdout);
            }
        }
        if (perr[0] < 0 && pout[0] < 0) {
            /* The output from our child has been closed; see if it has exited.
             */
            xstatus = 0;
            waited = 0;
            for (;;) {
                xstatus1 = 0;
                reaped = waitpid(-1, &xstatus1, WNOHANG);
                if (reaped == child) {
                    xstatus = xstatus1;
                }
                if (reaped < 0) {
                    if (waited) {
                        /* don't wait any more */
                        break;
                    } else {
                        /* see if waiting brings us more */
                        usleep(wait_reap);
                        waited = 1;
                    }
                } else {
                    /* we got some, see if there are any more */
                    waited = 0;
                }
            }
            break;
        }
    }

    /* With command done, print final information.
     * If you want it to accurately detect signals/coredumps, include the
     * "-x" option to get the shell out of the way.
     */
    demit(stderr, fp, "\n%s\n", bar);
    time_emit(stderr, fp, 0, 0, "\n");
    if (WIFEXITED(xstatus)) {
        demit(stderr, fp, "EXIT STATUS: %u\n", (unsigned)WEXITSTATUS(xstatus));
        xstatus2 = WEXITSTATUS(xstatus);
    } else if (WIFSIGNALED(xstatus)) {
        unsigned sig = WTERMSIG(xstatus);
        demit(stderr, fp, "EXIT SIGNAL: %s%s\n",
              strsignal(sig),
              WCOREDUMP(xstatus) ? " (core dumped)" : "");
        xstatus2 = 1;
    } else {
        /* shouldn't happen */
        demit(stderr, fp, "EXIT STATUS UNKNOWN?\n");
        xstatus2 = 1;
    }
    demit(stderr, fp, "%s\n", bar);
    fclose(fp);
    fprintf(stderr, "(This output saved to file: %s)\n", path);

    /* and exit */
    return(xstatus2);
}
