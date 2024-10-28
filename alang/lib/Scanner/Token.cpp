#include "Scanner/Token.h"
#include "llvm/Support/raw_ostream.h"

using namespace alang;

llvm::StringRef Token::getTypeString() const {
  switch (TheType) {
    case Token::Type::close_parenthesis: return "close_parenthesis";
    case Token::Type::eof: return "eof";
    case Token::Type::number_literal: return "number_literal";
    case Type::open_parenthesis: return "open_parenthesis";
    case Type::plus: return "plus";
  }
}
void Token::dump() {
  llvm::errs() << "Type: " << getTypeString() << ", Lexeme: " << Lexeme << ", Value: " << Value;
}
