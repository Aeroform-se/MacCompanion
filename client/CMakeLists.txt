cmake_minimum_required(VERSION 3.10)
project(MacCompanionSpotify C)

set(CMAKE_C_STANDARD 99)

add_application(MacCompanionSpotify
    CREATOR "MCSP"
    src/main.c
    src/resources.r
)

target_compile_options(MacCompanionSpotify PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign)
