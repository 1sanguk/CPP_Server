cmake_minimum_required(VERSION 3.20)
project(MMOServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

if(WIN32)
    # v6는 native Windows의 Winsock2/IOCP 학습 트랙이다.
    add_subdirectory(v6)
else()
    # v1~v3는 POSIX/BSD socket API를 사용해 macOS와 Linux에서 빌드한다.
    add_subdirectory(v1)
    add_subdirectory(v2)
    add_subdirectory(v3)

    # v4~v5는 epoll/eventfd를 사용하는 Linux 전용 서버다.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_subdirectory(v4)
        add_subdirectory(v5)
    endif()
endif()
