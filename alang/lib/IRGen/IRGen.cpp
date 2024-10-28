#include "IRGen/IRGen.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/ErrorHandling.h"

using namespace alang;

IRGen::IRGen(Expr &E) : TheModule("alang module", TheContext), Builder(TheContext) {
  auto *ReturnType = llvm::Type::getInt32Ty(TheContext);
  auto *FunctionType = llvm::FunctionType::get(ReturnType, {}, false);
  MainFunction = llvm::Function::Create(
      FunctionType, llvm::Function::ExternalLinkage, "main", TheModule);
  BB = llvm::BasicBlock::Create(TheContext, "entry", MainFunction);
  Builder.SetInsertPoint(BB);
  assert(MainFunction);
  llvm::Value *RV = E.accept(*this);
  Builder.CreateRet(RV);
  MainFunction->dump();
}
llvm::Value *IRGen::valueVisit(IntegerExpr &IE) {
  // Creating a phi node is a hack to keep constant propagation from optimizing
  // all othe additions away.
  llvm::PHINode *Phi = Builder.CreatePHI(llvm::Type::getInt64Ty(TheContext), 1);
  llvm::ConstantInt *Value =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(TheContext), IE.getValue());
  Phi->addIncoming(Value, BB);
  return Phi;
}

llvm::Value *IRGen::valueVisit(BinaryExpr &BE) {
  llvm::Value *LHS = BE.getLHS().accept(*this);
  llvm::Value *RHS = BE.getRHS().accept(*this);
  const Token &Op = BE.getOperator();
  switch (Op.getType()) {
  case Token::Type::plus:
    return Builder.CreateAdd(LHS, RHS);
  default:
    llvm_unreachable("Unexpected token type");
  }
}
