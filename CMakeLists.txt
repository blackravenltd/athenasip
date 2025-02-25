cmake_minimum_required(VERSION 3.31)
project(athenasip LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use new policy behaviours (if not already default in 3.31.5)
cmake_policy(SET CMP0177 NEW)
cmake_policy(SET CMP0167 NEW)

# FetchContent module
include(FetchContent)

# Use /usr/local also
list(APPEND CMAKE_PREFIX_PATH "/usr/local")

# Find Boost libraries, including system and thread
find_package(Boost 1.87 REQUIRED COMPONENTS system thread)

# Find OpenSSL
if(APPLE)
  set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
  set(OPENSSL_LIBRARIES "/usr/local/opt/openssl@3/lib/libssl.dylib;/usr/local/opt/openssl@3/lib/libcrypto.dylib")
  set(OPENSSL_INCLUDE_DIR "/usr/local/opt/openssl@3/include")
  link_directories(/usr/local/opt/openssl@3/lib)
endif()

find_package(OpenSSL REQUIRED)

# Find MySQL C++ Connector
find_package(mysql-concpp REQUIRED)

# Find libpqxx using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Add libpqxx include directories to the project
include_directories(${PQXX_INCLUDE_DIRS})
link_directories(${PQXX_LIBRARY_DIRS})
add_definitions(${PQXX_CFLAGS_OTHER})

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

# ---- Add SQLite support ----
find_package(SQLite3 REQUIRED)
message(STATUS "SQLite3 Include Directory: ${SQLite3_INCLUDE_DIRS}")
message(STATUS "SQLite3 Libraries: ${SQLite3_LIBRARIES}")

# Collect all source files from the src directory
file(GLOB_RECURSE ATHENA_SOURCES src/*.cpp)

# Create the executable
add_executable(athenasip ${ATHENA_SOURCES})

# Include directories
target_include_directories(athenasip PRIVATE 
    ${Boost_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(athenasip PRIVATE
    Boost::system
    Boost::thread
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    yaml-cpp::yaml-cpp
    tinyxml2::tinyxml2
    ${PQXX_LIBRARIES}        # Link PostgreSQL libpqxx
    ${SQLite3_LIBRARIES}     # Link SQLite3 library
)

# Output build messages
message(STATUS "Building AthenaSIP (athenasip) with C++17")
message(STATUS "Boost Include Directory: ${Boost_INCLUDE_DIRS}")
message(STATUS "OpenSSL Include Directory: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "libpqxx Include Directory: ${PQXX_INCLUDE_DIRS}")
