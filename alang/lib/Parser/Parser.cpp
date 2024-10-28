#include "Parser/Parser.h"
#include "Parser/Expr.h"
#include "Scanner/Token.h"
#include "llvm/Support/ErrorHandling.h"
#include <initializer_list>
#include <memory>

using namespace alang;

std::unique_ptr<Expr> Parser::parseExpression() {
  if (auto Literal = parseLiteral()) {
    return Literal;;
  } 
  if (auto Binary = parseBinary()) {
    return Binary;
  }

  return nullptr;
}

std::unique_ptr<Expr> Parser::parseLiteral() {
  if (match({Token::Type::number_literal})) {
    return std::make_unique<IntegerExpr>(getPrevious().getIntValue());
  }
  return nullptr;
}

std::unique_ptr<Expr> Parser::parseBinary() {
  if (match({Token::Type::open_parenthesis})) {
    if (!match({Token::Type::plus})) 
      llvm_unreachable("Unexpected token on parseBinary");

    auto Operator = getPrevious();
    auto LHS = parseExpression();
    auto RHS = parseExpression();
    if (!match({Token::Type::close_parenthesis})) 
      llvm_unreachable("Unexpected token on parseBinary");
    return std::make_unique<BinaryExpr>(Operator, std::move(LHS), std::move(RHS));
  }

  return nullptr;
}

bool Parser::match(std::initializer_list<Token::Type> TokenTypes) {
  for (auto &TokenType : TokenTypes) {
    if (getCurrent().getType() == TokenType) {
      advanceTokens();
      return true;
    }
  }

  return false;
}
void Parser::advanceTokens() {
  Previous = Current;
  Current = Scanner.lexToken();
}

Token &Parser::getCurrent() {
  if (!Current)
    advanceTokens();
  return *Current;
}

Token &Parser::getPrevious() {
  return *Previous;
}
