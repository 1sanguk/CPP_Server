# v10: v9와 같은 게임 로직(틱 루프 + 격자 AOI)을 io_uring 네트워크 계층 위에 올린 Linux 서버
#
# liburing이 필요하다(Ubuntu: sudo apt install liburing-dev). 없으면 v10만 건너뛰어 다른 버전 빌드가 깨지지 않게 한다.
# 테스트·측정 도구는 v9의 로컬 도구(server/v9/tools)를 서버 경로만 바꿔 쓴다(프로토콜이 같다).

# pkg-config가 없어도 찾을 수 있게 헤더와 라이브러리를 직접 찾는다.
find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)
if(NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
    message(STATUS "v10: liburing not found, skipping (install liburing-dev)")
    return()
endif()
message(STATUS "v10: liburing ${LIBURING_LIBRARY}")
add_library(v10_liburing INTERFACE)
target_include_directories(v10_liburing INTERFACE ${LIBURING_INCLUDE_DIR})
target_link_libraries(v10_liburing INTERFACE ${LIBURING_LIBRARY})

find_package(Threads REQUIRED)

add_library(v10_core STATIC
    src/packet.cpp
    src/packet_assembler.cpp
    src/game_job_queue.cpp
    src/async_logger.cpp
    src/world.cpp
    src/tick_scheduler.cpp
)
target_include_directories(v10_core PUBLIC include)
target_link_libraries(v10_core PUBLIC Threads::Threads)

add_executable(v10_server
    src/main.cpp
    src/uring_world_server.cpp
)
target_link_libraries(v10_server PRIVATE v10_core v10_liburing)

option(V10_ENABLE_ASAN "Build v10 with AddressSanitizer + LeakSanitizer" OFF)
if(V10_ENABLE_ASAN)
    foreach(target v10_core v10_server)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    endforeach()
    target_link_options(v10_server PRIVATE -fsanitize=address)
endif()
