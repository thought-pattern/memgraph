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

// VM execution trace tests: validate instruction-level behavior using RecordingTracer.
//
// These tests verify that the VM executes correct instruction sequences for
// representative scenarios. Unlike vm_matching tests (which verify match results),
// these tests inspect the execution trace to validate:
//   - Correct instruction ordering and backtracking
//   - Slot binding and deduplication events
//   - Check failures and their reasons
//   - Yield events with correct slot values
//   - Stats collection (instruction counts, filter rates)

#include <gtest/gtest.h>

#include <format>
#include <sstream>

#include "planner/pattern/vm/tracer.hpp"
#include "test_matcher_fixture.hpp"

import memgraph.planner.core.egraph;

namespace memgraph::planner::core {

using namespace test;
using namespace pattern;
using namespace pattern::vm;

// ============================================================================
// Trace Test Fixture
// ============================================================================
//
// PatternVM_Matching plus a RecordingTracer + DevMode executor. Trace tests
// inherit the compile-and-run cycle (use_patterns, rebuild_index, matches)
// and add trace-event introspection.

class PatternVM_Trace : public PatternVM_Matching {
 protected:
  RecordingTracer tracer;
  TestDevVMExecutor dev_executor{egraph, &tracer};

  void run_traced() {
    ASSERT_TRUE(compiled_.has_value());
    tracer.clear();
    matches.clear();
    dev_executor.execute(*compiled_, index, ctx.arena(), matches);
  }

  // ---------------------------------------------------------------------------
  // Trace Query Helpers
  // ---------------------------------------------------------------------------

  using EventType = RecordingTracer::Event::Type;

  [[nodiscard]] auto events_of_type(EventType type) const -> std::vector<RecordingTracer::Event const *> {
    std::vector<RecordingTracer::Event const *> result;
    for (auto const &e : tracer.events) {
      if (e.type == type) result.push_back(&e);
    }
    return result;
  }

  [[nodiscard]] auto count_events(EventType type) const -> std::size_t { return events_of_type(type).size(); }

  [[nodiscard]] auto yields() const { return events_of_type(EventType::Yield); }

  [[nodiscard]] auto binds() const { return events_of_type(EventType::Bind); }

  [[nodiscard]] auto check_fails() const { return events_of_type(EventType::CheckFail); }

  /// Format the full trace for diagnostic output.
  [[nodiscard]] auto trace_dump() const -> std::string {
    std::ostringstream os;
    tracer.print(os);
    return os.str();
  }

  /// Get VMStats from the dev executor.
  [[nodiscard]] auto stats() const -> VMStats const & { return dev_executor.stats(); }
};

// ============================================================================
// Single Pattern Trace Tests
// ============================================================================

TEST_F(PatternVM_Trace, SimpleMatch_TracesBindAndYield) {
  // Neg(?x) against a single Neg(a) - verify we see exactly one bind and one yield.
  //
  //   E-graph:    Neg(a)
  //   Pattern:    Neg(?x)
  //   Expected:   Bind ?x=a, then Yield
  auto a = leaf(Op::A);
  node(Op::Neg, a);
  rebuild_index();

  use_patterns(TestPattern::build(kTestRoot, Op::Neg, {Var{kVarX}}));
  run_traced();

  EXPECT_EQ(matches.size(), 1u);
  EXPECT_EQ(stats().yields, 1u) << trace_dump();

  // Should have exactly one Yield event
  auto yield_events = yields();
  ASSERT_EQ(yield_events.size(), 1u) << trace_dump();

  // Should have bind events for root and ?x
  auto bind_events = binds();
  ASSERT_GE(bind_events.size(), 2u) << "Expected binds for root and ?x\n" << trace_dump();
}

TEST_F(PatternVM_Trace, NoMatch_TracesCheckFailure) {
  // Neg(Add(?x, ?y)) against Neg(Mul(a, b)) and Mul(Add(a, b), a):
  // whether the compiler enters at deepest symbol Add (walking parents to Mul != Neg)
  // or at root Neg (loading child Mul != Add), CheckSymbol runs and fails with 0 yields.
  auto a = leaf(Op::A);
  auto b = leaf(Op::B);
  node(Op::Neg, node(Op::Mul, a, b));
  node(Op::Mul, node(Op::Add, a, b), a);
  rebuild_index();

  use_patterns(TestPattern::build(kTestRoot, Op::Neg, {Sym(Op::Add, Var{kVarX}, Var{kVarY})}));
  run_traced();

  EXPECT_TRUE(matches.empty());
  EXPECT_EQ(stats().yields, 0u);
  EXPECT_EQ(yields().size(), 0u) << trace_dump();
  EXPECT_GT(stats().symbol_check_misses, 0u) << "Expected inner CheckSymbol miss\n" << trace_dump();
  auto fails = check_fails();
  bool has_symbol_mismatch = std::ranges::any_of(
      fails, [](auto const *e) { return e->details().find("symbol mismatch") != std::string::npos; });
  EXPECT_TRUE(has_symbol_mismatch) << "Expected CheckFail event for symbol mismatch\n" << trace_dump();
}

TEST_F(PatternVM_Trace, Deduplication_TracesBindDuplicate) {
  // Two F e-nodes in the same e-class whose children canonicalize to the
  // same value. We skip rebuild_egraph() so congruence closure doesn't
  // merge the F e-nodes - this lets the VM encounter the duplicate path.
  //
  //   E-graph (no congruence closure):
  //     a = A(1), b = A(2), merge(a, b) → same canonical e-class
  //     f1 = F(a), f2 = F(b), merge(f1, f2) → same e-class, 2 e-nodes
  //
  //   Pattern:    F(?x)
  //   Expected:   First F(a) yields ?x=canonical(a), marks seen.
  //               Second F(b) loads child → canonical(b)=canonical(a) →
  //               BindSlot ?x rejected as duplicate → backtrack.
  auto a = leaf(Op::A, 1);
  auto b = leaf(Op::A, 2);
  auto f1 = node(Op::F, a);
  auto f2 = node(Op::F, b);
  merge(a, b);
  merge(f1, f2);
  // Deliberately skip rebuild_egraph() - congruence closure would merge
  // the F e-nodes since they now have the same canonical children.
  rebuild_index();

  use_patterns(TestPattern::build(kTestRoot, Op::F, {Var{kVarX}}));
  run_traced();

  EXPECT_EQ(matches.size(), 1u) << trace_dump();
  EXPECT_EQ(stats().yields, 1u);

  // The F e-class has 2 e-nodes. Both LoadChild → same canonical.
  // First yields, second is rejected as duplicate.
  auto fails = check_fails();
  bool has_duplicate =
      std::ranges::any_of(fails, [](auto const *e) { return e->details().find("duplicate") != std::string::npos; });
  EXPECT_TRUE(has_duplicate) << "Expected duplicate binding rejection\n" << trace_dump();

  // Verify the duplicate is followed by a backtrack
  for (std::size_t idx = 0; idx + 1 < tracer.events.size(); ++idx) {
    if (tracer.events[idx].type == EventType::CheckFail &&
        tracer.events[idx].details().find("duplicate") != std::string::npos) {
      EXPECT_EQ(tracer.events[idx + 1].type, EventType::Backtrack)
          << "Expected Backtrack after duplicate binding at event[" << idx << "]\n"
          << trace_dump();
      break;
    }
  }
}

// ============================================================================
// Multi-Pattern Join Trace Tests
// ============================================================================

TEST_F(PatternVM_Trace, MultiPatternJoin_TracesParentTraversal) {
  // Two-pattern join that exercises the full feature set:
  //   Pattern 1 (anchor): Bind(_, ?sym, ?expr)
  //   Pattern 2 (joined):  Ident(?sym)
  //
  // The join shares ?sym, so the VM should:
  //   1. IterSymbolEClasses for Bind (anchor outer loop)
  //   2. IterENodes + CheckSymbol/CheckArity for anchor matching
  //   3. LoadChild + BindSlot for each child
  //   4. IterParents from ?sym to find Ident parents (parent traversal)
  //   5. CheckSymbol/CheckArity on parent e-nodes
  //   6. GetENodeEClass + BindSlot for joined root
  //   7. Yield
  //
  //   E-graph:
  //     Bind(placeholder, sym, expr)
  //     Ident(sym)
  //
  auto placeholder = leaf(Op::Const, 0);
  auto sym = leaf(Op::Const, 1);
  auto expr = leaf(Op::Const, 2);
  [[maybe_unused]] auto bind = node(Op::Bind, placeholder, sym, expr);
  [[maybe_unused]] auto ident = node(Op::Ident, sym);
  rebuild_index();

  use_patterns(TestPattern::build(kTestRoot, Op::Bind, {Wildcard{}, Var{kVarX}, Var{kVarY}}),
               TestPattern::build(kVarZ, Op::Ident, {Var{kVarX}}));
  run_traced();

  ASSERT_EQ(matches.size(), 1u) << trace_dump();
  EXPECT_EQ(stats().yields, 1u);

  // Verify parent traversal was used (not Cartesian product)
  EXPECT_GT(stats().iter_parent_calls, 0u) << "Expected parent traversal for joined pattern\n" << trace_dump();

  // Verify we got bind events for all 4 variables: root, ?x, ?y, ?z
  EXPECT_GE(binds().size(), 4u) << "Expected binds for root, ?x, ?y, ?z\n" << trace_dump();

  // Verify exactly one yield with correct slot values
  auto yield_events = yields();
  ASSERT_EQ(yield_events.size(), 1u);
}

TEST_F(PatternVM_Trace, MultiPatternJoin_NoMatch_TracesCheckSlotMiss) {
  // Join reaches CheckSlot regardless of whether the compiler walks parents from ?x or ?y:
  //   - F(sym1, expr2) shares ?x=sym1 with Bind(_, sym1, expr1) and mismatches ?y (expr2 != expr1)
  //   - F(sym2, expr1) shares ?y=expr1 with Bind(_, sym1, expr1) and mismatches ?x (sym2 != sym1)
  auto placeholder = leaf(Op::Const, 0);
  auto sym1 = leaf(Op::Const, 1);
  auto sym2 = leaf(Op::Const, 2);
  auto expr1 = leaf(Op::Const, 3);
  auto expr2 = leaf(Op::Const, 4);
  node(Op::Bind, placeholder, sym1, expr1);
  node(Op::F, sym1, expr2);
  node(Op::F, sym2, expr1);
  rebuild_index();

  use_patterns(TestPattern::build(kTestRoot, Op::Bind, {Wildcard{}, Var{kVarX}, Var{kVarY}}),
               TestPattern::build(kVarZ, Op::F, {Var{kVarX}, Var{kVarY}}));
  run_traced();

  EXPECT_TRUE(matches.empty()) << trace_dump();
  EXPECT_EQ(stats().yields, 0u);
  EXPECT_EQ(yields().size(), 0u);
  EXPECT_GT(stats().check_slot_misses, 0u) << "Expected CheckSlot miss on second shared variable\n" << trace_dump();
  auto fails = check_fails();
  bool has_slot_mismatch =
      std::ranges::any_of(fails, [](auto const *e) { return e->details().find("slot mismatch") != std::string::npos; });
  EXPECT_TRUE(has_slot_mismatch) << "Expected CheckFail event for slot mismatch\n" << trace_dump();
}

// ============================================================================
// Comprehensive Feature Exercise
// ============================================================================

TEST_F(PatternVM_Trace, FullFeatureExercise_ThreePatternJoinWithMerge) {
  // Comprehensive trace test exercising nearly every VM feature and asserting
  // the exact execution order. This validates that the VM processes instructions
  // in the correct sequence with proper backtracking.
  //
  //   E-graph:    F(a, b), G(a, c), H(b)      (a=A, b=B, c=C)
  //
  //   Patterns:
  //     P1 (anchor): F(?x, ?y)
  //     P2 (joined): G(?x, ?z)  - shares ?x with P1, parent traversal
  //     P3 (joined): H(?y)      - shares ?y with P1, parent traversal
  //
  //   Expected execution order (annotated):
  //
  //   Join order: [F(?x,?y), H(?y), G(?x,?z)] - H before G (fewer new vars)
  //   Since ?x,?y bound at enode level (children of F), H and G parent walks
  //   follow F's enode verification in join order.
  //
  //     [pc=0]  IterSymbolEClasses  → find F e-classes (count=1)
  //     [pc=1]  Jump @3             → skip NextSymbolEClass first time
  //     [pc=3]  IterENodes          → e-nodes in F e-class (count=1)
  //     [pc=4]  Jump @6             → skip NextENode first time
  //     [pc=6]  CheckSymbol F       → pass
  //     [pc=7]  CheckArity 2        → pass
  //     [pc=8]  LoadChild 0         → extract ?x's e-class
  //     [pc=9]  BindSlot ?x         → bind ?x=a
  //     [pc=10] LoadChild 1         → extract ?y's e-class
  //     [pc=11] BindSlot ?y         → bind ?y=b
  //     [pc=12] IterParents ?y      → parents of b (count=2: H(b), F(a,b))
  //     [pc=13] Jump @15            → skip NextParent first time
  //     [pc=15] CheckSymbol H       → pass (first parent is H(b))
  //     [pc=16] CheckArity 1        → pass
  //     [pc=17] IterParents ?x      → parents of a (count=2: G(a,c), F(a,b))
  //     [pc=18] Jump @20            → skip NextParent first time
  //     [pc=20] CheckSymbol G       → pass (first parent is G(a,c))
  //     [pc=21] CheckArity 2        → pass
  //     [pc=22] LoadChild 0         → G's first child
  //     [pc=23] CheckEClassEq       → verify child == ?x's e-class → pass
  //     [pc=24] LoadChild 1         → G's second child → ?z's e-class
  //     [pc=25] BindSlot ?z         → bind ?z=c
  //     [pc=26] Yield               → emit match {?x=a, ?y=b, ?z=c}
  //     [pc=27] Jump @19            → continue G parent loop
  //     [pc=19] NextParent          → second parent of a (remaining=1)
  //     [pc=20] CheckSymbol G       → FAIL (F(a,b) is not G) - backtrack
  //     [pc=19] NextParent          → exhausted (remaining=0) → backtrack to @5
  //     [pc=5]  NextENode           → exhausted (remaining=0) → backtrack to @2
  //     [pc=2]  NextSymbolEClass    → exhausted (remaining=0) → jump to @28
  //     [pc=28] Halt                → done
  //
  auto a = leaf(Op::A);
  auto b = leaf(Op::B);
  auto c = leaf(Op::C);
  [[maybe_unused]] auto f = node(Op::F, a, b);
  [[maybe_unused]] auto g = node(Op::G, a, c);
  [[maybe_unused]] auto h = node(Op::H, b);
  rebuild_index();

  use_patterns(TestPattern::build(Op::F, {Var{kVarX}, Var{kVarY}}),
               TestPattern::build(Op::G, {Var{kVarX}, Var{kVarZ}}),
               TestPattern::build(Op::H, {Var{kVarY}}));
  run_traced();

  ASSERT_EQ(matches.size(), 1u) << trace_dump();

  // ---------------------------------------------------------------------------
  // Bytecode structure (29 instructions)
  // ---------------------------------------------------------------------------
  auto code = compiled_->code();
  ASSERT_EQ(code.size(), 29u) << trace_dump();

  EXPECT_EQ(code[0].op, VMOp::IterSymbolEClasses);
  EXPECT_EQ(code[1].op, VMOp::Jump);
  EXPECT_EQ(code[2].op, VMOp::NextSymbolEClass);
  EXPECT_EQ(code[3].op, VMOp::IterENodes);
  EXPECT_EQ(code[4].op, VMOp::Jump);
  EXPECT_EQ(code[5].op, VMOp::NextENode);
  EXPECT_EQ(code[6].op, VMOp::CheckSymbol);
  EXPECT_EQ(code[7].op, VMOp::CheckArity);
  EXPECT_EQ(code[8].op, VMOp::LoadChild);
  EXPECT_EQ(code[9].op, VMOp::BindSlot);
  EXPECT_EQ(code[10].op, VMOp::LoadChild);
  EXPECT_EQ(code[11].op, VMOp::BindSlot);
  EXPECT_EQ(code[12].op, VMOp::IterParents);  // H parents from ?y (H before G in join order)
  EXPECT_EQ(code[13].op, VMOp::Jump);
  EXPECT_EQ(code[14].op, VMOp::NextParent);
  EXPECT_EQ(code[15].op, VMOp::CheckSymbol);  // H
  EXPECT_EQ(code[16].op, VMOp::CheckArity);   // 1
  EXPECT_EQ(code[17].op, VMOp::IterParents);  // G parents from ?x
  EXPECT_EQ(code[18].op, VMOp::Jump);
  EXPECT_EQ(code[19].op, VMOp::NextParent);
  EXPECT_EQ(code[20].op, VMOp::CheckSymbol);    // G
  EXPECT_EQ(code[21].op, VMOp::CheckArity);     // 2
  EXPECT_EQ(code[22].op, VMOp::LoadChild);      // G's child 0
  EXPECT_EQ(code[23].op, VMOp::CheckEClassEq);  // child 0 == ?x
  EXPECT_EQ(code[24].op, VMOp::LoadChild);      // G's child 1
  EXPECT_EQ(code[25].op, VMOp::BindSlot);       // ?z
  EXPECT_EQ(code[26].op, VMOp::Yield);
  EXPECT_EQ(code[27].op, VMOp::Jump);
  EXPECT_EQ(code[28].op, VMOp::Halt);

  // ---------------------------------------------------------------------------
  // Exact execution trace - assert every event in order
  // ---------------------------------------------------------------------------
  //
  // Each event is (Type, pc, detail_substring). We walk through the trace
  // sequentially, asserting both the event type and the program counter
  // to verify the exact execution order including backtracking.

  auto const &ev = tracer.events;
  std::size_t i = 0;

  auto expect_instr = [&](std::size_t pc, std::string_view op_substr) {
    ASSERT_LT(i, ev.size()) << "Trace ended prematurely at index " << i << "\n" << trace_dump();
    EXPECT_EQ(ev[i].type, EventType::Instruction) << "event[" << i << "]\n" << trace_dump();
    EXPECT_EQ(ev[i].pc, pc) << "event[" << i << "] wrong pc\n" << trace_dump();
    EXPECT_NE(ev[i].details().find(op_substr), std::string::npos)
        << "event[" << i << "] expected " << op_substr << " got: " << ev[i].details() << "\n"
        << trace_dump();
    ++i;
  };

  auto expect_event = [&](EventType type, std::size_t pc, std::string_view detail_substr) {
    ASSERT_LT(i, ev.size()) << "Trace ended prematurely at index " << i << "\n" << trace_dump();
    EXPECT_EQ(ev[i].type, type) << "event[" << i << "]\n" << trace_dump();
    EXPECT_EQ(ev[i].pc, pc) << "event[" << i << "] wrong pc\n" << trace_dump();
    EXPECT_NE(ev[i].details().find(detail_substr), std::string::npos)
        << "event[" << i << "] expected '" << detail_substr << "' got: " << ev[i].details() << "\n"
        << trace_dump();
    ++i;
  };

  // --- Anchor: outer symbol iteration for F ---
  expect_instr(0, "IterSymbolEClasses");
  expect_event(EventType::IterStart, 0, "count=1");  // 1 F e-class
  expect_instr(1, "Jump");

  // --- Anchor: inner e-node iteration ---
  expect_instr(3, "IterENodes");
  expect_event(EventType::IterStart, 3, "count=1");  // 1 F e-node
  expect_instr(4, "Jump");

  // --- Match F e-node: CheckSymbol, CheckArity, LoadChild+BindSlot per child ---
  expect_instr(6, "CheckSymbol");
  expect_event(EventType::CheckPass, 6, "symbol match");
  expect_instr(7, "CheckArity");
  expect_event(EventType::CheckPass, 7, "arity match");
  expect_instr(8, "LoadChild");
  expect_instr(9, "BindSlot");
  expect_event(EventType::Bind, 9, std::format("slot=0 value={}", a.value_of()));  // ?x=a
  expect_instr(10, "LoadChild");
  expect_instr(11, "BindSlot");
  expect_event(EventType::Bind, 11, std::format("slot=1 value={}", b.value_of()));  // ?y=b

  // --- Join P2: parent traversal from ?y(=b) for H (H first in join order) ---
  // Parent sets are unordered hash sets, so b's parents {H(b), F(a,b)} may be
  // visited in either order: either H(b) immediately, or F(a,b) mismatch then H(b).
  expect_instr(12, "IterParents");
  expect_event(EventType::IterStart, 12, "count=2");
  expect_instr(13, "Jump");

  std::size_t expected_misses = 1u;  // F(a,b) always fails G's symbol check
  std::size_t expected_total_instrs = 30u;

  expect_instr(15, "CheckSymbol");
  ASSERT_LT(i, ev.size()) << trace_dump();
  if (ev[i].type == EventType::CheckFail) {
    // F(a,b) visited before H(b) in b's parent set
    expect_event(EventType::CheckFail, 15, "symbol mismatch");
    expect_event(EventType::Backtrack, 15, "target=14");
    expect_instr(14, "NextParent");
    expect_event(EventType::IterAdvance, 14, "remaining=1");
    expect_instr(15, "CheckSymbol");
    expected_misses = 2u;
    expected_total_instrs = 32u;
  }
  expect_event(EventType::CheckPass, 15, "symbol match");
  expect_instr(16, "CheckArity");
  expect_event(EventType::CheckPass, 16, "arity match");

  // --- Join P3: parent traversal from ?x(=a) for G ---
  // G(?x, ?z) introduces a new binding (?z), so BOTH parents of a ({G(a,c), F(a,b)})
  // are always visited regardless of hash-set iteration order.
  expect_instr(17, "IterParents");
  expect_event(EventType::IterStart, 17, "count=2");
  expect_instr(18, "Jump");

  auto expect_g_match_and_yield = [&]() {
    expect_event(EventType::CheckPass, 20, "symbol match");
    expect_instr(21, "CheckArity");
    expect_event(EventType::CheckPass, 21, "arity match");
    expect_instr(22, "LoadChild");
    expect_instr(23, "CheckEClassEq");
    expect_event(EventType::CheckPass, 23, std::format("eclass match: value={}", a.value_of()));
    expect_instr(24, "LoadChild");
    expect_instr(25, "BindSlot");
    expect_event(EventType::Bind, 25, std::format("slot=2 value={}", c.value_of()));
    expect_instr(26, "Yield");
    expect_event(EventType::MarkSeen, 26, std::format("slot=2 value={}", c.value_of()));
    expect_event(EventType::Yield, 26, std::format("slots=[{}, {}, {}]", a.value_of(), b.value_of(), c.value_of()));
    expect_instr(27, "Jump");
  };

  auto expect_g_symbol_miss = [&]() {
    expect_event(EventType::CheckFail, 20, "symbol mismatch");
    expect_event(EventType::Backtrack, 20, "target=19");
  };

  expect_instr(20, "CheckSymbol");
  ASSERT_LT(i, ev.size()) << trace_dump();
  if (ev[i].type == EventType::CheckPass) {
    // Order 1: G(a,c) visited first, then F(a,b)
    expect_g_match_and_yield();
    expect_instr(19, "NextParent");
    expect_event(EventType::IterAdvance, 19, "remaining=1");
    expect_instr(20, "CheckSymbol");
    expect_g_symbol_miss();
  } else {
    // Order 2: F(a,b) visited first, then G(a,c)
    expect_g_symbol_miss();
    expect_instr(19, "NextParent");
    expect_event(EventType::IterAdvance, 19, "remaining=1");
    expect_instr(20, "CheckSymbol");
    expect_g_match_and_yield();
  }

  // Exhaust G parents → backtrack to @5 (skipping H's existential parent walk)
  expect_instr(19, "NextParent");
  expect_event(EventType::IterAdvance, 19, "remaining=0");
  expect_event(EventType::Backtrack, 19, "target=5");

  // Exhaust F e-nodes → backtrack to outer symbol loop
  expect_instr(5, "NextENode");
  expect_event(EventType::IterAdvance, 5, "remaining=0");
  expect_event(EventType::Backtrack, 5, "target=2");

  // Exhaust F e-classes → jump to halt
  expect_instr(2, "NextSymbolEClass");
  expect_event(EventType::IterAdvance, 2, "remaining=0");
  expect_event(EventType::Backtrack, 2, "target=28");

  // --- Halt ---
  expect_instr(28, "Halt");
  expect_event(EventType::Halt, 28, std::format("total={}", expected_total_instrs));

  // Verify we consumed the entire trace
  EXPECT_EQ(i, ev.size()) << "Unexpected extra events after index " << i << "\n" << trace_dump();

  // Stats cross-check
  EXPECT_EQ(stats().instructions_executed, expected_total_instrs);
  EXPECT_EQ(stats().yields, 1u);
  EXPECT_EQ(stats().iter_parent_calls, 2u);
  EXPECT_EQ(stats().symbol_check_misses, expected_misses);
}

TEST_F(PatternVM_Trace, FullFeatureExercise_WithMergeAndDedup) {
  // Three-pattern join with merged e-classes - exercises deduplication and canonicalization.
  //
  //   E-graph:
  //     a1, a2 = distinct A leaves, then merged into one e-class
  //     F(a, b), G(a, c), H(b)
  //     After merge(a1, a2), both F(a1, b) and F(a2, b) become equivalent,
  //     but the e-class for a now has two e-nodes. Dedup ensures one match.
  //
  //   Pattern 1: F(?x, ?y)
  //   Pattern 2: G(?x, ?z)
  //   Pattern 3: H(?y)
  //
  auto a1 = leaf(Op::A, 1);
  auto a2 = leaf(Op::A, 2);
  auto b = leaf(Op::B);
  auto c = leaf(Op::C);
  node(Op::F, a1, b);
  node(Op::G, a2, c);
  node(Op::H, b);

  // Merge a1 and a2 - now ?x from F and ?x from G resolve to the same e-class
  merge(a1, a2);
  rebuild_egraph();
  rebuild_index();

  use_patterns(TestPattern::build(Op::F, {Var{kVarX}, Var{kVarY}}),
               TestPattern::build(Op::G, {Var{kVarX}, Var{kVarZ}}),
               TestPattern::build(Op::H, {Var{kVarY}}));
  run_traced();

  ASSERT_EQ(matches.size(), 1u) << trace_dump();
  EXPECT_EQ(stats().yields, 1u) << trace_dump();

  // After merge, parent traversal from the merged a e-class should find
  // both F and G as parents, exercising canonicalization in the process.
  EXPECT_GT(stats().iter_parent_calls, 0u) << trace_dump();
}

TEST_F(PatternVM_Trace, Stats_ParentFilterRate) {
  // Verify symbol_filter_rate reflects actual symbol filtering during parent traversal.
  //
  //   E-graph:
  //     a is child of F(a, b), G(a, c), H(a)
  //     Pattern: F(?x, ?y) joined with G(?x, ?z)
  //
  //   G(?x, ?z) binds new variable ?z, so the parent loop continues after yield.
  //   During parent traversal from ?x, H and F are symbol misses for G.
  auto a = leaf(Op::A);
  auto b = leaf(Op::B);
  auto c = leaf(Op::C);
  node(Op::F, a, b);
  node(Op::G, a, c);
  node(Op::H, a);
  rebuild_index();

  use_patterns(TestPattern::build(Op::F, {Var{kVarX}, Var{kVarY}}),
               TestPattern::build(Op::G, {Var{kVarX}, Var{kVarZ}}));
  run_traced();

  ASSERT_EQ(matches.size(), 1u) << trace_dump();

  auto const &s = stats();
  EXPECT_GT(s.symbol_check_hits, 0u) << "Expected at least one parent symbol hit\n" << trace_dump();
  // H(a) and F(a,b) are parents of a but don't match G - should be misses
  EXPECT_GT(s.symbol_check_misses, 0u) << "Expected parent symbol miss for H/F\n" << trace_dump();
  EXPECT_GT(s.symbol_filter_rate(), 0.0) << "Filter rate should be non-zero\n" << trace_dump();
}

TEST_F(PatternVM_Trace, Stats_CheckSlotHitsAndMisses) {
  // Verify CheckSlot stats when a second shared variable both matches and mismatches.
  //
  //   E-graph:
  //     F(a, b), G(a, b), G(a, c), G(d, b)
  //     Pattern: F(?x, ?y) joined with G(?x, ?y)
  //
  //   Regardless of whether parent traversal starts from ?x (= a) or ?y (= b),
  //   G(a, b) produces one CheckSlot hit and G(a, c) / G(d, b) produces one CheckSlot miss.
  auto a = leaf(Op::A);
  auto b = leaf(Op::B);
  auto c = leaf(Op::C);
  auto d = leaf(Op::D);
  node(Op::F, a, b);
  node(Op::G, a, b);
  node(Op::G, a, c);
  node(Op::G, d, b);
  rebuild_index();

  use_patterns(TestPattern::build(kTestRoot, Op::F, {Var{kVarX}, Var{kVarY}}),
               TestPattern::build(kVarZ, Op::G, {Var{kVarX}, Var{kVarY}}));
  run_traced();

  ASSERT_EQ(matches.size(), 1u) << trace_dump();

  auto const &s = stats();
  EXPECT_EQ(s.yields, 1u);
  EXPECT_EQ(s.check_slot_hits, 1u) << "Expected one CheckSlot hit for G(a, b)\n" << trace_dump();
  EXPECT_EQ(s.check_slot_misses, 1u) << "Expected one CheckSlot miss\n" << trace_dump();
  EXPECT_GT(s.instructions_executed, 0u) << trace_dump();
}

}  // namespace memgraph::planner::core
