cmake_minimum_required(VERSION 3.15)
# C is here for tests/c_header_check.c: the C ABI header has to keep
# compiling as C, and only a C compiler can say so.
project(EWR VERSION 1.6.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime, for every target including the fetched libusb. Against
# the DLL one, ewr.exe crashed at startup on a PC whose VC++ redistributable
# predated our toolset (std::mutex in MSVCP140 14.29), and ewrc.dll would get
# whatever MSVCP140 its host process loaded first.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Kernels with 16 KB pages (Android 15 and later, Asahi Linux, some ARM
# servers) refuse ELF segments aligned for 4 KB. Aligned for 16 KB, a binary
# loads on both. Before any target, so every one of them gets it.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options("LINKER:-z,max-page-size=16384")
endif()

# On Android the app is the host: no command-line tool, no tests to run on
# the build machine, and no updater - the app downloads database.json itself.
if(ANDROID)
    set(EWR_HOST_DEFAULT OFF)
else()
    set(EWR_HOST_DEFAULT ON)
endif()
option(EWR_BUILD_CLI "Build the ewr command-line tool" ${EWR_HOST_DEFAULT})
option(EWR_WITH_UPDATER "Build the database update and release check" ${EWR_HOST_DEFAULT})

# libusb from one pinned source, linked statically: on Windows for the
# WinUSB-bound vendor interfaces, on Android because there is no system copy,
# on macOS so one universal binary needs no Homebrew, and in Linux release
# builds so an archive runs without a matching distribution package. A Linux
# developer build takes the distribution's.
if(WIN32 OR ANDROID OR APPLE)
    set(EWR_BUNDLED_LIBUSB_DEFAULT ON)
else()
    set(EWR_BUNDLED_LIBUSB_DEFAULT OFF)
endif()
option(EWR_BUNDLED_LIBUSB "Build the pinned libusb and link it statically" ${EWR_BUNDLED_LIBUSB_DEFAULT})

# The version string the self-updater compares against GitHub release tags is
# generated from the project() version above, so there is exactly one copy of it.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ewr/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ewr/version.h
    @ONLY
)

set(LIB_SOURCES
    src/backup.cpp src/database.cpp src/deviceid.cpp src/discovery.cpp src/ewr_c.cpp src/info.cpp
    src/json.cpp src/keysearch.cpp src/maintenance.cpp src/paths.cpp src/replay.cpp src/restore.cpp
    src/run_lock.cpp src/session.cpp
    src/protocol/ctrl.cpp src/protocol/d4session.cpp src/protocol/end4.cpp src/protocol/executor.cpp
    src/protocol/jig.cpp src/protocol/remote_mode.cpp src/protocol/replies.cpp
    src/transport/snmp.cpp src/transport/snmp_gateway.cpp src/transport/usb_composite.cpp
    src/transport/usb_driver.cpp src/transport/usb_gateway.cpp
    src/update/updater_common.cpp)

if(WIN32)
    list(APPEND LIB_SOURCES src/transport/usb_windows.cpp src/transport/usb_libusb.cpp src/update/updater_windows.cpp)
elseif(EWR_WITH_UPDATER)
    list(APPEND LIB_SOURCES src/transport/usb_libusb.cpp src/update/updater_linux.cpp)
else()
    list(APPEND LIB_SOURCES src/transport/usb_libusb.cpp src/update/updater_none.cpp)
endif()

add_library(ewr_core STATIC ${LIB_SOURCES})

target_include_directories(ewr_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)
# protocol/, transport/ and update/ are the library's own; hosts get include/ewr.
target_include_directories(ewr_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_link_libraries(ewr_core PUBLIC Threads::Threads)

if(EWR_BUNDLED_LIBUSB)
    include(FetchContent)
    set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    # Netlink instead of libudev: no runtime dependency on the system's udev.
    set(LIBUSB_ENABLE_UDEV OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libusb_cmake
        GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
        GIT_TAG v1.0.29-0
    )
    FetchContent_MakeAvailable(libusb_cmake)

    # ewrc is a shared library over this static one.
    set_property(TARGET usb-1.0 PROPERTY POSITION_INDEPENDENT_CODE ON)

    # Bare target name: the libusb::usb-1.0 alias is only on libusb-cmake's
    # main branch, which has no release tag.
    target_link_libraries(ewr_core PRIVATE usb-1.0)
elseif(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    target_include_directories(ewr_core PRIVATE ${LIBUSB_INCLUDE_DIRS})
    # Homebrew keeps its libs outside the default linker search path.
    target_link_directories(ewr_core PUBLIC ${LIBUSB_LIBRARY_DIRS})
    target_link_libraries(ewr_core PRIVATE ${LIBUSB_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(ewr_core PRIVATE setupapi winspool urlmon wininet ws2_32 iphlpapi)
elseif(EWR_WITH_UPDATER AND APPLE)
    # The SDK's libcurl carries both architectures; Homebrew's would not.
    find_package(CURL REQUIRED)
    target_link_libraries(ewr_core PRIVATE CURL::libcurl)
elseif(EWR_WITH_UPDATER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBCURL REQUIRED libcurl)
    target_include_directories(ewr_core PRIVATE ${LIBCURL_INCLUDE_DIRS})
    target_link_directories(ewr_core PUBLIC ${LIBCURL_LIBRARY_DIRS})
    target_link_libraries(ewr_core PRIVATE ${LIBCURL_LIBRARIES})
endif()

# --restore-all writes bytes no reset ever writes. It stays out of release
# builds until it is verified on hardware beyond the one model it was built on.
option(EWR_RESTORE_ALL "Expose --restore-all and ewr_restore_all" OFF)
if(EWR_RESTORE_ALL)
    target_compile_definitions(ewr_core PRIVATE EWR_RESTORE_ALL)
endif()

if(EWR_BUILD_CLI)
    add_executable(ewr cli/main.cpp cli/args.cpp cli/run.cpp cli/render.cpp cli/select.cpp cli/list.cpp
                       cli/status.cpp cli/dump.cpp cli/discovery.cpp cli/maintenance.cpp cli/restore.cpp
                       cli/reset.cpp)
    target_link_libraries(ewr PRIVATE ewr_core)
    if(EWR_RESTORE_ALL)
        target_compile_definitions(ewr PRIVATE EWR_RESTORE_ALL)
    endif()

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
endif()

# The C ABI (include/ewr/ewr_c.h) is in ewr_core for anyone linking
# statically; this builds it as a shared library for the callers that cannot -
# Python, C#, Rust, anything with an FFI. Only the ewr_* symbols are exported.
option(EWR_BUILD_C_SHARED "Build the C ABI as a shared library" ON)
if(EWR_BUILD_C_SHARED)
    set_property(TARGET ewr_core PROPERTY POSITION_INDEPENDENT_CODE ON)

    add_library(ewrc SHARED src/ewr_c.cpp)
    target_link_libraries(ewrc PRIVATE ewr_core)
    target_include_directories(ewrc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(ewrc PRIVATE EWR_C_BUILD_SHARED)
    set_target_properties(ewrc PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        VERSION ${PROJECT_VERSION}
    )
endif()

option(EWR_BUILD_TESTS "Build EWR unit tests" ${EWR_HOST_DEFAULT})
if(EWR_BUILD_TESTS)
    enable_testing()
    # Guards include/ewr/ewr_c.h against C++-only syntax: it is compiled as C
    # and linked against the same library a caller would link.
    add_executable(ewr_c_header_check tests/c_header_check.c)
    target_link_libraries(ewr_c_header_check PRIVATE ewr_core)
    add_test(NAME CApiHeader COMMAND ewr_c_header_check)

    add_executable(ewr_tests tests/main.cpp tests/test_backup.cpp tests/test_contracts.cpp tests/test_d4.cpp
                             tests/test_database.cpp tests/test_discovery.cpp tests/test_executor.cpp
                             tests/test_maintenance.cpp tests/test_output.cpp tests/test_packets.cpp
                             tests/test_session.cpp tests/test_snmp.cpp tests/test_updater.cpp tests/test_usb.cpp)
    target_link_libraries(ewr_tests PRIVATE ewr_core)
    target_include_directories(ewr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # Tests read database.json and the models/ folder, so they must run
    # from the repository root regardless of the build directory layout.
    add_test(NAME UnitTests COMMAND ewr_tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
