if(NOT BNL OR NOT SCRIPT OR NOT EXPECTED)
    message(FATAL_ERROR "run_bnl_test.cmake requires -DBNL=, -DSCRIPT=, -DEXPECTED=")
endif()

get_filename_component(SCRIPT_DIR "${SCRIPT}" DIRECTORY)

execute_process(
    COMMAND "${BNL}" --quiet "${SCRIPT}"
    WORKING_DIRECTORY "${SCRIPT_DIR}"
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE  err
    RESULT_VARIABLE rc
)

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
