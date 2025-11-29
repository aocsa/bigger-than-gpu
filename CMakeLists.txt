cmake_minimum_required(VERSION 3.25)

project(bigger-than-gpu
    VERSION 0.1.0
    DESCRIPTION "CPU/Disk focused batch processing with taskflow execution"
    LANGUAGES CXX
)

# Require C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(QUEST_BUILD_TESTS "Build tests" ON)
option(QUEST_BUILD_EXAMPLES "Build examples" ON)

# Compiler warnings
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
)

# Find Snappy compression library
find_package(Snappy REQUIRED)

# Library sources
set(QUEST_SOURCES
    src/quest/memory/memory_resource.cpp
    src/quest/cache/cache_data.cpp
    src/quest/cache/batch_holder.cpp
    src/quest/taskflow/port.cpp
    src/quest/taskflow/operator.cpp
    src/quest/taskflow/graph.cpp
)

# Create library
add_library(quest ${QUEST_SOURCES})
add_library(quest::quest ALIAS quest)

target_include_directories(quest
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

# Link required libraries
target_link_libraries(quest
    PUBLIC
        pthread
        Snappy::snappy
)

# Examples
if(QUEST_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(QUEST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS quest
    EXPORT quest-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY src/quest
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT quest-targets
    FILE quest-config.cmake
    NAMESPACE quest::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/quest
)
