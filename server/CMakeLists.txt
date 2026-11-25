cmake_minimum_required(VERSION 3.20)
project(MMOServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

enable_testing()

if(WIN32)
    # v6~v8은 native Windows의 Winsock2/IOCP 학습 트랙이다.
    add_subdirectory(v6)
    add_subdirectory(v7)
    add_subdirectory(v8)
    # v9 서버는 Linux 전용이지만, 소켓과 무관한 코어·단위 테스트·게임 로직 벤치마크는 MSVC로도 빌드해 g++ 결과와 비교한다.
    add_subdirectory(v9)
else()
    # v1~v3는 POSIX/BSD socket API를 사용해 macOS와 Linux에서 빌드한다.
    add_subdirectory(v1)
    add_subdirectory(v2)
    add_subdirectory(v3)

    # v4~v5는 epoll/eventfd를 사용하는 Linux 전용 서버다.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_subdirectory(v4)
        add_subdirectory(v5)
        # v9는 v8(틱 + 격자 AOI)을 epoll로 옮긴 Linux 서버다.
        add_subdirectory(v9)
        # v10은 같은 게임 로직을 io_uring으로 돌린다(liburing이 없으면 건너뛴다).
        add_subdirectory(v10)
    endif()
endif()
