# v1: 싱글 스레드 blocking TCP echo 서버
add_executable(v1_server
    src/main.cpp
    src/blocking_echo_server.cpp
)

target_include_directories(v1_server PRIVATE include)
