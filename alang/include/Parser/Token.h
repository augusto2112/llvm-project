#ifndef ALANG_TOKEN_H
#define ALANG_TOKEN_H


#include "llvm/ADT/StringRef.h"
#include <optional>
struct Token {
  enum class Type {
    close_parenthesis,
    number_literal,
    open_parenthesis,
    plus,
  };

  Token(Type Type, std::optional<llvm::StringRef> Lexeme = std::nullopt,
        std::optional<uint64_t> Value = std::nullopt)
      : Type(Type), Lexeme(Lexeme), Value(Value) {}

  Type getType() const { return Type; }

  llvm::StringRef getLexeme() { return *Lexeme; }

  uint64_t getIntValue() const { return *Value; }

  void dump();

private:
  llvm::StringRef getTypeString() const;
  Type Type;
  std::optional<llvm::StringRef> Lexeme;
  std::optional<uint64_t> Value;
};

#endif

