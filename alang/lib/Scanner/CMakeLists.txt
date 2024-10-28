ADD_LIBRARY(Scanner STATIC
  Scanner.cpp
  Token.cpp
)
target_link_libraries(Scanner PRIVATE LLVMSupport LLVMCore)
