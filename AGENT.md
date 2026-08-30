# AGENT.md — Clean Code Engineering Rules

This repository follows strict engineering discipline.

The goal is not merely to make code work. Every change must be:

* correct,
* minimal,
* readable,
* testable,
* reviewable,
* independently verifiable,
* easy to revert,
* and safe to maintain.

These rules apply to all coding agents and subagents working in this repository.

---

# 1. Core Principles

Always optimize for:

1. **Correctness**
2. **Simplicity**
3. **Clarity**
4. **Testability**
5. **Small changes**
6. **Low coupling**
7. **Easy reviewability**

Prefer boring, obvious code over clever code.

Do not introduce abstraction unless it reduces real complexity.

Do not optimize for fewer files or fewer functions.

Optimize for code that another engineer can understand without needing additional explanation.

---

# 2. Understand Before Editing

Before modifying code:

1. Identify the exact behavior being changed.
2. Locate the smallest relevant code path.
3. Read existing tests.
4. Understand surrounding conventions.
5. Determine expected behavior before implementation.
6. Identify potential regressions.

Never start by blindly editing the first matching function.

Do not rewrite unrelated code while implementing a change.

---

# 3. Smallest Possible Change

Every task must be implemented using the smallest reasonable change.

Avoid:

* unrelated refactors,
* formatting unrelated files,
* renaming unrelated symbols,
* changing public APIs unnecessarily,
* speculative cleanup,
* premature abstractions,
* dependency upgrades unrelated to the task.

If unrelated problems are discovered, document them separately rather than silently fixing them in the same change.

A patch should answer one clear question:

> What single behavior does this change introduce, fix, or improve?

If the answer contains multiple unrelated behaviors, split the work.

---

# 4. 70-Line Rule

No function or method may exceed **70 logical lines of code**.

Logical lines exclude:

* blank lines,
* comments,
* docstrings used purely as documentation.

A function approaching 70 lines should already be considered a refactoring candidate.

If implementing a change requires modifying or adding more than approximately **70 lines inside one logical unit**, stop and reconsider the design.

Prefer extracting:

* helper functions,
* data transformations,
* validation logic,
* policy decisions,
* parsing logic,
* state transitions,
* reusable components.

Do not evade the rule by compressing statements onto fewer physical lines.

Bad:

```c
if (a) { foo(); bar(); baz(); }
```

The goal is lower cognitive complexity, not artificially lower line count.

Large generated files, tables, machine-generated code, schemas, or unavoidable declarative data may be exempt, but generated code must not contain handwritten business logic.

---

# 5. Function Design

Functions should do one conceptual thing.

Prefer:

```text
parse_input()
validate_request()
build_plan()
execute_plan()
format_result()
```

over:

```text
handle_everything()
```

A function should normally have:

* one clear responsibility,
* a descriptive name,
* limited state mutation,
* explicit inputs,
* explicit outputs,
* minimal hidden dependencies.

Avoid boolean arguments whose meaning is unclear:

```python
process(data, True, False)
```

Prefer explicit options, enums, or separate functions.

---

# 6. Complexity Limits

Avoid deeply nested control flow.

As a guideline:

* nesting depth should normally be <= 3,
* complex conditions should be named,
* repeated conditions should be extracted,
* large `if/else` trees should be reconsidered,
* large `switch` statements should represent an intentional dispatch model rather than accumulated complexity.

Prefer early returns.

Bad:

```python
if request:
    if request.valid:
        if user:
            if user.enabled:
                ...
```

Better:

```python
if not request:
    return error(...)

if not request.valid:
    return error(...)

if not user or not user.enabled:
    return error(...)

...
```

---

# 7. Naming

Names must communicate intent.

Avoid meaningless names such as:

```text
tmp
foo
bar
data2
result2
thing
obj
manager
helper
utils
misc
```

unless their meaning is genuinely obvious from a very small local scope.

Prefer domain language.

Comments must not compensate for bad names.

Bad:

```python
# Check whether the user can perform the operation
if u.s == 2:
```

Better:

```python
if user.has_write_permission():
```

---

# 8. Comments

Comments should explain **why**, not repeat **what** the code already says.

Bad:

```python
# Increment counter
counter += 1
```

Useful:

```python
# The protocol numbers requests from 1 rather than 0.
counter += 1
```

Delete obsolete comments immediately when behavior changes.

TODO comments must contain enough context to be actionable.

Do not leave commented-out code.

Git already stores history.

---

# 9. Error Handling

Errors must be explicit.

Never silently ignore errors unless ignoring them is intentional and documented.

Avoid:

```python
try:
    operation()
except Exception:
    pass
```

Errors should contain enough context to diagnose the failure.

Do not expose sensitive information in errors.

Validate assumptions close to system boundaries.

Internal code may rely on validated invariants where appropriate.

---

# 10. Tests Are Mandatory

Every behavior change requires tests.

This includes:

* new features,
* bug fixes,
* refactors,
* edge-case handling,
* API changes,
* parser changes,
* state transitions,
* concurrency fixes.

A task is not complete because the code compiles.

A task is complete only when the required tests pass.

---

# 11. Unit Tests

Every meaningful logical unit must have unit-test coverage.

Tests should cover:

1. normal behavior,
2. important edge cases,
3. invalid input,
4. failure behavior,
5. regression cases for bugs.

For every bug fix, first create or identify a test that reproduces the bug whenever practical.

The preferred sequence is:

```text
reproduce failure
→ add regression test
→ implement fix
→ verify test passes
→ run broader test suite
```

Do not write tests that merely execute code without asserting behavior.

Bad:

```python
def test_parser():
    parse("hello")
```

Better:

```python
def test_parser_extracts_name():
    assert parse("hello alice").name == "alice"
```

---

# 12. Tests Must Be Deterministic

Tests must not depend unnecessarily on:

* execution order,
* real wall-clock timing,
* external internet access,
* random values without fixed seeds,
* local machine configuration,
* shared mutable global state.

Avoid arbitrary sleeps.

Bad:

```python
time.sleep(2)
assert worker.finished
```

Prefer explicit synchronization or controlled clocks.

Flaky tests are bugs.

---

# 13. Test the Interface, Not the Implementation

Prefer asserting externally meaningful behavior.

Do not couple tests unnecessarily to internal implementation details.

Refactoring should not require rewriting every test if observable behavior remains unchanged.

Use mocks only where they create a useful boundary.

Do not mock everything.

---

# 14. Compilation Is a Hard Gate

Every commit must leave the repository in a valid buildable state.

At minimum, before committing:

```text
build / compile
unit tests
relevant static checks
```

must pass.

A commit that intentionally does not compile is forbidden.

Examples of forbidden commits:

```text
"WIP"
"half implementation"
"add interface, implementation later"
"temporary compile break"
"fix tests later"
```

Every commit must independently represent a healthy repository state.

---

# 15. Full Test Gate

Before considering the task complete, run:

1. tests directly related to the changed code,
2. all unit tests for the affected component,
3. the full test suite when reasonably practical.

If the full suite cannot be executed, explicitly report:

* what was run,
* what was not run,
* why it was not run.

Never claim tests passed if they were not executed.

---

# 16. Atomic Git Commits

Every Git commit must be:

* minimal,
* atomic,
* coherent,
* compilable,
* tested,
* independently reviewable,
* independently revertible.

One commit should contain one logical change.

A reviewer should be able to describe a commit in one sentence.

Good:

```text
parser: reject malformed frame length
```

Good:

```text
scheduler: extract queue selection policy
```

Bad:

```text
fix parser and refactor scheduler and update docs
```

Bad:

```text
misc cleanup
```

Bad:

```text
WIP
```

Bad:

```text
fix stuff
```

---

# 17. Commit Ordering

When a task requires several changes, structure commits so each one remains valid.

Example:

```text
1. Add regression test demonstrating existing behavior boundary
2. Extract parser helper without changing behavior
3. Implement protocol validation
4. Add edge-case tests
```

However, never create a commit containing a deliberately failing test unless repository policy explicitly requires red/green commits.

By default:

> every commit on the branch must build and pass its applicable tests.

Use local uncommitted work for intermediate failing states.

---

# 18. Commit Size

Commits should be as small as practical.

Large commits require justification.

If a commit:

* touches multiple subsystems,
* changes unrelated APIs,
* mixes formatting with behavior,
* combines refactoring with unrelated functionality,
* or becomes difficult to review,

split it.

Do not optimize for fewer commits.

Optimize for clean history.

---

# 19. Separate Refactoring From Behavior Changes

Whenever practical:

```text
commit A: behavior-preserving refactor
commit B: behavior change
```

Do not mix large structural changes with functional changes.

This makes review and rollback easier.

Tests must demonstrate that refactoring preserves existing behavior.

---

# 20. Clean Diff Rule

Before committing, inspect the complete diff.

Check for:

* accidental formatting changes,
* debug logging,
* temporary code,
* commented-out code,
* unused imports,
* dead code,
* unrelated file changes,
* generated artifacts,
* secrets,
* local paths,
* editor files,
* accidental dependency changes.

Use:

```bash
git diff
git diff --cached
git status
```

A commit is not ready until its diff is clean.

---

# 21. No Drive-By Changes

Do not modify unrelated code just because you notice it.

If you discover unrelated technical debt:

```text
record it
→ report it
→ optionally propose a separate task
```

Do not silently expand scope.

---

# 22. Independent Subagent Review Is Mandatory

After implementation and before finalizing the task, assign an independent review to a separate subagent.

The reviewer must not simply confirm the implementation.

The review subagent must actively search for problems.

The reviewer should inspect:

* correctness,
* edge cases,
* regressions,
* API behavior,
* test coverage,
* concurrency issues,
* resource lifetime,
* error handling,
* security implications,
* unnecessary complexity,
* violation of repository conventions,
* violation of this AGENT.md,
* commit structure,
* diff cleanliness.

The review agent should assume that bugs exist until evidence suggests otherwise.

---

# 23. Reviewer Independence

The implementation agent must provide the reviewer with:

* task requirements,
* resulting diff,
* relevant code,
* tests,
* commands used for validation.

Do **not** bias the reviewer with statements such as:

```text
Everything should be correct.
Please confirm this implementation.
```

Instead request:

```text
Perform an independent adversarial review.
Look for correctness issues, missing tests, unnecessary complexity,
API regressions, and violations of AGENT.md.
Do not assume the implementation is correct.
```

---

# 24. Review Findings Must Be Resolved

Every reviewer finding must be categorized as:

```text
FIX
NOT APPLICABLE
INTENTIONAL
FOLLOW-UP
```

`FIX` findings must be resolved before completion.

`NOT APPLICABLE` and `INTENTIONAL` findings require a concrete explanation.

A serious correctness issue may not be deferred as a follow-up merely to complete the task.

After substantial fixes, request another independent review.

---

# 25. Review Tests, Not Just Production Code

The independent reviewer must inspect tests as carefully as implementation code.

The reviewer should ask:

* Could the implementation be broken while these tests still pass?
* Are assertions strong enough?
* Are important failure paths untested?
* Are tests testing mocks rather than behavior?
* Is there a missing regression case?
* Are tests deterministic?
* Are boundary conditions tested?

Passing weak tests does not imply correctness.

---

# 26. Test Coverage for Changed Behavior

Every branch of newly introduced meaningful behavior should normally be exercised.

Do not chase arbitrary global coverage percentages.

Coverage is a tool, not the goal.

The goal is:

> every important behavior introduced by the patch has evidence that it works.

Uncovered new error-handling paths require explicit justification.

---

# 27. Dependencies

Adding a dependency is a design decision.

Before adding one, consider whether the functionality can reasonably be implemented using:

* the standard library,
* existing project dependencies,
* a small local implementation.

Do not add a dependency for trivial functionality.

If adding a dependency, consider:

* maintenance,
* licensing,
* security,
* binary size,
* build impact,
* runtime impact,
* transitive dependencies.

---

# 28. Public API Changes

Changing a public API requires explicit consideration of compatibility.

Do not rename, remove, or alter externally visible behavior casually.

When changing an API, consider:

* backwards compatibility,
* migration paths,
* serialization formats,
* ABI implications,
* configuration compatibility,
* error semantics.

Tests should protect intended compatibility.

---

# 29. Concurrency

Concurrency changes require extra scrutiny.

Explicitly reason about:

* ownership,
* races,
* lifetime,
* cancellation,
* ordering,
* synchronization,
* lock ordering,
* atomicity,
* memory visibility,
* shutdown behavior,
* error propagation.

Do not use sleeps to hide synchronization bugs.

Concurrency tests should test invariants rather than timing assumptions.

---

# 30. Performance

Do not sacrifice readability for speculative performance improvements.

Performance optimization requires evidence.

When performance matters:

```text
measure
→ identify bottleneck
→ optimize
→ measure again
```

Keep optimized code isolated and documented when its implementation is non-obvious.

Correctness remains mandatory.

---

# 31. Avoid Premature Abstraction

Do not introduce:

* factories,
* managers,
* providers,
* registries,
* generic wrappers,
* plugin systems,
* inheritance trees,

without a demonstrated need.

Three clear duplicated lines are often preferable to a premature abstraction.

Extract abstractions when they represent a genuine shared concept.

---

# 32. Avoid Giant Utility Modules

Do not dump unrelated functions into:

```text
utils
helpers
common
misc
```

Prefer modules named after the responsibility they implement.

For example:

```text
frame_parser
retry_policy
path_validation
request_codec
```

---

# 33. State and Side Effects

Keep side effects near boundaries.

Prefer separating:

```text
compute decision
```

from:

```text
perform I/O
```

Pure logic is easier to test and reason about.

Avoid hidden mutation.

Make ownership clear.

---

# 34. Delete Dead Code

Do not retain old implementations “just in case.”

Delete:

* unreachable code,
* obsolete compatibility paths,
* unused functions,
* abandoned experimental implementations,
* commented-out implementations.

Version control is the backup.

---

# 35. Formatting and Linting

Use repository-standard formatting and linting tools.

Do not manually fight the formatter.

Formatting-only changes should not be mixed with behavioral changes unless unavoidable.

Never reformat an entire file because one small behavior changed.

---

# 36. Documentation

Update documentation when changing:

* public interfaces,
* configuration,
* user-visible behavior,
* architecture assumptions,
* non-obvious invariants.

Do not document implementation trivia that is obvious from the code.

Documentation and code must agree.

---

# 37. Required Workflow

For every non-trivial task, use the following workflow.

## Step 1 — Inspect

Understand:

```text
requirements
existing implementation
existing tests
architecture
repository conventions
```

## Step 2 — Plan

Identify the smallest logical changes.

Plan commit boundaries before making large edits.

## Step 3 — Test Existing Behavior

Run relevant existing tests before modifying behavior when practical.

## Step 4 — Implement Incrementally

Make the smallest working change.

Keep functions below 70 logical lines.

Refactor when complexity grows.

## Step 5 — Add Tests

Add complete unit tests for the changed behavior.

Include regression and edge-case tests.

## Step 6 — Validate

Run:

```text
formatter
linter/static analysis
compiler/build
relevant unit tests
component tests
full test suite when practical
```

## Step 7 — Inspect Diff

Review:

```bash
git status
git diff
git diff --cached
```

Remove unrelated changes.

## Step 8 — Commit Atomically

Each commit must:

```text
build
pass applicable tests
represent one logical change
contain a clean diff
```

## Step 9 — Independent Review

Launch a separate review subagent.

Require adversarial review.

## Step 10 — Resolve Findings

Fix legitimate issues.

Run tests again.

Request another review if fixes are substantial.

## Step 11 — Final Verification

Verify the final repository state from scratch as much as practical.

---

# 38. Definition of Done

A coding task is **not complete** until all applicable items below are true:

* [ ] Requirements are satisfied.
* [ ] Scope is minimal.
* [ ] No unrelated changes are included.
* [ ] No function exceeds 70 logical lines without explicit justification.
* [ ] Complex code has been refactored into understandable units.
* [ ] New behavior has unit tests.
* [ ] Bug fixes have regression tests.
* [ ] Important edge cases are tested.
* [ ] Tests are deterministic.
* [ ] Code builds successfully.
* [ ] Relevant unit tests pass.
* [ ] Full test suite passes when practical.
* [ ] Formatter passes.
* [ ] Linter/static analysis passes where configured.
* [ ] No debug code remains.
* [ ] No dead/commented-out code remains.
* [ ] No secrets or machine-specific artifacts are present.
* [ ] Git diff has been manually inspected.
* [ ] Commits are minimal and atomic.
* [ ] Every commit builds successfully.
* [ ] Every commit passes its applicable tests.
* [ ] An independent subagent has reviewed the final change.
* [ ] All serious review findings have been resolved.
* [ ] Documentation has been updated where necessary.

---

# 39. Forbidden Completion Shortcuts

The following are never acceptable reasons to declare completion:

```text
"It should compile."
"The change is small."
"The tests probably cover it."
"The reviewer will catch it."
"This is only temporary."
"We can clean it up later."
"The old code already did this."
"It works on my machine."
```

Verify instead of assuming.

---

# 40. Final Agent Report

When finishing a coding task, provide a concise report containing:

```text
Implementation:
- What changed.

Tests:
- What tests were added.
- Exact validation commands executed.
- Whether they passed.

Commits:
- Commit list and purpose of each commit.

Independent review:
- Reviewer findings.
- How each finding was resolved.

Limitations:
- Anything that was not validated and why.
```

Never claim execution, compilation, testing, review, or verification that did not actually occur.

---

# 41. Prime Directive

When forced to choose between:

```text
fast implementation
```

and:

```text
small, correct, tested, reviewable implementation
```

choose the second.

A clean change should be easy to understand,
easy to verify,
easy to revert,
and difficult to misuse.

