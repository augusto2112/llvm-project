
#ifndef ALANG_PARSER_H
#define ALANG_PARSER_H

// expression     → literal | binary
// literal        → NUMBER 
// binary         → "(" operator expression expression ")"
// operator       → "+"

#include "../Scanner/Scanner.h"
#include "../Scanner/Token.h"
#include "Expr.h"
#include <initializer_list>

namespace alang {

struct Parser {
  explicit Parser(Scanner &&Scanner) : Scanner(std::move(Scanner)) {
  }

  std::unique_ptr<Expr> parseExpression();
private:
  std::unique_ptr<Expr> parseLiteral();
  std::unique_ptr<Expr> parseBinary();
  Token parseOperator();

  bool match(std::initializer_list<Token::Type> Tokens);
  void advanceTokens();
  Token &getCurrent();
  Token &getPrevious();

  Scanner Scanner;
  std::optional<Token> Current;
  std::optional<Token> Previous;
};
} // namespace alang
#endif
