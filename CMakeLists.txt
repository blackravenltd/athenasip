cmake_minimum_required(VERSION 3.23...4.2)

project(athenasip VERSION 0.8.0 LANGUAGES CXX)

option(ATHENA_BUILD_TESTING "Build test targets" OFF)
set(ATHENA_SANITIZE "none" CACHE STRING "Sanitizer to build with: none, address, thread")
set_property(CACHE ATHENA_SANITIZE PROPERTY STRINGS none address thread)

# Single-config generators leave CMAKE_BUILD_TYPE empty: no optimisation, no debug info.
get_property(ATHENA_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(NOT ATHENA_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ATHENA_SANITIZE STREQUAL "address")
    set(ATHENA_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
elseif(ATHENA_SANITIZE STREQUAL "thread")
    set(ATHENA_SANITIZE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT ATHENA_SANITIZE STREQUAL "none")
    message(FATAL_ERROR "ATHENA_SANITIZE must be one of: none, address, thread")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/build_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_version.h"
    @ONLY
)

function(athena_add_prefix prefix)
    if(prefix AND IS_DIRECTORY "${prefix}")
        list(PREPEND CMAKE_PREFIX_PATH "${prefix}")
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)

        if(IS_DIRECTORY "${prefix}/lib/pkgconfig")
            list(PREPEND ATHENA_PKG_CONFIG_PATHS "${prefix}/lib/pkgconfig")
            set(ATHENA_PKG_CONFIG_PATHS "${ATHENA_PKG_CONFIG_PATHS}" PARENT_SCOPE)
        endif()

        if(IS_DIRECTORY "${prefix}/share/pkgconfig")
            list(PREPEND ATHENA_PKG_CONFIG_PATHS "${prefix}/share/pkgconfig")
            set(ATHENA_PKG_CONFIG_PATHS "${ATHENA_PKG_CONFIG_PATHS}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

if(APPLE)
    athena_add_prefix("/usr/local")
    athena_add_prefix("/opt/homebrew")

    find_program(ATHENA_BREW brew)

    if(ATHENA_BREW)
        foreach(ATHENA_FORMULA
            boost
            openssl@3
            yaml-cpp
            libnghttp2
            googletest
        )
            execute_process(
                COMMAND "${ATHENA_BREW}" --prefix "${ATHENA_FORMULA}"
                RESULT_VARIABLE ATHENA_BREW_RESULT
                OUTPUT_VARIABLE ATHENA_BREW_PREFIX
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            if(ATHENA_BREW_RESULT EQUAL 0)
                athena_add_prefix("${ATHENA_BREW_PREFIX}")
            endif()
        endforeach()
    endif()

    if(CMAKE_PREFIX_PATH)
        list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    endif()

    if(ATHENA_PKG_CONFIG_PATHS)
        list(REMOVE_DUPLICATES ATHENA_PKG_CONFIG_PATHS)
        list(JOIN ATHENA_PKG_CONFIG_PATHS ":" ATHENA_PKG_CONFIG_PATH)

        if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
            set(ENV{PKG_CONFIG_PATH} "${ATHENA_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
        else()
            set(ENV{PKG_CONFIG_PATH} "${ATHENA_PKG_CONFIG_PATH}")
        endif()
    endif()
endif()

find_package(Boost 1.87.0 CONFIG REQUIRED COMPONENTS thread json charconv)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

find_package(yaml-cpp CONFIG REQUIRED)

# APNs speaks only HTTP/2.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

if(TARGET yaml-cpp::yaml-cpp)
    set(ATHENA_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(ATHENA_YAML_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "yaml-cpp was found, but no usable target was provided")
endif()

file(GLOB_RECURSE ATHENA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

list(REMOVE_ITEM ATHENA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

# Lua scripting is not built, and the runtime has no Lua dependency.
list(FILTER ATHENA_SOURCES EXCLUDE REGEX "/src/script/")

# The rtpproxy client is not built: nothing constructs it. It is the starting point for
# an rtpproxy MediaEngine driver.
list(FILTER ATHENA_SOURCES EXCLUDE REGEX "/src/rtp/rtp_proxy_client")

add_library(athena_core STATIC ${ATHENA_SOURCES})

target_compile_features(athena_core PUBLIC cxx_std_20)

target_include_directories(athena_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
)

if(ATHENA_SANITIZE_FLAGS)
    target_compile_options(athena_core PUBLIC ${ATHENA_SANITIZE_FLAGS})
    target_link_options(athena_core PUBLIC ${ATHENA_SANITIZE_FLAGS})
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_include_directories(athena_core
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    )
endif()

target_link_libraries(athena_core
    PUBLIC
        Boost::thread
        Boost::json
        Boost::charconv
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::NGHTTP2
        "${ATHENA_YAML_TARGET}"
)

add_executable(athenasip "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

target_link_libraries(athenasip PRIVATE athena_core)

# Plugin modules (plugins.path) use the server's own code, so it exports its symbols.
set_target_properties(athenasip PROPERTIES ENABLE_EXPORTS ON)
target_compile_features(athenasip PRIVATE cxx_std_20)

if(MSVC AND TARGET OpenSSL::applink)
    target_link_libraries(athenasip PRIVATE OpenSSL::applink)
endif()

if(ATHENA_BUILD_TESTING)
    include(CTest)
    enable_testing()

    find_package(GTest REQUIRED)

    file(GLOB_RECURSE ATHENA_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )

    # The test modules are shared libraries of their own, loaded by the tests.
    list(FILTER ATHENA_TEST_SOURCES EXCLUDE REGEX "/tests/modules/")

    add_executable(athenasip_tests ${ATHENA_TEST_SOURCES})
    set_target_properties(athenasip_tests PROPERTIES ENABLE_EXPORTS ON)

    # Each in a directory of its own, as plugins.path would name them. They are built against the headers and
    # never link athena_core, which would give them a registry of their own.
    foreach(module sample_module mismatched_module not_a_module)
        add_library(${module} MODULE "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/${module}.cpp")
        target_include_directories(${module} PRIVATE $<TARGET_PROPERTY:athena_core,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_features(${module} PRIVATE cxx_std_20)
        set_target_properties(${module} PROPERTIES PREFIX "" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-modules/${module}")
        if(APPLE)
            target_link_options(${module} PRIVATE "-undefined" "dynamic_lookup")
        endif()
        add_dependencies(athenasip_tests ${module})
    endforeach()
    target_compile_definitions(athenasip_tests PRIVATE ATHENA_TEST_MODULE_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-modules")

    target_link_libraries(athenasip_tests PRIVATE athena_core)

    if(TARGET GTest::gtest_main)
        target_link_libraries(athenasip_tests PRIVATE GTest::gtest_main)
    elseif(TARGET GTest::Main)
        target_link_libraries(athenasip_tests PRIVATE GTest::Main)
    else()
        message(FATAL_ERROR "GTest was found, but no usable main target was provided")
    endif()

    target_compile_features(athenasip_tests PRIVATE cxx_std_20)

    # The TLS listener tests need the certificates, and tests run in the build tree.
    target_compile_definitions(athenasip_tests PRIVATE ATHENA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    if(MSVC AND TARGET OpenSSL::applink)
        target_link_libraries(athenasip_tests PRIVATE OpenSSL::applink)
    endif()

    include(GoogleTest)
    gtest_discover_tests(athenasip_tests DISCOVERY_TIMEOUT 60)
endif()
# --- Install ------------------------------------------------------------------------
#
# The binary, a systemd unit, and a configuration file that is never overwritten.

include(GNUInstallDirs)

# Live configuration: /etc for a system prefix, under the prefix otherwise.
if(NOT DEFINED ATHENA_CONFIG_INSTALL_DIR)
    if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
        set(ATHENA_CONFIG_INSTALL_DIR "/etc/athenasip")
    else()
        set(ATHENA_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/etc/athenasip")
    endif()
endif()

install(TARGETS athenasip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The annotated example is always installed with the docs, as a reference.
install(FILES config/config.example.yaml
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME config.example.yaml)

# It becomes the live configuration only where none exists, so an upgrade never
# replaces one. install(CODE) does not prepend DESTDIR, so it is applied by hand.
install(CODE "
    set(ATHENA_CONFIG_DIR \"${ATHENA_CONFIG_INSTALL_DIR}\")

    if(NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")
        set(ATHENA_CONFIG_DIR \"\$ENV{DESTDIR}/\${ATHENA_CONFIG_DIR}\")
    endif()

    if(EXISTS \"\${ATHENA_CONFIG_DIR}/config.yaml\")
        message(STATUS \"Keeping existing \${ATHENA_CONFIG_DIR}/config.yaml\")
    else()
        message(STATUS \"Installing \${ATHENA_CONFIG_DIR}/config.yaml\")
        file(MAKE_DIRECTORY \"\${ATHENA_CONFIG_DIR}\")
        file(COPY_FILE \"${CMAKE_CURRENT_SOURCE_DIR}/config/config.example.yaml\"
                       \"\${ATHENA_CONFIG_DIR}/config.yaml\")
    endif()
")

# The systemd unit, in the directory for packaged units. ATHENA_SYSTEMD_UNIT_DIR
# overrides it.
if(NOT DEFINED ATHENA_SYSTEMD_UNIT_DIR)
    set(ATHENA_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")

    if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
        set(ATHENA_SYSTEMD_UNIT_DIR "/lib/systemd/system")
    endif()
endif()

if(UNIX AND NOT APPLE)
    # Generated, so ExecStart follows the install prefix.
    configure_file(packaging/athenasip.service.in
                   ${CMAKE_CURRENT_BINARY_DIR}/athenasip.service @ONLY)

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/athenasip.service
            DESTINATION ${ATHENA_SYSTEMD_UNIT_DIR})
endif()
