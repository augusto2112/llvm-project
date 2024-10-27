#include <iostream>
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

int main() {
  llvm::StringRef f("SA");
  llvm::errs() << "saddsa\n";
  std::cout << "Hello world\n";
  return 0;
}
