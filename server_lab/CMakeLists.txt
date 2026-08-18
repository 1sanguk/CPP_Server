cmake_minimum_required(VERSION 3.20)
project(ServerArchitectureLab CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_library(server_lab_core src/server_catalog.cpp)
target_include_directories(server_lab_core PUBLIC include)

add_executable(server_lab WIN32 MACOSX_BUNDLE src/main.cpp src/main_window.cpp src/client_panel.cpp)
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

enable_testing()
add_executable(server_catalog_test tests/server_catalog_test.cpp)
target_link_libraries(server_catalog_test PRIVATE server_lab_core)
add_test(NAME server_catalog_test COMMAND server_catalog_test)
