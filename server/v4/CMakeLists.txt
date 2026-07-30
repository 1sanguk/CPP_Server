# v4: Linux epoll을 사용하는 single-thread reactor echo 서버
add_executable(v4_server
    src/main.cpp
    src/epoll_echo_server.cpp
)

target_include_directories(v4_server PRIVATE include)
