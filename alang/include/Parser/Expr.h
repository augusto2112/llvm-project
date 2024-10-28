
#ifndef ALANG_EXPR_H
#define ALANG_EXPR_H

#include "../Scanner/Token.h"
#include <cstdint>
#include <memory>

namespace alang {
struct Expr;
struct IntegerExpr;
struct BinaryExpr;

struct ExprVisitor {
  virtual void visit(IntegerExpr &) = 0;
  virtual void visit(BinaryExpr &) = 0;
  virtual ~ExprVisitor() = default;
};

struct Expr {
  Expr() = default;

  virtual void accept(ExprVisitor &Visitor) = 0;
  virtual ~Expr() = default;
};

struct IntegerExpr : Expr {
  explicit IntegerExpr(uint64_t Value) : Value(Value) {}
  uint64_t getValue() const { return Value; }
  void accept(ExprVisitor &Visitor) override { Visitor.visit(*this); }

private:
  uint64_t Value;
};

struct BinaryExpr : Expr {
  explicit BinaryExpr(Token Operator, std::unique_ptr<Expr> &&LHS,
                      std::unique_ptr<Expr> RHS)
      : Operator(Operator), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

  void accept(ExprVisitor &Visitor) override { Visitor.visit(*this); }

  Token getOperator() const { return Operator; }
  
  Expr &getRHS() const { return *RHS.get(); }

  Expr &getLHS() const { return *LHS.get(); }

private:
  Token Operator;
  std::unique_ptr<Expr> LHS;
  std::unique_ptr<Expr> RHS;
};

struct ExprPrinter : ExprVisitor {
  ExprPrinter(Expr &TheExpr) {
    TheExpr.accept(*this);
  }
  void visit(IntegerExpr &) override;
  void visit(BinaryExpr &) override;
private:
  uint16_t Indent = 0;
};
} // namespace alang
#endif
