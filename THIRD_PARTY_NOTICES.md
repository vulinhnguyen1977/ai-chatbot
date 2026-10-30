# Third-party notices

Components redistributed in this project, and their licences. Review this
before shipping to a client.

The project's own code is MIT-licensed (see `LICENSE`). Full licence texts for
the bundled engine and model weights are in `licenses/`, which `ship\package.ps1`
always includes in the package.

## Vendored source (`third_party/`)

| Component | Licence | Notes |
|---|---|---|
| [SQLite](https://sqlite.org) (amalgamation) | Public domain | No attribution required. |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | MIT | Retain the copyright notice. |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | Retain the copyright notice. |

The MIT notices are contained in the header files themselves, which ship as-is,
so redistributing `third_party/` satisfies both.

## Redistributed binaries (`runtime/bin/`)

| Component | Licence | Notes |
|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) (`llama-server` and its DLLs) | MIT | Prebuilt Windows binaries from the project's own releases. Retain the copyright notice. |

The MIT licence requires the notice to travel with the binaries. The llama.cpp
LICENSE (Copyright (c) 2023-2026 The ggml authors) is kept at
`licenses/llama.cpp-LICENSE.txt` and ships in every package.

## Runtime dependencies (not redistributed)

| Component | Licence | Notes |
|---|---|---|
| Windows CNG / Schannel / Win32 | Part of Windows | Used through the public API. |
| [Ollama](https://ollama.com) | MIT | Optional alternative backend, installed by the user if they want it. Not required and not shipped. |

## Model weights (`runtime/models/`)

Weights **are** included in the default package. That means redistributing
someone else's model, and their licence governs:

| Model | Author | Licence | Redistributable |
|---|---|---|---|
| `qwen3-4b.gguf` | Qwen team, Alibaba Cloud | Apache-2.0 | Yes, with attribution |
| `qwen3-1.7b.gguf` | Qwen team, Alibaba Cloud | Apache-2.0 | Yes, with attribution |
| `nomic-embed-text.gguf` | Nomic AI | Apache-2.0 | Yes, with attribution |
| Llama-family models | — | Meta Llama Community License | Conditional — read it |
| Gemma-family models | — | Gemma Terms of Use | Conditional — read it |

The bundled set is Apache-2.0 throughout, which permits redistribution provided
attribution and the licence text are retained. The weights are redistributed
unmodified (copied byte-for-byte from Ollama's published GGUF files); the
licence text is at `licenses/Apache-2.0.txt`.

**If you swap in a different model, re-check this table before packaging.**
`ship\package.ps1 -NoModels` produces a package with no weights at all.

## Textbook corpus

Anything you ingest through `training/` stays your responsibility. The pipeline
does not check licensing.

`training/data/textbooks/sample_physics_notes.txt` was written for this project
and carries no third-party rights.

Openly licensed sources suitable for this level: CK-12 (CC BY-NC), OpenStax
(CC BY 4.0), Siyavula (CC BY). Attribution requirements still apply, and CC
BY-NC excludes commercial use — relevant if you sell this product.
