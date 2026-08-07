# v5: Linux epoll event loop와 worker thread pool을 결합한 서버
add_executable(v5_server
    src/main.cpp
    src/epoll_thread_pool_server.cpp
)

target_include_directories(v5_server PRIVATE include)
