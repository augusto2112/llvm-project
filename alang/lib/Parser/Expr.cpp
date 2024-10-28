#include "Parser/Expr.h"
#include "llvm/Support/raw_ostream.h"

using namespace alang;

void ExprPrinter::visit(IntegerExpr &Integer) {
  std::string Spaces(Indent, ' ');
  llvm::errs() << Spaces << "[ Integer = " << Integer.getValue() << " ]\n";
}

void ExprPrinter::visit(BinaryExpr &Binary) {
  std::string Spaces(Indent, ' ');
  llvm::errs() << Spaces << "["; 
  Indent += 2;
  {
    std::string Spaces(Indent, ' ');
    llvm::errs() << "\n" << Spaces << "Binary = \"";
    Binary.getOperator().dump();
    llvm::errs() << "\"";
    llvm::errs() << "\n" << Spaces << "LHS = ";
    Binary.getLHS().accept(*this);
    llvm::errs() << Spaces << "RHS = ";
    Binary.getRHS().accept(*this);
  }
  {
    Indent -= 2;
    std::string Spaces(Indent, ' ');
    llvm::errs() << Spaces << "]\n";
  }
}
