
#ifndef ALANG_SCANNER_H
#define ALANG_SCANNER_H

#include "Token.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace alang {
struct Scanner {
  explicit Scanner(const std::string &&);

  Token lexToken();

  bool isAtEnd() const { return Current.empty(); }
private:
  void advance();

  uint64_t parseDigit();

  std::string Input;
  llvm::StringRef Current;
};

} // namespace alang
#endif
