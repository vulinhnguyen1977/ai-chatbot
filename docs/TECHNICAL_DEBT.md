TECHNICAL DEBT REGISTER

Findings from the architecture audit of 2026-09-27. Each item has an ID used by
CURRENT_ARCHITECTURE.md and ROADMAP.md. Nothing here has been fixed yet.

File references are written as path:line, relative to the repository root.

Severity levels:

- High: wrong results, undefined behaviour, or a security exposure reachable in normal
  use.
- Medium: degrades reliability, quality or maintainability; reachable, but with a
  limited blast radius.
- Low: cosmetic, unlikely, or cheap to live with for now.

No item is rated Critical. Nothing found causes silent loss of stored data in normal use.

Confidence labels:

- Verified (run): reproduced or measured during the audit.
- Verified (code): deterministic from reading the code; not executed.
- Inferred: likely from reading the code, but depends on runtime behaviour that was not
  reproduced.


SUMMARY

ID     Severity      Confidence           Milestone   Title
TD-01  High          Inferred             M3          Two threads can launch the model server concurrently at startup
TD-02  High          Verified (code)      M3          Questions can be sent during a model swap; unsynchronised URL write
TD-03  High          Verified (code)      M3          Switching chats while an answer streams corrupts the transcript
TD-04  High          Verified (run)       M3          Local model server usable by any web page (no key, reflected CORS)
TD-05  High          Verified (code)      M3          No context or token budgeting for prompts (overflow effect inferred)
TD-06  High          Verified (code)      M3          Off-topic word list refuses legitimate word problems
TD-07  High          Verified (code)      M3 then M8  Maths normaliser changes the meaning of expressions
TD-08  Medium        Verified (code)      M3          Stopped answers are stored and replayed as complete
TD-09  Medium        Verified (code)      M3          Refused and injection messages are replayed to the model
TD-10  Medium        Verified (code)      M3          Closing the window can leave a hidden, hung process
TD-11  Medium        Verified (code)      M3 (D5)     llm.backend = ollama broken whenever the bundled runtime exists
TD-12  Medium        Verified (code)      M5 (D2)     Sign-in work (PBKDF2, SMTP) blocks the UI thread
TD-13  Medium        Verified (code)      M6          Streaming re-renders the whole answer for every token
TD-14  Medium        Verified (code)      M4          Embedding identity mismatch risk; weak retrieval defaults (quality unmeasured)
TD-15  Medium        Verified (code)      M5          One shared SQLite connection with convention-based locking
TD-16  Medium        Verified (code)      M3 then M5  No schema migrations; settings stored in schema_meta
TD-17  Medium        Verified (code)      M3          Config parser: inline comments, silent fallbacks, default mismatch
TD-18  Medium        Verified (code+run)  M5 (D2)     Account and email "2FA" model mismatched to a local app
TD-19  Low           Verified (code)      M5 (D2)     STARTTLS keeps bytes buffered before TLS
TD-20  Low           Verified (code)      M3          Tutor generation has no length or wall-clock limit
TD-21  Low           Inferred             M6          Word-delete key handler installed on the read-only transcript
TD-22  Low           Verified (run)       M5 (D2)     Outbox never purged; empty From address in file mode
TD-23  Low           Verified (code)      M5 (D2)     Residual account-enumeration signals
TD-30  High (arch.)  Verified (code)      M6          MainWindow is the application controller (god object)
TD-31  Medium        Verified (code)      M9          Subject and audience hard-coded across about 10 files
TD-32  High (arch.)  Verified (code)      M7          Free-text-only answers; the "Check" step is model self-assessment
TD-33  Low           Verified (code)      M5/M6       Web-server-era remnants and vestigial session model
TD-34  Low           Verified (code)      M6          Ollama-named configuration used for the llama.cpp path
TD-35  Medium        Verified (code)      M6          Three error-handling styles, silent swallowing, no logging
TD-36  Medium        Verified (code)      M6          Portable logic entangled with Windows-only code
TD-37  Low           Inferred             M6          Retrieved context sent as a mid-conversation system message
TD-40  Low           Verified (code)      M2/M6       Dead code and unused columns and fields
TD-41  Medium        Verified (run)       M2/M6       Duplicated logic, some of it already diverged (prompt drift verified)
TD-42  Medium        Verified (code)      M2          Fine-tuning pipeline cannot affect the shipped product
TD-43  Low           Verified (run)       M2          Invisible characters in a Python regular expression
TD-50  Medium        Verified (code)      M2          Test code compiled into the shipping binary
TD-51  Medium        Verified (code)      M2          Assertions that cannot fail; untested success paths
TD-52  Low           Verified (run)       M2          Tests are not hermetic
TD-53  High          Verified (code)      M2/M4       Large untested surface; no answer-quality evaluation at all
TD-60  High          Verified (run)       M0/M1       No version control, no CI
TD-61  High          Verified (code)      M0          No LICENSE; third-party notice obligations unmet
TD-62  Medium        Verified (run)       M1          Three build paths; unpinned toolchain; unknown binary provenance
TD-63  Medium        Verified (code)      M1          Engine download has no integrity check
TD-64  Low           Verified (code)      M0          No single version source
TD-65  Medium        Verified (run)       M0          Working tree contains personal data and machine-specific files
TD-66  Low           Verified (code)      M1          Packaging tests whatever binary happens to exist
TD-70  Low           Verified (code)      M0          Documentation drift from the code


1. CORRECTNESS, LIFECYCLE AND CONCURRENCY

TD-01  Two threads can launch the model server concurrently at startup

Evidence: WM_CREATE calls try_restore_session() and then show_chat() before
start_runtime() (src/gui.cpp:1546-1551). show_chat() calls populate_presets(), which
compares the user's stored preset with runtime.active_preset() and, if they differ,
calls on_preset_changed() (src/gui.cpp:1409). That spawns switch_thread_, which calls
switch_preset() and then launch_chat(). At that moment active_preset() still holds the
constructor's presets.default, not the boot preset that start() is about to use.
Immediately afterwards start_runtime() spawns runtime_thread_, which calls start() and
then launch_chat() as well.

Failure scenario: a user ticks "keep me signed in" and switches to Fast (while
presets.default = smart), then reopens the app. Two threads now run launch_chat() at the
same time against the unsynchronised impl_->chat_process, chat_port and status_. Each
may terminate or close the other's process handle (src/runtime.cpp:297-302), and both
probe port 11500. The job object is only created inside start() (src/runtime.cpp:443),
so a child spawned first by the swap thread is not assigned to it (src/runtime.cpp:144)
and can outlive the app while holding about 3 GB of memory. The same race happens when
any user with a different preset signs in while the initial load is still running. This
contradicts the README, which says the app "starts up on the last one used so it does
not load one model and immediately swap it".

Direction: give the runtime a single owner (a command queue serviced by one thread).
Compare against the requested boot preset. Create the job object in the constructor. Let
the UI read immutable snapshots of the status.


TD-02  Questions can be sent during a model swap; unsynchronised URL write

Evidence: on_preset_changed() disables only the Send button. send_question() checks
generating_ but not switching_ (src/gui.cpp:1117). Pressing Enter in the input box calls
send_question() directly (src/gui.cpp:385-391), and so does Ctrl+Enter through the
accelerator table (src/gui.cpp:1287).

Failure scenario: the question goes to a server that is being terminated or is still
loading, and fails with a raw error. When the swap completes, on_preset_switched()
reassigns llm::Client's chat_url_ on the UI thread (src/gui.cpp:1463) while the worker
may be reading it in chat_stream(). That is a data race on a std::string, which is
undefined behaviour. The same write happens in WM_APP_READY (src/gui.cpp:1687), which
can overlap a worker started through the is_available() fallback, because the default
llm.url is the same port the runtime binds first. The README says "Send is disabled
while a swap is in progress".

Direction: a single UI state machine (Idle, Generating, Switching, Starting) that gates
every entry point. Make the endpoint fixed for each request, passed by value or copied
under a lock.


TD-03  Switching chats while an answer streams corrupts the transcript

Evidence: chat selection, New chat, Ctrl+Tab and Delete are not disabled while an
answer is generated. on_stream_update() truncates the transcript from stream_anchor_ and
re-renders (src/gui.cpp:1215-1217), whatever chat is currently displayed. The worker
reads user_, active_chat_ and pending_question_ from MainWindow without synchronisation
(src/gui.cpp:1174). Choosing Account and then Sign out during generation resets user_ at
the same time.

Failure scenario: the student opens another chat mid-answer. The streamed answer is
painted into that chat, part of its transcript disappears from view, and on completion
the view jumps back to the original chat. The database is correct; the display is not.

Direction: bind each generation to a fixed request (user id, chat id, text). Only render
stream updates if the displayed chat matches, or lock navigation while generating.


TD-05  No context or token budgeting for prompts

Evidence: build_prompt() (src/chat.cpp:221-262) always includes the tutor prompt of
about 1,000 tokens, the last 2 x chat.history_turns (16) stored messages, and up to 4
passages of 1,500 bytes each. None of it is counted. Qwen3's reasoning tokens are
generated into the same 8,192-token window (the -c option, confirmed via /props). The
token usage returned by the server is parsed and then discarded (src/llm.cpp:366-369).

Failure scenario: after a handful of long exchanges, the prompt plus the reasoning
exceeds the context. What happens then (the request is rejected, or context is silently
shifted out) depends on llama-server and was not reproduced. Either outcome is bad: an
opaque error, or context silently lost in the middle of a maths problem.

Direction: count tokens (llama-server exposes a tokenizer endpoint), reserve a budget
for generation, drop the oldest turns first, and record usage.


TD-06  Off-topic word list refuses legitimate word problems

Evidence: in lexical_stage(), any whole-word match in kOffTopicTerms returns OffTopic
before maths or physics vocabulary is considered (src/guard.cpp:175-179). The list
(src/guard.cpp:54-69) contains words that routinely appear in school word problems.

Failure scenario: each of these is refused instantly, with no model call (the triggering
term is in brackets):

- "An athlete runs 100 m in 12.5 s. What is her average speed?" [athlete]
- "A recipe needs 2/3 cup of sugar. How much for 3 batches?" [recipe]
- "Translate the triangle 3 units to the right." [translate, even though "translation"
  is in the maths list]
- "A battery is made of two cells giving 3 V. What is the voltage of one cell?" [cell,
  cells]
- "The population of a town grows by 5% a year..." [population of]
- "In a tournament every team plays every other team once..." [tournament]
- "A shop has 200 items in stock and sells 15%..." [stock]
- "A football is kicked at 20 m/s..." [football]
- "A doctor prescribes 5 ml of medicine per 10 kg..." [doctor, medicine]

This directly contradicts the classifier prompt, which says "A word problem that is
dressed up in a story is still math or physics". No self-test covers these cases.

Direction: off-topic vocabulary should be a signal, not a verdict, when there is strong
in-scope evidence (numbers with units or operators, subject terms). Measure the
false-refusal rate on a labelled set (M4) before and after.


TD-07  Maths normaliser changes the meaning of expressions

Evidence: simplify_scripts() removes the braces from every superscript and subscript
group, not only single characters (src/markdown.cpp:192-211). \times and \cdot both
become " x " (src/markdown.cpp:28), and \circ becomes nothing.

Failure scenario: the model writes correct LaTeX, and the student sees a different
expression:

- x^{n+1} is shown as x^n+1, which reads as "x to the power n, plus 1".
- 2^{3x} is shown as 2^3x, which reads as "2 cubed, times x".
- e^{-kt} is shown as e^-kt, which is ambiguous.
- 3x \cdot x is shown as "3x x x": the multiplication sign collides with the variable x.
- 90^\circ is shown as "90^", with a dangling caret.

The tests cover only single-character groups such as x^{2}. The system prompt itself
tells the model to write "12 x 7", which builds the same ambiguity in.

Direction: in the interim, put parentheses around multi-character groups and use the
multiplication sign (Unicode U+00D7). In the long term, render real mathematics instead
of lossy plain text (M8).


TD-08  Stopped answers are stored and replayed as complete

Evidence: when the user stops an answer, chat_stream() returns ok = true with the
partial text (src/llm.cpp:390-394). ask() stores it as an ordinary assistant message
(src/chat.cpp:375-376). There is no status column.

Impact: half-finished working is later shown without any marker, and is replayed to the
model as if it were authoritative.

Direction: add a status column to messages (complete, stopped or failed). Leave
incomplete turns out of the history, or mark them there.


TD-09  Refused and injection messages are replayed to the model

Evidence: off-topic and small-talk exchanges are stored as normal user and assistant
turns (src/chat.cpp:310-312). build_prompt() replays every user and assistant row
regardless of the off_topic flag (src/chat.cpp:249-252).

Failure scenario: "Ignore your instructions and ..." is refused by the lexical gate. The
next in-scope question sends that exact text, plus the canned refusal, to the tutor
model as history. The gate becomes a delivery mechanism for the injection it blocked.

Direction: do not replay turns flagged off-topic, or replace them with a neutral
placeholder.


TD-10  Closing the window can leave a hidden, hung process

Evidence: WM_DESTROY joins worker_, runtime_thread_ and switch_thread_ on the UI thread
(src/gui.cpp:1714-1722). cancel_ is only noticed when a token or reasoning chunk
arrives. The non-streaming classifier call (60-second timeout), the embedding call and
prompt evaluation produce no callbacks. Model loading cannot be cancelled at all (up to
startup_timeout_s, 300 seconds, plus 120 seconds for embeddings).

Failure scenario: closing the window while the model loads on first run makes the window
disappear while the process stays alive for up to several minutes. Relaunching then
starts a second instance, which picks new ports.

Direction: terminate the child processes first, make waits cancellable, and never join
unbounded work on the UI thread.


TD-11  llm.backend = ollama broken whenever the bundled runtime exists

Evidence: start_runtime() checks only runtime.available() (src/gui.cpp:1493), never the
backend. WM_APP_READY and run_index then overwrite the client URL with llama-server's
(src/gui.cpp:1687, src/main.cpp:176). The client keeps speaking the Ollama dialect
(/api/chat and /api/embed).

Impact: the documented alternative backend sends its requests to the wrong server, and
several GB of unused weights are loaded as well.

Direction: decision D5. Either remove the Ollama dialect (recommended; llama-server and
Ollama both offer OpenAI-compatible endpoints), or start the bundled runtime only for
the llama.cpp backend.


TD-12  Sign-in work blocks the UI thread

Evidence: do_auth_primary(), do_resend() and account deletion call Auth synchronously
(src/gui.cpp:908-947 and src/gui.cpp:1085-1099). These calls run PBKDF2 (210,000
iterations for passwords, 50,000 for each issued code) and, with SMTP, blocking network
I/O with 30-second socket timeouts at each step. Session restore also runs on the UI
thread, in WM_CREATE.

Impact: the window freezes ("Not responding") during sign-in, for a long time if the
SMTP server is slow or unreachable.

Direction: only worth fixing if accounts survive decision D2. Otherwise the problem
disappears with them.


TD-13  Streaming re-renders the whole answer for every token

Evidence: each chunk posts WM_APP_STREAM (src/gui.cpp:1160). Each handler truncates the
transcript and re-parses and re-renders the whole text received so far
(src/gui.cpp:1190-1220).

Impact: the cost grows with the square of the answer length. It is tolerable at the
12 to 15 tokens per second measured on CPU (see runtime/ls.err). With a GPU (tens to
hundreds of tokens per second), the UI thread and its message queue will be saturated.

Direction: combine updates on a timer (for example every 50 ms) and render
incrementally.


TD-14  Embedding identity mismatch risk; weak retrieval defaults

Evidence:

- Vectors are labelled with llm.embed_model (nomic-embed-text, src/rag.cpp:85), while
  the file actually loaded is runtime.embed_model (src/runtime.cpp:265-267). Swapping
  the GGUF file leaves old vectors that look current.
- dot() silently truncates to the shorter vector (src/rag.cpp:43-48) instead of
  rejecting a dimension mismatch.
- nomic-embed-text expects task prefixes ("search_query:" and "search_document:"), and
  none are used.
- The lexical search joins every token of 3 or more characters with OR, with no
  stop-word list (src/rag.cpp:53-76), so "what", "the" and "how" match almost every
  chunk. rag.min_score filters only the dense search, so some passage is added to the
  prompt for nearly every question.

Impact: irrelevant context is added to prompts, and after a model swap retrieval can
silently turn into noise. Retrieval quality has never been measured.

Direction: store a fingerprint of the embedding model file with the vectors. Reject
dimension mismatches. Apply the prefixes. Add stop-words and a lexical threshold.
Evaluate with and without retrieval (M4).


TD-15  One shared SQLite connection with convention-based locking

Evidence: a single connection opened with FULLMUTEX is shared by the UI and worker
threads (src/db.cpp:9-25), and callers lock Database::mutex() by hand. Auth::audit()
runs without the lock (src/auth.cpp:61-71), although it is called both inside and
outside locked sections. Transactions belong to the connection, so an unlocked statement
can execute inside another thread's BEGIN IMMEDIATE. last_insert_rowid() is global to
the connection. PBKDF2 runs while the lock is held (src/auth.cpp:207-240 and
src/auth.cpp:285-290).

Impact: correctness depends on every future contributor remembering an undocumented
rule. Today the consequences are small; the pattern does not scale.

Direction: a repository layer that owns the connections (one per thread, or a single
database executor), with no raw handle or mutex exposed to callers.


TD-16  No schema migrations; settings stored in schema_meta

Evidence: migrate() only runs CREATE ... IF NOT EXISTS statements and hard-codes schema
version 1 (src/db.cpp:175). There is no way to alter a table on an existing
installation. App-wide preferences are written into schema_meta (src/prefs.cpp:46-54).
The corpus table definitions are duplicated in training/3_ingest_corpus.py.

Impact: every fix that needs a new column (the status column of TD-08, profiles,
verification results) is blocked on this.

Direction: numbered, tested migrations; a separate settings table; one owner for the
table definitions.


TD-17  Config parser: inline comments, silent fallbacks, default mismatch

Evidence: Config::load() does not strip inline comments (src/config.cpp:30-39). The
README's own example line "accelerator = cpu      ; or vulkan, or auto (default)"
(README.md:116) produces an unrecognised value, which LocalRuntime treats like auto, so a
user forcing CPU silently gets Vulkan (src/runtime.cpp:169-178). Numbers that cannot be
parsed silently fall back to the defaults. The code default for llm.timeout_s is 300
(src/config.h:51), while the shipped ini says 600.

Direction: support or reject inline comments explicitly. Report unknown keys and invalid
values in "mstutor check" and in the log. Keep a single source for the defaults.


TD-20  Tutor generation has no length or wall-clock limit

Evidence: the tutor call sets no num_predict or max_tokens (src/chat.cpp:342-350). The
HTTP read timeout applies to each read, so a response that keeps streaming never times
out.

Impact: a reasoning loop can run until the context is exhausted; only the Stop button
ends it.


TD-21  Word-delete key handler installed on the read-only transcript

Evidence: install_edit_key_fixes(transcript_) (src/gui.cpp:571). The handler deletes
text with EM_REPLACESEL, the same message the app uses to write into the read-only
transcript. It also mixes GetWindowText offsets (lines end in CR LF) with RichEdit
selection offsets (lines end in CR only).

Impact (inferred): Ctrl+Backspace or Ctrl+Delete while the transcript has focus probably
deletes displayed text and may shift the streaming anchor.


2. SECURITY AND PRIVACY

TD-04  Local model server usable by any web page

Evidence (verified by probing the running instance): requests to 127.0.0.1:11501 and
127.0.0.1:11600 carrying the header "Origin: https://attacker.example" returned HTTP 200
with "Access-Control-Allow-Origin: https://attacker.example" on /health, /props, /slots
and /v1/models. No --api-key is passed (src/runtime.cpp:103-120). The leftover log
runtime/ls.err records llama-server's own warning that CORS allows all origins and no
API key is set, and that "this can be a security risk". The ports are predictable
(from 11500 and from 11600).

Impact: any website open in the student's browser can run completions on the local
model, slow the tutor down by keeping it busy, and read the server's properties. On
build b10331, /slots exposed only id, n_ctx, speculative and is_processing; no
conversation content was seen, though this was not tested exhaustively. The README's
"Nothing listens on the network" is true only for remote hosts.

Direction: generate a random --api-key for each launch and send it from the client;
disable engine endpoints the app does not use, where the engine allows it; add a
regression test that an unauthenticated request is rejected.


TD-18  Account and email "2FA" model mismatched to a local app

Evidence:

- All data (email addresses, chats, the audit trail) sits unencrypted in
  data/mstutor.db. Anyone who can read the folder can read everything, and anyone who
  can write to it can mark accounts verified or replace password hashes. The password
  protects only the UI.
- With the default mail.transport = file, the "second factor" is a plain-text .eml file
  in data/outbox, the same folder as the database. A verification email from 2026-08-17
  is still there, because the outbox is never purged.
- The README itself argues that "the Windows sign-in is the real access control" (to
  justify a 30-day remember-me). By that argument, the password and email layer adds
  little protection.
- The app collects the email addresses of 11-14-year-olds, which the tutoring function
  does not need. That is a privacy-law burden (for example COPPA, or GDPR Article 8) for
  anyone who redistributes it.
- About 1,760 non-blank lines (auth, crypto, credential store, SMTP, TLS), roughly 20% of
  the C++, exist to maintain this.

Direction: decision D2. Replace it with local profiles bound to the Windows account
(optionally with a PIN), or keep accounts behind a build option if a deployment really
requires them. If a client contract mandates the 180-day cap or 2FA, record that
explicitly.


TD-19  STARTTLS keeps bytes buffered before TLS

Evidence: the SmtpSession object is created before STARTTLS (src/mailer.cpp:247). Its
buffer_ survives the TLS upgrade (src/mailer.cpp:260-267), so plain text injected by an
attacker on the network path would be parsed as replies received after TLS. This is the
known STARTTLS response-injection class of bug. Credentials are still only sent after
TLS, so the severity is low.


TD-22  Outbox never purged; empty From address in file mode

Evidence: .eml files in data/outbox, each containing a code, accumulate indefinitely,
and their file names contain the recipient's address. With mail.from empty, the header
reads "From: Middle School Tutor" followed by an empty address in angle brackets (seen in
the snapshot).


TD-23  Residual account-enumeration signals

Evidence: sign-up returns Invalid for an existing verified address but
NeedsVerification for a new one, so the UI visibly differs (src/auth.cpp:214-217). The
Locked message can only appear for existing accounts (src/auth.cpp:305-311). Signing up
again with an unverified address overwrites its password. This is a low risk in a local
app, and moot if D2 removes accounts.


3. ARCHITECTURE AND COUPLING

TD-30  MainWindow is the application controller

About 1,500 non-blank lines in one class (src/gui.cpp) own the widgets, the sign-in
state machine, remember-me persistence, the generation worker, runtime start-up, preset
switching and its persistence, and the hand-off of the server URL from the runtime to the
client (which is wired again, separately, in main.cpp for the index command). The class
receives eight concrete services through AppContext. None of this logic can be tested
without a window, and TD-01, TD-02, TD-03 and TD-10 all live here. This is the single
largest obstacle to change.


TD-31  Subject and audience hard-coded across the tree

"Middle school maths and physics" is built into prompts.h, the guard vocabularies
(guard.cpp), the UI strings and examples (gui.cpp), the email bodies (auth.cpp), the
config defaults (mail.from_name), res/mstutor.rc, the normaliser's plain-text choices
(markdown.cpp), and the training vocabulary (training/common.py). The README tells users
to fix false refusals by editing guard.cpp and rebuilding. Any change of scope or level
touches about ten files.


TD-32  Free-text-only answers; the "Check" step is model self-assessment

The model returns prose only. Step 5 of the answer format ("Check") asks the same model
to verify itself (src/prompts.h:22). There is no structured representation of the
givens, relations, steps or final answer, so no deterministic tool can check anything.
This blocks the project vision (see PROJECT_VISION.md).


TD-33  Web-server-era remnants and vestigial session model

Comments refer to an "HTTP layer" and "routes" (src/chat.h:3-5), an "API" that must not
enumerate users (src/auth.h:8-10), "the server" (src/config.cpp:62), "unrelated
requests" (src/auth.cpp:136-137), an "hourly" purge that does not exist
(src/auth.h:102), and asking "a question in the browser"
(training/7_build_ollama_model.py:157). The bearer-token session model comes from that
design. In the desktop app the UI never presents a token after sign-in, so sessions only
matter for remember-me.


TD-34  Ollama-named configuration drives the llama.cpp path

ollama_temperature(), ollama_num_ctx(), ollama_timeout_s() and ollama_think()
(src/config.h:48-51) are used for the default llama.cpp backend (src/chat.cpp:343-350,
src/runtime.cpp:269). The Ollama model name "mstutor" is sent to llama-server as the
model field.


TD-35  Three error-handling styles, silent swallowing, no logging

The code mixes exceptions, functions returning bool with a std::string pointer
out-parameter for the error, and status structs. Many functions catch every exception
and return an empty result without recording anything: list_chats, get_messages, the
prefs functions, the Retriever methods, purge_expired and log_out. In GUI mode stdout is
detached, so the [warn] output is lost, and llama-server runs with --log-disable.
Failures in the field therefore cannot be diagnosed. Some internals leak into text shown
to students ("Details: ...", "Internal error: step failed: ...").


TD-36  Portable logic entangled with Windows-only code

Crypto (CNG), TLS (Schannel), DPAPI, job objects, the registry and the Win32 UI are
expected to be platform-specific. But generic helpers also live in UI modules: main.cpp
includes gui_richtext.h just for widen and narrow. There is no boundary that would let
the chat, guard, retrieval or rendering logic be built or tested on another operating
system.


TD-37  Retrieved context sent as a mid-conversation system message

Passages are added as a second system message after the history
(src/chat.cpp:254-259). Qwen3's chat template accepts this; the templates of some other
model families reject system messages that are not first. Switching to another model
family may therefore fail or degrade (inferred).


4. DEAD AND DUPLICATED CODE

TD-40  Dead code and unused fields

- Auth::load_user_by_email (src/auth.cpp:88): never called; log_in repeats the same
  query inline.
- Client::has_model and Client::list_models (src/llm.cpp:195-248): no callers.
- Config::values() (src/config.h:105): no callers.
- md::to_plain_text (src/markdown.cpp:679): the header says it is used for the
  clipboard, but there is no clipboard code; only the tests use it.
- ChatSummary::preview and message_count (src/chat.cpp:88-104): two correlated
  sub-queries per chat on every refresh of the chat list; never displayed.
- AskResult::user_message_id, title_changed and chat_title (src/chat.h:41-52): not used
  by the UI.
- ChatResult::prompt_tokens and completion_tokens (src/llm.h:59-60): parsed, never used.
- guard::Decision::reason (src/guard.h:34): never logged or shown.
- runtime::Status::error (src/runtime.h:34): set, but not read by the UI.
- chats.deleted_at (src/db.cpp:110): every query filters on it, but it is never set
  (deletes are hard deletes).
- doc_chunks.token_est (src/db.cpp:133): written by Python, never read.


TD-41  Duplicated logic, some of it already diverged

- Tutor system prompt: src/prompts.h and training/Modelfile. Diverged (verified). The
  Modelfile still has the old one-line rule about LaTeX, despite the stated guarantee
  that the two "cannot drift apart".
- Subject vocabulary: src/guard.cpp:16-49 and training/common.py:27-45. Diverged.
- Corpus table definitions: src/db.cpp:126-163 and training/3_ingest_corpus.py:27-56.
  Identical today.
- Sources JSON: src/chat.cpp:20-30 (built with nlohmann/json) and src/gui.cpp:1233-1242
  (built by string concatenation, without escaping). A source name containing a double
  quote makes the citation line disappear from the live answer.
- executable_dir(): src/main.cpp:77 and src/runtime.cpp:33. Identical.
- UTF-8 to UTF-16 widen(): src/runtime.cpp:59 and src/gui_richtext.cpp:132. Nearly
  identical.
- Window-text getter: src/gui.cpp:100 and src/gui_edit.cpp:24. Identical.
- Vulkan-to-CPU fallback: src/runtime.cpp:319-327 and src/runtime.cpp:344-355.
  Identical.
- Build source list: CMakeLists.txt:32-55 and Makefile:32-35. Identical today.
- Keyboard shortcut list: README.md, src/gui.cpp:1342-1379, and the accelerator table at
  src/gui.cpp:1272-1288. Kept in sync by hand.


TD-42  Fine-tuning pipeline cannot affect the shipped product

Steps 4 to 7 produce a LoRA adapter and an Ollama model ("ollama create mstutor"). The
bundled llama-server is never given an adapter (--lora) or the Modelfile, so with the
default backend the fine-tune has no effect. training/README.md still calls the Ollama
step "the only step that is mandatory" and says the index command embeds "via Ollama".
No adapter or dataset exists in the tree, so the pipeline's end-to-end state is unknown.


TD-43  Invisible characters in a Python regular expression

In every viewer, line 82 of training/common.py appears as

  re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f-]", "", text)

which looks as if it strips every hyphen and minus sign from a maths corpus. In fact,
the source holds the invisible private-use characters U+E000 and U+F8FF on either side
of that final hyphen, forming a character range that no viewer renders. Verified during
the audit by dumping the code points and running clean_text(): minus signs survive. It
is not a bug, but it is guaranteed to mislead a reviewer. Rewrite the range with escape
sequences for U+E000 and U+F8FF instead of the literal characters.


5. TESTS

TD-50  Test code compiled into the shipping binary

selftest.cpp (about 800 lines) and its dependencies ship inside mstutor.exe. Tests
cannot use a test framework, fakes or fixtures without bloating the product, and
test-only hooks leak into the product's API.


TD-51  Assertions that cannot fail; untested success paths

- src/selftest.cpp:173 accepts either outcome (Invalid, or ok()).
- src/selftest.cpp:299 checks "not ok(), or status == Ok", which is always true.
- The success path of code verification is never exercised, although a comment says the
  code is found by brute force (src/selftest.cpp:176-177).
- Account deletion is tested with a raw DELETE statement rather than
  confirm_account_deletion(), so audit anonymisation is untested.


TD-52  Tests are not hermetic

Config reads MSTUTOR_ environment variables, including in tests that assert default
values, so a developer's environment changes the results. Each run leaves .eml files in
%TEMP%\mstutor-selftest-outbox (four per run, verified).


TD-53  Large untested surface; no answer-quality evaluation

The self-test (275 checks, all passing, verified) covers utilities, crypto test vectors,
sign-in flows, remember-me, DPAPI, preferences, UTF-8, word boundaries, 32 guard cases
and the normaliser.

Not tested at all:

- ChatService::ask(), prompt assembly and history
- parsing of the event stream, aborts and error paths
- the runtime supervisor
- every GUI state transition and thread interaction (where TD-01, TD-02, TD-03 and TD-10
  live)
- MIME and SMTP, TLS
- retrieval ranking and context limits

There is no evaluation of answer correctness at all, which is the product's core claim.
training/README.md admits that "a confidently-formatted wrong answer is worse than no
answer", but offers only manual spot-checks.


6. BUILD, PACKAGING AND REPOSITORY

TD-60  No version control, no CI

The tree is not a git repository, and git is not installed on the development machine
(verified). There is no history, no review trail and no automated build.


TD-61  No LICENSE; third-party notice obligations unmet

There is no LICENSE file. package.ps1 lists one, and silently skips it. Without a
licence the project cannot be open source. THIRD_PARTY_NOTICES.md says the llama.cpp MIT
licence "should be kept in runtime/bin/". It is not there, and no Apache-2.0 licence
text ships with the Qwen3 or nomic weights.


TD-62  Three build paths; unpinned toolchain; unknown binary provenance

CMake, the Makefile and the fallbacks in build.bat duplicate configuration. No compiler
is installed on the audit machine and no toolchain version is recorded, so mstutor.exe
cannot be tied to the sources. File timestamps suggest it matches, which is weak
evidence.


TD-63  Engine download has no integrity check

fetch_runtime.ps1 downloads and extracts executables from GitHub with no SHA-256
verification (ship/fetch_runtime.ps1:56).


TD-64  No single version source

res/mstutor.rc hard-codes version 1.0.0.0, the folder is named v0.1.0, and there is no
--version option.


TD-65  Working tree contains personal data and machine-specific files

data/ holds a real development account, its chats and a verification email.
.claude/settings.json holds machine-specific paths. Multi-gigabyte artefacts (a 4 GB zip,
4 GB of weights, about 300 MB of engines) sit next to the sources. .gitignore covers most
of this, but not .claude/. Everything must be checked before the first commit.


TD-66  Packaging tests whatever binary happens to exist

package.ps1 runs selftest on the existing mstutor.exe, but neither rebuilds it nor
checks that it is current.


TD-70  Documentation drift

- The README links to a "Speed, honestly" section (anchor speed-honestly) that does not
  exist.
- Its example config line with an inline comment breaks the parser (TD-17).
- It says "Send is disabled while a swap is in progress" (TD-02).
- It says the model server "cannot outlive the app" (TD-01).
- A comment at src/gui.cpp:114-116 says the app has no resource script, but
  res/mstutor.rc exists.
- training/README.md describes the Ollama-era architecture (TD-42).


7. BEHAVIOUR THAT IS UNCLEAR OR UNVERIFIABLE TODAY

U-01  How often are the tutor's answers actually correct?
      No evaluation set or results exist. The README's timings are one machine's
      anecdote; runtime/ls.err confirms about 12 to 15 tokens per second for the 1.7B
      model on CPU.

U-02  Does mstutor.exe correspond to these sources?
      There is no version control, no build record and no compiler available.

U-03  What happens when the context overflows on llama.cpp b10331?
      Not reproduced (TD-05).

U-04  Does the Vulkan-to-CPU fallback work on machines without drivers, and is Vulkan
      slower than CPU on weak integrated GPUs?
      Needs other hardware; no benchmark exists.

U-05  Does the Ollama backend work end to end without the bundled runtime?
      Untested; broken with it (TD-11).

U-06  Does the fine-tuning pipeline run end to end, and does the adapter help?
      No adapter or dataset is present, and it needs a CUDA GPU.

U-07  Does SMTP delivery work with current providers?
      The README says "Verified against Gmail"; there is no automated test.

U-08  How accurate is the second-stage classifier?
      There is no labelled set.

U-09  Does retrieval improve answers?
      Never measured; the corpus is a 6-chunk sample.

U-10  How does prompt caching interact across classifier and tutor calls on 4 slots?
      Internal to the engine; affects latency only.

U-11  Is the account of the Microsoft Defender false positive accurate?
      External; cannot be verified here.
