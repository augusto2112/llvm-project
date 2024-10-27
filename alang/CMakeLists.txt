cmake_minimum_required(VERSION 3.20.0)
set(LLVM_SUBPROJECT_TITLE "Alang")

include_directories(${LLVM_INCLUDE_DIRS})
link_directories("${LLVM_LIBRARY_DIR}")

add_subdirectory(lib)
