# CMakeLists.txt
# this file controls "cmake" for doing a build of "logrun"

cmake_minimum_required(VERSION 2.8)
enable_language(C)
project(logrun)
include_directories("${PROJECT_BINARY_DIR}")

## version identification

set(LOGRUN_VERSION "pre-release 1.0")
set(LOGRUN_SRC_HASH_ALGO "SHA1")
file(${LOGRUN_SRC_HASH_ALGO} "${PROJECT_SOURCE_DIR}/logrun.c" LOGRUN_SRC_HASH)
# Could use a stronger hash than SHA1, but since it's only used to help
# identify software variants, it's not really warranted.

# A bigger problem is that LOGRUN_SRC_HASH isn't updated whenever the source
# is modified, only if you remove logrun_config.h.  Oh well.

## Settings that control compatibility

include(CheckFunctionExists)
include(CheckCSourceRuns)

# HAVE_GETRUSAGE: Whether we have getrusage() or not.  We probably do,
# on any reasonable POSIX-ey system, since it appeared in 4.2BSD.

check_function_exists(getrusage HAVE_GETRUSAGE)
#check_prototype_exists(getrusage "sys/resource.h" HAVE_GETRUSAGE)

# HAVE_FOPEN_X: On some systems, fopen() takes an "x" character as a modifier
# to its 'mode' parameter, to add O_EXCL.  This might be of use if you
# don't have fdopen(), but you probably do.  So I'm just leaving this at
# "no."

#set (HAVE_FOPEN_X 1)

# HAVE_FDOPEN: Whether we have fdopen() or not.  We probably do, it
# appeared in POSIX.1.

check_function_exists(fdopen HAVE_FDOPEN)

# USE_GETOPT_PLUS: Traditional getopt() behavior is to stop parsing
# options as soon as the first non-option argument is seen.  But some
# implementations don't do this by default and take a '+' character to
# change it.

# The user often sees a failure message from "cmake" while running this
# check, but that's ok.

set(CMAKE_REQUIRED_INCLUDES "${PROJECT_SOURCE_DIR}")
check_c_source_runs("#include \"check_getopt_plus.c\"" USE_GETOPT_PLUS)

set (HAVE_GETOPT_PLUS 1)

## documentation for logrun
# add_custom_target(logrun.1 ALL)

## build instructions

configure_file (
    "${PROJECT_SOURCE_DIR}/logrun_config.h.in"
    "${PROJECT_BINARY_DIR}/logrun_config.h"
)
add_executable(logrun logrun.c)

## installation instructions

install (TARGETS logrun DESTINATION bin)
install (FILES logrun.1 DESTINATION man/man1)

## tests (very limited)

include(CTest)

# does logrun run
add_test (LogrunRuns logrun -d ${PROJECT_SOURCE_DIR}/Test true)

# does logrun show the output of the command it ran; and if no command
# is given does it show a usage message
add_test(
    NAME LogrunSelf1
    COMMAND logrun -d . ${PROJECT_BINARY_DIR}/logrun
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/Test  
)
set_tests_properties(
    LogrunSelf1 PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage:.* command.*EXIT STATUS: 1"
)

# does logrun correctly tell the difference between options for the
# command being run & for itself?
add_test(
    NAME LogrunOptOpt
    COMMAND logrun -d . echo -n testing
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/Test  
)
set_tests_properties(
    LogrunOptOpt PROPERTIES
    PASS_REGULAR_EXPRESSION "COMMAND:.*n testing"
)

add_test(
    NAME LogrunExit0
    COMMAND logrun -d . ${PROJECT_BINARY_DIR}/logrun -d . :
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/Test  
)
set_tests_properties(
    LogrunExit0 PROPERTIES
    PASS_REGULAR_EXPRESSION "EXIT STATUS: 0"
)

# more complex tests on passing exit status
add_test(
    NAME LogrunExit01
    COMMAND logrun -d . ${PROJECT_BINARY_DIR}/logrun -d . "${PROJECT_BINARY_DIR}/logrun -d . : ; ${PROJECT_BINARY_DIR}/logrun -d ."
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/Test  
)
set_tests_properties(
    LogrunExit01 PROPERTIES
    PASS_REGULAR_EXPRESSION "EXIT STATUS: 0.*EXIT STATUS: 0.*EXIT STATUS: [^0]"
)

add_test(
    NAME LogrunExit10
    COMMAND logrun -d . ${PROJECT_BINARY_DIR}/logrun -d . "${PROJECT_BINARY_DIR}/logrun -d . ; ${PROJECT_BINARY_DIR}/logrun -d . :"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/Test  
)
set_tests_properties(
    LogrunExit10 PROPERTIES
    PASS_REGULAR_EXPRESSION "EXIT STATUS: [^0].*EXIT STATUS: 0.*EXIT STATUS: 0"
)
