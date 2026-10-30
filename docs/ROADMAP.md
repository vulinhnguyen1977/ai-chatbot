ROADMAP

Status: proposal. Milestones are ordered by dependency, not by date. Each one ends in a
working, shippable state and has explicit acceptance criteria. A milestone is done only
when every criterion can be shown to be met with evidence: a CI run, a committed report,
or a test. Problem IDs (TD-nn) refer to TECHNICAL_DEBT.md, and decisions (D1 to D7)
refer to section 9 of PROJECT_VISION.md.


GROUND RULES

1. Stabilise and measure before building. No new features until M3 is done. No large
   refactor until M4 has produced a baseline, so that the refactor can be checked for
   changes in behaviour.
2. Bugs are fixed test-first. Every fixed TD item lands together with a test that failed
   before the fix.
3. Targets come from data. Where a criterion depends on model quality, the number is
   marked provisional until the M4 baseline exists. A missed target is reported and
   discussed openly, never quietly lowered.
4. Definition of done, for every milestone:
   - CI passes;
   - CURRENT_ARCHITECTURE.md reflects the change;
   - each closed TD item is marked with the change that closed it;
   - from M4 onwards, the evaluation is run again if behaviour could have changed.

Reference machine RM-1: the current development laptop (Intel i5-10310U, inference on
CPU only). The M4 baseline report records its exact memory, operating-system build,
llama.cpp build and model file hashes. All latency criteria refer to RM-1.


OVERVIEW

M0  Repository and legal foundation
    Outcome: a publishable, clean source repository. Depends on: nothing.
    Decisions needed: D1.

M1  Reproducible build and CI
    Outcome: every commit builds and tests the same way. Depends on: M0.

M2  Test harness and safety net
    Outcome: tests outside the product; known bugs captured as failing tests.
    Depends on: M1.

M3  Stabilisation
    Outcome: high-severity correctness and security bugs fixed. Depends on: M2.
    Decisions needed: D5.

M4  Evaluation harness and baseline
    Outcome: accuracy, scope handling and latency measured. Depends on: M3.

M5  Storage and account model
    Outcome: sound persistence, and an account model according to D2. Depends on: M4.
    Decisions needed: D2.

M6  Core extraction
    Outcome: a portable core and a thin UI. Depends on: M5.
    Decisions needed: D3 (in part).

M7  Deterministic verification, version 1
    Outcome: checked answers with honest labels. Depends on: M6 and Track B.

M8  Maths rendering and UI shell
    Outcome: real typeset mathematics, offline. Depends on: M7.
    Decisions needed: D7.

M9  Profiles and broader STEM scope
    Outcome: scope defined as data; new profiles with evaluation sets. Depends on: M8.
    Decisions needed: D4, D6.

Track B (in parallel): the verifier library (exact arithmetic, units, substitution) is
pure code with no dependency on the application. It can be developed and fully tested at
any time after M2. Only its integration into the answer pipeline waits for M7.


M0  REPOSITORY AND LEGAL FOUNDATION

Goal: a repository that can be published without leaking personal data, violating
licences, or misdescribing the software.

Scope: git initialisation; the licence (D1); third-party notices; cleaning the working
tree; a single source for the version; documentation corrections (part of TD-60, TD-61,
TD-64, TD-65, TD-70).

Acceptance criteria:

- A git repository exists. Its first commit contains the source, docs, config, scripts,
  res/, third_party/, and the training code with the sample text, and nothing else.
- A committed script, tools/check-tree.ps1, fails if the output of "git ls-files"
  contains any file with the extension .exe, .dll, .gguf, .zip, .eml or .bin, any
  database file (.db and its -wal and -shm companions), or anything under data/,
  runtime/bin/ or .claude/. The script passes on the first commit.
- A LICENSE file exists, with the licence chosen under D1.
- ship/package.ps1 places the licence texts for llama.cpp (MIT), Qwen3 and
  nomic-embed-text (Apache-2.0) in the package. A script that lists the zip's entries
  shows them.
- The version is defined in exactly one place. "mstutor.exe --version" and the
  executable's file properties show the same value.
- README fixes: the broken links, the example config line with an inline comment, and
  the claims contradicted in TD-70. CONTRIBUTING.md and SECURITY.md (with a private way
  to report vulnerabilities) exist.
- The development account, its chats and the outbox are removed from the working copy.


M1  REPRODUCIBLE BUILD AND CI

Goal: every commit produces the same, tested binary on a clean machine.

Scope: TD-60 (the CI part), TD-62, TD-63, TD-66.

Acceptance criteria:

- CI on a Windows runner builds with MSVC (through CMake) and with one pinned version of
  MinGW-w64, and runs the test suite with both.
- CMake is the only build system. The list of source files exists in exactly one place.
  If build.bat is kept, it only calls CMake.
- The project's own code (not third_party/) builds without warnings at /W4 and at
  -Wall -Wextra. CI fails on any new warning.
- ship/fetch_runtime.ps1 checks each downloaded archive against a committed SHA-256
  hash. A run with a deliberately wrong hash stops before extracting anything.
- The packaged executable reports the commit hash it was built from, and package.ps1
  refuses to package an executable whose hash does not match the current commit.
- CI runs tools/check-tree.ps1.


M2  TEST HARNESS AND SAFETY NET

Goal: tests that can catch the bugs this audit found, kept out of the shipping binary.

Scope: TD-50, TD-51, TD-52, the unit and integration part of TD-53, the deletions in
TD-40, the trivial duplicates in TD-41, TD-42 and TD-43.

Acceptance criteria:

- The tests live in a separate executable that uses a vendored single-header test
  framework (for example doctest, MIT). mstutor.exe links no test code and has no
  selftest command. The check command is kept for diagnostics in the field.
- All 275 existing checks are ported. The two assertions in TD-51 are rewritten so that
  they fail when the behaviour is broken. This is shown by deliberately breaking the
  code once and recording the failure in the pull request.
- The success path of code verification, and confirm_account_deletion() including audit
  anonymisation, are tested.
- Runs are hermetic: the test runner clears all MSTUTOR_ environment variables, and all
  files go to a temporary directory for that run, which is then removed. After a run,
  %TEMP% contains nothing whose name starts with "mstutor-".
- A fake OpenAI-compatible server running inside the test process makes it possible to
  test ChatService::ask() end to end, parsing of the event stream (chunks split mid-line,
  null fields, error objects), user aborts, HTTP failures, and handling of the
  classifier's JSON.
- A stub llama-server executable, built in the test tree, serves /health with a
  configurable delay and records the arguments it was started with. It is the basis for
  the process tests in M3.
- Each of TD-01 to TD-10 has a committed failing test, marked as expected to fail and
  labelled with its ID. Where a window cannot be avoided, a scripted reproduction goes
  under tools/repro/ instead.
- The dead code in TD-40 that has no planned use is deleted. The regular expression in
  TD-43 uses escape sequences. Training steps 4 to 7 move to tools/experimental/, with a
  note that they do not affect the shipped application.
- The whole suite runs in under 60 seconds on CI, with no model files and no network.


M3  STABILISATION

Goal: fix every high-severity correctness and security defect. No new features.

Scope: TD-01 to TD-11, TD-17, TD-20, and a minimal runner for numbered database
migrations (needed for TD-08, and made general in M5).

Acceptance criteria:

- TD-01: the lifecycle of the model server has a single owner. With the stub server,
  starting the app with a remembered user whose preset differs from presets.default
  gives exactly one chat-server process at every sample, taken every 100 ms for 30
  seconds, and every child process is inside the job object (checked with
  IsProcessInJob).
- TD-01: 100 random sequences of start, switch, sign-in and close against the stub never
  leave a stub process running after the application exits.
- TD-02 and TD-03: a controller state machine that does not depend on the window, with
  the states Starting, Idle, Generating and Switching, gates every entry point: the
  button, Enter, Ctrl+Enter, chat navigation and preset changes. Unit tests cover every
  entry point in every state. Streamed text is only rendered into the chat that owns the
  request.
- TD-04: each launch uses a random API key. Tests show that a request without the key,
  including one that carries a foreign Origin header, is rejected.
- TD-05: prompts have a token budget. With the fake server enforcing a limit of 8,192
  tokens, a chat of 40 turns with 1,500-token answers never exceeds the limit, and
  always keeps the system prompt and the current question.
- TD-06: a regression set of at least 40 legitimate word problems containing the current
  off-topic terms (athlete, recipe, translate, cell, population, tournament, stock,
  football, medicine and so on) is classified as in scope 100% of the time. All existing
  off-topic and injection cases are still refused.
- TD-07 (interim fix): x^{n+1} becomes x^(n+1); 2^{3x} becomes 2^(3x); multiplication is
  shown with the multiplication sign (U+00D7); 90^\circ becomes "90 degrees" or uses the
  degree sign. Each case has a test.
- TD-08: a status column is added to messages through the first numbered migration.
  Stopped answers show a visible "stopped" marker and are left out of the history sent
  to the model. The migration is tested on a copy of a database with the version 1
  schema.
- TD-09: off-topic turns never appear in a request received by the fake server.
- TD-10: closing the window while the model is loading, or during a classifier call,
  ends the process and all its children within 5 seconds (tested with a stub whose
  /health never answers).
- TD-11, according to D5: either the Ollama dialect is gone (no "/api/chat" anywhere in
  the source), or the bundled runtime is not started for a backend other than llama.cpp
  (tested).
- TD-17: inline comments are either supported, or rejected with a visible error.
  "mstutor check" lists unknown keys and invalid values. A test asserts that the shipped
  ini and the defaults in the code agree.
- TD-20: tutor requests carry a token limit and a configurable time limit. Exceeding
  either ends the answer with a clear, non-technical message.
- Exit condition: no High item in section 1 or section 2 of TECHNICAL_DEBT.md is still
  open.


M4  EVALUATION HARNESS AND BASELINE

Goal: know how good the system is before its structure is changed.

Scope: headless evaluation; datasets; baseline reports; retrieval fixes driven by
measurement (TD-14); the evaluation part of TD-53.

Acceptance criteria:

- "mstutor eval --set FILE.jsonl --out REPORT.json" runs, without a window, through the
  same ChatService path as the UI, against the bundled engine.
- The datasets committed under eval/ carry licence notes:
  - problems: at least 200 middle-school maths and physics problems with a ground-truth
    value and unit, tagged by topic, written for the project or taken from permissively
    licensed sources with attribution;
  - scope: at least 300 labelled messages (in scope, off-topic, injection attempts, and
    follow-ups with their context), including the cases from TD-06.
- Extracting and comparing answers (numeric tolerance, normalising units, fractions
  compared with decimals) has unit tests.
- The report contains:
  - accuracy by topic;
  - false-refusal and false-acceptance rates;
  - accuracy with and without retrieval;
  - median (p50) and 95th-percentile (p95) latency;
  - token usage;
  - failures by category.
- Two consecutive runs on RM-1 at temperature 0, with a fixed seed and one server slot,
  agree to within 1 percentage point either way. If they are identical, that is
  recorded.
- Baseline reports for qwen3-1.7b and qwen3-4b on RM-1 are committed under docs/eval/,
  with the hardware, the engine build and the model file hashes.
- TD-14:
  - each vector records a fingerprint of the embedding model file, and vectors with a
    mismatched model or dimension are rejected (tested);
  - the nomic task prefixes and a lexical stop-word list are applied;
  - whether retrieval is on by default is justified by the comparison with and without
    it, and the decision is recorded.


M5  STORAGE AND ACCOUNT MODEL

Goal: persistence that is safe to extend, and an account model that suits a local
application (decision D2).

Scope: TD-15, TD-16, the session model in TD-33, and either TD-18 (if accounts are
removed) or TD-12, TD-19, TD-22 and TD-23 (if they are kept).

Acceptance criteria, in all cases:

- A storage layer owns every database connection (one per thread, or a single
  executor). A CI rule fails if code outside storage/ calls sqlite3 functions or
  Database::mutex(), or creates a statement.
- schema_meta holds only the schema version. Settings move to a settings table through a
  migration.
- Test databases of every released schema version upgrade to the current schema, with
  the row counts and content hashes of chats and messages preserved.
- The corpus schema has one owner, and the Python ingest script uses it instead of
  duplicating the table definitions.

Additional criteria if D2 chooses local profiles (recommended):

- The default build contains no SMTP, TLS or email code (checked in CI from the link
  map or the source).
- A version 1 test database migrates every user's chats to a profile, with nothing
  lost.
- A fresh installation reaches the chat screen with no credentials typed and no network
  access.
- No email address is stored in the default mode (a test scans the database).

Additional criteria if D2 keeps accounts:

- No sign-in operation runs on the UI thread. With a 5-second SMTP delay injected, the
  UI thread never stalls for more than 100 ms (measured).
- TD-19 (the buffer is cleared at STARTTLS), TD-22 (the outbox is purged after
  verification or expiry, and the From address is valid) and TD-23 are fixed, each with
  a test.


M6  CORE EXTRACTION

Goal: a portable core with a thin UI, so that new features stop landing in MainWindow.

Scope: TD-13, TD-21, TD-30, the comments in TD-33, TD-34, TD-35, TD-36, TD-37, and the
rest of TD-40 and TD-41.

Acceptance criteria:

- The source is split into core/, platform/windows/, app/win32/ and cli/. The public
  headers of core do not include windows.h.
- core and its tests build and pass on Linux (GCC or Clang) in CI. This proves that it
  is portable, even while the product ships for Windows only.
- CI rules fail if app/win32 contains SQL, calls an HTTP client, or parses JSON.
  MainWindow only renders state and forwards commands.
- There is one inference-client interface. Cancellation takes effect within 250 ms,
  including before the first token (tested with a fake server that delays its first
  byte).
- There is a structured local log (a rotating file in the data folder, with no network
  use). Every exception that used to be swallowed is logged. No message shown to a
  student contains internal diagnostic text (tested).
- TD-13: at most 25 re-renders of the transcript per second, whatever the token rate
  (counted with the fake server sending 200 tokens per second).
- Running the M4 evaluation again gives accuracy within 1 percentage point of the
  baseline, either way: the refactor did not change behaviour.


M7  DETERMINISTIC VERIFICATION, VERSION 1

Goal: the central feature of PROJECT_VISION.md: answers whose working is checked by
deterministic tools and labelled honestly.

Scope: TD-32; the Track B verifier library; the solution IR; integration into the
pipeline; the labels in the UI.

Acceptance criteria for the verifier library (Track B, which may finish early):

- Exact rational arithmetic, an expression parser, SI dimension vectors, a unit table
  for the curriculum, and a substitution checker, each with unit tests.
- 10,000 generated expressions give exactly the same results as reference values
  produced by an independent implementation (for example Python's fractions module, run
  offline to create the test data).
- Every error in a planted set of at least 200 unit errors, and every error in a planted
  set of at least 200 arithmetic-step errors, is reported as a failure. No error passes.

Acceptance criteria for the integration:

- Version 1 of the solution IR's JSON schema is documented and versioned. Model calls
  use decoding constrained to that schema. When the IR cannot be parsed, the answer
  falls back to prose labelled Not checkable or Partly checked.
- Pipeline: IR, then verifiers, then at most one repair attempt, then the explanation.
  The "Check" section is generated from the verifier results, and never written by the
  model.
- Rule, tested with randomised (property-based) tests: an answer is never labelled
  Checked when any verifier reported a failure.
- The student can expand "What was checked" to see each check and its result, using
  only the keyboard.
- Evaluation on the M4 problem set, on RM-1, with qwen3-4b. All these targets are
  provisional until the M4 baseline confirms them:
  - the IR is valid against the schema for at least 95% of problems;
  - check coverage is at least 80%;
  - the precision of the Checked label is at least 95%;
  - overall accuracy is no lower than the M4 baseline;
  - median latency is at most 1.5 times the baseline.


M8  MATHS RENDERING AND UI SHELL

Goal: show real mathematics, offline, and replace the lossy conversion to plain text
(TD-07).

Scope: decision D7, recorded as an architecture decision record (ADR); integration of
the renderer; keeping accessibility at least as good as today.

Acceptance criteria:

- An ADR in docs/adr/ compares at least two options (for example Win32 with MicroTeX,
  and an embedded web view with a bundled copy of KaTeX) on offline operation, keyboard
  and screen-reader accessibility, correctness of rendering, package size,
  cross-platform potential and maintenance cost.
- The chosen renderer correctly displays the 50 most frequent LaTeX constructs found in
  the M4 and M7 evaluation outputs (tested with reference images or markup snapshots).
- The plain-text normaliser is no longer used for display.
- A full evaluation session with the new UI makes no outbound network connection
  (checked with a connection log or a firewall rule).
- Every keyboard shortcut listed in the README works (checked by an automated UI test,
  or by a signed checklist in the release notes).


M9  PROFILES AND BROADER STEM SCOPE

Goal: turn the hard-coded tutor into the first of several measured profiles (TD-31;
decisions D4 and D6).

Acceptance criteria:

- A profile is a data file: prompt, scope policy, enabled verifiers, evaluation set and
  default model. The middle-school profile reproduces the M7 evaluation results to
  within 1 percentage point, either way.
- Adding a profile needs no C++ change (a test loads a test profile).
- Every new profile ships with its own evaluation set of at least 150 problems and a
  committed baseline. No profile ships without one.
- If a computer algebra system is adopted (D6): an ADR records the choice and its
  licence compatibility with D1. On a curated set of at least 300 pairs of steps, the
  algebraic-step checker accepts at least 90% of correct steps and rejects at least 95%
  of planted incorrect ones.


DELIBERATELY DEFERRED

Each of these has a condition for entering the roadmap. Until it is met, the item stays
off the roadmap.

- Tool calls during generation (the model calls a calculator or algebra system in the
  middle of a solution). Enters when M7 shows that formalisation is not the main source
  of errors, and the chosen model produces valid tool calls in at least 95% of a test
  set.
- Linux and macOS shells. Enter when M6 is done and D3 chooses cross-platform.
- Code signing. Enters when there is a budget for distribution; recommended before any
  broad release.
- Fine-tuning. Enters when the evaluation shows that following the answer format, not
  reasoning, limits accuracy.
- Larger retrieval corpora. Enter when M4 shows that retrieval measurably improves
  accuracy.
- Other model families. Enter when the M4 harness has been run on them and compatibility
  of their prompt templates (TD-37) has been tested.
