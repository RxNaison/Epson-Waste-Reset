cmake_minimum_required(VERSION 3.15)
project(EWR VERSION 1.2.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIB_SOURCES src/parser.cpp src/generator.cpp src/executor.cpp)

if(WIN32)
    list(APPEND LIB_SOURCES src/usb_windows.cpp)
else()
    list(APPEND LIB_SOURCES src/usb_linux.cpp)
endif()

add_library(ewr_core STATIC ${LIB_SOURCES})

target_include_directories(ewr_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
)

if(WIN32)
    target_link_libraries(ewr_core PRIVATE setupapi urlmon)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    pkg_check_modules(LIBCURL REQUIRED libcurl)
    
    target_include_directories(ewr_core PRIVATE ${LIBUSB_INCLUDE_DIRS} ${LIBCURL_INCLUDE_DIRS})
    # Homebrew keeps its libs outside the default linker search path.
    target_link_directories(ewr_core PUBLIC ${LIBUSB_LIBRARY_DIRS} ${LIBCURL_LIBRARY_DIRS})
    target_link_libraries(ewr_core PRIVATE ${LIBUSB_LIBRARIES} ${LIBCURL_LIBRARIES})
endif()

add_executable(ewr cli/main.cpp)
target_link_libraries(ewr PRIVATE ewr_core)
set_target_properties(ewr PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

option(EWR_BUILD_TESTS "Build EWR unit tests" ON)
if(EWR_BUILD_TESTS)
    enable_testing()
    add_executable(ewr_tests tests/main_test.cpp)
    target_link_libraries(ewr_tests PRIVATE ewr_core)
    # Tests read database.json and the models/ folder, so they must run
    # from the repository root regardless of the build directory layout.
    add_test(NAME UnitTests COMMAND ewr_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
