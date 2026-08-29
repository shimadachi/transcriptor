# Install rules and CPack setup.
#
# The goal is one redistributable artifact per platform: a .exe on Windows, a
# .app (in a .dmg) on macOS, a plain binary elsewhere. The web UI is compiled
# in, so nothing but the executable has to ship.

include(GNUInstallDirs)

if(APPLE)
    install(TARGETS transcriptor BUNDLE DESTINATION .)
else()
    install(TARGETS transcriptor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Backends built as shared libraries (a CUDA or Vulkan ggml backend can be one)
# have to travel with the binary.
if(BUILD_SHARED_LIBS)
    install(TARGETS ggml ggml-base
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
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
            DESTINATION ${CMAKE_INSTALL_BINDIR} OPTIONAL)
endif()

set(CPACK_PACKAGE_NAME "transcriptor")
set(CPACK_PACKAGE_VENDOR "Transcriptor")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Transcriptor")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Transcriptor")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
