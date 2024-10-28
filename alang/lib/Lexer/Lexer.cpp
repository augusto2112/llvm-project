
#include "Lexer/Lexer.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <string>

using namespace alang;

Lexer::Lexer(const std::string &&Input) {
  this->Input = std::move(Input);
  this->Current = llvm::StringRef(Input);
}

void Lexer::advance() {
  Current = Current.drop_front();

}
uint64_t Lexer::parseDigit() {
  // size_t End = 0;
  // while (Current.size() < End && isdigit(Current[End]))
  //   End++;
  // auto Digit = Current.drop_front(End);
  llvm::APInt Result;
  assert(!Current.consumeInteger(10, Result));
  auto Digit = Result.getLimitedValue();
  assert(Digit != UINT64_MAX);
  return Digit;
}

Token Lexer::lexToken() {
  advance();
  switch (Current.front()) {
  case '(':
    return Token::open_parenthesis;
  case ')':
    return Token::close_parenthesis;
  case '+':
    return Token::plus;
  default: {
    if (isdigit(Current.front())) {
        auto Digit = parseDigit();
        llvm::errs() << Digit;
    }
    }

  }
  llvm_unreachable("Unexpected token");
}
