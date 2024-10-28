#ifndef ALANG_IRGEN_H
#define ALANG_IRGEN_H

#include "llvm/IR/IRBuilder.h"
#include "../Parser/Expr.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"


namespace alang {
struct IRGen : ValueExprVisitor {
  IRGen(Expr &E); 
  llvm::Value *valueVisit(IntegerExpr &) override;
  llvm::Value *valueVisit(BinaryExpr &) override;

  llvm::Function *getMainFunction() const { return MainFunction; }
  ~IRGen() = default;
private:
  llvm::LLVMContext TheContext;
  llvm::Module TheModule;
  llvm::IRBuilder<> Builder;
  llvm::Function *MainFunction;
  llvm::BasicBlock *BB;
};
} // namespace alang

#endif
