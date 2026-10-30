"""Step 4 -- assemble the instruction-tuning dataset.

    python training/4_build_dataset.py                  # seeds only
    python training/4_build_dataset.py --synthetic 400  # + generated from textbooks

Two sources are combined:

  * training/seeds/*.jsonl -- hand-written examples. These carry the behaviour
    that matters most and that textbooks cannot teach: the five-part answer
    format, the refusal wording, and resistance to instruction-override
    attempts. They are duplicated `--seed-weight` times so that a few hundred
    synthetic examples cannot drown them out.

  * synthetic pairs generated from the ingested corpus by the local model. This
    is self-instruct: the teacher model reads a textbook passage and writes a
    question and a house-format answer grounded in it. It is cheap and needs no
    labelled data, but it inherits the teacher's mistakes -- so every generated
    answer is checked for the required structure before it is kept.

Output: training/data/dataset/train.jsonl and val.jsonl, in the messages format
that step 5 consumes.
"""

from __future__ import annotations

import argparse
import json
import random
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

from common import CORPUS, DATASET, TRAINING, read_jsonl, read_tutor_system_prompt, write_jsonl

SEEDS_DIR = TRAINING / "seeds"

GENERATE_PROMPT = """You are writing training data for a middle school maths and physics tutor.

Below is a passage from a school textbook. Write ONE question a student aged 11-14 might realistically ask about this material, and then the tutor's ideal answer.

The answer MUST use exactly this structure, with these headings:

**What we know**
(the given quantities with units, and what is being asked)

**Idea**
(the concept or formula that applies, in one or two plain sentences)

**Working**
(every step shown, nothing skipped)

**Answer**
(stated clearly, with units)

**Check**
(verify by substituting back, estimating, or checking units)

Then one final line beginning "Common mistake to avoid:".

Rules: stay at middle school level. Write maths in plain text (x^2, 3/4, sqrt(16), 12 m/s) and never LaTeX. Be accurate.

Reply with JSON only, in the form {"question": "...", "answer": "..."}.

PASSAGE:
"""


def ollama_generate(base_url: str, model: str, passage: str, timeout: int) -> dict | None:
    """Ask the local model for one question/answer pair. None on any failure."""
    body = json.dumps({
        "model": model,
        "stream": False,
        # Some variety, or every generated question comes out phrased the same.
        "options": {"temperature": 0.8, "num_ctx": 8192},
        "format": {
            "type": "object",
            "properties": {"question": {"type": "string"}, "answer": {"type": "string"}},
            "required": ["question", "answer"],
        },
        "messages": [{"role": "user", "content": GENERATE_PROMPT + passage}],
    }).encode("utf-8")

    request = urllib.request.Request(
        f"{base_url}/api/chat", data=body, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
        return json.loads(payload["message"]["content"])
    except (urllib.error.URLError, OSError, KeyError, json.JSONDecodeError, TimeoutError):
        return None


def is_well_formed(answer: str) -> bool:
    """Reject generations that do not follow the house format.

    Without this filter the fine-tune learns the teacher's inconsistency rather
    than the format we actually want.
    """
    required = ["**What we know**", "**Idea**", "**Working**", "**Answer**", "**Check**"]
    if not all(section in answer for section in required):
        return False
    if len(answer) < 200:
        return False
    # The prompt forbids LaTeX; catch the common escapes.
    if re.search(r"\$\$|\\frac|\\times|\\begin\{", answer):
        return False
    return True


def load_seeds(system_prompt: str) -> list[dict]:
    rows = []
    if not SEEDS_DIR.exists():
        return rows
    for path in sorted(SEEDS_DIR.glob("*.jsonl")):
        for row in read_jsonl(path):
            user = (row.get("user") or "").strip()
            assistant = (row.get("assistant") or "").strip()
            if user and assistant:
                rows.append({"messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user},
                    {"role": "assistant", "content": assistant},
                ]})
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--synthetic", type=int, default=0,
                        help="number of synthetic examples to generate from the corpus")
    parser.add_argument("--model", default="qwen3:4b", help="teacher model for generation")
    parser.add_argument("--url", default="http://127.0.0.1:11434", help="Ollama base URL")
    parser.add_argument("--seed-weight", type=int, default=8,
                        help="how many times to repeat each seed example (default 8)")
    parser.add_argument("--val-fraction", type=float, default=0.05)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--seed", type=int, default=17, help="RNG seed for a reproducible split")
    args = parser.parse_args()

    system_prompt = read_tutor_system_prompt()
    random.seed(args.seed)

    seeds = load_seeds(system_prompt)
    if not seeds:
        print(f"No seed examples found in {SEEDS_DIR}")
        return 1
    print(f"Loaded {len(seeds)} seed examples (repeated {args.seed_weight}x)")

    rows = seeds * args.seed_weight

    if args.synthetic > 0:
        passages = [
            r for r in read_jsonl(CORPUS)
            if len((r.get("content") or "")) > 400
        ]
        if not passages:
            print(f"\nNo corpus at {CORPUS} -- skipping synthetic generation.")
            print("Run steps 1-3 first if you want textbook-grounded examples.")
        else:
            random.shuffle(passages)
            print(f"\nGenerating up to {args.synthetic} examples from {len(passages)} passages")
            print(f"using '{args.model}'. This is slow; leave it running.\n")

            kept = attempted = rejected = 0
            for passage in passages:
                if kept >= args.synthetic:
                    break
                attempted += 1
                pair = ollama_generate(args.url, args.model, passage["content"], args.timeout)
                if not pair:
                    continue
                question = (pair.get("question") or "").strip()
                answer = (pair.get("answer") or "").strip()
                if not question or not is_well_formed(answer):
                    rejected += 1
                    continue
                rows.append({"messages": [
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": question},
                    {"role": "assistant", "content": answer},
                ]})
                kept += 1
                if kept % 10 == 0:
                    print(f"  kept {kept}/{args.synthetic}  (rejected {rejected})")

            print(f"\nGenerated {kept} usable examples from {attempted} attempts "
                  f"({rejected} failed the format check).")

    random.shuffle(rows)
    split = max(1, int(len(rows) * args.val_fraction))
    validation, train = rows[:split], rows[split:]

    write_jsonl(DATASET / "train.jsonl", train)
    write_jsonl(DATASET / "val.jsonl", validation)

    print(f"\nWrote {len(train)} training and {len(validation)} validation examples")
    print(f"  -> {DATASET}")
    print("\nNext:  python training/5_finetune_lora.py   (needs a GPU)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
