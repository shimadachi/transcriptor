# Regression tests for bugs that have actually shipped. Each one fails against
# the code as it stood before its fix, so a revert is caught here rather than
# rediscovered by someone losing a recording.
#
# The C++ tests recompile the handful of sources they need instead of linking
# the application: those sources are deliberately free of llama, whisper and
# miniaudio, so a test binary builds in about a second and needs no models, no
# audio device and no network.

set(TESTS_SRC "${CMAKE_CURRENT_SOURCE_DIR}")
set(APP_SRC   "${CMAKE_SOURCE_DIR}/src")

function(transcriptor_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${TESTS_SRC}" "${APP_SRC}")
    target_compile_features(${name} PRIVATE cxx_std_17)
endfunction()

# -- summary chunking (R8) ----------------------------------------------------
transcriptor_test(test_chunking
    "${TESTS_SRC}/test_chunking.cpp"
    "${APP_SRC}/llm/chunking.cpp")
target_link_libraries(test_chunking PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME chunking COMMAND test_chunking)

# -- reasoning budget, and reasoning in the summary (V41, V51, V5, V36) -------
transcriptor_test(test_reasoning
    "${TESTS_SRC}/test_reasoning.cpp"
    "${APP_SRC}/llm/reasoning.cpp")
add_test(NAME reasoning COMMAND test_reasoning)

# -- hallucinated subtitle / audio-description credits (V50, V32) -------------
transcriptor_test(test_credits
    "${TESTS_SRC}/test_credits.cpp"
    "${APP_SRC}/stt/credits.cpp")
target_link_libraries(test_credits PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME credits COMMAND test_credits)

# -- diarization clustering threshold (V49) -----------------------------------
transcriptor_test(test_diar_threshold
    "${TESTS_SRC}/test_diar_threshold.cpp"
    "${APP_SRC}/config.cpp"
    "${APP_SRC}/util/paths.cpp")
target_link_libraries(test_diar_threshold PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME diar_threshold COMMAND test_diar_threshold)

# -- speaker attribution (V10) -------------------------------------------------
transcriptor_test(test_attribute
    "${TESTS_SRC}/test_attribute.cpp"
    "${APP_SRC}/pipeline/attribute.cpp")
target_link_libraries(test_attribute PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME attribute COMMAND test_attribute)

# -- choosing the compute device (V22) -----------------------------------------
# The rule is header-only and ggml-free, so this needs no GPU and no libraries.
transcriptor_test(test_device_choice "${TESTS_SRC}/test_device_choice.cpp")
add_test(NAME device_choice COMMAND test_device_choice)

# -- adopting and keeping settings (V7, V11, V57) ------------------------------
transcriptor_test(test_settings
    "${TESTS_SRC}/test_settings.cpp"
    "${APP_SRC}/config.cpp"
    "${APP_SRC}/util/paths.cpp")
target_link_libraries(test_settings PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME settings
         COMMAND test_settings "${CMAKE_CURRENT_BINARY_DIR}/settings_scratch")

# -- library versions, and what counts as a session (V16, V20, V29, V53) ------
transcriptor_test(test_library
    "${TESTS_SRC}/test_library.cpp"
    "${APP_SRC}/util/library.cpp"
    "${APP_SRC}/util/paths.cpp")
add_test(NAME library
         COMMAND test_library "${CMAKE_CURRENT_BINARY_DIR}/library_scratch")

# -- names ASCII cannot spell: downloads (V13) and uploads (V15) ---------------
# Runs everywhere; the download half can only fail the old way on Windows.
transcriptor_test(test_unicode_paths
    "${TESTS_SRC}/test_unicode_paths.cpp"
    "${APP_SRC}/util/net.cpp"
    "${APP_SRC}/util/paths.cpp")
find_package(Threads REQUIRED)
target_link_libraries(test_unicode_paths PRIVATE Threads::Threads)
add_test(NAME unicode_paths
         COMMAND test_unicode_paths "${CMAKE_CURRENT_BINARY_DIR}/unicode_paths_scratch")
set_tests_properties(unicode_paths PROPERTIES TIMEOUT 60)

# -- output writing (R2, V54, V4, V37) ----------------------------------------
# RLIMIT_FSIZE is how the failure is provoked, so POSIX only.
if(UNIX)
    transcriptor_test(test_write_failure
        "${TESTS_SRC}/test_write_failure.cpp"
        "${APP_SRC}/util/paths.cpp"
        "${APP_SRC}/util/export.cpp")
    target_link_libraries(test_write_failure PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME write_failure
             COMMAND test_write_failure "${CMAKE_CURRENT_BINARY_DIR}/write_scratch")

    # -- child process runner, and downloads that stall (R11, R12, V40) -------
    # Drives real /bin/sh children; the Windows half of this code cannot run here.
    transcriptor_test(test_process_runner
        "${TESTS_SRC}/test_process_runner.cpp"
        "${APP_SRC}/util/net.cpp"
        "${APP_SRC}/util/paths.cpp")
    find_package(Threads REQUIRED)
    target_link_libraries(test_process_runner PRIVATE Threads::Threads)
    add_test(NAME process_runner COMMAND test_process_runner)
    set_tests_properties(process_runner PROPERTIES TIMEOUT 60)
endif()

# -- counting cores on a hybrid CPU (V39) --------------------------------------
# Hands the Linux query a sysfs tree of its own, so Linux only.
if(UNIX AND NOT APPLE)
    transcriptor_test(test_cpu
        "${TESTS_SRC}/test_cpu.cpp"
        "${APP_SRC}/util/cpu.cpp")
    add_test(NAME cpu COMMAND test_cpu "${CMAKE_CURRENT_BINARY_DIR}/cpu_scratch")
endif()

# -- the take that lived in RAM (R14) ------------------------------------------
# Streaming a recording to disk instead of growing a vector for hours. Two of
# these watch the process's own RSS: one proves the spooled path stays flat,
# and the other proves that probe can see the old behaviour, so the first is
# not passing for free.
transcriptor_test(test_spool
    "${TESTS_SRC}/test_spool.cpp"
    "${TESTS_SRC}/fake_capture.cpp"
    "${APP_SRC}/audio/recorder.cpp"
    "${APP_SRC}/audio/spool.cpp"
    "${APP_SRC}/util/paths.cpp")
find_package(Threads REQUIRED)
target_link_libraries(test_spool PRIVATE Threads::Threads)
add_test(NAME spool
         COMMAND test_spool "${CMAKE_CURRENT_BINARY_DIR}/spool_scratch")
set_tests_properties(spool PROPERTIES TIMEOUT 120)

# -- mixing a microphone into system audio (V2) --------------------------------
# The real Recorder against the stand-in capture, which can make one source
# silent the way WASAPI loopback is while nothing plays.
transcriptor_test(test_mixing
    "${TESTS_SRC}/test_mixing.cpp"
    "${TESTS_SRC}/fake_capture.cpp"
    "${APP_SRC}/audio/recorder.cpp"
    "${APP_SRC}/audio/spool.cpp"
    "${APP_SRC}/util/paths.cpp")
find_package(Threads REQUIRED)
target_link_libraries(test_mixing PRIVATE Threads::Threads)
add_test(NAME mixing COMMAND test_mixing)
set_tests_properties(mixing PROPERTIES TIMEOUT 60)

# -- system audio on macOS: converting what a tap delivers (M1) ----------------
# The tap is the one source this app resamples itself, and the conversion is
# plain arithmetic, so it is checked on every platform rather than only where a
# tap exists. miniaudio is compiled without its device backends for this, so the
# test needs no audio library to link.
transcriptor_test(test_tap_convert
    "${TESTS_SRC}/test_tap_convert.cpp"
    "${TESTS_SRC}/miniaudio_dsp.c"
    "${APP_SRC}/audio/tap_convert.cpp")
target_include_directories(test_tap_convert PRIVATE "${miniaudio_SOURCE_DIR}")
if(UNIX)
    target_link_libraries(test_tap_convert PRIVATE m)
endif()
add_test(NAME tap_convert COMMAND test_tap_convert)

# -- system audio on macOS: the tap itself, against the real OS (M1) -----------
# The real capture, the real source list and the real tap -- nothing stood in
# for. It plays a tone and needs a permission only a person can grant, so it
# skips itself (exit 77) unless TRANSCRIPTOR_TEST_SYSTEM_AUDIO is set; see the
# top of the test for the modes. Built on every macOS run all the same: that the
# Objective-C++ compiles and links against the SDK is most of what CI can say
# about it. .github/workflows/mac-system-audio.yml is where it is run.
if(APPLE)
    transcriptor_test(test_system_audio_mac
        "${TESTS_SRC}/test_system_audio_mac.cpp"
        "${APP_SRC}/audio/capture.cpp"
        "${APP_SRC}/audio/sources.cpp"
        "${APP_SRC}/audio/tap_convert.cpp"
        "${APP_SRC}/audio/system_tap_mac.mm"
        "${APP_SRC}/audio/miniaudio_impl.c")
    # Source properties are per directory, so the one in the top-level file
    # does not reach this target.
    set_source_files_properties("${APP_SRC}/audio/system_tap_mac.mm" PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(test_system_audio_mac PRIVATE "${miniaudio_SOURCE_DIR}")
    find_package(Threads REQUIRED)
    target_link_libraries(test_system_audio_mac PRIVATE
        Threads::Threads
        "-framework CoreAudio" "-framework CoreFoundation" "-framework AudioToolbox"
        "-framework AudioUnit" "-framework Foundation")
    add_test(NAME system_audio_mac COMMAND test_system_audio_mac)
    set_tests_properties(system_audio_mac PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
endif()

# -- recording lifecycle (R1, R4, G6, N6, V42-V48) ----------------------------
# The one test that links the application itself, because these bugs are in how
# AppState, the Recorder and the worker thread fit together. Only the audio
# device is substituted: AudioCapture is a pimpl, so tests/fake_capture.cpp can
# take its place and hold a device open or closed for as long as a race needs.
# No model is loaded -- auto_transcribe is off throughout.
transcriptor_test(test_recording_lifecycle
    "${TESTS_SRC}/test_recording_lifecycle.cpp"
    "${TESTS_SRC}/fake_capture.cpp")
target_link_libraries(test_recording_lifecycle PRIVATE transcriptor_core)
# Taken from the option, deliberately not from the define the application code
# is compiled with: the check this feeds exists to notice that define failing to
# reach the code, so reading it from the same place would prove nothing.
target_compile_definitions(test_recording_lifecycle PRIVATE
    TRANSCRIPTOR_EXPECT_DIARIZE=$<IF:$<BOOL:${TRANSCRIPTOR_DIARIZE}>,1,0>)
add_test(NAME recording_lifecycle
         COMMAND test_recording_lifecycle
                 "${CMAKE_CURRENT_BINARY_DIR}/recording_scratch")
# Labelled "app", like every test that links transcriptor_core: with it comes
# ggml's GPU backend, which on the CUDA presets needs the NVIDIA driver to load
# at all. CI leaves the label out on the Windows CUDA runner, which has no
# driver to give it (see the Test step in .github/workflows/_build.yml), so a
# new test linking the core needs the label to be left out with them.
set_tests_properties(recording_lifecycle PROPERTIES TIMEOUT 120 LABELS app)

# -- keeping the live transcript (V55, V56) ------------------------------------
# The real LiveTranscriber against a stand-in engine. It stamps its lines with
# the pipeline's timestamps, so it links the application the way the lifecycle
# test does -- the stand-in capture included, in the device's place.
transcriptor_test(test_live
    "${TESTS_SRC}/test_live.cpp"
    "${TESTS_SRC}/fake_capture.cpp")
target_link_libraries(test_live PRIVATE transcriptor_core)
add_test(NAME live COMMAND test_live)
set_tests_properties(live PROPERTIES TIMEOUT 60 LABELS app)

# -- the HTTP API (E1, V3, V7, V12, V18, V23, V30, V34, V38) -------------------
# What the page is told, read through the real Server over a loopback socket.
# Links the application the way the lifecycle test does. POSIX only: the config
# directory can only be moved out of the machine's own through the environment
# there, and the stand-in ffmpeg on PATH is a shell script.
if(UNIX)
    transcriptor_test(test_api
        "${TESTS_SRC}/test_api.cpp"
        "${TESTS_SRC}/fake_capture.cpp")
    target_link_libraries(test_api PRIVATE transcriptor_core)
    add_test(NAME api COMMAND test_api "${CMAKE_CURRENT_BINARY_DIR}/api_scratch")
    set_tests_properties(api PROPERTIES TIMEOUT 120 LABELS app)
endif()

# -- the desktop app's tables (D1, V49) ---------------------------------------
# Constants the window and the engine each hold a copy of, and the rule that
# every string the interface shows exists in English and Turkish. Reads
# desktop/src with node's builtins alone, so it needs no `npm install`. Skipped
# where node is absent rather than failing: node is not needed to build the
# engine.
find_program(TRANSCRIPTOR_NODE NAMES node nodejs)
if(TRANSCRIPTOR_NODE)
    add_test(NAME desktop_tables
             COMMAND "${TRANSCRIPTOR_NODE}" "${TESTS_SRC}/desktop/test_tables.mjs"
                     "${CMAKE_SOURCE_DIR}")
    set_tests_properties(desktop_tables PROPERTIES TIMEOUT 60)
else()
    message(STATUS "node not found -- the desktop table checks will not run")
endif()
