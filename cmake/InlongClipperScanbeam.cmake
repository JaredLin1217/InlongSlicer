# Apply a project-owned, exact no-intersection fast path to a build-tree copy.
# The checked-in third-party source remains the reference and is never written.
function(inlong_prepare_clipper_scanbeam source output result)
    file(READ "${source}" engine)
    string(REPLACE "\r\n" "\n" engine "${engine}")
    string(SHA256 engine_sha "${engine}")
    # Reviewed Clipper2 2.0.1 engine with the existing Orca node allocator.
    if(NOT engine_sha STREQUAL "f48b00ee684a3d852fd9b9543f195d23f3c8c9a0563b2280ff1c75928653839a")
        set(${result} FALSE PARENT_SCOPE)
        return()
    endif()
    get_filename_component(source_absolute "${source}" REALPATH)
    get_filename_component(output_absolute "${output}" REALPATH)
    if(source_absolute STREQUAL output_absolute OR IS_SYMLINK "${output}")
        message(FATAL_ERROR "Clipper fast path must not overwrite its reference source")
    endif()

    set(anchor "    AdjustCurrXAndCopyToSEL(top_y);\n")
    string(FIND "${engine}" "${anchor}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Reviewed Clipper scanbeam anchor is missing")
    endif()
    set(fast_path [=[
    // Inlong: a stable merge sort cannot generate an intersection when its
    // current-X keys are already ordered. Keep ties in their original order.
    // DoTopOfScanbeam resets SEL before it is reused for horizontal edges;
    // the next AdjustCurrXAndCopyToSEL also rebuilds every jump pointer.
    bool already_ordered = true;
    for (Active* edge = sel_; edge->next_in_sel; edge = edge->next_in_sel)
      if (edge->next_in_sel->curr_x < edge->curr_x)
      {
        already_ordered = false;
        break;
      }
    if (already_ordered) return false;

]=])
    string(REPLACE "${anchor}" "${anchor}${fast_path}" optimized "${engine}")
    get_filename_component(output_directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_directory}")
    set(existing "")
    if(EXISTS "${output}")
        file(READ "${output}" existing)
    endif()
    if(NOT existing STREQUAL optimized)
        file(WRITE "${output}" "${optimized}")
    endif()
    set(${result} TRUE PARENT_SCOPE)
endfunction()

function(inlong_enable_clipper_scanbeam_fast_path target)
    get_target_property(source_directory ${target} SOURCE_DIR)
    set(original "${source_directory}/Clipper2Lib/src/clipper.engine.cpp")
    set(generated "${CMAKE_BINARY_DIR}/generated/inlong_clipper/clipper.engine.cpp")
    inlong_prepare_clipper_scanbeam("${original}" "${generated}" prepared)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    if(NOT prepared)
        message(STATUS "Clipper scanbeam fast path inactive: engine revision needs review")
        return()
    endif()

    get_target_property(original_sources ${target} SOURCES)
    set(optimized_sources)
    foreach(source IN LISTS original_sources)
        get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${source_directory}")
        if(absolute STREQUAL original)
            list(APPEND optimized_sources "${generated}")
        else()
            list(APPEND optimized_sources "${absolute}")
        endif()
    endforeach()
    set_property(TARGET ${target} PROPERTY SOURCES "${optimized_sources}")
    message(STATUS "Clipper scanbeam fast path enabled for the reviewed engine")
endfunction()
