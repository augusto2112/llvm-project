#include "IRGen/IRGen.h"
#include "Parser/Expr.h"
#include "Parser/Parser.h"
#include "Scanner/Scanner.h"

using namespace alang;

int main() {
  std::string Source("(+ (+ (+ 344 453) (+ (+ 4332 (+ 433 833) ) 999) ) (+ 234 (+ (+ 329432 93) 923823) ) )");
  Scanner Scanner(std::move(Source));
  Parser Parser(std::move(Scanner));
  auto Expr = Parser.parseExpression();
  ExprPrinter Printer(*Expr.get());
  IRGen Gen(*Expr.get());

  return 0;
}
