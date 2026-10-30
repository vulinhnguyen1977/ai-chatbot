CURRENT ARCHITECTURE

Status: descriptive, not aspirational. This documents the prototype as it is, so that
later changes can be judged against it. Problems found during the audit are listed
separately in TECHNICAL_DEBT.md and referenced here by ID (TD-nn).

Basis: full read of every file under src/, training/, ship/, res/, the build files and
the README, plus a small number of read-only checks (self-test run, read-only database
inspection, probes of a running model server). The tree is not under version control,
so there is no commit hash. The snapshot was taken on 2026-09-27 from a copy of
C:\ai chatbot v0.1.0-prototype, and the most recent source file is dated 2026-08-17.

File references are written as path:line, relative to the repository root.


1. WHAT THE APPLICATION IS

A single-process native Windows desktop application (mstutor.exe, C++17, Win32) that
tutors middle-school (ages 11-14) maths and physics. It:

- bundles the llama.cpp inference server (llama-server.exe) and GGUF weights (Qwen3 1.7B
  and 4B, nomic-embed-text), launches them as child processes, and talks to them over
  loopback HTTP;
- gates every question through a topic filter (lexical rules, then an optional
  model-based classifier), optionally retrieves textbook passages (hybrid embeddings
  plus SQLite FTS5), and streams a five-part worked answer;
- stores accounts, sessions, chats and messages in SQLite, with password login, emailed
  one-time codes ("2FA") and a DPAPI-encrypted "keep me signed in" token;
- renders model output in a RichEdit control after converting LaTeX to plain text.

The README's claim that Ollama is not used is accurate for the shipped product. Ollama
appears only in an alternative, effectively broken backend (TD-11) and in the offline
training scripts.

Key facts:

- Application code: about 8,600 non-blank lines of C++ in 44 files under src/, of which
  about 800 are the built-in self-test.
- Offline tooling: about 930 non-blank lines of Python in training/, and about 400 lines
  of PowerShell in ship/ and res/.
- Largest unit: src/gui.cpp, about 1,500 non-blank lines, one class (MainWindow).
- Platforms: Windows 10/11 x64 only (CMake refuses non-Windows).
- Binary: statically linked, GUI subsystem, about 6.4 MB.
- Runtime payload: runtime/bin/cpu and runtime/bin/vulkan (about 150 MB each, including
  the Vulkan DLL) and runtime/models (about 4.1 GB).
- Version control: none. No CI.
- Licence: none (there is no LICENSE file).


2. REPOSITORY LAYOUT

  mstutor.exe              built binary (.gitignore excludes all .exe files, but it is in the tree)
  CMakeLists.txt           primary build (MSVC or MinGW)
  Makefile                 secondary build (MinGW); duplicates the source list
  build.bat                tries CMake, then mingw32-make, then make; runs selftest
  start.bat                launches mstutor.exe
  config/config.ini        shipped configuration (MSTUTOR_ environment variables override it)
  src/                     all application code (see section 4)
  third_party/             vendored: SQLite 3.50.4 (amalgamation), cpp-httplib 0.52.0,
                           nlohmann/json 3.12.0
  res/                     icon, version resource (.rc), icon generator script
  runtime/bin/cpu          llama.cpp release b10331 binaries, CPU build (fetched, not
  runtime/bin/vulkan       source-controlled) and Vulkan build
  runtime/models/          qwen3-1.7b.gguf, qwen3-4b.gguf, nomic-embed-text.gguf
  runtime/ls.err           a leftover llama-server log from a manual run during development
  data/                    runtime state: mstutor.db (with -wal and -shm files), outbox/
                           (.eml files), remember.bin
  ship/                    packaging scripts and a 4 GB release zip
  training/                offline corpus and fine-tuning pipeline (Python), sample textbook
  .claude/settings.json    local assistant-tool permissions (machine-specific paths)

.gitignore already excludes data/, binaries, weights, zips and training artefacts, but
since there is no repository it has never been exercised.


3. LANGUAGES, FRAMEWORKS AND DEPENDENCIES

- C++17 (language): the application.
- Win32, ComCtl32 and RichEdit via Msftedit.dll (Windows API): the UI.
- Windows CNG, bcrypt (Windows API): PBKDF2-HMAC-SHA512, SHA-256, random numbers.
- Schannel / SSPI, secur32 and crypt32 (Windows API): TLS for SMTP.
- DPAPI, CryptProtectData (Windows API): the "keep me signed in" file.
- Job objects and CreateProcessW (Windows API): child-process supervision.
- SQLite 3.50.4 amalgamation with FTS5 on and extension loading off (vendored C,
  public domain): all persistence and lexical search.
- cpp-httplib 0.52.0 (vendored header, MIT): HTTP client to llama-server.
- nlohmann/json 3.12.0 (vendored header, MIT): JSON.
- llama.cpp llama-server, build b10331 (bundled binary, MIT): inference, both chat and
  embeddings.
- Qwen3 1.7B and 4B in GGUF format (bundled weights, Apache-2.0): the chat model, offered
  as "Fast" and "Smart".
- nomic-embed-text in GGUF format (bundled weights, Apache-2.0): 768-dimensional
  embeddings.
- Python 3 with pypdf 4.2 or later (offline tooling): corpus extraction and ingest.
- torch, transformers, peft and trl (offline tooling, see
  training/requirements-train.txt): optional LoRA fine-tune.
- Ollama (optional, external, MIT): alternative backend; used by training scripts.

There is no package manager, and the build needs no network access. Supported
toolchains: MSVC via CMake, or MinGW-w64 via CMake or the Makefile. No toolchain version
is pinned, and none is installed on the audit machine.


4. COMPONENT MAP

Inside mstutor.exe (one process):

- main.cpp (wWinMain and the command line) builds the services and hands them to the
  window in gui.cpp.
- gui.cpp (MainWindow, the UI plus orchestration) uses Auth, ChatService, LocalRuntime,
  llm::Client, prefs, the credential store (DPAPI) and the renderer (gui_richtext.cpp
  and markdown.cpp).
- ChatService (chat.cpp) uses DomainGuard (guard.cpp), Retriever (rag.cpp), llm::Client
  and SQLite.
- DomainGuard and Retriever both call llm::Client. Retriever and prefs also use SQLite.
- Auth (auth.cpp) uses Mailer (mailer.cpp and tls_socket.cpp) and SQLite
  (data/mstutor.db).

Outside the process:

- LocalRuntime starts two llama-server processes with CreateProcess inside a job object:
  the chat server on 127.0.0.1 ports 11500 to 11559, and the embeddings server on ports
  11600 to 11659.
- llm::Client sends POST /v1/chat/completions (streamed as server-sent events) to the
  chat server, and POST /v1/embeddings to the embeddings server.
- Mailer writes .eml files to data/outbox (file transport, the default) or talks SMTP
  with STARTTLS to an external mail server (optional).

Module responsibilities:

- Entry and CLI (main.cpp): DPI awareness, argument parsing, console attach for check,
  index and selftest, construction of the object graph, and a message box for any
  top-level exception.
- Configuration (config.h, config.cpp): INI parser, environment-variable override
  (MSTUTOR_ followed by the key), about 45 typed accessors with defaults.
- Utilities (util.h, util.cpp): time formatting, hex and base64, trimming, word
  matching, UTF-8 truncation, email and password validation.
- Storage (db.h, db.cpp): RAII wrapper over one SQLite connection, Stmt, Transaction,
  schema creation.
- Crypto (crypto.h, crypto.cpp): CNG-backed random numbers, SHA-256, PBKDF2
  encode/verify, constant-time comparison.
- Accounts (auth.h, auth.cpp): sign-up, login, emailed codes, sessions, lockout, account
  deletion, audit log.
- Remember-me (credential_store.h, credential_store.cpp): DPAPI encryption and
  decryption of the stored session token, with an atomic write.
- Email (mailer.h, mailer.cpp, tls_socket.h, tls_socket.cpp): MIME building, file
  transport, and a hand-written SMTP client over Winsock and Schannel.
- Inference client (llm.h, llm.cpp): two dialects (OpenAI-compatible llama-server, and
  Ollama), a streaming SSE parser, embeddings, JSON-schema constrained output.
- Runtime supervisor (runtime.h, runtime.cpp): discovers the engine and weights, builds
  presets, picks CPU or Vulkan, spawns llama-server in a kill-on-close job object, polls
  /health, swaps models.
- Topic gate (guard.h, guard.cpp, prompts.h): lexical allow and deny lists, then an
  optional classifier call.
- Retrieval (rag.h, rag.cpp): embeds corpus chunks, runs an in-memory cosine scan plus
  FTS5 BM25, fuses them with reciprocal rank fusion, and formats the result for the
  prompt.
- Chat pipeline (chat.h, chat.cpp): chat create/read/update/delete, orchestration of
  ask(), prompt assembly, persistence.
- Preferences (prefs.h, prefs.cpp): per-user key/value settings; app-wide values are
  stored in schema_meta.
- Rendering (markdown.h, markdown.cpp, gui_richtext.h, gui_richtext.cpp): LaTeX to
  plain-text normaliser, markdown block and inline parser, RichEdit formatting.
- UI (gui.h, gui.cpp, gui_edit.h, gui_edit.cpp): the window, both "pages" (sign-in and
  chat), threads, keyboard handling.
- Tests (selftest.h, selftest.cpp): 275 built-in checks, compiled into the product
  binary.
- Prompts (prompts.h): tutor system prompt, classifier prompt and JSON schema, canned
  refusal and greeting.


5. PROCESS AND THREAD MODEL

- UI thread. Created by wWinMain; lives as long as the process. Runs the message loop,
  all sign-in operations (PBKDF2, SMTP I/O), session restore, chat list and history
  queries, and rendering.
- worker_. Created by send_question (src/gui.cpp:1154), one per question. Runs
  ChatService::ask(): the classifier call, embeddings, streaming generation and
  database writes.
- runtime_thread_. Created by start_runtime (src/gui.cpp:1512) at startup. Runs
  LocalRuntime::start(), which spawns the servers and polls their health for up to
  startup_timeout_s, plus 120 seconds for embeddings.
- switch_thread_. Created by on_preset_changed (src/gui.cpp:1441), one per model swap.
  Runs LocalRuntime::switch_preset(), which kills and relaunches the chat server.
- llama-server (chat). Started by LocalRuntime; runs until the app exits or the model is
  swapped. Inference with 4 slots, each reporting n_ctx 8,192 over a unified (shared) KV
  cache. Verified via /props and /slots on a running instance.
- llama-server (embeddings). Started by LocalRuntime; runs until the app exits.
  Embeddings with --pooling mean and a 2,048-token context.

Worker threads report to the UI with PostMessageW (WM_APP_STREAM, WM_APP_DONE,
WM_APP_RUNTIME, WM_APP_READY, WM_APP_SWITCHED). Shared state is guarded by stream_mutex_,
runtime_mutex_ and the atomics generating_, cancel_ and switching_. LocalRuntime itself
has no internal synchronisation, and neither does the endpoint URL held by llm::Client
(TD-01, TD-02). See section 12.


6. STARTUP AND SHUTDOWN LIFECYCLE

6.1 Startup (src/main.cpp, src/gui.cpp)

1. SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2); parse CommandLineToArgvW.
2. If a sub-command was given, attach to (or allocate) a console and bind stdio to the
   inherited handles so that redirection works.
3. selftest runs here, before any real state is touched, and exits.
4. current_path(executable_dir()): all relative paths (config, data/, runtime/) resolve
   against the executable's folder.
5. Load config/config.ini; create data/; open SQLite (WAL, foreign_keys=ON,
   synchronous=NORMAL, 5-second busy timeout); run migrate().
6. Construct, in order: Mailer, Auth, llm::Client, LocalRuntime (scans runtime/models,
   builds presets, chooses the accelerator), DomainGuard, Retriever, ChatService.
7. The check and index sub-commands run here and exit. index starts the runtime itself.
8. auth.purge_expired() deletes expired, revoked and idle sessions, and codes older than
   a day. It runs only here, although auth.h says "startup and hourly".
9. gui::run() creates the window. WM_CREATE (src/gui.cpp:1536) then:
   a. builds the controls and the accelerator table;
   b. runs try_restore_session() synchronously on the UI thread. It decrypts
      data/remember.bin, then Auth::restore_session() rotates the token and the new one
      is encrypted and stored;
   c. calls show_chat() or show_auth(). show_chat() may already start a model swap
      (TD-01);
   d. calls start_runtime(), which spawns runtime_thread_. That thread launches the chat
      server, then the embeddings server, and posts WM_APP_READY. The UI then points
      llm::Client at the ports the runtime actually bound.
10. Message loop: TranslateAccelerator, then IsDialogMessage, then dispatch.

Cold start is dominated by reading the weights: 30 to 90 seconds by the README's
account. The UI is usable immediately. Questions sent before the runtime is ready are
refused unless llm.url already answers /health.

6.2 Shutdown

1. WM_CLOSE: if an answer is being generated, ask for confirmation, then set cancel_.
2. WM_DESTROY (src/gui.cpp:1714) sets cancel_ and joins worker_, runtime_thread_ and
   switch_thread_. Each join can block for a long time (TD-10), because cancellation is
   only observed when the next token arrives, and model loading cannot be cancelled.
3. GDI objects are freed; PostQuitMessage.
4. gui::run returns. main calls LocalRuntime::stop(), which closes the job handle, and
   so kills every child assigned to it, then terminates the tracked process handles.
5. Destructors close SQLite.

If the process crashes, the kill-on-close job object terminates the model servers,
provided they were assigned to the job. The job is created in start(), so a chat server
spawned by a swap that races ahead of start() is not in it (TD-01).


7. UI ARCHITECTURE

- One top-level window, two "pages". All sign-in and chat controls are created once in
  build_controls() and shown or hidden by show_auth() and show_chat(). There is no
  dialog resource. Modal prompts (rename, deletion code) use a hand-rolled nested
  message loop (prompt_text).
- MainWindow is the application controller. Besides the widgets it owns the sign-in
  state machine, remember-me persistence, the question worker thread, runtime start-up,
  model-preset switching and its persistence, and the hand-off of server URLs to
  llm::Client. It reaches every service through AppContext, a struct of eight
  references (src/gui.h).
- The transcript is a read-only RichEdit 4.1 control. Assistant text goes through
  md::parse() (LaTeX normaliser plus markdown parser) into formatted runs. User text is
  shown verbatim. While streaming, each WM_APP_STREAM truncates the transcript back to
  stream_anchor_ and re-renders the entire partial answer (TD-13).
- Keyboard: a real effort went into this. There is an accelerator table (Ctrl+N, F2,
  Ctrl+D, F6, Ctrl+Tab, Ctrl+1 and Ctrl+2, Ctrl+Enter, F1, Ctrl+W), IsDialogMessage tab
  order, DM_GETDEFID so Enter submits, and a subclass that fixes Ctrl+Backspace,
  Ctrl+Delete and Tab in multi-line edits (src/gui_edit.cpp).
- Theming: a light or dark palette chosen from the AppsUseLightTheme registry value at
  startup. Per-monitor-v2 DPI scaling via scaled().
- The account screen is a MessageBoxW where Yes signs out (all sessions), No deletes the
  account, and Cancel closes it.


8. MODEL RUNTIME AND INFERENCE INTEGRATION

8.1 Bundled llama.cpp (the default, llm.backend = llamacpp)

LocalRuntime (src/runtime.cpp):

- Engine selection: runtime.accelerator is auto, vulkan or cpu. auto prefers
  runtime/bin/vulkan if that folder exists, and falls back to CPU only if the Vulkan
  process fails to launch or exits during start-up.
- Presets: presets.fast and presets.smart name files in runtime/models. Only files that
  exist are offered. If neither exists, the largest .gguf file whose name does not
  contain "embed" becomes a single "Default" preset. runtime.chat_model pins one file
  and hides the dropdown.
- Launch command (src/runtime.cpp:103-120):
    llama-server -m MODEL.gguf --host 127.0.0.1 --port PORT -c CONTEXT -ngl GPU_LAYERS
  with -t THREADS when set, where CONTEXT is llm.num_ctx for chat and 2048 for
  embeddings. Chat adds --reasoning-format deepseek --jinja; embeddings add --embedding
  --pooling mean; both always add --log-disable. There is no --api-key, and --parallel
  is left at the engine default (4 slots observed).
- Ports: the first free port from 11500 (chat) or 11600 (embeddings), found by a
  bind-and-close probe.
- Readiness: poll GET /health every 500 ms until runtime.startup_timeout_s (default
  300 seconds). Progress messages go to the UI status bar.
- Swap (switch_preset): terminate the chat server, relaunch it with the other weights,
  and restore the previous model on failure. The embeddings server is untouched.

8.2 Client (llm::Client, src/llm.cpp)

- A new httplib::Client per call, with no keep-alive reuse. Timeouts: connect 5
  seconds, read llm.timeout_s per read (not per request), write 30 seconds.
- chat() is non-streaming and used by the classifier. chat_stream() parses server-sent
  events (lines of the form "data: {...}"), sending reasoning_content to on_thinking
  (used only as a liveness indicator) and content to the answer. A callback that returns
  false aborts the HTTP stream, and the partial text is returned with ok = true.
- Constrained output: the format option becomes an OpenAI response_format with a
  json_schema for llama-server, or a format field for Ollama.
- Reasoning control: the think option becomes chat_template_kwargs.enable_thinking for
  llama-server, or think for Ollama. The tutor leaves it at the model default (on); the
  classifier forces it off.
- Embeddings: POST /v1/embeddings with the whole batch; results are placed by their
  index field.

8.3 Ollama (llm.backend = ollama)

The client speaks /api/chat and /api/embed and sets keep_alive to 30m. However, nothing
stops LocalRuntime from launching the bundled servers, and on WM_APP_READY the GUI
overwrites the client URL with llama-server's. With the bundled runtime present (always
the case in the shipped zip), this backend therefore sends Ollama-dialect requests to
llama-server (TD-11). Ollama's other role is as a source of weights:
ship/extract_models.ps1 copies GGUF blobs out of a local Ollama store.


9. QUESTION PIPELINE

What happens when a question is sent:

1. On the UI thread, send_question() starts worker_, which calls
   ChatService::ask(user, active_chat_, question).
2. ask() creates the chat if it is new, or checks that the user owns it.
3. DomainGuard::classify() runs the lexical stage (no model call). If the result is
   still ambiguous and guard.use_model is on, it calls /v1/chat/completions with a JSON
   schema, thinking off and a 60-second timeout.
4. Off-topic or small talk: the user turn and a canned reply are stored, and the
   pipeline ends here.
5. In scope: unless the message is a follow-up or rag.enabled is off, Retriever embeds
   the question using the embeddings server, and fuses a cosine search over cached
   vectors with an FTS5 bm25 search.
6. The last 2 x chat.history_turns messages are read, and the user turn is stored.
7. /v1/chat/completions is called with stream=true. For every server-sent event chunk
   (a reasoning_content or content delta), the worker posts WM_APP_STREAM, and the UI
   thread truncates and re-renders the whole answer.
8. The assistant turn is stored together with its sources JSON.
9. The worker posts WM_APP_DONE.

9.1 Topic gate (src/guard.cpp)

Evaluation order in lexical_stage():

1. An empty message is small talk.
2. Injection phrases (substring match, for example "you are now" or "system prompt")
   make the message off-topic.
3. Off-topic vocabulary (about 100 whole-word terms, for example athlete, recipe, cell,
   translate, stock, "population of", tournament) makes the message off-topic. This wins
   over subject vocabulary (TD-06).
4. Maths terms only: Math. Physics terms only: Physics. Both: Physics.
5. An exact greeting or thanks phrase is small talk.
6. In a chat with history, any message of 24 characters or fewer, or a known follow-up
   phrase, is a FollowUp.
7. Digits plus an operator or "=" make it Math.
8. Anything else is ambiguous and goes to the classifier call, which is
   schema-constrained to one of math, physics, followup, smalltalk or other. If the
   classifier is unreachable, the gate fails open to Math or FollowUp and relies on the
   tutor prompt's own refusal policy. With guard.use_model = false it fails closed.

Off-topic and small-talk outcomes store the user turn and a canned reply with no model
call. Both are later replayed as ordinary history (TD-09).

9.2 Retrieval (src/rag.cpp)

- Corpus: doc_chunks rows inserted by training/3_ingest_corpus.py (overlapping windows
  of about 220 words, with page locators). The shipped database holds the 6-chunk
  sample.
- Dense search: the question is embedded, and a linear dot-product scan runs over all
  vectors cached in memory (L2-normalised at index time). Results below rag.min_score
  (0.35) are dropped.
- Lexical search: every alphanumeric token of 3 or more characters is quoted and joined
  with OR into an FTS5 MATCH, ranked by bm25. There is no stop-word list and no score
  threshold.
- Fusion: reciprocal rank fusion (k = 60) over max(3 x top_k, 10) hits from each search,
  keeping rag.top_k (4).
- Injection into the prompt: passages (each truncated to 1,500 bytes) are formatted as a
  second system message placed after the history and before the question.
- Follow-ups skip retrieval.

9.3 Prompt assembly (ChatService::build_prompt)

The request consists of, in order: a system message with the tutor prompt
(kTutorSystem); the last 2 x history_turns stored messages; a system message with the
retrieved passages, if any; and the user message with the question.

There is no token counting or truncation beyond the message count and the per-passage
byte cap (TD-05). Generation uses llm.temperature (0.2) with no maximum token limit.

9.4 Persistence of the answer

The user turn is written before generation, so a crash leaves a coherent transcript.
The assistant turn is written only on success, including after a user-requested stop,
and nothing marks the text as partial (TD-08). The raw model text is stored, and LaTeX
and markdown clean-up happens at render time, so renderer fixes apply retroactively to
old chats. This is a good property worth keeping.

9.5 Rendering (src/markdown.cpp)

normalise_math() strips the maths delimiters $$...$$, \[...\], \(...\) and $...$ (the
last with a heuristic that leaves currency amounts alone) and removes \begin and \end.
It rewrites commands: \frac{a}{b} becomes a/b, with parentheses around compound parts;
\sqrt{x} becomes sqrt(x); \times and \cdot both become " x "; Greek letters become their
names; \Delta becomes "change in ". It drops the braces of superscripts and subscripts,
so ^{...} becomes ^... (TD-07). It then marks markdown hard line breaks and collapses
spaces.

parse_normalised() produces blocks (paragraph, heading, bullet, numbered item, code,
quote, rule, table row) whose text runs carry bold, italic and code flags. A line that
is entirely bold (for example the word Working wrapped in double asterisks) is promoted
to a heading.


10. AUTHENTICATION, ACCOUNTS AND "2FA"

- Password storage: the format pbkdf2_sha512$210000$SALT$HASH (16-byte salt, 32-byte
  hash) computed with CNG. The iteration count is read from the stored row and capped at
  5 million, so a tampered row cannot demand an absurd cost.
- Password policy: 10 to 200 characters, with at least one letter and one digit.
- Sign-up: creates an unverified user and emails a 6-digit code (purpose
  signup_verify). Signing up again with an unverified address overwrites that account's
  name and password.
- Login: the same message ("Incorrect email or password.") for an unknown address and a
  wrong password, with a dummy PBKDF2 computation for unknown addresses. Lockout after
  max_failed_logins (8) for lockout_minutes (15). An unverified account is sent a new
  sign-up code instead.
- One-time codes: 6 uniformly random digits (rejection sampling), stored as PBKDF2 with
  50,000 iterations, expiring after code_ttl_minutes (10), limited to
  max_code_attempts (5). A newer code supersedes older ones, and each is bound to a
  random 128-bit challenge_id.
- "2FA": the emailed code is required at sign-up, at every login (auth.require_2fa) and
  for account deletion. With the default mail.transport = file, the code is written in
  plain text to an .eml file in data/outbox, beside the database (TD-18).
- Sessions: a 256-bit random token, of which only the SHA-256 is stored. Absolute expiry
  after session_days (180). Idle timeout after remember_days (30), which authenticate()
  applies to all sessions, not only remembered ones.
- Remember-me: a JSON object holding a version, the token and the email address,
  DPAPI-encrypted with fixed application entropy to data/remember.bin (written to a
  temporary file and renamed). Restoring rotates the token, and the new token inherits
  the original absolute deadline.
- After sign-in: the UI keeps only the User struct. The session token is not held, and
  its validity is never re-checked while the app runs. "Sign out" revokes all sessions
  of the user.
- Deletion: confirmed by an emailed code. DELETE FROM users cascades to sessions, codes,
  preferences, chats and messages. Audit rows are detached from the user and their
  detail replaced with "[erased]".
- Audit: audit_log events (signup, session_started, login_failed, code_incorrect,
  email_failed and others), written on a best-effort basis.

Email: Mailer builds RFC 5322 / MIME messages (RFC 2047 encoded-word headers, CRLF line
endings, dot-stuffing, stripping of characters that could inject headers) and either
writes an .eml file or speaks SMTP itself (EHLO, STARTTLS or implicit TLS, and AUTH
LOGIN only over TLS). The TLS layer is a hand-written Schannel client that uses the
operating system's protocol policy and SCH_USE_STRONG_CRYPTO, validates the certificate
chain and host name automatically, and handles the anonymous client-certificate retry.

About 1,760 non-blank lines, roughly 20% of the C++, exist to support accounts, email
and TLS.


11. DATA PERSISTENCE

Engine: one SQLite file, data/mstutor.db (setting db.path), in WAL mode. The schema is
created by Database::migrate() (src/db.cpp:44) with CREATE ... IF NOT EXISTS
statements. The schema version is hard-coded to 1, and there is no upgrade path (TD-16).

Tables:

- schema_meta: key/value pairs. Holds the schema version and also the app-wide
  last_model_preset.
- users: accounts. email_lower is unique; the table holds the lockout counters.
- sessions: bearer sessions. token_hash is unique; columns expires_at, last_seen_at and
  revoked_at.
- email_codes: one-time codes, with purpose, code_hash, challenge_id, attempts and
  consumed_at.
- user_prefs: per-user settings. Currently only model_preset.
- chats: conversations. A deleted_at column exists but is never set; deletes are hard
  deletes.
- messages: conversation turns, with role, raw content, the off_topic flag and a sources
  JSON value.
- doc_chunks: the retrieval corpus, written by Python. token_est is never read.
- doc_vectors: embeddings, stored as raw little-endian float32 blobs with a model label
  and a dimension.
- doc_fts: an FTS5 external-content index, kept in sync by three triggers.
- audit_log: the security trail. user_id becomes null after erasure.

Other state on disk: the .eml files in data/outbox (never purged), data/remember.bin
(DPAPI), config/config.ini, the .gguf files in runtime/models, and training/data
(offline).

Snapshot contents (read-only inspection): 1 user, 1 session, 1 chat, 4 messages, 6
corpus chunks with 6 vectors of 768 dimensions labelled nomic-embed-text, and
last_model_preset = smart. The data/ folder therefore contains a real development
account and a verification email, and must not be published.


12. CONCURRENCY AND LOCKING

- One SQLite connection is opened with SQLITE_OPEN_FULLMUTEX and shared by all threads.
  SQLite serialises individual API calls. Consistency across multiple statements relies
  on an application std::mutex (Database::mutex()) that each call site locks by
  convention:
  - Auth::audit() and the self-tests run statements without that mutex.
  - Transactions (BEGIN IMMEDIATE) belong to the connection, so an unlocked statement
    from another thread would join an open transaction.
  - last_insert_rowid() and changes() are global to the connection.
  - PBKDF2 (hundreds of milliseconds) runs while the mutex is held, in sign_up and in
    the unknown-address path of log_in.
- The lock order, Retriever::cache_mutex_ before the database mutex, is consistent. No
  inversion was found.
- Between the GUI and the workers: stream text and runtime messages are protected by
  mutexes, and the flags are atomics. However, the worker reads user_, active_chat_ and
  pending_question_ from MainWindow without synchronisation. LocalRuntime's status_,
  active_preset_ and impl_ are changed on background threads and read on the UI thread
  without synchronisation. llm::Client's chat_url_ is reassigned on the UI thread while a
  worker may be reading it (TD-01 to TD-03).


13. ERROR HANDLING AND DIAGNOSTICS

- Three styles coexist: exceptions (db::DbError, and std::runtime_error from crypto);
  functions returning bool with a std::string pointer out-parameter for the error (llm,
  runtime, mailer, TLS, credentials); and result structs with status enums (AuthResult,
  ChatResult, AskResult, guard::Decision).
- Catching every exception and returning an empty result is pervasive: list_chats,
  get_messages, the prefs functions, the Retriever methods, purge_expired and log_out
  all swallow exceptions and return empty or default values without recording anything.
- There is no logging. In GUI mode stdout and stderr are not attached, so the [warn] and
  [mail] messages printed with fprintf go nowhere. llama-server runs with
  --log-disable. The only persistent diagnostic trail is audit_log.
- Messages shown to the student sometimes include internals, for example "Details: chat:
  cannot reach the model server (...)" or "Internal error: step failed: FOREIGN KEY
  constraint failed".
- Field diagnostics: "mstutor.exe check" prints paths, engine, weights and presets,
  corpus and embedding counts, the mail transport and the session settings.
  "mstutor.exe selftest" runs the internal checks.
- Top level: any exception during start-up becomes a message box ("The application
  could not start").


14. BUILD, TEST AND PACKAGING

- Build systems: CMakeLists.txt (MSVC with /W4; MinGW with -Wall -Wextra and a static
  C runtime), a Makefile (MinGW, with header dependency tracking), and build.bat, which
  tries both. The source list is duplicated between CMake and the Makefile.
- Output: mstutor.exe is written to the tree root, so relative paths match the shipped
  layout.
- Resources: res/mstutor.rc (icon, and version information hard-coded to 1.0.0.0).
- Tests: "mstutor.exe selftest", with 12 sections and 275 checks, all passing (verified;
  13 seconds). ctest wraps the same command. There are no other tests and no CI.
- Runtime fetch: ship/fetch_runtime.ps1 downloads the llama.cpp b10331 CPU and Vulkan
  zips from GitHub without checksum verification, and removes unused tools.
- Weights: ship/extract_models.ps1 copies GGUF blobs from a local Ollama store (it checks
  the GGUF magic bytes).
- Packaging: ship/package.ps1 runs the self-test on whatever executable exists; stages
  the executable, docs, config and training folder, the sources (unless -NoSource), the
  runtime, and the weights (unless -NoModels); creates an empty data/ folder; and writes
  a zip whose entry names use forward slashes. It lists a LICENSE file, which does not
  exist.
- Signing: none. The README documents Microsoft Defender's machine-learning false
  positive and recommends OV or EV code signing.


15. OFFLINE TRAINING AND CORPUS PIPELINE (training/)

- Step 1, 1_extract_pdfs.py: pypdf extracts one cleaned text file per page. Connected to
  the shipped app: yes (it feeds step 2).
- Step 2, 2_build_corpus.py: overlapping chunks of about 220 words, a boilerplate and
  relevance filter, and a subject guess, written to corpus.jsonl. Connected: yes.
- Step 3, 3_ingest_corpus.py: inserts the chunks into data/mstutor.db (duplicating the
  corpus table definitions) and de-duplicates by hash. Connected: yes; afterwards
  "mstutor.exe index" builds the embeddings.
- Step 4, 4_build_dataset.py: hand-written seeds (23 rows, each repeated 8 times) plus
  optional self-instruct pairs generated with Ollama. Connected: no.
- Step 5, 5_finetune_lora.py: LoRA or QLoRA fine-tuning of Qwen3-4B (needs CUDA).
  Connected: no.
- Step 6, 6_export_gguf.py: converts the adapter to GGUF using a llama.cpp checkout.
  Connected: no.
- Step 7, 7_build_ollama_model.py: writes training/Modelfile and runs "ollama create
  mstutor". Connected: no. The bundled llama-server never loads an adapter or this
  Modelfile.

common.read_tutor_system_prompt() extracts the prompt from src/prompts.h with a regular
expression, so that training and serving share one prompt. The committed
training/Modelfile already differs from prompts.h: it has the older, shorter rule about
LaTeX (TD-41).


16. CONFIGURATION REFERENCE (EFFECTIVE DEFAULTS)

Precedence: the environment variable (MSTUTOR_ followed by SECTION_KEY in upper case),
then config/config.ini, then the default in the code. An empty value in the file falls
through to the default. Inline comments are not supported (TD-17).

Unless noted, the shipped config.ini uses the same value as the code default.

- db.path: data/mstutor.db.
- runtime.chat_model: unset. Pins a model and hides the dropdown.
- runtime.embed_model: nomic-embed-text.gguf. This is the file that is loaded; it is not
  the label stored with the vectors.
- runtime.accelerator: auto. Any unrecognised value behaves like auto.
- runtime.gpu_layers, runtime.threads, runtime.startup_timeout_s: 99, 0 and 300.
- presets.fast, presets.smart, presets.default: qwen3-1.7b.gguf, qwen3-4b.gguf and smart.
- llm.backend: llamacpp. The value ollama is broken while the bundled runtime is present
  (TD-11).
- llm.url: http://127.0.0.1:11500 (or port 11434 for Ollama); commented out in the ini.
  Overwritten once the runtime reports its port.
- llm.embed_url: unset.
- llm.model, llm.embed_model, llm.guard_model: mstutor, nomic-embed-text, and the same
  as llm.model. These are Ollama model names. llm.embed_model is also the label stored
  with the vectors.
- llm.temperature, llm.num_ctx, llm.think: 0.2, 8192, and the model's default.
  llm.num_ctx also sets llama-server's -c option.
- llm.timeout_s: 300 in the code, but 600 in the shipped ini. It is a per-read timeout,
  not a per-request one.
- auth.session_days, auth.remember_days: 180 and 30. remember_days is the idle limit for
  every session.
- auth.require_2fa, auth.remember_enabled: true and true.
- auth.code_ttl_minutes, auth.max_code_attempts, auth.max_failed_logins,
  auth.lockout_minutes: 10, 5, 8 and 15.
- mail.transport, mail.outbox_dir: file and data/outbox.
- mail.host, mail.port, mail.security: smtp.gmail.com, 587 and starttls.
- mail.username, mail.password: unset.
- mail.from: defaults to mail.username. Empty in file mode, which produces a From header
  with an empty address.
- mail.from_name: Middle School Tutor.
- rag.enabled, rag.top_k, rag.min_score: true, 4 and 0.35. min_score applies to the
  dense search only.
- guard.use_model: true.
- chat.history_turns: 8 (question-and-answer pairs replayed).


17. STRENGTHS WORTH PRESERVING

The audit is critical by request, but these parts are sound and should survive any
restructuring:

- Zero-install distribution with a supervised, loopback-only inference engine in a
  kill-on-close job object.
- Raw model output stored and cleaned at render time, so renderer fixes are
  retroactive.
- Parameterised SQL throughout (no string-built queries were found), quoting of FTS5
  input, and a size-bounded, atomically written credential file.
- Sensible cryptographic primitives: CNG PBKDF2 with the cost stored alongside the hash,
  hashed session tokens, constant-time comparison, unbiased code generation, and DPAPI
  with application entropy.
- Deliberate keyboard accessibility and DPI handling.
- A pipeline that already uses schema-constrained JSON output (the classifier). This is
  the mechanism the verification vision builds on (see PROJECT_VISION.md).
