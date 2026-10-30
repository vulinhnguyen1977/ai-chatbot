# Middle School Maths & Physics Tutor

A private desktop tutor for middle school mathematics and physics (ages 11–14).

**Unzip it and double-click `mstutor.exe`.** There is nothing to install — no
Ollama, no Python, no runtime, no browser. The inference engine and the model
weights are inside the folder, and nothing a student types ever leaves the
machine.

It answers maths and physics at school level in a fixed, teachable structure —
and it declines everything else, including chemistry, biology, coding and
homework in other subjects.

---

## Getting started

1. Download **`mstutor-v0.1.0-windows-x64.zip`** from the
   [latest release](https://github.com/vulinhnguyen1977/ai-chatbot/releases/latest).
2. Unzip anywhere you like (Desktop, Documents, a USB stick).
3. Double-click **`mstutor.exe`**.
4. Create an account. You will be asked for a 6-digit code.

The zip includes the **Fast** model. For the slower but more reliable **Smart**
model, double-click **`get-smart-model.bat`** in the same folder (a 2.5 GB
download, which GitHub's 2 GB file limit keeps out of the zip) and restart the
app. See [Answer style](#answer-style-fast-or-smart).

**About that code:** by default the app does not send real email. It writes the
message to `data\outbox\` instead, so the whole flow works before any mailbox is
configured. Open the newest `.eml` file there to read the code. To send real
mail, see [Email and 2FA](#email-and-2fa).

The first launch takes 30–90 seconds while the model is read into memory. The
status line at the bottom of the window tells you what it is doing.

---

## What it does

**Answers in a fixed structure.** Every problem is worked as *What we know →
Idea → Working → Answer → Check*, then one line naming a common mistake or
offering a practice question. The check step is not decoration: the model is
told to substitute back or estimate, and to correct itself when the check fails.

**Stays in its lane.** Three layers, cheapest first:

1. A lexical gate settles clear cases with **no model call at all** — subject
   vocabulary, off-domain vocabulary, greetings, and attempts to talk it out of
   its role. Refusals here are instant.
2. Anything still ambiguous goes to a constrained-JSON classifier call.
3. The tutor's own system prompt refuses off-topic questions independently, so a
   classifier mistake still does not produce an off-topic answer.

**Renders maths readably.** Models trained on maths corpora emit LaTeX no matter
how firmly the prompt forbids it. Everything the model writes passes through a
normaliser before it reaches the screen, so `$$\frac{3}{4} \times 7$$` is shown
as `3/4 x 7` and `**Working**` becomes a bold heading rather than four
asterisks. The raw reply is kept in the database; the cleanup happens at render
time, so improving it fixes old chats too.

**Grounds answers in your textbooks.** Ingest PDFs and the tutor retrieves
relevant passages at question time and cites the page. Retrieval is hybrid:
embeddings catch paraphrases, SQLite FTS5/BM25 catches exact terms, and the two
are fused by reciprocal rank fusion.

**Remembers.** Accounts, sessions, chats and messages live in SQLite. Chats can
be renamed and deleted; accounts can be deleted outright.

---

## Requirements

| | |
|---|---|
| OS | Windows 10 or 11 (x64) |
| RAM | 8 GB for the 1.7B model, 16 GB comfortable for 4B |
| Disk | ~4 GB |
| Everything else | **nothing** — engine and weights are bundled |

### Answer style: Fast or Smart

The sidebar has an **Answer style** dropdown. Both bundled models reason before
they answer; the difference is size, not technique.

| | Model | Typical answer | Trade-off |
|---|---|---|---|
| **Fast** | `qwen3-1.7b.gguf` | ~1 minute | Noticeably weaker on multi-step arithmetic and more likely to drift from the answer format |
| **Smart** | `qwen3-4b.gguf` | 3–5 minutes | More reliable working and arithmetic |

Times are from the development machine — an i5-10310U laptop, **CPU only, no
usable GPU**. A machine with a GPU is far quicker.

Switching restarts the model server, which took about **10 seconds** in testing.
The choice is remembered per account, and the app starts up on the last one used
so it does not load one model and immediately swap it.

Send is disabled while a swap is in progress, and the dropdown is disabled while
an answer is being written — the model cannot be pulled out from under a running
generation.

Which models the two entries map to is configurable:

```ini
[presets]
fast    = qwen3-1.7b.gguf
smart   = qwen3-4b.gguf
default = smart
```

Setting `runtime.chat_model` pins one model and hides the dropdown entirely.

The model reasons before it writes, so nothing appears for a while. The window
shows a live *"Working it out…"* indicator from about two seconds in rather than
sitting blank.

**A GPU is by far the largest factor** — often 10–30×. The package includes a
Vulkan build that works on Intel, AMD and NVIDIA; it is selected automatically
and falls back to CPU if there is no usable driver. Force it either way with:

```ini
[runtime]
accelerator = cpu      ; or vulkan, or auto (default)
```

---

## Configuration

`config\config.ini`. Environment variables win over the file: any key maps to
`MSTUTOR_` + the key upper-cased with dots as underscores, so `mail.password` →
`MSTUTOR_MAIL_PASSWORD`. **Keep secrets in the environment** — the file ships
inside the zip.

| Key | Default | Meaning |
|---|---|---|
| `presets.fast` / `presets.smart` | the two bundled models | What the Answer style dropdown offers |
| `presets.default` | `smart` | Style a new account starts on |
| `runtime.chat_model` | *(unset)* | Pins one model and hides the dropdown |
| `runtime.accelerator` | `auto` | `auto` / `vulkan` / `cpu` |
| `runtime.gpu_layers` | `99` | Layers offloaded to the GPU |
| `auth.session_days` | `180` | Absolute session lifetime |
| `auth.require_2fa` | `true` | Email code on every sign-in |
| `auth.remember_enabled` | `true` | Offer "keep me signed in" |
| `auth.remember_days` | `30` | Idle timeout for a stored sign-in |
| `llm.num_ctx` | `8192` | Context window |
| `llm.think` | *(model default)* | `low` / `medium` / `high` / `false` |
| `rag.top_k` | `4` | Passages injected per question |

Do **not** set `llm.think = false` on a Qwen3 model: it then writes its
reasoning into the answer instead of keeping it separate, which is worse in both
speed and output. Use a smaller model if you want speed.

---

## Keyboard

The whole application can be operated without a mouse, from signing in to
asking a question. **Press F1 for the shortcut list inside the app.**

Combinations follow Microsoft's published table of common Windows accelerators,
so they match what other applications already do.

### Moving around

| Key | Action |
|---|---|
| `Tab` / `Shift+Tab` | Next / previous control |
| `F6` / `Shift+F6` | Next / previous pane (chat list, message box, answers) |
| `Ctrl+L` | Jump to the message box |
| `Alt` + underlined letter | Any button or field, e.g. `Alt+E` for Email |

### Chats

| Key | Action |
|---|---|
| `Ctrl+N` | New chat |
| `F2` | Rename the selected chat |
| `Ctrl+D` | Delete the selected chat |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next / previous chat |
| `Up` / `Down` | Move through the chat list |
| `Enter` | Open the highlighted chat |
| `Delete` | Delete the highlighted chat (only when the list has focus) |

### Asking a question

| Key | Action |
|---|---|
| `Enter` | Send |
| `Shift+Enter` | New line |
| `Ctrl+Enter` | Send from anywhere in the window |
| `Esc` | Stop generating |

### Editing text

| Key | Action |
|---|---|
| `Ctrl+Backspace` | Delete the word to the left |
| `Ctrl+Delete` | Delete the word to the right |
| `Ctrl+A` `Ctrl+C` `Ctrl+V` `Ctrl+X` `Ctrl+Z` | Select all, copy, paste, cut, undo |

### Other

| Key | Action |
|---|---|
| `Ctrl+1` / `Ctrl+2` | Answer style: Fast / Smart |
| `F1` | Shortcut list |
| `Ctrl+W` | Close the window |

`Delete` removes a chat only when the chat list has focus, so it still deletes
characters while you are typing.

---

## Staying signed in

Tick **Keep me signed in on this device** and the app opens straight into your
chats next time instead of asking for a password and an emailed code.

| | |
|---|---|
| Idle timeout | **30 days.** Not opened for 30 days → sign in again. |
| Absolute cap | **180 days**, unchanged. Reached regardless of use. |
| Where it is stored | `data\remember.bin` |
| How it is protected | Encrypted with the Windows Data Protection API |

The stored value is a live session token, so it is encrypted rather than written
in the clear. The key is derived from the signed-in Windows account, which means
another user on the same PC cannot read it, and copying the application folder
to a different machine or account leaves the file undecryptable — a remembered
sign-in cannot travel on a USB copy of the app.

The token is **rotated on every launch**: restoring a sign-in issues a fresh
token and revokes the old one, so a copy of the file taken earlier stops working
the moment the real user opens the app. The replacement inherits the original
180-day deadline, so reopening the app daily cannot roll that cap forward.

It is cleared by signing out, by "sign out everywhere", and by deleting the
account. Turn the feature off entirely with:

```ini
[auth]
remember_enabled = false
remember_days    = 30
```

**Why 30 days and not 30 minutes.** OWASP's familiar 15–30 minute idle window is
aimed at network-exposed web applications, and the same guidance says to set the
value according to the nature of the application. This one is a local desktop
app for schoolwork: nothing listens on the network, the data is maths homework,
the stored credential is already encrypted to the Windows account, and the
Windows sign-in is the real access control. 30 days is the usual ceiling for a
"remember me" token and is the right balance here. Shorten it if the machine is
shared.

---

## Antivirus warnings

**Microsoft Defender may quarantine `mstutor.exe` as
`Trojan:Win32/Bearfoos.A!ml`. This is a false positive.** It happened during
development of this project and it will likely happen to your client.

The `!ml` suffix means the verdict came from a machine-learning heuristic rather
than a signature match. The heuristic fires on the general profile of "freshly
compiled, unsigned, native executable" and is a widely reported nuisance — it
has hit Rust projects, OpenAI's Codex CLI, and Microsoft's own tooling. Nothing
in this application is doing anything the heuristic claims.

The build already does what it can without money changing hands: the executable
carries an icon, a version resource, a product name and a company name, because
"no icon, no metadata, no signature" is precisely the profile that gets flagged.

**The actual fix is a code signing certificate.** If this is being sold or
deployed to schools, budget for one:

- An **OV** certificate signs the binary and builds reputation over time.
- An **EV** certificate carries SmartScreen reputation immediately, which
  matters if end users will download it themselves.

Sign with `signtool sign /fd SHA256 /tr <timestamp-url> /td SHA256 mstutor.exe`
as the last step before packaging.

Until then:

1. Report the binary at
   <https://www.microsoft.com/en-us/wdsi/filesubmission> as a false positive.
   This is free and usually resolved within a few days.
2. On an affected machine, add an exclusion for the install folder
   (Windows Security → Virus & threat protection → Manage settings →
   Exclusions). This needs administrator rights.

---

## Email and 2FA

Two-factor verification is required at sign-up, at every sign-in, and to delete
an account.

**Default (`mail.transport = file`):** messages are written to `data\outbox\`.
The whole flow is testable with no mailbox configured.

**Real delivery (`mail.transport = smtp`):** SMTP over TLS, implemented directly
on Windows Schannel so no OpenSSL binary has to ship. Verified against Gmail's
submission service.

For Gmail use an **App Password** (Google Account → Security → 2-Step
Verification → App passwords), not the account password:

```ini
[mail]
transport = smtp
host      = smtp.gmail.com
port      = 587
security  = starttls
username  = you@gmail.com
from      = you@gmail.com
```

```
set MSTUTOR_MAIL_PASSWORD=your-app-password
```

The client refuses to send credentials over an unencrypted connection.

---

## Using your own textbooks

This is the biggest quality win for a curriculum-specific tutor, and it needs no
GPU. It does need Python.

```
pip install -r training\requirements.txt
REM put PDFs in training\data\textbooks\
python training\1_extract_pdfs.py
python training\2_build_corpus.py
python training\3_ingest_corpus.py
mstutor.exe index
```

Answers then cite the source and page. Full detail, including where to obtain
openly licensed textbooks and how to fine-tune the model, is in
[training/README.md](training/README.md).

---

## Diagnostics

Run from a terminal (the app is a window, but these print to the console):

```
mstutor.exe check      REM engine, weights, corpus, email, security settings
mstutor.exe selftest   REM 275 internal consistency checks
```

`selftest` is the one to run after any change. It covers the crypto against
published test vectors, account and session behaviour, the topic gate, and the
maths/markdown rendering.

---

## Shipping to a client

```
powershell -ExecutionPolicy Bypass -File ship\package.ps1
```

Produces `ship\mstutor-<date>.zip`, self-contained. Useful switches:

| Switch | Effect |
|---|---|
| `-NoModels` | Leave out the weights (small update package) |
| `-SkipModels <file>` | Leave out named weights, e.g. `qwen3-4b.gguf` for the GitHub release |
| `-CpuOnly` | Leave out the Vulkan engine (~90 MB smaller) |
| `-NoSource` | Binary-only delivery |

The script refuses to package a build that fails its self-tests, and strips
`data\` — that holds real password hashes, chat history and live verification
codes.

**Bundling weights means redistributing someone else's model.** Qwen3 and
nomic-embed-text are Apache-2.0 and may be redistributed with attribution. If
you swap in a Llama or Gemma model, read its licence first. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

To rebuild the runtime from scratch (new llama.cpp release, different weights):

```
powershell -ExecutionPolicy Bypass -File ship\fetch_runtime.ps1
powershell -ExecutionPolicy Bypass -File ship\extract_models.ps1
```

---

## Security

- Passwords: **PBKDF2-HMAC-SHA512, 210,000 iterations** (OWASP guidance), random
  per-password salt, via Windows CNG. The cost is stored with the hash so it can
  be raised later without a migration.
- Sessions: a 256-bit random token; **only its SHA-256 is stored**, so a stolen
  database file does not yield usable sessions. Absolute 180-day expiry.
- 2FA codes are hashed the same way, expire in 10 minutes, and are
  attempt-limited.
- Login responses are uniform: an unknown address and a wrong password give the
  same message and take comparable time, so the app cannot be used to discover
  which addresses have accounts.
- Repeated failures lock an account temporarily.
- Deleting an account cascades to its sessions, chats and messages, and
  anonymises its audit rows.
- The student's own text is never re-interpreted as markup, so a stray asterisk
  cannot silently change what they typed.

**Nothing listens on the network.** The only socket is the bundled model server,
which binds `127.0.0.1` on a port chosen at startup and is terminated when the
window closes. There is no HTTP API and no browser involved.

The model server is a child process inside a Windows job object configured to
kill on close, so it cannot outlive the app even if the app crashes.

---

## Building from source

```
build.bat
```

Or directly:

```
mingw32-make
cmake -S . -B build-cmake && cmake --build build-cmake --config Release
```

Dependencies are vendored in `third_party\` — SQLite, cpp-httplib and
nlohmann/json — so the build needs no package manager and no network. TLS,
password hashing and the UI use Windows APIs (Schannel, CNG, Win32/RichEdit)
rather than third-party libraries, which is why the tree is Windows-only.

Always run `mstutor.exe selftest` after building.

---

## Layout

```
mstutor.exe      the application
runtime/
  bin/cpu/         llama.cpp engine, works anywhere
  bin/vulkan/      GPU-accelerated engine
  models/          .gguf weights
src/
  gui.cpp          the desktop window
  gui_richtext.cpp markdown -> formatted text
  markdown.cpp     LaTeX normaliser and markdown parser
  prompts.h        the teaching contract and refusal policy
  guard.cpp        topic gate
  rag.cpp          hybrid retrieval
  runtime.cpp      supervises the bundled model server
  crypto.cpp       PBKDF2/SHA-256 via Windows CNG
  tls_socket.cpp   TLS via Windows Schannel
  selftest.cpp     built-in checks
config/          configuration
licenses/        licence texts for the bundled engine and weights
training/        corpus ingestion and fine-tuning pipeline
ship/            packaging
get-smart-model.bat  downloads the optional Smart model
data/            accounts, chats, outbox (created on first run, never shipped)
```

---

## Troubleshooting

**"No bundled model found"** — `runtime\models` has no `.gguf`. Either the
package was built with `-NoModels`, or the folder did not survive the copy.

**It sat on "Loading the model into memory" and gave up** — the machine is short
on RAM. Switch to `qwen3-1.7b.gguf` (see [Speed](#speed-honestly)).

**No verification email** — that is the default. Look in `data\outbox\` for the
newest `.eml`.

**Answers are very slow** — see [Speed](#speed-honestly). A GPU, or the 1.7B
model, are the two real levers.

**It refuses a legitimate maths question** — the lexical gate is deliberately
strict. Add the vocabulary to `kMathTerms` or `kPhysicsTerms` in
`src\guard.cpp`, add a case to `selftest.cpp`, and rebuild.

**Maths still looks wrong** — the normaliser handles the common LaTeX. If you
find a construct it misses, add it to `kSymbols` or `kTransparentCommands` in
`src\markdown.cpp` and add a case to `test_markdown()` in `selftest.cpp`.

---

## License

The code in this repository is released under the [MIT License](LICENSE).
Bundled third-party components keep their own licences — llama.cpp (MIT),
SQLite (public domain), cpp-httplib and nlohmann/json (MIT), and the Qwen3 and
nomic-embed-text weights (Apache-2.0). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [licenses/](licenses/).
