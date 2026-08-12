# v6: Windows IOCP 기반 비동기 echo 서버
add_executable(v6_server
    src/main.cpp
    src/iocp_echo_server.cpp
)

target_include_directories(v6_server PRIVATE include)
target_link_libraries(v6_server PRIVATE Ws2_32)
