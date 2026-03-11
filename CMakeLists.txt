cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SERAPH_VERSION LIMIT_COUNT 1)
if(NOT SERAPH_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION file must contain SemVer 'MAJOR.MINOR.PATCH'.")
endif()

project(Seraph VERSION ${SERAPH_VERSION} LANGUAGES CXX)

# This targets Apple Silicon only.
if(NOT APPLE)
    message(FATAL_ERROR "Seraph currently supports macOS on Apple Silicon only.")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "Seraph currently supports Apple Silicon (arm64) only.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SERAPH_BUILD_TESTS "Build Seraph tests" ON)
option(SERAPH_ENABLE_INSTALL "Enable install/export rules" ON)

# Header-first library
add_library(seraph INTERFACE)
add_library(seraph::seraph ALIAS seraph)

target_compile_features(seraph INTERFACE cxx_std_23)
target_include_directories(seraph
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(SERAPH_BUILD_TESTS)
    enable_testing()

    add_executable(seraph_tests
        tests/basic_compile_test.cpp
    )
    target_link_libraries(seraph_tests PRIVATE seraph::seraph)
    add_test(NAME seraph_tests COMMAND seraph_tests)

    add_executable(seraph_stack_perf
        tests/stack_performance_test.cpp
    )
    target_link_libraries(seraph_stack_perf PRIVATE seraph::seraph)

    add_executable(seraph_queue_perf
        tests/queue_performance_test.cpp
    )
    target_link_libraries(seraph_queue_perf PRIVATE seraph::seraph)

    add_executable(seraph_linearizability_tests
        tests/linearizability_test.cpp
    )
    target_link_libraries(seraph_linearizability_tests PRIVATE seraph::seraph)
    add_test(NAME seraph_linearizability_tests COMMAND seraph_linearizability_tests)

    find_package(Boost CONFIG QUIET)
    if(Boost_FOUND)
        if(TARGET Boost::headers)
            target_link_libraries(seraph_stack_perf PRIVATE Boost::headers)
            target_link_libraries(seraph_queue_perf PRIVATE Boost::headers)
        elseif(TARGET Boost::boost)
            target_link_libraries(seraph_stack_perf PRIVATE Boost::boost)
            target_link_libraries(seraph_queue_perf PRIVATE Boost::boost)
        else()
            target_include_directories(seraph_stack_perf PRIVATE ${Boost_INCLUDE_DIRS})
            target_include_directories(seraph_queue_perf PRIVATE ${Boost_INCLUDE_DIRS})
        endif()
    elseif(EXISTS "/opt/homebrew/include/boost")
        target_include_directories(seraph_stack_perf PRIVATE /opt/homebrew/include)
        target_include_directories(seraph_queue_perf PRIVATE /opt/homebrew/include)
    endif()
endif()

if(SERAPH_ENABLE_INSTALL)
    install(
        TARGETS seraph
        EXPORT seraphTargets
    )

    install(
        DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(
        EXPORT seraphTargets
        FILE seraphTargets.cmake
        NAMESPACE seraph::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/seraph
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/seraphConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/seraphConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/seraph
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/seraphConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/seraphConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/seraphConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/seraph
    )
endif()

find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE SERAPH_SOURCE_FILES
        include/*.hpp
        src/*.cpp
        tests/*.cpp
    )

    if(SERAPH_SOURCE_FILES)
        add_custom_target(
            format
            COMMAND ${CLANG_FORMAT_EXE} -i ${SERAPH_SOURCE_FILES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-format on Seraph sources"
        )
    endif()
endif()
