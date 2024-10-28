cmake_minimum_required(VERSION 3.20.0)
set(LLVM_SUBPROJECT_TITLE "Alang")

include_directories(BEFORE
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

include_directories(${LLVM_INCLUDE_DIRS})
link_directories("${LLVM_LIBRARY_DIR}")

add_subdirectory(lib)
