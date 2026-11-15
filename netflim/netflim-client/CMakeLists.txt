cmake_minimum_required(VERSION 3.16)
project(NetFlim C)

set_source_files_properties(src/codec.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
set_source_files_properties(src/codec_020.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer;-m68020;-msoft-float")
set_source_files_properties(src/player.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
set_source_files_properties(src/player_hot.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
set_source_files_properties(src/player_hot_020.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer;-m68020;-msoft-float")
set_source_files_properties(src/net.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
set_source_files_properties(src/net_hot.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer")
set_source_files_properties(src/net_hot_020.c PROPERTIES COMPILE_OPTIONS "-O3;-fomit-frame-pointer;-m68020;-msoft-float")
set_source_files_properties(src/main.c PROPERTIES COMPILE_OPTIONS "-O2;-fomit-frame-pointer")

# Unified single binary: 68000 base runtime with Gestalt-dispatched 68020+ codec, network & player hot paths
add_application(NetFlim
    CREATOR "NFLM"
    src/main.c
    src/codec.c
    src/codec_020.c
    src/net.c
    src/net_hot.c
    src/net_hot_020.c
    src/player.c
    src/player_hot.c
    src/player_hot_020.c
    src/resources.r
)

target_include_directories(NetFlim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(NetFlim PRIVATE
    -Wall
    -Wextra
    -Wno-multichar
    -Wno-unused-parameter
    -Wno-pointer-sign
    -ffunction-sections
    -fdata-sections
)

target_link_options(NetFlim PRIVATE "LINKER:--gc-sections")

target_link_libraries(NetFlim PRIVATE
    OpenTransport
    OpenTransportApp
    OpenTptInet
)

set_target_properties(NetFlim PROPERTIES
    REZ_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
