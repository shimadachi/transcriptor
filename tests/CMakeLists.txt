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
add_test(NAME chunking COMMAND test_chunking)

# -- reasoning budget ---------------------------------------------------------
transcriptor_test(test_reasoning
    "${TESTS_SRC}/test_reasoning.cpp"
    "${APP_SRC}/llm/reasoning.cpp")
add_test(NAME reasoning COMMAND test_reasoning)

# -- library versions ---------------------------------------------------------
transcriptor_test(test_library
    "${TESTS_SRC}/test_library.cpp"
    "${APP_SRC}/util/library.cpp"
    "${APP_SRC}/util/paths.cpp")
add_test(NAME library
         COMMAND test_library "${CMAKE_CURRENT_BINARY_DIR}/library_scratch")

# -- output writing (R2) ------------------------------------------------------
# RLIMIT_FSIZE is how the failure is provoked, so POSIX only.
if(UNIX)
    transcriptor_test(test_write_failure
        "${TESTS_SRC}/test_write_failure.cpp"
        "${APP_SRC}/util/paths.cpp"
        "${APP_SRC}/util/export.cpp")
    target_link_libraries(test_write_failure PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME write_failure
             COMMAND test_write_failure "${CMAKE_CURRENT_BINARY_DIR}/write_scratch")

    # -- child process runner (R11, R12) --------------------------------------
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

# -- recording lifecycle (R1, R4, G6, N6) -------------------------------------
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
set_tests_properties(recording_lifecycle PROPERTIES TIMEOUT 120)

# -- browser recording UI (R3, R10, R13, G1, G2, G3) --------------------------
# Runs the real web/app.js under a mocked DOM. Skipped where node is absent
# rather than failing: node is not a build dependency of the application.
find_program(TRANSCRIPTOR_NODE NAMES node nodejs)
if(TRANSCRIPTOR_NODE)
    add_test(NAME ui
             COMMAND "${TRANSCRIPTOR_NODE}" "${TESTS_SRC}/ui/test_ui.js"
                     "${CMAKE_SOURCE_DIR}")
    set_tests_properties(ui PROPERTIES TIMEOUT 60)
else()
    message(STATUS "node not found -- the UI regression tests will not run")
endif()
