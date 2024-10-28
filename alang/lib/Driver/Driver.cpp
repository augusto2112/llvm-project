#include "Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

using namespace alang;
int main() {
  llvm::errs() << "saddsa\n";
  std::cout << "Hello world\n";
  std::string Source("(433 + 453)");
  Parser Parser(std::move(Source));
  while (!Parser.isAtEnd()) {
    auto Token = Parser.lexToken();
    Token.dump();;

  }
  return 0;
}
