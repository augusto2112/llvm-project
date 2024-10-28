ADD_LIBRARY(Parser STATIC
  Expr.cpp
  Parser.cpp
)
target_link_libraries(Parser PRIVATE LLVMSupport)
