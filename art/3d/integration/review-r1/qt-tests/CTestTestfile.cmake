# CMake generated Testfile for 
# Source directory: E:/projects/Desktop-Companion
# Build directory: E:/projects/Desktop-Companion/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("PetControllerTest" "E:/projects/Desktop-Companion/build/PetControllerTest.exe")
set_tests_properties("PetControllerTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;121;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("PetPointerGestureTest" "E:/projects/Desktop-Companion/build/PetPointerGestureTest.exe")
set_tests_properties("PetPointerGestureTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;125;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("TriggerDirectorTest" "E:/projects/Desktop-Companion/build/TriggerDirectorTest.exe")
set_tests_properties("TriggerDirectorTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;129;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("ToonEventBridgeTest" "E:/projects/Desktop-Companion/build/ToonEventBridgeTest.exe")
set_tests_properties("ToonEventBridgeTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;134;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("ParameterMotionTest" "E:/projects/Desktop-Companion/build/ParameterMotionTest.exe")
set_tests_properties("ParameterMotionTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;148;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("MotionClipTest" "E:/projects/Desktop-Companion/build/MotionClipTest.exe")
set_tests_properties("MotionClipTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;163;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("CubismMaterialBindingTest" "E:/projects/Desktop-Companion/build/CubismMaterialBindingTest.exe")
set_tests_properties("CubismMaterialBindingTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;172;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("CubismNeckBindingTest" "E:/projects/Desktop-Companion/build/CubismNeckBindingTest.exe")
set_tests_properties("CubismNeckBindingTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;178;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("CubismPostureTransitionTest" "E:/projects/Desktop-Companion/build/CubismPostureTransitionTest.exe")
set_tests_properties("CubismPostureTransitionTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;184;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("CubismMaterialTextureTest" "E:/projects/Desktop-Companion/build/CubismMaterialTextureTest.exe")
set_tests_properties("CubismMaterialTextureTest" PROPERTIES  ENVIRONMENT "TEMP=E:/projects/Desktop-Companion/build/_tmp;TMP=E:/projects/Desktop-Companion/build/_tmp" _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;189;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
add_test("CodexHookTest" "C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/native/powershell/pwsh.exe" "-NoLogo" "-NoProfile" "-NonInteractive" "-File" "E:/projects/Desktop-Companion/tests/CodexHookTest.ps1" "-HelperPath" "E:/projects/Desktop-Companion/build/DesktopCompanionHook.exe" "-InstallerPath" "E:/projects/Desktop-Companion/tools/install-codex-hooks.ps1")
set_tests_properties("CodexHookTest" PROPERTIES  _BACKTRACE_TRIPLES "E:/projects/Desktop-Companion/CMakeLists.txt;194;add_test;E:/projects/Desktop-Companion/CMakeLists.txt;0;")
subdirs("cubism-framework")
