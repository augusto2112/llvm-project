#include "Parser/Expr.h"
#include "Scanner/Scanner.h"
#include "Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

using namespace alang;
int main() {
  llvm::errs() << "saddsa\n";
  std::cout << "Hello world\n";
  std::string Source("(+ (+ 344 453) (+ (+ 4332 (+ 433 833) ) 999) )");
  Scanner Scanner(std::move(Source));
  Parser Parser(std::move(Scanner));
  auto Expr = Parser.parseExpression();
  ExprPrinter Printer(*Expr.get());

  return 0;
}
