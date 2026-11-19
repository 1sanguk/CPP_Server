# v7: IOCP network + game logic queue 채팅 서버

# 소켓과 무관한 순수 로직은 별도 라이브러리로 분리해 단위 테스트에서 재사용한다.
add_library(v7_core STATIC
    src/packet.cpp
    src/packet_assembler.cpp
    src/game_job_queue.cpp
    src/async_logger.cpp
)
target_include_directories(v7_core PUBLIC include)

add_executable(v7_server
    src/main.cpp
    src/iocp_game_server.cpp
)
target_link_libraries(v7_server PRIVATE v7_core Ws2_32)

option(V7_ENABLE_ASAN "Build v7 with Microsoft AddressSanitizer" OFF)
if(V7_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "V7_ENABLE_ASAN requires MSVC")
    endif()
    target_compile_options(v7_core PRIVATE /fsanitize=address)
    target_compile_options(v7_server PRIVATE /fsanitize=address)
    target_link_options(v7_server PRIVATE /INCREMENTAL:NO)
endif()

add_executable(v7_unit_tests tests/v7_unit_tests.cpp)
target_link_libraries(v7_unit_tests PRIVATE v7_core)
add_test(NAME v7_unit_tests COMMAND v7_unit_tests)
