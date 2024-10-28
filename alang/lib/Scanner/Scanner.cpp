
#include "Scanner/Scanner.h"
#include "Scanner/Token.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <string>

using namespace alang;

Scanner::Scanner(const std::string &&Input) {
  this->Input = std::move(Input);
  this->Current = llvm::StringRef(Input);
}

uint64_t Scanner::parseDigit() {
  llvm::APInt Result;
  auto Failure = Current.consumeInteger(10, Result);
  assert(!Failure);
  auto Digit = Result.getLimitedValue();
  assert(Digit != UINT64_MAX);
  return Digit;;
}


Token Scanner::lexToken() {
  while (!Current.empty() && Current.front() == ' ')
    Current = Current.drop_front();

  if (Current.empty()) {
    return Token(Token::Type::eof);
  }
  switch (Current.front()) {
  case '(': {
    Current = Current.drop_front();
    return Token(Token::Type::open_parenthesis);
  }
  case ')': {
    Current = Current.drop_front();
    return Token(Token::Type::close_parenthesis);
  }

  case '+': {
    Current = Current.drop_front();
    return Token(Token::Type::plus);
  }

  default: {
    if (isdigit(Current.front())) {
      auto Digit = parseDigit();
      return Token(Token::Type::number_literal, {}, Digit);
    }
    llvm::errs() << Current.front() << "\n";
    llvm_unreachable("Unexpected token");
  }
  }
}

