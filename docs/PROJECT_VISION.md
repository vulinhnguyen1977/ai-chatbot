PROJECT VISION

Status: proposal for discussion. The decisions marked D1 to D7 are open and are listed
at the end. The current state is described in CURRENT_ARCHITECTURE.md, and its problems
in TECHNICAL_DEBT.md.


1. IN ONE SENTENCE

A fully local STEM reasoning platform in which a local language model proposes
solutions and deterministic tools check them, and the user is always told which parts
were checked, which were not, and what failed.


2. THE PROBLEM THIS SOLVES

Small local models (1 to 8 billion parameters) are fluent, fast enough on ordinary
hardware, and private. They are also unreliable at exactly what STEM work depends on:
multi-step arithmetic, algebraic manipulation, units and sign conventions. Asking the
model to check itself, which is what the current tutor's "Check" step does, is weak
evidence. The model that made the mistake grades its own paper.

The prototype's own training notes state the risk plainly: "a confidently-formatted
wrong answer is worse than no answer." Today the application cannot tell a correct
answer from a confidently wrong one (TD-32), and nobody has measured how often each
happens (TD-53).

Much of school and early-university STEM work, by contrast, can be checked
mechanically:

- a numeric step can be evaluated exactly;
- a physical equation can be checked for dimensional consistency;
- a solution can be substituted back into the equation it claims to solve;
- two algebraic forms can be tested for equivalence.

These checks are cheap, offline and deterministic. The opportunity is to combine them
with the model, rather than rely on either one alone.


3. CORE THESIS: STRUCTURE FIRST, THEN VERIFY, THEN EXPLAIN

The prototype already teaches with a five-part answer structure. The vision keeps that
teaching method and changes who is responsible for each part:

- What we know. Today: prose written by the model. Target: the model extracts the
  givens (symbol, value, unit) as structured data, and they are shown to the user.
- Idea. Today: prose. Target: the model names the relations (equations or formulas) as
  expressions that can be parsed.
- Working. Today: prose. Target: the model writes the steps as expressions with their
  stated results, and each one is checked.
- Answer. Today: prose. Target: a structured value and unit, checked against the
  relations.
- Check. Today: the model grades itself. Target: deterministic verifiers produce the
  check, and the model does not write this section at all.

The explanation the student reads is rendered from, or written alongside, that verified
structure. Verification is the centre of gravity of the architecture, not a feature
bolted onto a chatbot.

3.1 What "verified" means, and what it does not

This distinction must be explicit in the product, the code and the documentation:

- Arithmetic of each step: exact rational evaluation. Deterministic: yes.
- Units and dimensions of each relation and of the answer: SI dimension vectors and a
  unit table. Deterministic: yes.
- The answer satisfies the stated relations: substitution. Deterministic: yes.
- One algebraic form is equivalent to the next: computer algebra (a later milestone).
  Deterministic: mostly, within the known limits of computer algebra systems.
- Plausibility (sign, magnitude, range): rule checks. Heuristic, and labelled as such.
- The word problem was translated into the right givens and relations: no tool can
  check this. Deterministic: no.

The last item is the main remaining risk, and no tool can remove it. The honest
response is to show the formalisation to the user ("Here is what I understood") and
never to claim more than was checked. A "Verified" label means that the working is
internally consistent and dimensionally sound. It does not mean that the model
understood the question. The UI and the documentation must say so.

3.2 What the user sees for every answer

Exactly one of these labels:

- Checked: all applicable checks ran and passed. The list of checks can be expanded.
- Partly checked: some steps could not be checked (for example because they could not
  be parsed), and the unchecked steps are marked.
- Problem found: a check failed. The model gets one repair attempt, given the
  verifier's finding. If the repair fails, the user sees the failure, not a hidden
  retry.
- Not checkable: conceptual or explanatory questions ("why does ice float?"). These are
  labelled as unverified explanations.

The product never shows "Checked" if any check failed. That rule can be tested.


4. PRINCIPLES

1. Local by default, offline always. No telemetry, no cloud fallback, no account
   server. The only network traffic is what the user explicitly starts (for example,
   downloading a model). The local inference server requires authentication, so that
   other local software and web pages cannot use it (TD-04).
2. Verification over fluency. When the two conflict, prefer a shorter answer that was
   checked over a longer one that was not.
3. Measure before and after. An evaluation harness with fixed problem sets is a
   first-class component. No change to prompts, models, the topic gate or retrieval is
   merged without numbers.
4. A small, boring core. A portable C++ library holds the conversation engine, the
   solution representation, the verifiers, storage and the inference client. Platform
   code and UI shells stay thin.
5. Models are replaceable. Any GGUF model served by a local OpenAI-compatible server
   (llama.cpp today) must work. Nothing depends on the quirks of one model family.
   Model choice is data (profiles), not code.
6. Scope is a profile, not a hard-coded identity. "Middle-school maths and physics
   tutor" becomes the first profile: a system prompt, a scope policy, a set of enabled
   verifiers, and an evaluation set. The platform itself is not tied to it.
7. Privacy by design. Collect nothing the function does not need. A local tool does not
   need email addresses (D2).
8. Keep what works. Keyboard accessibility, the supervised bundled engine that is
   killed when the app closes, storing raw output and processing it at render time,
   SQLite with FTS5, and vendored dependencies with no package manager at build time
   are all sound, and should survive.


5. TARGET ARCHITECTURE (DELIBERATELY MINIMAL)

Three layers, plus the engine:

- UI shells, thin and free of business logic: the Windows desktop app (the existing
  Win32 shell for now; its future is decision D7), and a command line for eval, check
  and index. Both talk only to the conversation engine.
- core, portable C++ with no operating-system headers in its public interface:
  - the conversation engine, a state machine that turns a request into an answer;
  - profiles: prompt, scope policy, enabled verifiers, evaluation set;
  - the solution IR (intermediate representation): givens, relations, steps, answer;
  - verifiers: exact arithmetic, units, substitution, and computer algebra later;
  - retrieval (optional);
  - storage: repositories with versioned migrations;
  - the inference client: OpenAI-compatible, streaming, cancellable.
- platform/windows:
  - the engine supervisor: job object, per-launch API key, single owner;
  - DPAPI and the operating-system profile.
- The engine: llama-server, local and authenticated. The supervisor starts it; the
  inference client talks to it.

The conversation engine uses the profiles, the inference client, the solution IR (which
feeds the verifiers), retrieval and storage.

Boundaries that matter:

- The UI never talks to SQLite, HTTP or JSON directly. It sends commands to the
  conversation engine and renders the events and state it gets back. This removes the
  class of bugs in TD-01, TD-02, TD-03 and TD-10.
- One owner per resource. The engine supervisor owns the model processes. Storage owns
  the database connections. The conversation engine owns the lifecycle of each request,
  including cancellation.
- The solution IR is the contract between the model and the verifiers. It is a small,
  versioned JSON schema. Constrained decoding already works in this codebase (the
  classifier uses a JSON schema), so this extends an existing mechanism rather than
  adding a new one.

5.1 Verification toolkit, first version

Build it small and in-house first, because the needs are narrow and fully testable:

- Exact arithmetic: arbitrary-precision rational numbers; a parser for addition,
  subtraction, multiplication, division and integer powers, with parentheses, decimals
  and fractions; declared rules for rounding and for comparing with stated decimal
  results.
- Units: the 7 SI base dimensions as exponent vectors; a table of the units a school
  curriculum uses (m, s, kg, N, J, W, Pa, V, A, ohm, Hz, K, degrees Celsius, and the
  common prefixes); conversion and consistency checks.
- Substitution: put the answer and the givens into each relation, and compare both
  sides exactly or within a declared tolerance.

Algebraic equivalence needs a computer algebra system (CAS). Adopt one only when a
profile needs it, and choose it through a recorded decision (D6). Candidate libraries
and their licences: SymEngine (C++, MIT), GiNaC (C++, GPL-2.0 or later), Giac (GPL-3.0),
SymPy (Python, BSD; it would add a Python runtime). The project licence (D1) constrains
this choice.

5.2 What the prototype keeps, changes and drops

Keep:

- the bundled llama.cpp engine with job-object supervision;
- SQLite, FTS5, and storage of raw model output;
- the keyboard-first behaviour of the UI;
- retrieval (hybrid search);
- the five-part teaching structure;
- constrained JSON decoding.

Change:

- the supervisor gets a single owner, a per-launch API key and a cancellable start
  (TD-01, TD-04, TD-10);
- storage gets versioned migrations and a repository layer (TD-15, TD-16);
- MainWindow is reduced to a view (TD-30);
- retrieval gets model-fingerprinted vectors, query prefixes, and a measured benefit
  (TD-14);
- the "Check" part is produced by verifiers, not by the model (TD-32);
- constrained JSON decoding is used for the solution IR.

Drop or archive (pending decisions):

- email/password accounts with emailed codes, and the SMTP and TLS client (about 1,760
  lines), replaced by local profiles (D2);
- the Ollama dialect in the client (D5, TD-11);
- the lexical rule that lets off-topic words override the subject (TD-06);
- the built-in self-test inside the product binary (TD-50);
- the fine-tuning pipeline, moved to tools/experimental/ until an evaluation shows a
  need for it (TD-42);
- the lossy conversion of LaTeX to plain text, replaced by real maths rendering (TD-07,
  D7).


6. SCOPE AND AUDIENCES

- Phase 1: the existing audience (ages 11 to 14, maths and physics) as the first
  profile. Verification is most tractable at this level, and the prototype's prompts
  and seed examples give a head start.
- Phase 2: further profiles (for example secondary-school algebra and mechanics, or
  introductory chemistry stoichiometry, where units and conservation checks apply). A
  profile is only added together with its own evaluation set and baseline.
- Topic restriction becomes a policy of each profile. A tutor profile for children may
  keep refusals. A general STEM profile may not need them. Either way the policy is
  measured by false-refusal and false-acceptance rates, not by word lists.


7. NON-GOALS

To keep the project small enough to finish:

- No hosted or cloud service, no multi-user server, no account backend.
- Not a general-purpose chatbot. Capability outside STEM is not a target.
- No model training in the core project. Fine-tuning is out of scope until an
  evaluation shows that following the answer format, not reasoning, is the bottleneck.
- No formal proof assistants (Lean, Coq) and no research-level mathematics.
- No plugin marketplace or third-party extension API in the foreseeable roadmap.
  Verifiers are built in, behind one small internal interface.
- No mobile apps. A cross-platform desktop app is a possible later step (D3), not a
  commitment.


8. HOW SUCCESS IS MEASURED

All measurements come from the evaluation harness (roadmap milestone M4) on a declared
reference machine:

- Answer accuracy: the final answer matches the ground truth (value within tolerance,
  and the correct unit).
- Precision of the "Checked" label: of the answers labelled Checked, the fraction that
  are correct against the ground truth. This is the product's honesty metric.
- Check coverage: the fraction of problems where at least one deterministic check ran.
- False refusal and false acceptance: for profiles with a scope policy, measured against
  a labelled set.
- Latency: the median (p50) and 95th-percentile (p95) time to a complete answer on the
  reference CPU-only machine.
- Network egress: zero connections other than to the local machine during a normal
  session.

Targets are set after the first baseline has been measured, not before. Any number
chosen now would be invented.


9. OPEN DECISIONS

D1  Project licence.
    Why it matters: it is required before publishing, and it constrains the choice of
    computer algebra system (GPL or permissive).
    Recommendation: choose before the first public commit. Apache-2.0 or MIT keeps all
    the listed options open except the GPL algebra libraries.

D2  Keep the email/password/"2FA" accounts, or replace them with local profiles?
    Why it matters: about 20% of the C++ code, privacy obligations for children's data,
    and blocking of the UI thread (TD-12, TD-18).
    Recommendation: replace them with local profiles bound to the Windows account,
    unless a deployment contract requires the current model. If it does, record that
    requirement.

D3  Windows only, or cross-platform?
    Why it matters: it determines the UI technology, and how strictly core must stay
    portable.
    Recommendation: keep core portable regardless. Decide the shell later, with data
    (D7).

D4  Audience: school level only, or broader STEM?
    Why it matters: the scope policy, the profiles, and how deep the verifiers must go.
    Recommendation: start at school level (Phase 1); broaden one profile at a time, and
    only with evaluation sets.

D5  Keep the Ollama backend?
    Why it matters: it is currently broken while the bundled runtime is present (TD-11),
    and it duplicates a protocol.
    Recommendation: remove the Ollama dialect, and support any OpenAI-compatible local
    server instead.

D6  Which computer algebra system, if any, and when?
    Why it matters: licence compatibility, binary size, maintenance.
    Recommendation: defer until a profile needs algebraic steps to be checked, then
    decide in a recorded architecture decision.

D7  UI shell technology for real maths rendering.
    Why it matters: the Win32 RichEdit shell cannot typeset mathematics.
    Recommendation: compare Win32 plus a C++ maths renderer (for example MicroTeX, MIT)
    with an embedded web view and a bundled copy of KaTeX (MIT), both fully offline,
    against explicit criteria (roadmap milestone M8).
