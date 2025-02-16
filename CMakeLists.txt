cmake_minimum_required(VERSION 3.14)
project(athenasip LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use new policy behaviours
cmake_policy(SET CMP0177 NEW)
cmake_policy(SET CMP0167 NEW)

# FetchContent
include(FetchContent)

# Use /usr/local also
list(APPEND CMAKE_PREFIX_PATH "/usr/local")

# Find Boost libraries, including system and thread
find_package(Boost 1.87 REQUIRED COMPONENTS system thread)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Workaround so MySQL Connector/C++ can find OpenSSL if it's in a nonstandard location (MacOSX)
add_library(my_mysql_openssl INTERFACE)
target_link_libraries(my_mysql_openssl INTERFACE OpenSSL::SSL OpenSSL::Crypto)
add_library(mysql::openssl ALIAS my_mysql_openssl)

# Find MySQL Connector/C++ dynamic library
find_package(mysql-concpp CONFIG REQUIRED)

# YAML: Use yaml-cpp instead of RapidYAML
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# tinyxml2
FetchContent_Declare(
  tinyxml2
  GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
  GIT_TAG 9.0.0  
)
FetchContent_MakeAvailable(tinyxml2)

# Collect all source files from the src directory
file(GLOB_RECURSE ATHENA_SOURCES src/*.cpp)

# Create the executable
add_executable(athenasip ${ATHENA_SOURCES})

# Include directories (Boost is added via target usage requirements)
target_include_directories(athenasip PRIVATE ${Boost_INCLUDE_DIRS})

# Link libraries
target_link_libraries(athenasip PRIVATE 
    Boost::system 
    Boost::thread 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    mysql::concpp-xdevapi
    yaml-cpp::yaml-cpp
    tinyxml2::tinyxml2
)

# Output build messages
message(STATUS "Building AthenaSIP (athenasip) with C++17")
message(STATUS "Boost Include Directory: ${Boost_INCLUDE_DIRS}")
message(STATUS "OpenSSL Include Directory: ${OPENSSL_INCLUDE_DIR}")
