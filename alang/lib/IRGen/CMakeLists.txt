ADD_LIBRARY(IRGen STATIC
  IRGen.cpp
)
target_link_libraries(IRGen PRIVATE LLVMSupport LLVMCore)
