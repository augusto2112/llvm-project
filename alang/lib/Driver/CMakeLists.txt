add_executable(alang
  Driver.cpp
)

target_link_libraries(alang PRIVATE LLVMSupport Parser)
