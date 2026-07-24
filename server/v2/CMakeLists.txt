# v1: 싱글 스레드 blocking TCP echo 서버
add_executable(v2_server
    src/main.cpp
    src/thread_connection_server.cpp
)

target_include_directories(v2_server PRIVATE include)
