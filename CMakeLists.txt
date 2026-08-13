cmake_minimum_required(VERSION 3.10)

project(c-rocket LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(c-rocket src/main.c)

if (MSVC)
    target_compile_options(c-rocket PRIVATE /W4)
else()
    target_compile_options(c-rocket PRIVATE -Wall -Wextra -Wpedantic)
endif()
