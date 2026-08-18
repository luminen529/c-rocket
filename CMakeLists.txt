cmake_minimum_required(VERSION 3.10)

project(c-rocket LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(c-rocket src/main.c)

# Keep the build tree for CMake's intermediate files, but place the executable
# itself and the simulation output directory in the project root.
set(C_ROCKET_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/output")
file(MAKE_DIRECTORY "${C_ROCKET_OUTPUT_DIR}")
file(TO_CMAKE_PATH "${C_ROCKET_OUTPUT_DIR}" C_ROCKET_OUTPUT_DIR)

set_target_properties(c-rocket PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}"
)

# main.c uses this absolute path so output/ is independent of the IDE's
# working-directory setting.
target_compile_definitions(c-rocket PRIVATE
    C_ROCKET_OUTPUT_DIR=\"${C_ROCKET_OUTPUT_DIR}\"
)

if (MSVC)
    target_compile_options(c-rocket PRIVATE /W4)
else()
    target_compile_options(c-rocket PRIVATE -Wall -Wextra -Wpedantic)
endif()
