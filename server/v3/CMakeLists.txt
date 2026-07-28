# v3: 고정 크기 thread pool과 작업 큐를 사용하는 echo 서버
add_executable(v3_server
    src/main.cpp
    src/thread_pool_echo_server.cpp
)

target_include_directories(v3_server PRIVATE include)
