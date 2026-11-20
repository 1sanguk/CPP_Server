cmake_minimum_required(VERSION 3.20)
project(ServerArchitectureLab CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_library(server_lab_core src/server_catalog.cpp)
target_include_directories(server_lab_core PUBLIC include)

add_executable(server_lab WIN32 MACOSX_BUNDLE src/main.cpp src/main_window.cpp src/client_panel.cpp src/log_list.cpp)
target_include_directories(server_lab PRIVATE include)
target_link_libraries(server_lab PRIVATE server_lab_core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
target_compile_definitions(server_lab PRIVATE SERVER_LAB_REPO_ROOT="${CMAKE_SOURCE_DIR}/..")
set_target_properties(server_lab PROPERTIES
    OUTPUT_NAME "MMO Server Lab"
    MACOSX_BUNDLE_BUNDLE_NAME "MMO Server Lab"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.cppserver.serverlab"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
)

# 화면에 창을 띄우지 않는 GUI 테스트. 실제 서버(build/server의 v6~v8)를 띄우고 버튼을 코드로 눌러
# 클라이언트 화면 동작을 확인한 뒤 스크린샷을 남긴다. 실행: ctest --test-dir build/server_lab -C Release -V
enable_testing()
add_executable(server_lab_gui_test tests/gui_offscreen_test.cpp src/client_panel.cpp src/log_list.cpp)
target_include_directories(server_lab_gui_test PRIVATE include)
target_link_libraries(server_lab_gui_test PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
target_compile_definitions(server_lab_gui_test PRIVATE SERVER_LAB_REPO_ROOT="${CMAKE_SOURCE_DIR}/..")
add_test(NAME server_lab_gui_test
    COMMAND server_lab_gui_test "${CMAKE_BINARY_DIR}/gui_screenshots")
# windeployqt는 qwindows 플랫폼 플러그인만 복사하므로, Qt 설치 경로의 플러그인(qoffscreen)과 DLL을 쓰게 한다.
get_target_property(server_lab_qmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
get_filename_component(server_lab_qt_bin "${server_lab_qmake}" DIRECTORY)
set_tests_properties(server_lab_gui_test PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${server_lab_qt_bin}/../plugins"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${server_lab_qt_bin}")
