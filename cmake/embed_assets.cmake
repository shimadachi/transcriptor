# Compile the contents of a directory into a static library of byte arrays, so
# the shipped binary carries its own web UI and needs no data directory.
#
#   transcriptor_embed_assets(TARGET_NAME <lib> ROOT <dir> OUTPUT <generated.cpp>)
#
# Exposes, in C++:  const transcriptor::Asset* transcriptor::find_asset(std::string_view path);
# where `path` is the file's path relative to ROOT, e.g. "index.html".

function(transcriptor_embed_assets)
    cmake_parse_arguments(A "" "TARGET_NAME;ROOT;OUTPUT" "" ${ARGN})

    file(GLOB_RECURSE _assets RELATIVE "${A_ROOT}" "${A_ROOT}/*")
    list(FILTER _assets EXCLUDE REGEX "(^|/)\\.")   # drop dotfiles

    set(_deps "")
    foreach(_f IN LISTS _assets)
        list(APPEND _deps "${A_ROOT}/${_f}")
    endforeach()

    get_filename_component(_outdir "${A_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${_outdir}")

    add_custom_command(
        OUTPUT  "${A_OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
                -DASSET_ROOT=${A_ROOT}
                -DASSET_OUT=${A_OUTPUT}
                -P "${CMAKE_CURRENT_LIST_DIR}/cmake/embed_assets_run.cmake"
        DEPENDS ${_deps} "${CMAKE_CURRENT_LIST_DIR}/cmake/embed_assets_run.cmake"
        COMMENT "Embedding web assets from ${A_ROOT}"
        VERBATIM)

    add_library(${A_TARGET_NAME} STATIC "${A_OUTPUT}")
    target_include_directories(${A_TARGET_NAME} PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/src")
    set_target_properties(${A_TARGET_NAME} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endfunction()
