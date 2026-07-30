cmake_minimum_required(VERSION 3.20)
project(MMOServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra)

add_subdirectory(v1)
add_subdirectory(v2)
add_subdirectory(v3)
add_subdirectory(v4)
