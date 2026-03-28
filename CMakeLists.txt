cmake_minimum_required(VERSION 3.15)
project(EWR VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIB_SOURCES src/parser.cpp)

if(WIN32)
    list(APPEND LIB_SOURCES src/usb_windows.cpp)
else()
    list(APPEND LIB_SOURCES src/usb_linux.cpp)
endif()

add_library(ewr_core STATIC ${LIB_SOURCES})

target_include_directories(ewr_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(WIN32)
    target_link_libraries(ewr_core PRIVATE setupapi)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    
    target_include_directories(ewr_core PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(ewr_core PRIVATE ${LIBUSB_LIBRARIES})
endif()

add_executable(ewr cli/main.cpp)

target_link_libraries(ewr PRIVATE ewr_core)

set_target_properties(ewr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")