
#ifndef ALANG_PARSER_H
#define ALANG_PARSER_H

#include "Token.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace alang {
struct Parser {
  explicit Parser(const std::string &&);

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
