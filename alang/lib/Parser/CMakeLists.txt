ADD_LIBRARY(Parser STATIC
  Parser.cpp
  Token.cpp
)
target_link_libraries(Parser PRIVATE LLVMSupport)
