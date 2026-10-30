"""Step 2 -- turn extracted pages into retrieval chunks.

    python training/2_build_corpus.py

Reads training/data/extracted/**/page_*.txt and writes training/data/corpus.jsonl,
one JSON object per chunk:

    {"source": "...", "subject": "math", "grade": "", "locator": "p. 42",
     "content": "..."}

Chunks are filtered to material that actually looks like middle school maths or
physics, so a general-interest book contributes only its relevant sections.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

from common import (
    CORPUS, EXTRACTED, MATH_TERMS, PHYSICS_TERMS, chunk_text, guess_subject, write_jsonl,
)

# Chunks with none of this vocabulary are prose that retrieval will never want.
RELEVANT_TERMS = MATH_TERMS | PHYSICS_TERMS


def is_relevant(text: str, min_hits: int) -> bool:
    lowered = text.lower()
    hits = sum(1 for term in RELEVANT_TERMS if term in lowered)
    if hits >= min_hits:
        return True
    # A passage dense with numbers and operators is usually a worked example
    # even when it uses little named vocabulary.
    digits = sum(1 for c in text if c.isdigit())
    return hits >= 1 and digits >= 12


def looks_like_boilerplate(text: str) -> bool:
    """Filter the front/back matter that pollutes textbook extractions."""
    lowered = text.lower()
    markers = (
        "table of contents", "all rights reserved", "isbn", "copyright ©",
        "printed in", "acknowledgements", "about the author", "index of terms",
    )
    if any(m in lowered for m in markers):
        return True
    # Contents pages are mostly dot leaders and page numbers.
    if len(re.findall(r"\.{4,}", text)) >= 3:
        return True
    letters = sum(1 for c in text if c.isalpha())
    return letters < len(text) * 0.5


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--words", type=int, default=220, help="target chunk size in words")
    parser.add_argument("--overlap", type=int, default=40, help="overlap in words")
    parser.add_argument("--min-hits", type=int, default=2,
                        help="subject terms needed to keep a chunk (default 2)")
    parser.add_argument("--keep-all", action="store_true",
                        help="skip relevance filtering and keep every chunk")
    args = parser.parse_args()

    if not EXTRACTED.exists():
        print(f"{EXTRACTED} does not exist. Run 1_extract_pdfs.py first.")
        return 1

    rows = []
    seen: set[str] = set()
    skipped_irrelevant = skipped_boilerplate = skipped_duplicate = 0

    for book_dir in sorted(p for p in EXTRACTED.iterdir() if p.is_dir()):
        source = book_dir.name.replace("_", " ")
        pages = sorted(book_dir.glob("page_*.txt"))
        kept_for_book = 0

        for page_path in pages:
            page_number = int(page_path.stem.split("_")[1])
            text = page_path.read_text(encoding="utf-8", errors="replace")

            if looks_like_boilerplate(text):
                skipped_boilerplate += 1
                continue

            for chunk in chunk_text(text, args.words, args.overlap):
                if not args.keep_all and not is_relevant(chunk, args.min_hits):
                    skipped_irrelevant += 1
                    continue
                # Textbooks repeat definitions and summaries verbatim; dedupe so
                # retrieval does not return the same paragraph four times.
                digest = hashlib.sha256(chunk.lower().encode("utf-8")).hexdigest()
                if digest in seen:
                    skipped_duplicate += 1
                    continue
                seen.add(digest)

                rows.append({
                    "source": source,
                    "subject": guess_subject(chunk),
                    "grade": "",
                    "locator": f"p. {page_number}",
                    "content": chunk,
                })
                kept_for_book += 1

        print(f"  {source}: {kept_for_book} chunks from {len(pages)} pages")

    if not rows:
        print("\nNo usable chunks were produced.")
        print("Try --keep-all if your source material uses unusual vocabulary.")
        return 1

    count = write_jsonl(CORPUS, rows)
    maths = sum(1 for r in rows if r["subject"] == "math")
    print(f"\nWrote {count} chunks to {CORPUS}")
    print(f"  maths: {maths}    physics: {count - maths}")
    print(f"  skipped -- off topic: {skipped_irrelevant}, "
          f"boilerplate: {skipped_boilerplate}, duplicate: {skipped_duplicate}")
    print("\nNext:  python training/3_ingest_corpus.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
