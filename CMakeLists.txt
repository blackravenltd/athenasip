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

# Find Boost (header-only mode by default)
find_package(Boost 1.84 REQUIRED CONFIG COMPONENTS system thread program_options)
find_package(OpenSSL REQUIRED)

# Collect all source files from the src directory
file(GLOB_RECURSE ATHENA_SOURCES src/*.cpp)

# Create the executable
add_executable(athenasip ${ATHENA_SOURCES})

# Include Boost headers
target_include_directories(athenasip PRIVATE ${Boost_INCLUDE_DIRS})

# If linking against Boost libraries is required, specify them here:
target_link_libraries(athenasip PRIVATE Boost::system Boost::thread Boost::program_options OpenSSL::SSL OpenSSL::Crypto)

# Output build messages
message(STATUS "Building AthenaSIP (athenasip) with C++17")
message(STATUS "Boost Include Directory: ${Boost_INCLUDE_DIRS}")
