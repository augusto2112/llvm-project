ADD_LIBRARY(Lexer STATIC
  Lexer.cpp
)
target_link_libraries(Lexer PRIVATE LLVMSupport)
