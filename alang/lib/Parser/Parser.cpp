
#include "Parser/Parser.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/ErrorHandling.h"
#include <cstdint>
#include <string>

using namespace alang;

Parser::Parser(const std::string &&Input) {
  this->Input = std::move(Input);
  this->Current = llvm::StringRef(Input);
}

uint64_t Parser::parseDigit() {
  llvm::APInt Result;
  auto Failure = Current.consumeInteger(10, Result);
  assert(!Failure);
  auto Digit = Result.getLimitedValue();
  assert(Digit != UINT64_MAX);
  return Digit;
}

Token Parser::lexToken() {
  switch (Current.front()) {
  case '(': {
    static_cast<void>(Current.drop_front());
    return Token(Token::Type::open_parenthesis);
  }
  case ')': {
    static_cast<void>(Current.drop_front());
    return Token(Token::Type::close_parenthesis);
  }

  case '+': {
    static_cast<void>(Current.drop_front());
    return Token(Token::Type::plus);
  }

  default: {
    if (isdigit(Current.front())) {
      auto Digit = parseDigit();
      return Token(Token::Type::number_literal, {}, Digit);
    }
  }
  }
  llvm_unreachable("Unexpected token");
}

