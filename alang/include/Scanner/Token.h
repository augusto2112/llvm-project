#ifndef ALANG_TOKEN_H
#define ALANG_TOKEN_H

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace alang {
struct Token {
  enum class Type {
    close_parenthesis,
    eof,
    number_literal,
    open_parenthesis,
    plus,
  };

  Token(Type Type, std::optional<llvm::StringRef> Lexeme = std::nullopt,
        std::optional<uint64_t> Value = std::nullopt)
      : TheType(Type), Lexeme(Lexeme), Value(Value) {}

  Type getType() const { return TheType; }

  llvm::StringRef getLexeme() { return *Lexeme; }

  uint64_t getIntValue() const { return *Value; }

  void dump() const;

private:
  llvm::StringRef getTypeString() const;
  Type TheType;
  std::optional<llvm::StringRef> Lexeme;
  std::optional<uint64_t> Value;
};

} // namespace alang
#endif

