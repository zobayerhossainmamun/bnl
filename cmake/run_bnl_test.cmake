# CMake script invoked by ctest for each .bnl test.
#
# Required vars (passed via -D on the cmake command line):
#   BNL       - path to the bnl executable
#   SCRIPT    - path to the .bnl source file
#   EXPECTED  - path to the .expected file (literal stdout to match)
#
# Compares stdout from `bnl --quiet SCRIPT` against the contents of EXPECTED
# after normalizing line endings. Fails the test on mismatch or non-zero exit.

if(NOT BNL OR NOT SCRIPT OR NOT EXPECTED)
    message(FATAL_ERROR "run_bnl_test.cmake requires -DBNL=, -DSCRIPT=, -DEXPECTED=")
endif()

execute_process(
    COMMAND "${BNL}" --quiet "${SCRIPT}"
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE  err
    RESULT_VARIABLE rc
)

# Normalize line endings + strip trailing whitespace on both sides so CRLF
# files and trailing newlines don't cause spurious failures.
file(READ "${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" actual   "${actual}")
string(REPLACE "\r\n" "\n" expected "${expected}")
string(REGEX REPLACE "[ \t\n]+$" "" actual   "${actual}")
string(REGEX REPLACE "[ \t\n]+$" "" expected "${expected}")

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "bnl exited with code ${rc} for ${SCRIPT}\n"
        "--- stderr ---\n${err}\n"
        "--- stdout ---\n${actual}\n")
endif()

if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR
        "Output mismatch for ${SCRIPT}\n"
        "--- expected ---\n${expected}\n"
        "--- actual ---\n${actual}\n")
endif()
