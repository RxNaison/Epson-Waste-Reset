cmake_minimum_required(VERSION 3.22)
project(ewr_android LANGUAGES C CXX)

# The library EWR's other hosts link, built from the repository itself: no
# second copy of any of it lives under android/.
set(EWR_BUILD_C_SHARED OFF CACHE BOOL "" FORCE)
set(EWR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EWR_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ewr)

# Android 15 and later run with 16 KB pages.
add_link_options("LINKER:-z,max-page-size=16384")

add_library(ewr_jni SHARED ewr_jni.cpp)
target_link_libraries(ewr_jni PRIVATE ewr_core log)
