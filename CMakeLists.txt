cmake_minimum_required(VERSION 3.21)

project(transcriptor
    VERSION 0.1.0
    DESCRIPTION "Offline speech-to-text + speaker diarization + local LLM summary"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TRANSCRIPTOR_DIARIZE   "Speaker diarization via sherpa-onnx" ON)
option(TRANSCRIPTOR_WEBVIEW   "Native window (WebView2 / WKWebView / WebKitGTK)" ON)
option(TRANSCRIPTOR_CUDA      "GPU acceleration via CUDA (Linux/Windows, NVIDIA)" OFF)
option(TRANSCRIPTOR_VULKAN    "GPU acceleration via Vulkan (Linux/Windows, any GPU)" OFF)
option(TRANSCRIPTOR_METAL     "GPU acceleration via Metal (macOS)" ${APPLE})
option(TRANSCRIPTOR_STATIC_RT "Statically link the MSVC runtime (Windows)" ON)

if(MSVC AND TRANSCRIPTOR_STATIC_RT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/dependencies.cmake)
include(cmake/embed_assets.cmake)

# ---------------------------------------------------------------------------
# Embedded web UI  (web/ -> a generated .cpp, so the binary needs no data dir)
# ---------------------------------------------------------------------------
transcriptor_embed_assets(
    TARGET_NAME transcriptor_assets
    ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/web"
    OUTPUT      "${CMAKE_CURRENT_BINARY_DIR}/generated/assets_generated.cpp")

# ---------------------------------------------------------------------------
# The application
# ---------------------------------------------------------------------------
set(TRANSCRIPTOR_SOURCES
    src/main.cpp
    src/config.cpp
    src/device.cpp
    src/util/paths.cpp
    src/util/export.cpp
    src/util/net.cpp
    src/util/models.cpp
    src/audio/sources.cpp
    src/audio/capture.cpp
    src/audio/recorder.cpp
    src/audio/decode.cpp
    src/audio/miniaudio_impl.c
    src/stt/whisper_stt.cpp
    src/diarize/diarizer.cpp
    src/llm/templates.cpp
    src/llm/summarizer.cpp
    src/llm/openai_backend.cpp
    src/llm/llama_backend.cpp
    src/pipeline/processor.cpp
    src/app/state.cpp
    src/app/server.cpp
    src/app/shell.cpp)

if(WIN32)
    # GUI subsystem: no console window behind the app.
    add_executable(transcriptor WIN32 ${TRANSCRIPTOR_SOURCES})
    target_sources(transcriptor PRIVATE src/platform/app.rc)
    # app.rc already embeds app.manifest as resource 1. The MSVC linker adds its
    # own by default, and two of them are a hard error at link time:
    #   CVT1100: duplicate resource. type:MANIFEST, name:1
    #   LNK1123: failure during conversion to COFF
    if(MSVC)
        target_link_options(transcriptor PRIVATE /MANIFEST:NO)
    endif()
elseif(APPLE)
    add_executable(transcriptor MACOSX_BUNDLE ${TRANSCRIPTOR_SOURCES})
else()
    add_executable(transcriptor ${TRANSCRIPTOR_SOURCES})
endif()

target_include_directories(transcriptor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(transcriptor PRIVATE
    transcriptor_assets
    transcriptor::deps)

target_compile_definitions(transcriptor PRIVATE
    TRANSCRIPTOR_VERSION="${PROJECT_VERSION}")

if(TRANSCRIPTOR_DIARIZE)
    target_compile_definitions(transcriptor PRIVATE TRANSCRIPTOR_HAVE_DIARIZE=1)
endif()
if(TRANSCRIPTOR_WEBVIEW)
    target_compile_definitions(transcriptor PRIVATE TRANSCRIPTOR_HAVE_WEBVIEW=1)
endif()

if(MSVC)
    target_compile_options(transcriptor PRIVATE /utf-8 /W3)
    target_compile_definitions(transcriptor PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    target_compile_options(transcriptor PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# macOS bundle
# ---------------------------------------------------------------------------
if(APPLE)
    set_target_properties(transcriptor PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME        "Transcriptor"
        MACOSX_BUNDLE_GUI_IDENTIFIER     "dev.transcriptor.app"
        MACOSX_BUNDLE_BUNDLE_VERSION     "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/Info.plist.in")
endif()

include(cmake/package.cmake)

message(STATUS "")
message(STATUS "transcriptor ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  diarization  : ${TRANSCRIPTOR_DIARIZE}")
message(STATUS "  native window: ${TRANSCRIPTOR_WEBVIEW}")
message(STATUS "  GPU          : CUDA=${TRANSCRIPTOR_CUDA} Vulkan=${TRANSCRIPTOR_VULKAN} Metal=${TRANSCRIPTOR_METAL}")
message(STATUS "")
