cmake_minimum_required(VERSION 3.15)
project(EWR VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The version string the self-updater compares against GitHub release tags is
# generated from the project() version above, so there is exactly one copy of it.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ewr/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ewr/version.h
    @ONLY
)

set(LIB_SOURCES src/parser.cpp src/generator.cpp src/executor.cpp src/d4session.cpp src/session.cpp src/status.cpp src/deviceid.cpp src/updater_common.cpp src/usb_driver.cpp src/end4.cpp src/usb_composite.cpp)

if(WIN32)
    list(APPEND LIB_SOURCES src/usb_windows.cpp src/usb_libusb.cpp src/updater_windows.cpp)
else()
    list(APPEND LIB_SOURCES src/usb_libusb.cpp src/updater_linux.cpp)
endif()

add_library(ewr_core STATIC ${LIB_SOURCES})

target_include_directories(ewr_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)

find_package(Threads REQUIRED)
target_link_libraries(ewr_core PUBLIC Threads::Threads)

if(WIN32)
    # Windows needs libusb too: the ET-2xxx maintenance engine sits on a
    # WinUSB-bound vendor interface usbprint.sys cannot see. Fetched and
    # static so a release stays one ewr.exe with no DLL and no vcpkg.
    include(FetchContent)
    set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libusb_cmake
        GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
        GIT_TAG v1.0.29-0
    )
    FetchContent_MakeAvailable(libusb_cmake)

    # Bare target name: the libusb::usb-1.0 alias is only on libusb-cmake's
    # main branch, which has no release tag.
    target_link_libraries(ewr_core PRIVATE setupapi urlmon wininet usb-1.0)
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
# Multi-config generators (MSVC) append the configuration name to the output
# directory unless every per-config variant is set. The release workflow copies
# the binary from a fixed path, so pin all of them.
set_target_properties(ewr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}"
)

option(EWR_BUILD_TESTS "Build EWR unit tests" ON)
if(EWR_BUILD_TESTS)
    enable_testing()
    add_executable(ewr_tests tests/main_test.cpp)
    target_link_libraries(ewr_tests PRIVATE ewr_core)
    # Tests read database.json and the models/ folder, so they must run
    # from the repository root regardless of the build directory layout.
    add_test(NAME UnitTests COMMAND ewr_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
