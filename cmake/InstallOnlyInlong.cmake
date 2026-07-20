cmake_minimum_required(VERSION 3.13)

foreach(required_var SOURCE_ROOT BUILD_ROOT SLIC3R_APP_KEY OUTPUT_DIR)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_var}")
    endif()
endforeach()

if(NOT DEFINED CONFIG)
    set(CONFIG "")
endif()

file(TO_CMAKE_PATH "${SOURCE_ROOT}" SOURCE_ROOT)
file(TO_CMAKE_PATH "${BUILD_ROOT}" BUILD_ROOT)
file(TO_CMAKE_PATH "${OUTPUT_DIR}" OUTPUT_DIR)

set(expected_output_dir "${BUILD_ROOT}/${SLIC3R_APP_KEY}_InlongOnly")
if(NOT OUTPUT_DIR STREQUAL expected_output_dir)
    message(FATAL_ERROR "Refusing to write unexpected install_only_inlong output: ${OUTPUT_DIR}")
endif()

if(OUTPUT_DIR STREQUAL BUILD_ROOT OR OUTPUT_DIR STREQUAL "${BUILD_ROOT}/${SLIC3R_APP_KEY}")
    message(FATAL_ERROR "Refusing to remove protected build/install directory: ${OUTPUT_DIR}")
endif()

set(binary_dir "${BUILD_ROOT}/src/${CONFIG}")
if(NOT IS_DIRECTORY "${binary_dir}")
    set(binary_dir "${BUILD_ROOT}/src")
endif()
if(NOT IS_DIRECTORY "${binary_dir}")
    message(FATAL_ERROR "Cannot find build binary directory: ${binary_dir}")
endif()

set(resources_dir "${SOURCE_ROOT}/resources")
set(profiles_dir "${resources_dir}/profiles")
if(NOT IS_DIRECTORY "${resources_dir}" OR NOT IS_DIRECTORY "${profiles_dir}")
    message(FATAL_ERROR "Cannot find source resources/profiles directory")
endif()

function(inlong_only_patch_guide_js guide_js)
    if(NOT EXISTS "${guide_js}")
        return()
    endif()

    file(READ "${guide_js}" guide_content)
    set(guide_original "${guide_content}")
    if(guide_content MATCHES "INLONG_ONLY_GUIDE_VENDOR_ORDER")
        return()
    endif()

    string(REPLACE
        "\t// INLONG ensure list correctly ordered\n\tpModel = pModel.sort((a, b)=>(a[\"vendor\"].localeCompare(b[\"vendor\"])))\n\tpModel = [ // move custom printers to top\n\t\t...pModel.filter(i=>i.vendor === \"Custom\"),\n\t\t...pModel.filter(i=>i.vendor !== \"Custom\")\n\t];"
        "\t// INLONG_ONLY_GUIDE_VENDOR_ORDER\n\tconst inlongOnlyVendorOrder = { \"INLONG\": 0, \"_Infinity3DP\": 1, \"Infinity3DP\": 1 }\n\tpModel = pModel\n\t\t.filter(i=>i.vendor !== \"Custom\")\n\t\t.sort((a, b)=>{\n\t\t\tlet orderA = Object.prototype.hasOwnProperty.call(inlongOnlyVendorOrder, a.vendor) ? inlongOnlyVendorOrder[a.vendor] : 99\n\t\t\tlet orderB = Object.prototype.hasOwnProperty.call(inlongOnlyVendorOrder, b.vendor) ? inlongOnlyVendorOrder[b.vendor] : 99\n\t\t\tif (orderA !== orderB)\n\t\t\t\treturn orderA - orderB\n\t\t\treturn a[\"vendor\"].localeCompare(b[\"vendor\"])\n\t\t})"
        guide_content
        "${guide_content}"
    )

    if(guide_content STREQUAL guide_original)
        message(FATAL_ERROR "Failed to patch Inlong-only guide JS: ${guide_js}")
    endif()

    file(WRITE "${guide_js}" "${guide_content}")
endfunction()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

file(GLOB runtime_files
    "${binary_dir}/*.dll"
    "${binary_dir}/*.exe"
)
foreach(runtime_file IN LISTS runtime_files)
    file(COPY "${runtime_file}" DESTINATION "${OUTPUT_DIR}")
endforeach()

set(python_runtime_dir "${binary_dir}/python")
if(NOT EXISTS "${python_runtime_dir}/python.exe")
    message(FATAL_ERROR "Cannot find bundled Python runtime: ${python_runtime_dir}")
endif()
file(COPY "${python_runtime_dir}" DESTINATION "${OUTPUT_DIR}")

if(DEFINED SYSTEM_RUNTIME_LIBS AND NOT "${SYSTEM_RUNTIME_LIBS}" STREQUAL "")
    string(REPLACE "|" ";" system_runtime_libs "${SYSTEM_RUNTIME_LIBS}")
    foreach(system_runtime IN LISTS system_runtime_libs)
        if(EXISTS "${system_runtime}")
            file(COPY "${system_runtime}" DESTINATION "${OUTPUT_DIR}")
        endif()
    endforeach()
endif()

if(EXISTS "${SOURCE_ROOT}/LICENSE.txt")
    file(COPY "${SOURCE_ROOT}/LICENSE.txt" DESTINATION "${OUTPUT_DIR}")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}/resources")
file(GLOB resource_entries RELATIVE "${resources_dir}" "${resources_dir}/*")
foreach(resource_entry IN LISTS resource_entries)
    if(NOT resource_entry STREQUAL "profiles")
        file(COPY "${resources_dir}/${resource_entry}" DESTINATION "${OUTPUT_DIR}/resources")
    endif()
endforeach()

inlong_only_patch_guide_js("${OUTPUT_DIR}/resources/web/guide/21/21.js")
inlong_only_patch_guide_js("${OUTPUT_DIR}/resources/web/guide/24/24.js")

set(output_profiles_dir "${OUTPUT_DIR}/resources/profiles")
file(MAKE_DIRECTORY "${output_profiles_dir}")

set(profile_dirs
    "InlongFilamentLibrary"
    "INLONG"
    "_Infinity3DP"
    "user"
)
foreach(profile_dir IN LISTS profile_dirs)
    if(IS_DIRECTORY "${profiles_dir}/${profile_dir}")
        file(COPY "${profiles_dir}/${profile_dir}" DESTINATION "${output_profiles_dir}")
    endif()
endforeach()

set(profile_files
    "InlongFilamentLibrary.json"
    "INLONG.json"
    "_Infinity3DP.json"
    "blacklist.json"
    "hotend.stl"
)
foreach(profile_file IN LISTS profile_files)
    if(EXISTS "${profiles_dir}/${profile_file}")
        file(COPY "${profiles_dir}/${profile_file}" DESTINATION "${output_profiles_dir}")
    endif()
endforeach()

foreach(required_profile "InlongFilamentLibrary" "INLONG" "_Infinity3DP" "InlongFilamentLibrary.json" "INLONG.json" "_Infinity3DP.json" "blacklist.json" "hotend.stl")
    if(NOT EXISTS "${output_profiles_dir}/${required_profile}")
        message(FATAL_ERROR "install_only_inlong did not copy required profile item: ${required_profile}")
    endif()
endforeach()

if(NOT EXISTS "${OUTPUT_DIR}/python/python.exe")
    message(FATAL_ERROR "install_only_inlong did not copy the bundled Python runtime")
endif()

set(DATA_DIR "${OUTPUT_DIR}/data_dir")
set(ALLOWED_ROOT "${OUTPUT_DIR}")
include("${SOURCE_ROOT}/cmake/EnsureEmptyDataDir.cmake")

file(GLOB copied_profile_entries RELATIVE "${output_profiles_dir}" "${output_profiles_dir}/*")
list(LENGTH copied_profile_entries copied_profile_count)
message(STATUS "Installed Inlong-only runtime to ${OUTPUT_DIR}")
message(STATUS "Copied ${copied_profile_count} profile root entries")
