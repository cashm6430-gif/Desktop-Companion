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
    src/PetPointerGesture.h src/PetPointerGesture.cpp
    src/ParameterMotion.h src/ParameterMotion.cpp
    src/motion/MotionMath.h
    src/motion/MotionParameterIds.h
    src/motion/SleepTrace.h
    src/motion/DragBehavior.h src/motion/DragBehavior.cpp
    src/motion/MemoBehavior.h src/motion/MemoBehavior.cpp
    src/motion/BoxBehavior.h src/motion/BoxBehavior.cpp
    src/motion/RiceBowlBehavior.h src/motion/RiceBowlBehavior.cpp
    src/motion/WaveBehavior.h src/motion/WaveBehavior.cpp
    src/TriggerDirector.h src/TriggerDirector.cpp
    src/motion/SleepBehavior.h src/motion/SleepBehavior.cpp
    src/motion/GrassTouchBehavior.h src/motion/GrassTouchBehavior.cpp
    src/overlay/FrameOverlay.h
    src/overlay/FrameComposer.h src/overlay/FrameComposer.cpp
    src/PetWindow.h src/PetWindow.cpp
    src/DesktopDeleteSource.h src/DesktopDeleteSource.cpp
    src/CodexTurnSource.h src/CodexTurnSource.cpp
)
target_link_libraries(DesktopCompanion PRIVATE Qt6::Widgets Qt6::Network shell32)
target_compile_definitions(DesktopCompanion PRIVATE UNICODE _UNICODE)
if(WIN32)
    # Application icon + VERSIONINFO (resources/app.rc references whale-girl.ico).
    # The .ico is referenced inside the .rc content, which CMake's dependency
    # scanner cannot see -- declare it explicitly or icon edits relink stale resources.
    set_source_files_properties(resources/app.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/whale-girl.ico")
    target_sources(DesktopCompanion PRIVATE resources/app.rc)
endif()
if(HAVE_CUBISM)
    target_sources(DesktopCompanion PRIVATE src/CubismCanvas.h src/CubismCanvas.cpp
        src/CubismMaterialBinding.h src/CubismMaterialBinding.cpp
        src/CubismNeckBinding.h src/CubismNeckBinding.cpp
        src/CubismPostureTransition.h
        src/CubismMaterialTexture.h src/CubismMaterialTexture.cpp)
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
    # Several tests build their fixtures in a QTemporaryDir, which Qt places in
    # the system %TEMP%. A sandboxed shell can deny child processes write access
    # there, so every temporary dir fails to be created and the tests report
    # "writeFixture(...) returned FALSE" instead of a real failure. Pointing TEMP
    # at a directory inside the build tree keeps the suite runnable in a
    # restricted terminal; outside a sandbox this is simply a normal temp dir.
    set(PET_TEST_TEMP "${CMAKE_CURRENT_BINARY_DIR}/_tmp")
    file(MAKE_DIRECTORY "${PET_TEST_TEMP}")
    set(PET_TEST_ENV "TEMP=${PET_TEST_TEMP};TMP=${PET_TEST_TEMP}")
    add_executable(PetControllerTest tests/PetControllerTest.cpp
        src/PetController.h src/PetController.cpp)
    # QIcon rides the eatTriggered signature, so the controller needs QtGui.
    target_link_libraries(PetControllerTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME PetControllerTest COMMAND PetControllerTest)
    add_executable(PetPointerGestureTest tests/PetPointerGestureTest.cpp
        src/PetPointerGesture.h src/PetPointerGesture.cpp)
    target_link_libraries(PetPointerGestureTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME PetPointerGestureTest COMMAND PetPointerGestureTest)
    add_executable(TriggerDirectorTest tests/TriggerDirectorTest.cpp
        src/TriggerDirector.h src/TriggerDirector.cpp)
    target_link_libraries(TriggerDirectorTest PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME TriggerDirectorTest COMMAND TriggerDirectorTest)
    add_executable(ParameterMotionTest tests/ParameterMotionTest.cpp
        src/ParameterMotion.h src/ParameterMotion.cpp
        src/motion/DragBehavior.h src/motion/DragBehavior.cpp
        src/motion/MemoBehavior.h src/motion/MemoBehavior.cpp
        src/motion/BoxBehavior.h src/motion/BoxBehavior.cpp
        src/motion/RiceBowlBehavior.h src/motion/RiceBowlBehavior.cpp
    src/motion/WaveBehavior.h src/motion/WaveBehavior.cpp
    src/TriggerDirector.h src/TriggerDirector.cpp
        src/motion/SleepBehavior.h src/motion/SleepBehavior.cpp
        src/motion/GrassTouchBehavior.h src/motion/GrassTouchBehavior.cpp
        src/MotionClip.h src/MotionClip.cpp
        src/MotionLibrary.h src/MotionLibrary.cpp)
    target_link_libraries(ParameterMotionTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME ParameterMotionTest COMMAND ParameterMotionTest)
    add_executable(MotionClipTest tests/MotionClipTest.cpp
        src/MotionClip.h src/MotionClip.cpp
        src/MotionLibrary.h src/MotionLibrary.cpp
        src/ParameterMotion.h src/ParameterMotion.cpp
        src/motion/DragBehavior.h src/motion/DragBehavior.cpp
        src/motion/MemoBehavior.h src/motion/MemoBehavior.cpp
        src/motion/BoxBehavior.h src/motion/BoxBehavior.cpp
        src/motion/RiceBowlBehavior.h src/motion/RiceBowlBehavior.cpp
    src/motion/WaveBehavior.h src/motion/WaveBehavior.cpp
    src/TriggerDirector.h src/TriggerDirector.cpp
        src/motion/SleepBehavior.h src/motion/SleepBehavior.cpp
        src/motion/GrassTouchBehavior.h src/motion/GrassTouchBehavior.cpp
        src/PetController.h src/PetController.cpp)
    target_link_libraries(MotionClipTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
    add_test(NAME MotionClipTest COMMAND MotionClipTest)
    foreach(test_name PetControllerTest PetPointerGestureTest TriggerDirectorTest ParameterMotionTest MotionClipTest)
        set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${PET_TEST_ENV}")
    endforeach()
    if(HAVE_CUBISM)
        add_executable(CubismMaterialBindingTest tests/CubismMaterialBindingTest.cpp
            src/CubismMaterialBinding.h src/CubismMaterialBinding.cpp)
        target_include_directories(CubismMaterialBindingTest PRIVATE "${CUBISM_SDK_ROOT}/Framework/src")
        target_link_libraries(CubismMaterialBindingTest PRIVATE Qt6::Core Qt6::Test Live2DCubismCore)
        add_test(NAME CubismMaterialBindingTest COMMAND CubismMaterialBindingTest)
        set_tests_properties(CubismMaterialBindingTest PROPERTIES ENVIRONMENT "${PET_TEST_ENV}")
        add_executable(CubismNeckBindingTest tests/CubismNeckBindingTest.cpp
            src/CubismNeckBinding.h src/CubismNeckBinding.cpp)
        target_include_directories(CubismNeckBindingTest PRIVATE "${CUBISM_SDK_ROOT}/Framework/src")
        target_link_libraries(CubismNeckBindingTest PRIVATE Qt6::Core Qt6::Test Live2DCubismCore)
        add_test(NAME CubismNeckBindingTest COMMAND CubismNeckBindingTest)
        set_tests_properties(CubismNeckBindingTest PROPERTIES ENVIRONMENT "${PET_TEST_ENV}")
        add_executable(CubismPostureTransitionTest tests/CubismPostureTransitionTest.cpp
            src/CubismPostureTransition.h)
        target_include_directories(CubismPostureTransitionTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(CubismPostureTransitionTest PRIVATE Qt6::Core Qt6::Test)
        add_test(NAME CubismPostureTransitionTest COMMAND CubismPostureTransitionTest)
        set_tests_properties(CubismPostureTransitionTest PROPERTIES ENVIRONMENT "${PET_TEST_ENV}")
        add_executable(CubismMaterialTextureTest tests/CubismMaterialTextureTest.cpp
            src/CubismMaterialTexture.h src/CubismMaterialTexture.cpp)
        target_link_libraries(CubismMaterialTextureTest PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
        add_test(NAME CubismMaterialTextureTest COMMAND CubismMaterialTextureTest)
        set_tests_properties(CubismMaterialTextureTest PROPERTIES ENVIRONMENT "${PET_TEST_ENV}")
    endif()
    if(WIN32)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
        add_test(NAME CodexHookTest COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/CodexHookTest.ps1"
            -HelperPath "$<TARGET_FILE:DesktopCompanionHook>"
            -InstallerPath "${CMAKE_CURRENT_SOURCE_DIR}/tools/install-codex-hooks.ps1")
    endif()
endif()
