cmake_minimum_required(VERSION 3.25)

project(bigger-than-gpu
    VERSION 0.1.0
    DESCRIPTION "CPU/Disk focused batch processing with taskflow execution"
    LANGUAGES CXX
)

# Default to Debug for single-config generators (Makefiles, Ninja)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Require C++26
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS: Homebrew prefix (centralizes the path so tests/CMakeLists.txt doesn't need it)
if(APPLE)
    list(INSERT CMAKE_PREFIX_PATH 0 "/opt/homebrew")
endif()

# Build options
option(QUEST_BUILD_TESTS "Build tests" ON)
option(QUEST_BUILD_EXAMPLES "Build examples" ON)
option(QUEST_SANITIZERS "Enable sanitizers (asan, tsan, ubsan)" "")

# Compiler warnings
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
)

# Sanitizers
if(QUEST_SANITIZERS)
    foreach(sanitizer IN ITEMS ${QUEST_SANITIZERS})
        add_compile_options(-fsanitize=${sanitizer})
        add_link_options(-fsanitize=${sanitizer})
    endforeach()
endif()

# Find Snappy compression library
find_package(Snappy REQUIRED)

# Library sources
set(QUEST_SOURCES
    src/quest/memory/memory_resource.cpp
    src/quest/cache/data_block.cpp
    src/quest/cache/batch_holder.cpp
    src/quest/taskflow/port.cpp
    src/quest/taskflow/operator.cpp
    src/quest/taskflow/graph.cpp
    src/quest/taskflow/graph_builder.cpp
    src/quest/taskflow/prepared_query.cpp
    src/quest/taskflow/pipeline.cpp
    src/quest/taskflow/meta_pipeline.cpp
    src/quest/taskflow/pipeline_executor.cpp
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
