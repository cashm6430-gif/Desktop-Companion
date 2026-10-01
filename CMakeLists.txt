cmake_minimum_required(VERSION 3.24)
project(DesktopCompanion VERSION 0.1.0 LANGUAGES CXX)

# Some Chinese MSVC installations report a mojibake /showIncludes prefix to
# CMake. Ninja then records zero header dependencies and reuses stale objects.
if(CMAKE_GENERATOR STREQUAL "Ninja" AND CMAKE_CXX_CL_SHOWINCLUDES_PREFIX MATCHES "^娉ㄦ剰:")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "注意: 包含文件:  ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)

set(CUBISM_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/CubismSdkForNative-5-r.5"
    CACHE PATH "Unpacked Live2D Cubism SDK for Native")
set(HAVE_CUBISM OFF)
if(WIN32 AND EXISTS "${CUBISM_SDK_ROOT}/Core/include/Live2DCubismCore.h")
    find_package(Qt6 6.8 REQUIRED COMPONENTS OpenGLWidgets)
    find_package(glew CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)
    add_library(Live2DCubismCore SHARED IMPORTED)
    set_target_properties(Live2DCubismCore PROPERTIES
        IMPORTED_IMPLIB "${CUBISM_SDK_ROOT}/Core/dll/windows/x86_64/Live2DCubismCore.lib"
        IMPORTED_LOCATION "${CUBISM_SDK_ROOT}/Core/dll/windows/x86_64/Live2DCubismCore.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_SDK_ROOT}/Core/include")
    set(FRAMEWORK_SOURCE OpenGL)
    add_subdirectory("${CUBISM_SDK_ROOT}/Framework" "${CMAKE_CURRENT_BINARY_DIR}/cubism-framework")
    target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
    target_link_libraries(Framework PUBLIC Live2DCubismCore GLEW::GLEW OpenGL::GL)
    set(HAVE_CUBISM ON)
endif()

qt_add_executable(DesktopCompanion WIN32
    src/main.cpp
    src/MotionClip.h src/MotionClip.cpp
    src/MotionLibrary.h src/MotionLibrary.cpp
    src/PetController.h src/PetController.cpp
    src/ParameterMotion.h src/ParameterMotion.cpp
    src/PetWindow.h src/PetWindow.cpp
    src/DesktopDeleteSource.h src/DesktopDeleteSource.cpp
    src/CodexTurnSource.h src/CodexTurnSource.cpp
)
target_link_libraries(DesktopCompanion PRIVATE Qt6::Widgets Qt6::Network shell32)
target_compile_definitions(DesktopCompanion PRIVATE UNICODE _UNICODE)
if(HAVE_CUBISM)
    target_sources(DesktopCompanion PRIVATE src/CubismCanvas.h src/CubismCanvas.cpp)
    target_link_libraries(DesktopCompanion PRIVATE Qt6::OpenGLWidgets Framework)
    target_compile_definitions(DesktopCompanion PRIVATE HAVE_CUBISM)
endif()

add_executable(DesktopCompanionHook src/hook_main.cpp src/CodexTurnSource.h src/CodexTurnSource.cpp)
target_link_libraries(DesktopCompanionHook PRIVATE Qt6::Core Qt6::Network)

add_custom_target(DesktopCompanionAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:DesktopCompanion>/assets"
    DEPENDS DesktopCompanion)
if(HAVE_CUBISM)
    add_custom_target(DesktopCompanionCubismRuntime ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CUBISM_SDK_ROOT}/Core/dll/windows/x86_64/Live2DCubismCore.dll"
            "$<TARGET_FILE_DIR:DesktopCompanion>/Live2DCubismCore.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CUBISM_SDK_ROOT}/Framework/src/Rendering/OpenGL/Shaders/Standard"
            "$<TARGET_FILE_DIR:DesktopCompanion>/FrameworkShaders"
        DEPENDS DesktopCompanion)
endif()

if(WIN32)
    set_target_properties(DesktopCompanion PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_executable(PetControllerTest tests/PetControllerTest.cpp
        src/PetController.h src/PetController.cpp)
    # QIcon rides the eatTriggered signature, so the controller needs QtGui.
    target_link_libraries(PetControllerTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME PetControllerTest COMMAND PetControllerTest)
    add_executable(ParameterMotionTest tests/ParameterMotionTest.cpp
        src/ParameterMotion.h src/ParameterMotion.cpp
        src/MotionClip.h src/MotionClip.cpp
        src/MotionLibrary.h src/MotionLibrary.cpp)
    target_link_libraries(ParameterMotionTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME ParameterMotionTest COMMAND ParameterMotionTest)
    add_executable(MotionClipTest tests/MotionClipTest.cpp
        src/MotionClip.h src/MotionClip.cpp
        src/MotionLibrary.h src/MotionLibrary.cpp
        src/ParameterMotion.h src/ParameterMotion.cpp
        src/PetController.h src/PetController.cpp)
    target_link_libraries(MotionClipTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME MotionClipTest COMMAND MotionClipTest)
    if(WIN32)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
        add_test(NAME CodexHookTest COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/CodexHookTest.ps1"
            -HelperPath "$<TARGET_FILE:DesktopCompanionHook>"
            -InstallerPath "${CMAKE_CURRENT_SOURCE_DIR}/tools/install-codex-hooks.ps1")
    endif()
endif()
