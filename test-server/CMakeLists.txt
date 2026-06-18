cmake_minimum_required(VERSION 3.15)
project(TestServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find libuv
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUV REQUIRED libuv)

# Include directories
include_directories(${LIBUV_INCLUDE_DIRS})
include_directories(include)

# Source files
set(SOURCES
    src/server.cpp
    src/protocol.cpp
)

set(HEADERS
    include/protocol.h
)

# Executable
add_executable(test_server ${SOURCES} ${HEADERS})
target_link_libraries(test_server ${LIBUV_LIBRARIES})
target_compile_options(test_server PRIVATE ${LIBUV_CFLAGS_OTHER})

# Install
install(TARGETS test_server
    RUNTIME DESTINATION bin
)
