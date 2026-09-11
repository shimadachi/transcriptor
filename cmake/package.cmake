# Install rules and CPack setup.
#
# The goal is one redistributable artifact per platform: a .exe on Windows, a
# .app (in a .dmg) on macOS, a plain binary elsewhere. The web UI is compiled
# in, so nothing but the executable has to ship.

include(GNUInstallDirs)

# Everything we actually want shipped goes in the "app" component, and CPack is
# told to package only that (see CPACK_COMPONENTS_ALL below).
#
# llama.cpp, whisper.cpp and ggml are pulled in with FetchContent, so their own
# install() rules join ours: without this filter the package also carries
# lib/*.a, lib/cmake/, share/ and two dozen headers — llama.h, ggml-*.h,
# whisper.h — none of which a compiled-in binary has any use for. On the CUDA
# build that is libggml-cuda.a alone at ~119 MB. They have no option to turn
# their install rules off, and none of them set a COMPONENT, so everything they
# add lands in "Unspecified" and drops out here.
if(APPLE)
    install(TARGETS transcriptor BUNDLE DESTINATION . COMPONENT app)
else()
    install(TARGETS transcriptor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT app)
endif()

# Desktop integration on Linux/BSD: the launcher and the icon the shell shows
# for it (and, via StartupWMClass, for the running window). The scalable SVG is
# what modern themes prefer; the PNG is there for the panels that still want a
# bitmap. All three land under the install prefix, so they take effect once the
# tarball is unpacked over ~/.local or /usr.
if(NOT WIN32 AND NOT APPLE)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/platform/transcriptor.desktop.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/transcriptor.desktop" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/transcriptor.desktop"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications"
            COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo-mark.svg"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
            RENAME "transcriptor.svg" COMPONENT app)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo-256.png"
            DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps"
            RENAME "transcriptor.png" COMPONENT app)
endif()

# Backends built as shared libraries (a CUDA or Vulkan ggml backend can be one)
# have to travel with the binary.
if(BUILD_SHARED_LIBS)
    install(TARGETS ggml ggml-base
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT app
            OPTIONAL)
endif()

# sherpa-onnx links ONNX Runtime, which is a prebuilt shared library on most
# platforms; copy it next to the executable so the app runs from the build dir.
if(TRANSCRIPTOR_DIARIZE AND TARGET onnxruntime)
    add_custom_command(TARGET transcriptor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:onnxruntime> $<TARGET_FILE_DIR:transcriptor>
        COMMENT "Staging ONNX Runtime next to the executable")
    install(FILES $<TARGET_FILE:onnxruntime>
            DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app OPTIONAL)
endif()

# CUDA builds link cudart/cublas/cublasLt, and those ship with the CUDA
# *toolkit*, not with the driver — the driver only provides libcuda / nvcuda.
# The package used to carry copies of all three so that a machine with nothing
# but a driver could run it. cuBLAS alone is most of a gigabyte, which made the
# CUDA download roughly ten times the size of every other one, for libraries a
# great many of its users already have and the rest can install once.
#
# So the toolkit is a requirement of the CUDA build rather than part of it, and
# both READMEs say so beside the download. Where it is missing the binary does
# not start at all, and says which library it wanted before main() runs:
#   libcudart.so.12: cannot open shared object file: No such file or directory
#   The code execution cannot proceed because cudart64_12.dll was not found
# Anyone who would rather not install it has two builds that need nothing:
# Vulkan runs on the same NVIDIA cards through the driver's own loader, and the
# CPU build runs anywhere.
if(TRANSCRIPTOR_CUDA)
    message(STATUS "  CUDA runtime : not bundled — the toolkit must be "
                   "installed to run this build")
endif()

# Package the "app" component and nothing else; ALL_COMPONENTS_IN_ONE keeps it
# a single flat archive named by CPACK_PACKAGE_FILE_NAME, exactly as before.
set(CPACK_COMPONENTS_ALL app)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_DMG_COMPONENT_INSTALL ON)

set(CPACK_PACKAGE_NAME "transcriptor")
set(CPACK_PACKAGE_VENDOR "Transcriptor")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Transcriptor")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(_pkg_os "windows")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Transcriptor")
    set(_pkg_os "macos")
else()
    set(CPACK_GENERATOR "TGZ")
    set(_pkg_os "linux")
endif()

# CPack's default file name is name-version-system, which every preset for one
# OS shares: the CPU, CUDA and Vulkan builds would all be
# transcriptor-X.Y.Z-Linux.tar.gz. That is fine per build tree and fatal the
# moment they meet as assets on a single release, so spell out the backend and
# the architecture too.
if(TRANSCRIPTOR_CUDA)
    set(_pkg_backend "cuda")
elseif(TRANSCRIPTOR_VULKAN)
    set(_pkg_backend "vulkan")
elseif(TRANSCRIPTOR_METAL)
    set(_pkg_backend "metal")
else()
    set(_pkg_backend "cpu")
endif()

# The mac presets cross-compile, so the target arch is what they were asked for,
# not what the runner happens to be.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_pkg_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(_pkg_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_pkg_arch}" _pkg_arch)
if(_pkg_arch STREQUAL "amd64")
    set(_pkg_arch "x86_64")   # what Windows calls x86_64
endif()

set(CPACK_PACKAGE_FILE_NAME
    "transcriptor-${PROJECT_VERSION}-${_pkg_os}-${_pkg_arch}-${_pkg_backend}")

include(CPack)
