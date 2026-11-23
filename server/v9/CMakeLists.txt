# v9: v8의 틱 루프 + 격자 AOI를 Linux epoll 위에 올린 서버
#
# 소켓과 무관한 코드(v9_core)는 Linux와 Windows 양쪽에서 빌드하고, 서버는 epoll을 쓰므로 Linux에서만 빌드한다.
# 테스트(tests/)와 측정 도구(tools/)는 저장소에 올리지 않고 로컬에만 둔다. 폴더가 있을 때만 그 대상을 만든다.

find_package(Threads REQUIRED)

add_library(v9_core STATIC
    src/packet.cpp
    src/packet_assembler.cpp
    src/game_job_queue.cpp
    src/async_logger.cpp
    src/world.cpp
    src/tick_scheduler.cpp
)
target_include_directories(v9_core PUBLIC include)
target_link_libraries(v9_core PUBLIC Threads::Threads)

set(V9_TARGETS v9_core)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_tests.cpp)
    # 단위 테스트: g++와 MSVC 양쪽에서 같은 테스트로 이식이 맞는지 확인한다.
    add_executable(v9_unit_tests tests/unit_tests.cpp)
    target_link_libraries(v9_unit_tests PRIVATE v9_core)
    add_test(NAME v9_unit_tests COMMAND v9_unit_tests)
    list(APPEND V9_TARGETS v9_unit_tests)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/logic_bench.cpp)
    # 게임 로직만(월드 이동 + 시야 수집 + 인코딩) 서버 없이 같은 입력으로 재는 벤치마크
    add_executable(v9_logic_bench tools/logic_bench.cpp)
    target_link_libraries(v9_logic_bench PRIVATE v9_core)
    list(APPEND V9_TARGETS v9_logic_bench)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(v9_server
        src/main.cpp
        src/epoll_world_server.cpp
    )
    target_link_libraries(v9_server PRIVATE v9_core)
    list(APPEND V9_TARGETS v9_server)

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/load_bot.cpp)
        # 부하 봇: epoll로 수천 개 연결을 열어 무작위 이동을 보내고, 받은 시야 패킷으로 주변 목록을 재구성해 확인한다.
        add_executable(v9_load_bot tools/load_bot.cpp)
        target_link_libraries(v9_load_bot PRIVATE v9_core)
        list(APPEND V9_TARGETS v9_load_bot)
    endif()
endif()

option(V9_ENABLE_ASAN "Build v9 with AddressSanitizer (+ LeakSanitizer on Linux)" OFF)
if(V9_ENABLE_ASAN)
    foreach(target ${V9_TARGETS})
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            get_target_property(target_type ${target} TYPE)
            if(NOT target_type STREQUAL "STATIC_LIBRARY")
                target_link_options(${target} PRIVATE -fsanitize=address)
            endif()
        endif()
    endforeach()
endif()
