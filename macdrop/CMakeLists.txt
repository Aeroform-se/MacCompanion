cmake_minimum_required(VERSION 3.10)
project(MacDrop C)

set(CMAKE_C_STANDARD 99)

list(APPEND REZ_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src")

# 1. Standalone System Extension (INIT) flat binary
add_executable(MacDropINIT
    src/sysmenu.c
    src/ShowInitIcon.c
)

set_target_properties(MacDropINIT PROPERTIES
    COMPILE_OPTIONS -ffunction-sections
    LINK_FLAGS "-Wl,--mac-flat -Wl,-gc-sections"
)

# 2. Package INIT resource into MacDropINIT.rsrc.bin
set(REZ_INCLUDE_FLAGS "")
foreach(dir ${REZ_INCLUDE_PATH})
    list(APPEND REZ_INCLUDE_FLAGS -I "${dir}")
endforeach()

add_custom_command(
    OUTPUT MacDropINIT.rsrc.bin
    COMMAND ${REZ} ${REZ_INCLUDE_FLAGS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/init.r
                    -o MacDropINIT.rsrc.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS MacDropINIT ${CMAKE_CURRENT_SOURCE_DIR}/src/init.r
)

# 3. Application Extension (appe)
add_application(MacDrop
    TYPE "appe"
    CREATOR "MDRO"
    src/main.c
    src/network.c
    src/file_io.c
    src/resources.r
    ${CMAKE_CURRENT_BINARY_DIR}/MacDropINIT.rsrc.bin
)

target_compile_options(MacDrop PRIVATE -O2 -Wall -Wno-multichar -Wno-pointer-sign)
