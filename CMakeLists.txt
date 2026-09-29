cmake_minimum_required(VERSION 3.24)
project(DesktopCompanion VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network)

qt_add_executable(DesktopCompanion WIN32
    src/main.cpp
    src/PetController.h src/PetController.cpp
    src/ParameterMotion.h src/ParameterMotion.cpp
    src/PetWindow.h src/PetWindow.cpp
    src/DesktopDeleteSource.h src/DesktopDeleteSource.cpp
    src/CodexTurnSource.h src/CodexTurnSource.cpp
)
target_link_libraries(DesktopCompanion PRIVATE Qt6::Widgets Qt6::Network shell32)
target_compile_definitions(DesktopCompanion PRIVATE UNICODE _UNICODE)

add_executable(DesktopCompanionHook src/hook_main.cpp src/CodexTurnSource.h src/CodexTurnSource.cpp)
target_link_libraries(DesktopCompanionHook PRIVATE Qt6::Core Qt6::Network)

add_custom_target(DesktopCompanionAssets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:DesktopCompanion>/assets"
    DEPENDS DesktopCompanion)

if(WIN32)
    set_target_properties(DesktopCompanion PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_executable(PetControllerTest tests/PetControllerTest.cpp src/PetController.h src/PetController.cpp)
    target_link_libraries(PetControllerTest PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME PetControllerTest COMMAND PetControllerTest)
    add_executable(ParameterMotionTest tests/ParameterMotionTest.cpp
        src/ParameterMotion.h src/ParameterMotion.cpp)
    target_link_libraries(ParameterMotionTest PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME ParameterMotionTest COMMAND ParameterMotionTest)
endif()
