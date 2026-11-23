# Copyright 2021 Memgraph Ltd.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
# License, and you may not use this file except in compliance with the Business Source License.
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0, included in the file
# licenses/APL.txt.

# -*- coding: utf-8 -*-

from behave import then
from neo4j.exceptions import AuthError, ClientError

INTERNAL_FAILURE_MARKERS = (
    "unexpected storage error",
    "failed to send result to client",
    "please contact memgraph support",
    "an unknown exception occurred",
)

EXPECTED_ERROR_KEYWORDS = {
    "NestedAggregation": (
        "using aggregation functions inside aggregation functions",
        "using aggregation functions inside an expression",
        "nested aggregation",
    ),
    "IncomparableValues": (
        "invalid types:",
        "invalid ",
        "operand types",
        "incomparable",
        "cannot compare",
    ),
    "RequiresDirectedRelationship": (
        "bidirectional relationship",
        "directed relationship",
    ),
    "InvalidRelationshipPattern": (
        "underlying parsing error",
        "relationship",
        "edge",
        "pattern",
    ),
    "MapElementAccessByNonString": (
        "expected a string as a map index",
        "expected a string as a property name",
    ),
    "DeleteConnectedNode": (
        "existing connections",
        "has edges",
    ),
    "ListElementAccessByNonInteger": (
        "expected an integer as a list index",
        "expected an integer for a bound in list slicing",
    ),
    "InvalidArgumentType": (
        "argument",
        "must be",
        "expected",
        "invalid type",
        "doesn't satisfy",
    ),
    "InvalidElementAccess": (
        "expected a list, a map, a node or an edge to index",
        "expected a list to slice",
    ),
    "NumberOutOfRange": (
        "outside the range",
        "exceeds 64 bits",
        "overflow",
        "out of range",
        "percentile",
        "between 0",
    ),
    "InvalidArgumentValue": (
        "argument",
        "must be",
        "expected",
        "invalid",
        "only edges have",
        "only nodes have",
        "range",
    ),
    "VariableAlreadyBound": (
        "redeclaring variable",
        "already declared",
        "duplicate variable",
    ),
    "PropertyAccessOnNonMap": (
        "have properties to be looked up",
        "coerce",
        "nested property",
        "not of type map",
    ),
    "InvalidUnicodeLiteral": (
        "utf codepoint",
        "unicode codepoint",
    ),
    "MergeReadOwnWrites": (
        "merge",
        "already declared",
        "unbound variable",
    ),
    "InvalidAggregation": (
        "aggregation functions",
        "aggregation",
    ),
    "NoExpressionAlias": (
        "non-aliased",
        "must be aliased",
        "alias",
    ),
    "UndefinedVariable": (
        "unbound variable",
        "unbounded variables",
        "not defined",
    ),
    "VariableTypeConflict": (
        "type mismatch",
        "already defined as",
    ),
    "DifferentColumnsInUnion": (
        "union",
        "same column names",
    ),
    "InvalidClauseComposition": (
        "can't be put",
        "can't be used",
        "only be one return",
        "either create or update",
        "write procedures cannot",
        "underlying parsing error",
    ),
    "InvalidPropertyType": (
        "cannot be used as a property value",
        "not a valid property value",
        "null literal properties inside merge",
    ),
    "ColumnNameConflict": (
        "multiple results with the same name",
        "same name",
    ),
    "NoVariablesInScope": (
        "no variables in scope",
    ),
    "InvalidDelete": (
        "edges, vertices and paths can be deleted",
        "delete",
        "underlying parsing error",
    ),
    "NegativeIntegerArgument": (
        "non-negative",
        "negative",
        "underlying parsing error",
    ),
    "DeletedEntityAccess": (
        "deleted",
        "record invalid after with",
        "doesn't exist",
    ),
    "RelationshipUniquenessViolation": (
        "already bound relationship",
        "uniqueness",
    ),
    "CreatingVarLength": (
        "variable length relationships are not supported when creating",
        "variable-length",
    ),
    "InvalidParameterUse": (
        "property map matching not supported",
        "parameter",
        "literal",
        "variables are not allowed",
        "underlying parsing error",
    ),
    "FloatingPointOverflow": (
        "couldn't parse string to double",
        "overflow",
        "exceeds",
    ),
    "InvalidArgumentExpression": (
        "exists supports only",
        "identifiers are not supported",
        "aggregation",
        "underlying parsing error",
    ),
    "InvalidUnicodeCharacter": (
        "invalid character",
        "wrong token",
        "utf codepoint",
        "unicode",
    ),
    "NonConstantExpression": (
        "variables are not allowed",
        "constant",
        "literal",
        "underlying parsing error",
    ),
    "NoSingleRelationshipType": (
        "single relationship type",
    ),
    "UnknownFunction": (
        "function '",
        "doesn't exist",
        "not implemented",
    ),
    "InvalidNumberLiteral": (
        "exceeds 64 bits",
        "couldn't parse string to double",
        "wrong token",
        "underlying parsing error",
    ),
    "InvalidArgumentPassingMode": (
        "yield",
        "arguments",
        "underlying parsing error",
    ),
    "InvalidNumberOfArguments": (
        "requires no arguments",
        "requires exactly",
        "requires between",
        "requires at least",
        "arguments",
    ),
    "MissingParameter": (
        "parameter",
        "not provided",
    ),
    "ProcedureNotFound": (
        "no procedure named",
        "procedure",
        "not found",
    ),
}


def handle_error(context, expected_detail=None):
    """
    Verify that query execution raised a genuine query ClientError matching expected_detail,
    rejecting missing exceptions, driver/connection errors, auth errors, transient errors,
    and internal database failures.
    """
    exc = getattr(context, "exception", None)
    assert exc is not None, "Expected a query ClientError, but query succeeded without raising."
    assert isinstance(exc, ClientError) and not isinstance(
        exc, AuthError
    ), f"Expected a query ClientError, got {type(exc).__name__}: {exc}"

    code = getattr(exc, "code", "") or ""
    assert ".ClientError." in code, f"Expected a Bolt ClientError code, got {code!r}: {exc}"
    assert ".Security." not in code and "SessionLimitReached" not in code, (
        f"Unexpected security/session error {code!r}: {exc}"
    )

    msg = str(getattr(exc, "message", None) or exc)
    msg_lower = msg.lower()
    for marker in INTERNAL_FAILURE_MARKERS:
        assert marker not in msg_lower, f"Unexpected internal database failure disguised as ClientError: {msg}"

    if expected_detail is not None:
        detail_lower = expected_detail.lower()
        keywords = EXPECTED_ERROR_KEYWORDS.get(expected_detail, ())
        matched = detail_lower in code.lower() or detail_lower in msg_lower or any(kw in msg_lower for kw in keywords)
        assert matched, f"Expected query error matching {expected_detail!r}, got code={code!r} message={msg!r}"


@then("an error should be raised")
def error(context):
    handle_error(context)


@then("a SyntaxError should be raised at compile time: NestedAggregation")
def syntax_error(context):
    handle_error(context, "NestedAggregation")


@then("TypeError should be raised at compile time: IncomparableValues")
def type_error(context):
    handle_error(context, "IncomparableValues")


@then("a TypeError should be raised at compile time: IncomparableValues")
def step(context):
    handle_error(context, "IncomparableValues")


@then("a SyntaxError should be raised at compile time: RequiresDirectedRelationship")
def step(context):
    handle_error(context, "RequiresDirectedRelationship")


@then("a SyntaxError should be raised at compile time: InvalidRelationshipPattern")
def syntax_error(context):
    handle_error(context, "InvalidRelationshipPattern")


@then("a TypeError should be raised at runtime: MapElementAccessByNonString")
def type_error(context):
    handle_error(context, "MapElementAccessByNonString")


@then("a ConstraintVerificationFailed should be raised at runtime: DeleteConnectedNode")
def step(context):
    handle_error(context, "DeleteConnectedNode")


@then("a TypeError should be raised at runtime: ListElementAccessByNonInteger")
def step(context):
    handle_error(context, "ListElementAccessByNonInteger")


@then("a SyntaxError should be raised at compile time: InvalidArgumentType")
def step(context):
    handle_error(context, "InvalidArgumentType")


@then("a TypeError should be raised at runtime: InvalidElementAccess")
def step(context):
    handle_error(context, "InvalidElementAccess")


@then("a ArgumentError should be raised at runtime: NumberOutOfRange")
def step(context):
    handle_error(context, "NumberOutOfRange")


@then("a TypeError should be raised at runtime: InvalidArgumentValue")
def step(context):
    handle_error(context, "InvalidArgumentValue")


@then("a SyntaxError should be raised at compile time: VariableAlreadyBound")
def step(context):
    handle_error(context, "VariableAlreadyBound")


@then("a TypeError should be raised at runtime: IncomparableValues")
def step(context):
    handle_error(context, "IncomparableValues")


@then("a TypeError should be raised at runtime: PropertyAccessOnNonMap")
def step(context):
    handle_error(context, "PropertyAccessOnNonMap")


@then("a SyntaxError should be raised at compile time: InvalidUnicodeLiteral")
def step(context):
    handle_error(context, "InvalidUnicodeLiteral")


@then("a SemanticError should be raised at compile time: MergeReadOwnWrites")
def step(context):
    handle_error(context, "MergeReadOwnWrites")


@then("a SyntaxError should be raised at compile time: InvalidAggregation")
def step(context):
    handle_error(context, "InvalidAggregation")


@then("a SyntaxError should be raised at compile time: NoExpressionAlias")
def step(context):
    handle_error(context, "NoExpressionAlias")


@then("a SyntaxError should be raised at compile time: UndefinedVariable")
def step(context):
    handle_error(context, "UndefinedVariable")


@then("a SyntaxError should be raised at compile time: VariableTypeConflict")
def step(context):
    handle_error(context, "VariableTypeConflict")


@then("a SyntaxError should be raised at compile time: DifferentColumnsInUnion")
def step(context):
    handle_error(context, "DifferentColumnsInUnion")


@then("a SyntaxError should be raised at compile time: InvalidClauseComposition")
def step(context):
    handle_error(context, "InvalidClauseComposition")


@then("a TypeError should be raised at compile time: InvalidPropertyType")
def step(context):
    handle_error(context, "InvalidPropertyType")


@then("a SyntaxError should be raised at compile time: ColumnNameConflict")
def step(context):
    handle_error(context, "ColumnNameConflict")


@then("a SyntaxError should be raised at compile time: NoVariablesInScope")
def step(context):
    handle_error(context, "NoVariablesInScope")


@then("a SyntaxError should be raised at compile time: InvalidDelete")
def step(context):
    handle_error(context, "InvalidDelete")


@then("a SyntaxError should be raised at compile time: NegativeIntegerArgument")
def step(context):
    handle_error(context, "NegativeIntegerArgument")


@then("a EntityNotFound should be raised at runtime: DeletedEntityAccess")
def step(context):
    handle_error(context, "DeletedEntityAccess")


@then("a SyntaxError should be raised at compile time: RelationshipUniquenessViolation")
def step(context):
    handle_error(context, "RelationshipUniquenessViolation")


@then("a SyntaxError should be raised at compile time: CreatingVarLength")
def step_impl(context):
    handle_error(context, "CreatingVarLength")


@then("a SyntaxError should be raised at compile time: InvalidParameterUse")
def step_impl(context):
    handle_error(context, "InvalidParameterUse")


@then("a SyntaxError should be raised at compile time: FloatingPointOverflow")
def step_impl(context):
    handle_error(context, "FloatingPointOverflow")


@then("a SyntaxError should be raised at compile time InvalidArgumentExpression")
def step_impl(context):
    handle_error(context, "InvalidArgumentExpression")


@then("a SyntaxError should be raised at compile time InvalidUnicodeCharacter")
def step_impl(context):
    handle_error(context, "InvalidUnicodeCharacter")


@then("a SyntaxError should be raised at compile time: NonConstantExpression")
def step_impl(context):
    handle_error(context, "NonConstantExpression")


@then("a SyntaxError should be raised at compile time: NoSingleRelationshipType")
def step_impl(context):
    handle_error(context, "NoSingleRelationshipType")


@then("a SyntaxError should be raised at compile time: UnknownFunction")
def step_impl(context):
    handle_error(context, "UnknownFunction")


@then("a SyntaxError should be raised at compile time: InvalidNumberLiteral")
def step_impl(context):
    handle_error(context, "InvalidNumberLiteral")


@then("a SyntaxError should be raised at compile time: InvalidArgumentExpression")
def step(context):
    handle_error(context, "InvalidArgumentExpression")


@then("a SyntaxError should be raised at compile time: InvalidUnicodeCharacter")
def step(context):
    handle_error(context, "InvalidUnicodeCharacter")


@then("a SyntaxError should be raised at compile time: InvalidArgumentPassingMode")
def step_impl(context):
    handle_error(context, "InvalidArgumentPassingMode")


@then("a SyntaxError should be raised at compile time: InvalidNumberOfArguments")
def step_impl(context):
    handle_error(context, "InvalidNumberOfArguments")


@then("a ParameterMissing should be raised at compile time: MissingParameter")
def step_impl(context):
    handle_error(context, "MissingParameter")


@then("a ProcedureError should be raised at compile time: ProcedureNotFound")
def step_impl(context):
    handle_error(context, "ProcedureNotFound")
