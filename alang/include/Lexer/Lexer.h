
#ifndef ALANG_LEXER_H
#define ALANG_LEXER_H

#include "Token.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace alang {
struct Lexer {
  explicit Lexer(const std::string &&);

  Token lexToken();
private:
  void advance();

  uint64_t parseDigit();

  std::string Input;
  llvm::StringRef Current;
};

} // namespace alang
#endif
