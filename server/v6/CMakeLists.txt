# v6: Windows IOCP 기반 비동기 echo 서버
add_executable(v6_server
    src/main.cpp
    src/iocp_echo_server.cpp
)

target_include_directories(v6_server PRIVATE include)
target_link_libraries(v6_server PRIVATE Ws2_32)

if(MSVC)
    target_compile_options(v6_server PRIVATE /utf-8)
endif()

option(V6_FORCE_PARTIAL_SEND_TEST "Limit each v6 WSASend request for the partial-send regression test" OFF)
if(V6_FORCE_PARTIAL_SEND_TEST)
    target_compile_definitions(v6_server PRIVATE V6_FORCE_PARTIAL_SEND_TEST=1)
endif()

option(V6_ENABLE_ASAN "Build v6 with Microsoft AddressSanitizer" OFF)
if(V6_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "V6_ENABLE_ASAN requires MSVC")
    endif()
    target_compile_options(v6_server PRIVATE /fsanitize=address)
    target_link_options(v6_server PRIVATE /INCREMENTAL:NO)
endif()
