cmake_minimum_required(VERSION 3.20)
project(MMOServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

add_subdirectory(v1)
# v2 이후 버전은 구현이 시작되면 여기에 add_subdirectory(v2) 형태로 추가
