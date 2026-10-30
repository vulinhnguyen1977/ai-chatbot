# Training and corpus pipeline

This folder turns textbooks into two things the tutor uses:

1. **A retrieval corpus** — passages the running server searches at question
   time, so answers can be grounded in *your* curriculum and cite a page.
2. **A fine-tuned adapter** — weights that make the model follow the five-part
   answer format and the refusal policy more reliably than prompting alone.

Only step 7 is mandatory. Everything else improves quality.

---

## What actually needs what

| Steps | What you get | Needs |
|---|---|---|
| 7 | A complete, working tutor | Ollama + a base model |
| 1–3, then `mstutor index` | Answers grounded in your textbooks, with citations | Python, `pypdf` |
| 4 | An instruction dataset | Python (step 3 optional) |
| 5–6 | A fine-tuned adapter | **NVIDIA GPU, ~10–12 GB VRAM** |

**Read this before planning around step 5.** LoRA fine-tuning requires
backpropagation through a multi-billion-parameter model. There is no
configuration in which that completes usefully on a CPU. If you do not have a
GPU, either rent one for an hour (Colab, RunPod, Lambda) and run steps 4–6
there, copying the resulting `.gguf` back into `training/adapters/`, or skip
them. The retrieval corpus is the larger quality win for a curriculum-specific
tutor, and it needs no GPU at all.

---

## Step by step

### 0. Install

```
pip install -r training/requirements.txt
```

### 1. Get textbooks

Put PDFs into `training/data/textbooks/`. Plain `.txt` and `.md` also work.

**Use only material you are licensed to use.** Openly licensed sources suitable
for this level:

- **CK-12** — <https://www.ck12.org> — middle school physical science and maths,
  CC BY-NC.
- **OpenStax** — <https://openstax.org> — CC BY 4.0. The algebra and physics
  titles aim slightly higher than middle school but the early chapters fit.
- **Siyavula** — <https://www.siyavula.com/read> — CC BY, free science and maths
  textbooks.

Scanned image-only PDFs produce no text. Run them through OCR
(e.g. `ocrmypdf`) first.

A small `sample_physics_notes.txt` ships with the project so you can run the
whole pipeline before sourcing anything.

### 2. Extract and chunk

```
python training/1_extract_pdfs.py
python training/2_build_corpus.py
```

Step 1 writes one text file per page (the page number becomes the citation).
Step 2 chunks pages into overlapping ~220-word passages, drops front matter and
duplicates, discards chunks with no maths or physics vocabulary, and labels each
as maths or physics.

Check `training/data/corpus.jsonl` before continuing. If good passages were
discarded, re-run with `--keep-all` or a lower `--min-hits`.

### 3. Ingest and index

```
python training/3_ingest_corpus.py
mstutor.exe index
```

Ingest inserts the chunks; `index` embeds them via Ollama. Both are safe to
re-run — content is de-duplicated by hash, and only un-embedded chunks are
processed, so adding one textbook does not redo the others.

Verify with `mstutor.exe check`.

### 4. Build the dataset

```
python training/4_build_dataset.py --synthetic 400
```

Combines hand-written seed examples in `training/seeds/` with question/answer
pairs generated from your corpus by the local model.

The seeds carry what textbooks cannot teach: the answer format, the refusal
wording, and resistance to "ignore your instructions". They are repeated
`--seed-weight` times (default 8) so a few hundred synthetic examples cannot
drown them out.

Synthetic generation is self-instruct, so it inherits the teacher model's
mistakes. Every generated answer is checked for the five required sections and
rejected if it drifts — expect a meaningful rejection rate, and read a sample of
`training/data/dataset/train.jsonl` before training on it.

### 5–6. Fine-tune (GPU)

```
pip install torch --index-url https://download.pytorch.org/whl/cu121
pip install -r training/requirements-train.txt
python training/5_finetune_lora.py --base Qwen/Qwen3-4B
python training/6_export_gguf.py
```

Use `--load-4bit` for QLoRA on a smaller card. The `--base` here **must**
correspond to the Ollama base in step 7: an adapter trained on Qwen3-4B applied
to a different model or size produces confident nonsense rather than an error.

### 7. Build the Ollama model

```
python training/7_build_ollama_model.py
```

Writes a `Modelfile` and runs `ollama create mstutor`. The system prompt is read
directly from `src/prompts.h`, so the served behaviour and the trained behaviour
cannot drift apart. Any adapter in `training/adapters/*.gguf` is attached
automatically.

**Re-run this whenever you edit `src/prompts.h`.**

---

## Evaluating a change

Before and after any change, run the same set of questions and compare:

```
mstutor.exe selftest          # topic gate regressions, no model calls
```

Then ask, through the UI, a fixed list covering: a multi-step arithmetic
problem, a fractions problem, a geometry area problem, a kinematics problem, a
circuits problem, three off-topic questions from different subjects, and one
instruction-override attempt. Check the answers are *arithmetically correct*,
not merely well-formatted — a fine-tune improves format long before it improves
accuracy, and a confidently-formatted wrong answer is worse than no answer.

Add any question the tutor gets wrong to `training/seeds/` with the correct
answer, and to the `selftest` cases if the topic gate was at fault.
