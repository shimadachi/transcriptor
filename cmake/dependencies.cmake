# All third-party code is fetched and compiled from source, so the result is a
# self-contained native binary. Every pin below is a released tag; bump them in
# one place. See README "Bumping dependency pins" before changing ggml-backed
# ones (llama.cpp and whisper.cpp must agree on their shared ggml).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(TRANSCRIPTOR_LLAMA_TAG   "b10687"       CACHE STRING "llama.cpp git tag")
set(TRANSCRIPTOR_WHISPER_TAG "v1.9.3"       CACHE STRING "whisper.cpp git tag")
set(TRANSCRIPTOR_SHERPA_TAG  "v1.10.46"     CACHE STRING "sherpa-onnx git tag")
set(TRANSCRIPTOR_HTTPLIB_TAG "v0.18.3"      CACHE STRING "cpp-httplib git tag")
set(TRANSCRIPTOR_JSON_TAG    "v3.11.3"      CACHE STRING "nlohmann/json git tag")
set(TRANSCRIPTOR_MINIAUDIO_TAG "0.11.21"    CACHE STRING "miniaudio git tag")
set(TRANSCRIPTOR_WEBVIEW_TAG "0.12.0"       CACHE STRING "webview git tag")

add_library(transcriptor_deps INTERFACE)
add_library(transcriptor::deps ALIAS transcriptor_deps)

# ---------------------------------------------------------------------------
# ggml backends — one setting, applied to both llama.cpp and whisper.cpp
# ---------------------------------------------------------------------------
set(GGML_CUDA   ${TRANSCRIPTOR_CUDA}   CACHE BOOL "" FORCE)
set(GGML_VULKAN ${TRANSCRIPTOR_VULKAN} CACHE BOOL "" FORCE)
set(GGML_METAL  ${TRANSCRIPTOR_METAL}  CACHE BOOL "" FORCE)
if(TRANSCRIPTOR_METAL)
    # Compile the Metal shaders into the binary rather than shipping a .metal file.
    set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)   # avoids a libomp runtime dependency

# ---------------------------------------------------------------------------
# CUDA architectures
#
# ggml picks "native" — one architecture, the build machine's — only while
# GGML_NATIVE is ON. The CPU-baseline block below turns GGML_NATIVE off for a
# reason that has nothing to do with the GPU, and that silently flips ggml's
# CUDA default to a list of eight architectures, each of which nvcc compiles
# every kernel for. That is what took the CUDA CI builds past three hours.
#
# So pin the list here, decoupled from the CPU baseline. Measured on ggml's
# largest kernel (mmvq.cu): ~16s of shared frontend plus ~31s and ~0.8 MB of
# fatbin per architecture, so the count is very nearly the whole build cost.
#
#   61          Pascal    GTX 10xx      SASS + the PTX fallback below
#   75-real     Turing    RTX 20xx, GTX 16xx
#   86-real     Ampere    RTX 30xx
#   89-real     Ada       RTX 40xx
#   120a-real   Blackwell RTX 50xx
#
# -real ships SASS that runs as-is; -virtual ships PTX, which the driver
# JIT-compiles on first launch (cached afterwards, but the first run stalls);
# a bare entry ships both. Every consumer card above therefore starts without
# JIT, and the one bare entry — deliberately the *lowest*, since PTX only JITs
# forward — is the catch-all that keeps anything unlisted working: V100, A100,
# H100, and whatever comes after Blackwell.
#
# Measured per architecture on mmvq.cu, which is why this shape and not PTX for
# the older cards: -virtual 95.8s, -real 106.4s, bare 117.9s. SASS costs ~11%
# over PTX and is *smaller*, so trading JIT away for native code is nearly free.
if(TRANSCRIPTOR_CUDA AND NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        if(TRANSCRIPTOR_NATIVE)
            # Same bargain as the CPU baseline: fastest to build and to run,
            # for the machine doing the building only.
            set(_tr_cuda_archs "native")
        else()
            set(_tr_cuda_archs "")
            # CUDA 13 removed Maxwell, Pascal and Volta. Asking it for
            # compute_61 is not a warning but a hard stop:
            #   nvcc fatal : Unsupported gpu architecture 'compute_61'
            # So GTX 10xx support and the CI toolkit pin stand or fall
            # together; see the CUDA step in .github/workflows/_build.yml.
            if(CUDAToolkit_VERSION VERSION_LESS "13")
                list(APPEND _tr_cuda_archs 61 75-real)
            else()
                list(APPEND _tr_cuda_archs 75)   # Turing inherits the PTX duty
            endif()
            list(APPEND _tr_cuda_archs 86-real 89-real)
            if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
                list(APPEND _tr_cuda_archs 120a-real)
            endif()
        endif()
        set(CMAKE_CUDA_ARCHITECTURES "${_tr_cuda_archs}" CACHE STRING
            "CUDA architectures to generate device code for")
    endif()
endif()

# A CPU baseline the packages can actually run on. ggml defaults GGML_NATIVE to
# ON, which builds the CPU backend with -march=native (/arch:AVX512 under MSVC,
# via FindSIMD.cmake) — tuned for whatever machine happened to run the build. A
# CI runner with AVX-512 therefore ships a binary that dies with SIGILL inside
# ggml_cpu_init, before the window ever opens, on any CPU without AVX-512: every
# AMD Zen 1-3, and every Intel consumer part since Alder Lake.
#
# Switching it off flips ggml's INS_ENB on instead, which enables SSE4.2, AVX,
# AVX2, BMI2, FMA and F16C while leaving the AVX-512 options off — one baseline
# that every x86-64 CPU from Haswell / Excavator (2013-15) onwards can run.
#
# x86 only. On Apple Silicon GGML_NATIVE drives -mcpu=native, and turning it off
# there would drop the NEON dotprod / i8mm paths whisper leans on; the arm64
# runner is an M1, so native there is already the conservative baseline.
#
# TRANSCRIPTOR_NATIVE brings -march=native back for a build you are making for
# the machine it will run on. Never set it for anything you intend to hand to
# someone else.
set(TRANSCRIPTOR_NATIVE_LABEL "not x86 (unchanged)")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86)$")
    if(TRANSCRIPTOR_NATIVE)
        set(TRANSCRIPTOR_NATIVE_LABEL "native (-march=native) — THIS MACHINE ONLY")
    else()
        set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
        set(TRANSCRIPTOR_NATIVE_LABEL "portable baseline (AVX2/FMA/F16C, no AVX-512)")
    endif()
endif()

# ---------------------------------------------------------------------------
# llama.cpp — the summarizer (replaces LM Studio)
#
# Fetched FIRST on purpose: it defines the `ggml` targets, and whisper.cpp's
# CMakeLists reuses an existing `ggml` target instead of adding its own. That
# keeps one ggml in the build and avoids duplicate-target errors.
# ---------------------------------------------------------------------------
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(llama_cpp
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG        ${TRANSCRIPTOR_LLAMA_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(llama_cpp)

target_link_libraries(transcriptor_deps INTERFACE llama)

# ---------------------------------------------------------------------------
# whisper.cpp — the STT engine (replaces faster-whisper)
# ---------------------------------------------------------------------------
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(WHISPER_CURL           OFF CACHE BOOL "" FORCE)

FetchContent_Declare(whisper_cpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        ${TRANSCRIPTOR_WHISPER_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(whisper_cpp)

target_link_libraries(transcriptor_deps INTERFACE whisper)

# ---------------------------------------------------------------------------
# sherpa-onnx — speaker diarization (replaces pyannote.audio)
# Pulls its own ONNX Runtime; that is the one prebuilt blob in the tree.
# ---------------------------------------------------------------------------
if(TRANSCRIPTOR_DIARIZE)
    set(SHERPA_ONNX_ENABLE_PYTHON       OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_TESTS        OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_CHECK        OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_PORTAUDIO    OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_JNI          OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_C_API        ON  CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_WEBSOCKET    OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_BINARY       OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_TTS          OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION ON CACHE BOOL "" FORCE)

    # sherpa-onnx expects to be the top-level project: its CMakeLists does
    # include(kaldi-native-fbank) and friends, resolved against a module path
    # built from CMAKE_SOURCE_DIR. Under FetchContent that points at *our* tree
    # and every include() fails. So populate first, put sherpa's own cmake/ on
    # the module path, then add the subdirectory by hand.
    #
    # SOURCE_SUBDIR names a directory with no CMakeLists.txt, which makes
    # MakeAvailable download without configuring — the documented way to split
    # those two steps.
    FetchContent_Declare(sherpa_onnx
        GIT_REPOSITORY https://github.com/k2-fsa/sherpa-onnx.git
        GIT_TAG        ${TRANSCRIPTOR_SHERPA_TAG}
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  cmake/transcriptor-populate-only)
    FetchContent_MakeAvailable(sherpa_onnx)

    list(APPEND CMAKE_MODULE_PATH "${sherpa_onnx_SOURCE_DIR}/cmake")

    # sherpa appends -static-libstdc++/-static-libgcc to the global flags, which
    # would otherwise leak into every target in this build. Restore them after.
    set(_transcriptor_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    set(_transcriptor_saved_c_flags   "${CMAKE_C_FLAGS}")

    # kaldi-native-fbank, kaldi-decoder and friends still declare
    # cmake_minimum_required(VERSION 3.x) with x < 5, which CMake 4 rejects
    # outright. Scope the compatibility shim to this subtree only.
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()

    # Eigen arrives under kaldi-decoder, and its blas/CMakeLists.txt opens with
    #   check_language(Fortran)
    #   if(CMAKE_Fortran_COMPILER)
    #     enable_language(Fortran)   # not OPTIONAL -- a failure here is fatal
    # Nothing we build is Fortran, but check_language searches PATH, and the
    # GitHub Windows image carries a mingw gfortran that cannot link against
    # MSVC objects: the compiler test then fails and takes configure with it.
    # The Visual Studio generator never found one, so this only appears under
    # Ninja. check_language is a no-op when the variable is already defined,
    # and NOTFOUND is false to if(), so this takes the else branch as intended.
    if(NOT DEFINED CMAKE_Fortran_COMPILER)
        set(CMAKE_Fortran_COMPILER NOTFOUND)
    endif()

    # sherpa picks its prebuilt ONNX Runtime by CMAKE_VS_PLATFORM_NAME, which
    # only the Visual Studio generator sets. Its dispatch actually copes -- an
    # empty value falls through to the x64 branch, which is the one we want --
    # but the file that branch selects then re-checks the same variable and
    # calls it fatal. Naming the architecture we are already building satisfies
    # the guard without moving the dispatch off x64.
    if(WIN32 AND NOT CMAKE_VS_PLATFORM_NAME)
        set(CMAKE_VS_PLATFORM_NAME "x64")
    endif()

    # sherpa's sources include their own headers as "sherpa-onnx/csrc/...", which
    # only resolves if its source root is an include dir. It normally arranges
    # that with include_directories(${CMAKE_SOURCE_DIR}) — wrong tree for us.
    # This is the last add_subdirectory() here, so scoping it to the rest of
    # this directory affects sherpa and nothing already configured.
    include_directories("${sherpa_onnx_SOURCE_DIR}" "${sherpa_onnx_BINARY_DIR}")

    add_subdirectory("${sherpa_onnx_SOURCE_DIR}" "${sherpa_onnx_BINARY_DIR}"
                     EXCLUDE_FROM_ALL)

    set(CMAKE_CXX_FLAGS "${_transcriptor_saved_cxx_flags}")
    set(CMAKE_C_FLAGS   "${_transcriptor_saved_c_flags}")
    unset(CMAKE_POLICY_VERSION_MINIMUM)

    target_link_libraries(transcriptor_deps INTERFACE sherpa-onnx-cxx-api sherpa-onnx-c-api)
    target_include_directories(transcriptor_deps INTERFACE "${sherpa_onnx_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Header-only bits
# ---------------------------------------------------------------------------
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${TRANSCRIPTOR_JSON_TAG}
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)
target_link_libraries(transcriptor_deps INTERFACE nlohmann_json::nlohmann_json)

FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        ${TRANSCRIPTOR_HTTPLIB_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(httplib)
target_include_directories(transcriptor_deps INTERFACE "${httplib_SOURCE_DIR}")

FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        ${TRANSCRIPTOR_MINIAUDIO_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(miniaudio)
target_include_directories(transcriptor_deps INTERFACE "${miniaudio_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# Native window
# ---------------------------------------------------------------------------
if(TRANSCRIPTOR_WEBVIEW)
    set(WEBVIEW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(webview
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG        ${TRANSCRIPTOR_WEBVIEW_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(webview)
    target_link_libraries(transcriptor_deps INTERFACE webview::core_static)
endif()

# ---------------------------------------------------------------------------
# Platform libraries
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(transcriptor_deps INTERFACE Threads::Threads)

if(WIN32)
    target_link_libraries(transcriptor_deps INTERFACE ole32 shell32 shlwapi winmm ws2_32)
elseif(APPLE)
    find_library(COREAUDIO CoreAudio)
    find_library(COREFOUNDATION CoreFoundation)
    find_library(AUDIOTOOLBOX AudioToolbox)
    find_library(AUDIOUNIT AudioUnit)
    target_link_libraries(transcriptor_deps INTERFACE
        ${COREAUDIO} ${COREFOUNDATION} ${AUDIOTOOLBOX} ${AUDIOUNIT})
else()
    target_link_libraries(transcriptor_deps INTERFACE ${CMAKE_DL_LIBS} m)
endif()
