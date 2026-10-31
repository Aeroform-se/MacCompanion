cmake_minimum_required(VERSION 3.10)
project(MacDrop C)

set(CMAKE_C_STANDARD 99)

list(APPEND REZ_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_application(MacDrop
    CREATOR "MCDP"
    src/main.c
    src/network.c
    src/file_io.c
    src/resources.r
)

target_compile_options(MacDrop PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign)
