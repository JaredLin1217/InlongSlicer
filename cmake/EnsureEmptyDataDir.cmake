cmake_minimum_required(VERSION 3.13)

foreach(required_var DATA_DIR ALLOWED_ROOT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_var}")
    endif()
endforeach()

get_filename_component(DATA_DIR "${DATA_DIR}" ABSOLUTE)
get_filename_component(ALLOWED_ROOT "${ALLOWED_ROOT}" ABSOLUTE)
file(TO_CMAKE_PATH "${DATA_DIR}" DATA_DIR)
file(TO_CMAKE_PATH "${ALLOWED_ROOT}" ALLOWED_ROOT)

get_filename_component(data_dir_name "${DATA_DIR}" NAME)
if(NOT data_dir_name STREQUAL "data_dir")
    message(FATAL_ERROR "Refusing to reset a directory not named data_dir: ${DATA_DIR}")
endif()

set(allowed_prefix "${ALLOWED_ROOT}/")
string(FIND "${DATA_DIR}/" "${allowed_prefix}" allowed_prefix_index)
if(NOT allowed_prefix_index EQUAL 0 OR DATA_DIR STREQUAL ALLOWED_ROOT)
    message(FATAL_ERROR "Refusing to reset data_dir outside its allowed root: ${DATA_DIR}")
endif()

file(REMOVE_RECURSE "${DATA_DIR}")
file(MAKE_DIRECTORY "${DATA_DIR}")

if(NOT IS_DIRECTORY "${DATA_DIR}")
    message(FATAL_ERROR "Failed to create empty data_dir: ${DATA_DIR}")
endif()
