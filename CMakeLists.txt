#cmake_minimum_required(VERSION 3.29)
#project(c_midiplayer C)
#
#set(CMAKE_C_STANDARD 11)
#set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -march=native -funroll-loops -fomit-frame-pointer")
#
#add_executable(c_midiplayer main.c
#        midiplayer.h
#        midiplayer.c)
#
#set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib")
#
#set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
#
#add_custom_command(TARGET c_midiplayer POST_BUILD
#        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIB_DIR}" "${OUTPUT_DIR}"
#        COMMENT "Copying shared libraries from lib/ to build output"
#)

cmake_minimum_required(VERSION 3.29)
project(c_midiplayer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -march=native -funroll-loops -fomit-frame-pointer")

# Find raylib package or build from source
find_package(raylib QUIET)
if (NOT raylib_FOUND)
    include(FetchContent)
    FetchContent_Declare(
            raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 4.5.0
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

# Find pthread - required for threading
find_package(Threads REQUIRED)

add_executable(c_midiplayer main.c
        midiplayer.h
        midiplayer.c)

target_link_libraries(c_midiplayer raylib ${CMAKE_THREAD_LIBS_INIT})

# On Linux, you may need to link to these libraries as well
if(UNIX AND NOT APPLE)
    target_link_libraries(c_midiplayer m dl)
endif()

set(LIB_DIR "${CMAKE_SOURCE_DIR}/lib")
set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_custom_command(TARGET c_midiplayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${LIB_DIR}" "${OUTPUT_DIR}"
        COMMENT "Copying shared libraries from lib/ to build output"
)