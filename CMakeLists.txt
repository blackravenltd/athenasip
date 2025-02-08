# 
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
# Copyright (C) 2024 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
# 
cmake_minimum_required(VERSION 3.14)
project(athenasip LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Find Boost libraries
find_package(Boost 1.84 REQUIRED COMPONENTS system thread program_options)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find MySQL Connector/C++

# --- MacOS ---
find_library(MYSQL_CONCPP_LIBRARY mysqlcppconnx-static
    PATHS /usr/local/mysql/connector-c++-9.2.0/lib64
    NO_DEFAULT_PATH
)
set(MYSQL_INCLUDE_DIR "/usr/local/mysql/connector-c++-9.2.0/include/")
# --- End MacOS ---

if(NOT MYSQL_CONCPP_LIBRARY)
    message(FATAL_ERROR "MySQL Connector/C++ static library not found!")
endif()

# YAML
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# Collect all source files from the src directory
file(GLOB_RECURSE ATHENA_SOURCES src/*.cpp)

# Create the executable
add_executable(athenasip ${ATHENA_SOURCES})

# Include directories
target_include_directories(athenasip PRIVATE ${Boost_INCLUDE_DIRS} ${MYSQL_INCLUDE_DIR})

# Link libraries
target_link_libraries(athenasip PRIVATE 
    Boost::system 
    Boost::thread 
    Boost::program_options 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    ${MYSQL_CONCPP_LIBRARY} resolv
    yaml-cpp::yaml-cpp)

# Output build messages
message(STATUS "Building AthenaSIP (athenasip) with C++17")
message(STATUS "Boost Include Directory: ${Boost_INCLUDE_DIRS}")
message(STATUS "OpenSSL Include Directory: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "MySQL Library: ${MYSQL_CONCPP_LIBRARY}")
