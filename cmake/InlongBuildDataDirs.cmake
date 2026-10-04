# Keep portable runtime settings disposable, including on no-op incremental builds.
# A POST_BUILD command would run only when the executable or DLL is relinked.
if (CMAKE_CONFIGURATION_TYPES)
    set(_inlong_data_dir_multi_config TRUE)
    set(_inlong_data_dir_binary "${CMAKE_BINARY_DIR}/src/$<CONFIG>")
else ()
    set(_inlong_data_dir_multi_config FALSE)
    set(_inlong_data_dir_binary "${CMAKE_BINARY_DIR}/src")
endif ()
set(_inlong_data_dir_script "${CMAKE_CURRENT_LIST_DIR}/ResetBuildDataDirs.cmake")

add_custom_target(prepare_empty_data_dirs ALL
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_ROOT=${CMAKE_SOURCE_DIR}"
        "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
        "-DSLIC3R_APP_KEY=${SLIC3R_APP_KEY}"
        "-DMULTI_CONFIG=${_inlong_data_dir_multi_config}"
        "-DCONFIG=$<CONFIG>"
        -P "${_inlong_data_dir_script}"
    COMMENT "Resetting portable data_dir folders in local build outputs"
    VERBATIM
)
add_dependencies(InlongSlicer prepare_empty_data_dirs)
if (TARGET InlongSlicer_profile_validator)
    add_dependencies(InlongSlicer_profile_validator prepare_empty_data_dirs)
endif ()
# InlongSlicer_app_gui already depends on InlongSlicer.

# Also cover direct installs and INSTALL builds that skip project dependencies.
# Reset only the three local build outputs, never a custom install prefix.
install(CODE "
    set(SOURCE_ROOT [=[${CMAKE_SOURCE_DIR}]=])
    set(BUILD_ROOT [=[${CMAKE_BINARY_DIR}]=])
    set(SLIC3R_APP_KEY [=[${SLIC3R_APP_KEY}]=])
    set(MULTI_CONFIG ${_inlong_data_dir_multi_config})
    set(CONFIG \"\${CMAKE_INSTALL_CONFIG_NAME}\")
    include([=[${_inlong_data_dir_script}]=])
")
install(DIRECTORY "${_inlong_data_dir_binary}/data_dir" DESTINATION ".")
