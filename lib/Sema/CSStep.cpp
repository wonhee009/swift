//===--- CSStep.cpp - Constraint Solver Steps -----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the \c SolverStep class and its related types,
// which is used by constraint solver to do iterative solving.
//
//===----------------------------------------------------------------------===//

#include "CSStep.h"
#include "ConstraintSystem.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;
using namespace swift;
using namespace constraints;

ComponentStep::Scope::Scope(const ComponentStep &component) : CS(component.CS) {
  TypeVars = std::move(CS.TypeVariables);

  for (auto *typeVar : component.TypeVars)
    CS.TypeVariables.push_back(typeVar);

  Constraints.splice(Constraints.end(), CS.InactiveConstraints);

  for (auto *constraint : component.Constraints)
    CS.InactiveConstraints.push_back(constraint);

  auto &CG = CS.getConstraintGraph();
  if (component.OrphanedConstraint)
    CG.setOrphanedConstraint(component.OrphanedConstraint);

  SolverScope = new ConstraintSystem::SolverScope(CS);
  PrevPartialScope = CS.solverState->PartialSolutionScope;
  CS.solverState->PartialSolutionScope = SolverScope;
}

StepResult SplitterStep::take(bool prevFailed) {
  SmallVector<SolverStep *, 4> components;
  computeFollowupSteps(components);
  /// Wait until all of the component steps are done.
  return suspend(components);
}

StepResult SplitterStep::resume(bool prevFailed) {
  // If we came back to this step and previous (one of the components)
  // failed, it means that we can't solve this step either.
  if (prevFailed)
    return done(/*isSuccess=*/false);

  // Otherwise let's try to merge partial soltuions together
  // and form a complete solution(s) for this split.
  return done(mergePartialSolutions());
}

void SplitterStep::computeFollowupSteps(
    SmallVectorImpl<SolverStep *> &nextSteps) {
  SmallVector<ComponentStep *, 4> componentSteps;

  // Compute next steps based on that connected components
  // algorithm tells us is splittable.

  auto &CG = CS.getConstraintGraph();
  // Contract the edges of the constraint graph.
  CG.optimize();

  // Compute the connected components of the constraint graph.
  // FIXME: We're seeding typeVars with TypeVariables so that the
  // connected-components algorithm only considers those type variables within
  // our component. There are clearly better ways to do this.
  SmallVector<TypeVariableType *, 16> typeVars(CS.TypeVariables);
  SmallVector<unsigned, 16> components;

  NumComponents = CG.computeConnectedComponents(typeVars, components);
  PartialSolutions = std::unique_ptr<SmallVector<Solution, 4>[]>(
      new SmallVector<Solution, 4>[NumComponents]);

  for (unsigned i = 0, n = NumComponents; i != n; ++i) {
    componentSteps.push_back(
        ComponentStep::create(CS, i, NumComponents == 1, PartialSolutions[i]));
  }

  if (CS.getASTContext().LangOpts.DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();

    // Verify that the constraint graph is valid.
    CG.verify();

    log << "---Constraint graph---\n";
    CG.print(log);

    log << "---Connected components---\n";
    CG.printConnectedComponents(log);
  }

  // Map type variables and constraints into appropriate steps.
  for (unsigned i = 0, n = typeVars.size(); i != n; ++i) {
    auto *typeVar = typeVars[i];
    auto &step = *componentSteps[components[i]];

    step.record(typeVar);
    for (auto *constraint : CG[typeVar].getConstraints())
      step.record(constraint);
  }

  // Add the orphaned components to the mapping from constraints to components.
  unsigned firstOrphanedConstraint =
      NumComponents - CG.getOrphanedConstraints().size();
  {
    unsigned component = firstOrphanedConstraint;
    for (auto constraint : CG.getOrphanedConstraints())
      componentSteps[component++]->recordOrphan(constraint);
  }

  // Remove all of the orphaned constraints; they'll be re-introduced
  // by each component independently.
  OrphanedConstraints = CG.takeOrphanedConstraints();

  for (auto *step : componentSteps)
    nextSteps.push_back(step);
}

bool SplitterStep::mergePartialSolutions() const {
  // TODO: Optimize when there is only one component
  //       because it would be inefficient to create all
  //       these data structures and do nothing.

  // Produce all combinations of partial solutions.
  SmallVector<unsigned, 2> indices(NumComponents, 0);
  bool done = false;
  bool anySolutions = false;
  do {
    // Create a new solver scope in which we apply all of the partial
    // solutions.
    ConstraintSystem::SolverScope scope(CS);
    for (unsigned i = 0; i != NumComponents; ++i)
      CS.applySolution(PartialSolutions[i][indices[i]]);

    // This solution might be worse than the best solution found so far.
    // If so, skip it.
    if (!CS.worseThanBestSolution()) {
      // Finalize this solution.
      auto solution = CS.finalize();
      if (CS.TC.getLangOpts().DebugConstraintSolver) {
        auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
        log.indent(CS.solverState->depth * 2)
            << "(composed solution " << CS.CurrentScore << ")\n";
      }

      // Save this solution.
      Solutions.push_back(std::move(solution));
      anySolutions = true;
    }

    // Find the next combination.
    for (unsigned n = NumComponents; n > 0; --n) {
      ++indices[n - 1];

      // If we haven't run out of solutions yet, we're done.
      if (indices[n - 1] < PartialSolutions[n - 1].size())
        break;

      // If we ran out of solutions at the first position, we're done.
      if (n == 1) {
        done = true;
        break;
      }

      // Zero out the indices from here to the end.
      for (unsigned i = n - 1; i != NumComponents; ++i)
        indices[i] = 0;
    }
  } while (!done);

  return anySolutions;
}

void ComponentStep::setup() {
  // If this is a single component, there is
  // no need to preliminary modify constraint system.
  if (!IsSingleComponent)
    ComponentScope = llvm::make_unique<Scope>(*this);
}

StepResult ComponentStep::take(bool prevFailed) {
  // If we came either back to this step and previous
  // (either disjunction or type var) failed, or
  // one of the previous components created by "split"
  // failed, it means that we can't solve this component.
  if (prevFailed)
    return done(/*isSuccess=*/false);

  if (CS.TC.getLangOpts().DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2)
        << "(solving component #" << Index << "\n";
  }

  /// Try to figure out what this step is going to be,
  /// after the scope has been established.
  auto *disjunction = CS.selectDisjunction();
  auto bestBindings = CS.determineBestBindings();

  if (bestBindings && (!disjunction || (!bestBindings->InvolvesTypeVariables &&
                                        !bestBindings->FullyBound))) {
    // Produce a type variable step.
    return suspend(TypeVariableStep::create(CS, *bestBindings, Solutions));
  } else if (disjunction) {
    // Produce a disjunction step.
    return suspend(DisjunctionStep::create(CS, disjunction, Solutions));
  }

  // If there are no disjunctions or type variables to bind
  // we can't solve this system unless we have free type variables
  // allowed in the solution.
  if (!CS.solverState->allowsFreeTypeVariables() || !CS.hasFreeTypeVariables())
    return done(/*isSuccess=*/false);

  // If this solution is worse than the best solution we've seen so far,
  // skip it.
  if (CS.worseThanBestSolution())
    return done(/*isSuccess=*/false);

  // If we only have relational or member constraints and are allowing
  // free type variables, save the solution.
  for (auto &constraint : CS.InactiveConstraints) {
    switch (constraint.getClassification()) {
    case ConstraintClassification::Relational:
    case ConstraintClassification::Member:
      continue;
    default:
      return done(/*isSuccess=*/false);
    }
  }

  auto solution = CS.finalize();
  if (CS.TC.getLangOpts().DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2) << "(found solution)\n";
  }

  Solutions.push_back(std::move(solution));
  return done(/*isSuccess=*/true);
}

StepResult ComponentStep::resume(bool prevFailed) {
  if (prevFailed)
    return done(/*isSuccess=*/false);

  // For each of the partial solutions, subtract off the current score.
  // It doesn't contribute.
  for (auto &solution : Solutions)
    solution.getFixedScore() -= OriginalScore;

  // When there are multiple partial solutions for a given connected component,
  // rank those solutions to pick the best ones. This limits the number of
  // combinations we need to produce; in the common case, down to a single
  // combination.
  filterSolutions(Solutions, /*minimize=*/true);
  return done(/*isSuccess=*/true);
}

void TypeVariableStep::setup() {
  auto &TC = CS.TC;
  ++CS.solverState->NumTypeVariablesBound;
  if (TC.getLangOpts().DebugConstraintSolver) {
    auto &log = TC.Context.TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2) << "Initial bindings: ";
    interleave(InitialBindings.begin(), InitialBindings.end(),
               [&](const Binding &binding) {
                 log << TypeVar->getString()
                     << " := " << binding.BindingType->getString();
               },
               [&log] { log << ", "; });

    log << '\n';
  }
}

StepResult TypeVariableStep::take(bool prevFailed) {
  auto &TC = CS.TC;
  while (auto binding = Producer()) {
    // Try each of the bindings in turn.
    ++CS.solverState->NumTypeVariableBindings;

    if (AnySolved) {
      // If this is a defaultable binding and we have found solutions,
      // don't explore the default binding.
      if (binding->isDefaultable())
        continue;

      // If we were able to solve this without considering
      // default literals, don't bother looking at default literals.
      if (binding->hasDefaultedProtocol() && !SawFirstLiteralConstraint)
        break;
    }

    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = TC.Context.TypeCheckerDebug->getStream();
      log.indent(CS.solverState->depth * 2) << "(trying ";
      binding->print(log, &TC.Context.SourceMgr);
      log << '\n';
    }

    if (binding->hasDefaultedProtocol())
      SawFirstLiteralConstraint = true;

    {
      // Try to solve the system with typeVar := type
      auto scope = llvm::make_unique<Scope>(CS);
      if (binding->attempt(CS)) {
        ActiveChoice = std::move(scope);
        // Looks like binding attempt has been successful,
        // let's try to see if it leads to any solutions.
        return suspend(SplitterStep::create(CS, Solutions));
      }
    }
  }

  // No more bindings to try, or producer has been short-circuited.
  return done(/*isSuccess=*/AnySolved);
}

StepResult TypeVariableStep::resume(bool prevFailed) {
  assert(ActiveChoice);

  // Rewind back all of the changes made to constraint system.
  ActiveChoice.reset();

  // If there was no failure in the sub-path it means
  // that active binding has a solution.
  AnySolved |= !prevFailed;

  auto &TC = CS.TC;
  if (TC.getLangOpts().DebugConstraintSolver) {
    auto &log = TC.Context.TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth * 2) << ")\n";
  }

  // If there has been at least one solution so far
  // at a current batch of bindings is done it's a
  // success because each new batch would be less
  // and less precise.
  if (AnySolved && Producer.needsToComputeNext())
    return done(/*isSuccess=*/true);

  // Attempt next type variable binding.
  return take(prevFailed);
}

StepResult DisjunctionStep::take(bool prevFailed) {
  while (auto binding = Producer()) {
    auto &currentChoice = *binding;

    if (shouldSkipChoice(currentChoice))
      continue;

    if (shouldShortCircuitAt(currentChoice))
      break;

    if (CS.TC.getLangOpts().DebugConstraintSolver) {
      auto &ctx = CS.getASTContext();
      auto &log = ctx.TypeCheckerDebug->getStream();
      log.indent(CS.solverState->depth) << "(assuming ";
      currentChoice.print(log, &ctx.SourceMgr);
      log << '\n';
    }

    {
      auto scope = llvm::make_unique<Scope>(CS);
      // Attempt current disjunction choice, which is going to simplify
      // constraint system by binding some of the type variables. Since
      // the system has been simplified and is splittable, we simplify
      // have to return "split" step which is going to take care of the rest.
      if (!currentChoice.attempt(CS))
        continue;

      // Establish the "active" choice which maintains new scope in the
      // constraint system, be be able to rollback all of the changes later.
      ActiveChoice.emplace(std::move(scope), currentChoice);
      return suspend(SplitterStep::create(CS, Solutions));
    }
  }

  return done(/*isSuccess=*/bool(LastSolvedChoice));
}

StepResult DisjunctionStep::resume(bool prevFailed) {
  // If disjunction step is re-taken and there should be
  // active choice, let's see if it has be solved or not.
  assert(ActiveChoice);

  if (CS.TC.getLangOpts().DebugConstraintSolver) {
    auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
    log.indent(CS.solverState->depth) << ")\n";
  }

  // If choice (sub-path) has failed, it's okay, other
  // choices have to be attempted regardless, since final
  // decision could be made only after attempting all
  // of the choices, so let's just ignore failed ones.
  if (!prevFailed) {
    auto &choice = ActiveChoice->second;
    auto score = getBestScore(Solutions);

    if (!choice.isGenericOperator() && choice.isSymmetricOperator()) {
      if (!BestNonGenericScore || score < BestNonGenericScore)
        BestNonGenericScore = score;
    }

    // Remember the last successfully solved choice,
    // it would be useful when disjunction is exhausted.
    LastSolvedChoice = {choice, *score};
  }

  // Rewind back the constraint system information.
  ActiveChoice.reset();

  // Attempt next disjunction choice (if any left).
  return take(prevFailed);
}

bool DisjunctionStep::shouldSkipChoice(const TypeBinding &choice) const {
  auto &TC = CS.TC;

  if (choice.isDisabled()) {
    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = CS.getASTContext().TypeCheckerDebug->getStream();
      log.indent(CS.solverState->depth) << "(skipping ";
      choice.print(log, &TC.Context.SourceMgr);
      log << '\n';
    }

    return true;
  }

  // Skip unavailable overloads unless solver is in the "diagnostic" mode.
  if (!CS.shouldAttemptFixes() && choice.isUnavailable())
    return true;

  if (TC.getLangOpts().DisableConstraintSolverPerformanceHacks)
    return false;

  // Don't attempt to solve for generic operators if we already have
  // a non-generic solution.

  // FIXME: Less-horrible but still horrible hack to attempt to
  //        speed things up. Skip the generic operators if we
  //        already have a solution involving non-generic operators,
  //        but continue looking for a better non-generic operator
  //        solution.
  if (BestNonGenericScore && choice.isGenericOperator()) {
    auto &score = BestNonGenericScore->Data;
    // Let's skip generic overload choices only in case if
    // non-generic score indicates that there were no forced
    // unwrappings of optional(s), no unavailable overload
    // choices present in the solution, no fixes required,
    // and there are no non-trivial function conversions.
    if (score[SK_ForceUnchecked] == 0 && score[SK_Unavailable] == 0 &&
        score[SK_Fix] == 0 && score[SK_FunctionConversion] == 0)
      return true;
  }

  return false;
}

bool DisjunctionStep::shouldShortCircuitAt(
    const DisjunctionChoice &choice) const {
  if (!LastSolvedChoice)
    return false;

  auto *lastChoice = LastSolvedChoice->first;
  auto delta = LastSolvedChoice->second - getCurrentScore();
  bool hasUnavailableOverloads = delta.Data[SK_Unavailable] > 0;
  bool hasFixes = delta.Data[SK_Fix] > 0;

  // Attempt to short-circuit evaluation of this disjunction only
  // if the disjunction choice we are comparing to did not involve
  // selecting unavailable overloads or result in fixes being
  // applied to reach a solution.
  return !hasUnavailableOverloads && !hasFixes &&
         shortCircuitDisjunctionAt(choice, lastChoice);
}

bool DisjunctionStep::shortCircuitDisjunctionAt(
    Constraint *currentChoice, Constraint *lastSuccessfulChoice) const {
  auto &ctx = CS.getASTContext();
  if (ctx.LangOpts.DisableConstraintSolverPerformanceHacks)
    return false;

  // If the successfully applied constraint is favored, we'll consider that to
  // be the "best".
  if (lastSuccessfulChoice->isFavored() && !currentChoice->isFavored()) {
#if !defined(NDEBUG)
    if (lastSuccessfulChoice->getKind() == ConstraintKind::BindOverload) {
      auto overloadChoice = lastSuccessfulChoice->getOverloadChoice();
      assert((!overloadChoice.isDecl() ||
              !overloadChoice.getDecl()->getAttrs().isUnavailable(ctx)) &&
             "Unavailable decl should not be favored!");
    }
#endif

    return true;
  }

  // Anything without a fix is better than anything with a fix.
  if (currentChoice->getFix() && !lastSuccessfulChoice->getFix())
    return true;

  if (auto restriction = currentChoice->getRestriction()) {
    // Non-optional conversions are better than optional-to-optional
    // conversions.
    if (*restriction == ConversionRestrictionKind::OptionalToOptional)
      return true;

    // Array-to-pointer conversions are better than inout-to-pointer
    // conversions.
    if (auto successfulRestriction = lastSuccessfulChoice->getRestriction()) {
      if (*successfulRestriction == ConversionRestrictionKind::ArrayToPointer &&
          *restriction == ConversionRestrictionKind::InoutToPointer)
        return true;
    }
  }

  // Implicit conversions are better than checked casts.
  if (currentChoice->getKind() == ConstraintKind::CheckedCast)
    return true;

  return false;
}
