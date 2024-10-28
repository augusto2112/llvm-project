#include "Lexer/Lexer.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>

using namespace alang;
int main() {
  llvm::errs() << "saddsa\n";
  std::cout << "Hello world\n";
  Lexer Lexer("433");
  Lexer.lexToken();
  return 0;
}
