/*
 * check_getopt_plus.c
 *
 * Check whether to include "+" in getopt()'s 'optstring' argument to
 * prevent make it stop parsing arguments with the first non-option it gets,
 * which is traditional.
 *
 * This program does some trial runs, prints a message, and returns exit
 * status 0 if '+' should be included, nonzero if not.
 * 
 * If something really goes wrong it'll return nonzero and '+' won't be
 * included.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void bogon(char *desc)
{
    fprintf(stderr, "check_getopt_plus: While checking getopt() for '+',\n"
            "got unexpectedly bogus result.  Assuming '+' is not needed.\n"
            "Result details:\n%s\n", desc);
    exit(2);
}

int main(int argc, char **argv)
{
    char *args[10];
    int nargs;
    char *os;

    /* Question 1: What happens without '+'? */
    args[0] = "check_getopt_plus";
    args[1] = "-w";
    args[2] = "x";
    args[3] = "y";
    args[4] = "-z";
    args[5] = NULL;
    nargs = 5;
    os = "w:z";
    if (getopt(nargs, args, os) != 'w') {
        bogon("expected -w option, didn't get it");
    }
    if (optarg == NULL || strcmp(optarg, "x")) {
        bogon("expected -w argument 'x', didn't get it");
    }
    if (getopt(nargs, args, os) < 0 && optind == 3) {
        fprintf(stderr, "check_getopt_plus: determined '+' is not needed\n");
        return(1);
    }

    /* So, without '+' there is trouble.  Now see if '+' fixes it. */
    optind = 0; /* setting optind to 0, not 1, is a special GNU thing */
    args[0] = "check_getopt_plus";
    args[1] = "-r";
    args[2] = "s";
    args[3] = "t";
    args[4] = "-u";
    args[5] = NULL;
    nargs = 5;
    os = "+r:s";
    if (getopt(nargs, args, os) != 'r') {
        bogon("expected -r option, didn't get it");
    }
    if (optarg == NULL || strcmp(optarg, "s")) {
        bogon("expected -r argument 's', didn't get it");
    }
    if (getopt(nargs, args, os) < 0 && optind == 3) {
        fprintf(stderr, "check_getopt_plus: determined '+' is needed\n");
        return(0);
    }

    bogon("neither with or without '+' is getopt() working right");
    return(1);
}
