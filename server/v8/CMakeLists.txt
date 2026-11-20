# v8: tick 기반 게임 루프 + 격자 AOI (v7 IOCP 네트워크 계층 재사용)

# 소켓과 무관한 순수 로직은 별도 라이브러리로 분리해 단위 테스트와 부하 봇에서 재사용한다.
add_library(v8_core STATIC
    src/packet.cpp
    src/packet_assembler.cpp
    src/game_job_queue.cpp
    src/async_logger.cpp
    src/world.cpp
    src/tick_scheduler.cpp
)
target_include_directories(v8_core PUBLIC include)

add_executable(v8_server
    src/main.cpp
    src/iocp_world_server.cpp
)
target_link_libraries(v8_server PRIVATE v8_core Ws2_32)

option(V8_ENABLE_ASAN "Build v8 with Microsoft AddressSanitizer" OFF)
if(V8_ENABLE_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "V8_ENABLE_ASAN requires MSVC")
    endif()
    # v8_core를 링크하는 모든 대상이 같은 ASan 설정이어야 링크 시 annotate_* 불일치가 나지 않는다.
    foreach(target v8_core v8_server)
        target_compile_options(${target} PRIVATE /fsanitize=address)
    endforeach()
    target_link_options(v8_server PRIVATE /INCREMENTAL:NO)
endif()
