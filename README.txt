"logrun" - A command-line utility for collecting the output of other
command-line programs into manageable files.  Meant to run in POSIX
environments like Linux or the macOS command line.

Sources of more information:
    INSTALL.txt describes installing logrun
    LICENSE describes the license for logrun (BSD-style)
    the install(1) manpage, once installed, describes using logrun

Example usage:

$ logrun make
(This output saved to file: /Users/dilatush/logs/Out_170923_03)
========================================================================
TIME: Sat Sep 23 12:09:40 2017 (PDT)
SHELL COMMAND: make
WORKING DIRECTORY: /Users/dilatush/src/hello
EFFECTIVE USER ID: 12345
========================================================================
cc     hello.c   -o hello

========================================================================
TIME: Sat Sep 23 12:09:40 2017 (PDT)
ELAPSED TIME:  0.074 sec
USER CPU TIME: 0.026 sec
SYS CPU TIME:  0.014 sec
EXIT STATUS: 0
========================================================================
(This output saved to file: /Users/dilatush/logs/Out_170923_03)
