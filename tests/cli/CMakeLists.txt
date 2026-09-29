# Runs the real inlong-slicer binary, so it needs the built app and resources/, not just build/tests.
# The CI unit-test job only receives build/tests, so the test carries the RequiresApp label that
# scripts/run_unit_tests.sh excludes. Run it with `ctest -C Release -L RequiresApp`. It also exits 77
# (skipped) when the binary is missing.

find_program(INLONG_CLI_TEST_PYTHON NAMES python3)
if (NOT INLONG_CLI_TEST_PYTHON)
    message(STATUS "python3 not found, not registering the CLI tests")
    return()
endif ()

add_test(NAME cli_strict_mode
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_strict.sh $<TARGET_FILE:InlongSlicer> ${INLONG_CLI_TEST_PYTHON})
set_tests_properties(cli_strict_mode PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)
