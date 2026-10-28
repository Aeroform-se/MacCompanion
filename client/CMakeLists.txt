cmake_minimum_required(VERSION 3.10)
project(SpotiMac C)

set(CMAKE_C_STANDARD 99)

list(APPEND REZ_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_application(SpotiMac
    CREATOR "MCSP"
    src/main.c
    src/network.c
    src/visualizer.c
    src/visualizer_020.c
    src/resources.r
)

target_compile_options(SpotiMac PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign)
set_source_files_properties(src/visualizer_020.c PROPERTIES COMPILE_FLAGS "-O2 -m68020")
