cmake_minimum_required(VERSION 3.15)
project(TcpClient VERSION 1.0.0 LANGUAGES CXX)

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
    src/tcp_client.cpp
    src/protocol.cpp
)

set(HEADERS
    include/tcp_client.h
    include/protocol.h
)

# Library
add_library(tcpclient STATIC ${SOURCES} ${HEADERS})
target_link_libraries(tcpclient ${LIBUV_LIBRARIES})
target_compile_options(tcpclient PRIVATE ${LIBUV_CFLAGS_OTHER})

# Test executable
add_executable(tcp_client_test test/main.cpp)
target_link_libraries(tcp_client_test tcpclient ${LIBUV_LIBRARIES})
target_compile_options(tcp_client_test PRIVATE ${LIBUV_CFLAGS_OTHER})

# Install
install(TARGETS tcpclient tcp_client_test
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
