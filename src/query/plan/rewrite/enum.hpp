// Copyright 2026 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#pragma once

#include <memory>
#include "query/plan/operator.hpp"

namespace memgraph::query::plan {

namespace impl {

template <class TDbAccessor>
class ExpressionEnumAccessRewriter : public ExpressionVisitor<void> {
 public:
  ExpressionEnumAccessRewriter(TDbAccessor *dba, AstStorage *storage) : dba_(dba), storage_(storage) {}

  void Rewrite(Expression *&expr) {
    if (expr) AcceptExpression(expr);
  }

 private:
  void Visit(EnumValueAccess &enum_value_access) override {
    auto maybe_enum = dba_->GetEnumValue(enum_value_access.enum_name_, enum_value_access.enum_value_);
    if (!maybe_enum) [[unlikely]] {
      throw QueryRuntimeException(
          "Enum value '{}' in enum '{}' not found.", enum_value_access.enum_value_, enum_value_access.enum_name_);
    }
    prev_expressions_.back() = storage_->Create<PrimitiveLiteral>(*maybe_enum);
  }

  // Helper method to maintain the previous expressions
  void AcceptExpression(Expression *&expr) {
    prev_expressions_.emplace_back(expr);
    expr->Accept(*this);
    // If modified, then replace the expression
    if (expr != prev_expressions_.back()) {
      expr = prev_expressions_.back();
    }
    prev_expressions_.pop_back();
  }

  // Unary operators
  void Visit(NotOperator &op) override { AcceptExpression(op.expression_); }

  void Visit(IsNullOperator &op) override { AcceptExpression(op.expression_); };

  void Visit(UnaryPlusOperator &op) override { AcceptExpression(op.expression_); }

  void Visit(UnaryMinusOperator &op) override { AcceptExpression(op.expression_); }

  // Binary operators
  void Visit(OrOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(XorOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(AndOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(NotEqualOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(EqualOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(InListOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(AdditionOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(SubtractionOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(MultiplicationOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(DivisionOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(ModOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(ExponentiationOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(LessOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(GreaterOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(LessEqualOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(GreaterEqualOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(RangeOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(SubscriptOperator &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  void Visit(Aggregation &op) override {
    AcceptExpression(op.expression1_);
    AcceptExpression(op.expression2_);
  }

  // Other
  void Visit(ListSlicingOperator &op) override {
    if (op.list_) AcceptExpression(op.list_);
    if (op.lower_bound_) AcceptExpression(op.lower_bound_);
    if (op.upper_bound_) AcceptExpression(op.upper_bound_);
  }

  void Visit(IfOperator &op) override {
    if (op.condition_) AcceptExpression(op.condition_);
    if (op.then_expression_) AcceptExpression(op.then_expression_);
    if (op.else_expression_) AcceptExpression(op.else_expression_);
  }

  void Visit(ListLiteral &op) override {
    for (auto *&element : op.elements_) {
      if (element) AcceptExpression(element);
    }
  }

  void Visit(MapLiteral &op) override {
    for (auto &[key, val] : op.elements_) {
      if (val) AcceptExpression(val);
    }
  }

  void Visit(MapProjectionLiteral &op) override {
    for (auto &[key, val] : op.elements_) {
      if (val) AcceptExpression(val);
    }
  }

  void Visit(LabelsTest &op) override {}

  void Visit(EdgeTypesTest &op) override {}

  void Visit(Function &op) override {
    for (auto *&argument : op.arguments_) {
      if (argument) AcceptExpression(argument);
    }
  }

  void Visit(Reduce &op) override { AcceptExpression(op.expression_); }

  void Visit(Coalesce &op) override {
    for (auto *&expr : op.expressions_) {
      if (expr) AcceptExpression(expr);
    }
  }

  void Visit(Extract &op) override { AcceptExpression(op.expression_); }

  void Visit(SubqueryExpression &op) override {}

  void Visit(All &op) override {}

  void Visit(Single &op) override {}

  void Visit(Any &op) override {}

  void Visit(None &op) override {}

  void Visit(ListComprehension &op) override {}

  void Visit(Identifier &op) override {}

  void Visit(PrimitiveLiteral &op) override {}

  void Visit(PropertyLookup &op) override { AcceptExpression(op.expression_); }

  void Visit(AllPropertiesLookup &op) override { AcceptExpression(op.expression_); }

  void Visit(ParameterLookup &op) override {}

  void Visit(RegexMatch &op) override {}

  void Visit(NamedExpression &op) override { AcceptExpression(op.expression_); }

  void Visit(PatternComprehension &op) override { AcceptExpression(op.resultExpr_); }

 private:
  TDbAccessor *dba_;
  AstStorage *storage_;
  std::vector<Expression *> prev_expressions_;
};

template <class TDbAccessor>
class EnumAccessRewriter final : public HierarchicalLogicalOperatorVisitor {
 public:
  EnumAccessRewriter(SymbolTable *symbolTable, AstStorage *astStorage, TDbAccessor *db)
      : symbol_table(symbolTable), ast_storage(astStorage), db(db) {}

  ~EnumAccessRewriter() override = default;

  using HierarchicalLogicalOperatorVisitor::PostVisit;
  using HierarchicalLogicalOperatorVisitor::PreVisit;
  using HierarchicalLogicalOperatorVisitor::Visit;

  bool Visit(Once &t) override { return true; }

  bool PostVisit(Filter &op) override {
    auto expression_rewriter = ExpressionEnumAccessRewriter{db, ast_storage};
    expression_rewriter.Rewrite(op.expression_);
    for (auto &filter : op.all_filters_) {
      expression_rewriter.Rewrite(filter.expression);
    }
    return true;
  }

  // TODO: there are more places where an expression could be holding onto an EnumValueAccess

 private:
  SymbolTable *symbol_table;
  AstStorage *ast_storage;
  TDbAccessor *db;
};

}  // namespace impl

template <class TDbAccessor>
std::unique_ptr<LogicalOperator> RewriteEnumAccess(std::unique_ptr<LogicalOperator> root_op, SymbolTable *symbol_table,
                                                   AstStorage *ast_storage, TDbAccessor *db) {
  auto rewriter = impl::EnumAccessRewriter<TDbAccessor>{symbol_table, ast_storage, db};
  root_op->Accept(rewriter);
  return root_op;
}

}  // namespace memgraph::query::plan
