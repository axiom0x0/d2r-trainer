cmake_minimum_required(VERSION 3.16)
project(d2r-trainer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Shared source files (memory, process, savefile)
set(COMMON_SRCS
    src/process.cpp
    src/memory.cpp
    src/savefile.cpp
)

# CLI version (original, readline-based)
add_executable(d2r-trainer
    src/main.cpp
    ${COMMON_SRCS}
)
target_include_directories(d2r-trainer PRIVATE src)
target_link_libraries(d2r-trainer PRIVATE readline pthread)

# TUI version (ncurses)
add_executable(d2r-trainer-tui
    src/main_tui.cpp
    ${COMMON_SRCS}
)
target_include_directories(d2r-trainer-tui PRIVATE src)
target_link_libraries(d2r-trainer-tui PRIVATE ncurses pthread)
