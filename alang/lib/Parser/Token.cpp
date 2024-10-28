
#include "Parser/Token.h"
#include "llvm/Support/raw_ostream.h"

llvm::StringRef Token::getTypeString() const {
  switch (Type) {
    case Type::close_parenthesis: return "close_parenthesis";
    case Type::number_literal: return "number_literal";
    case Type::open_parenthesis: return "open_parenthesis";
    case Type::plus: return "plus";
  }
}
void Token::dump() {
  llvm::errs() << "Type: " << getTypeString() << ", Lexeme: " << Lexeme << ", Value: " << Value << "\n";
}
