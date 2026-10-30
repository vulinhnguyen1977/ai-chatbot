"""Step 1 -- extract text from the source textbooks.

Drop PDFs (and plain .txt / .md files) into training/data/textbooks/, then:

    pip install pypdf
    python training/1_extract_pdfs.py

Each page is written to training/data/extracted/<book>/page_0001.txt so the
page number survives into the citation the tutor shows the student.

Only use material you are licensed to use. See training/README.md.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from common import EXTRACTED, TEXTBOOKS, clean_text, require


def extract_pdf(path: Path, out_dir: Path, min_chars: int) -> int:
    pypdf = require("pypdf", "pypdf")
    try:
        reader = pypdf.PdfReader(str(path))
    except Exception as exc:  # a corrupt or encrypted file should not stop the run
        print(f"  !! cannot read {path.name}: {exc}")
        return 0

    if getattr(reader, "is_encrypted", False):
        try:
            reader.decrypt("")  # many textbook PDFs carry an empty owner password
        except Exception:
            print(f"  !! {path.name} is encrypted; skipping")
            return 0

    out_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for index, page in enumerate(reader.pages, start=1):
        try:
            text = clean_text(page.extract_text() or "")
        except Exception:
            continue
        # Pages under the threshold are covers, blank pages, or image-only
        # scans that produced nothing useful.
        if len(text) < min_chars:
            continue
        (out_dir / f"page_{index:04d}.txt").write_text(text, encoding="utf-8")
        written += 1
    return written


def extract_plaintext(path: Path, out_dir: Path, min_chars: int) -> int:
    text = clean_text(path.read_text(encoding="utf-8", errors="replace"))
    if len(text) < min_chars:
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "page_0001.txt").write_text(text, encoding="utf-8")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default=str(TEXTBOOKS), help="folder of source documents")
    parser.add_argument("--min-chars", type=int, default=200,
                        help="skip pages with less text than this (default 200)")
    args = parser.parse_args()

    source = Path(args.source)
    source.mkdir(parents=True, exist_ok=True)

    documents = sorted(
        p for p in source.rglob("*")
        if p.is_file() and p.suffix.lower() in {".pdf", ".txt", ".md"}
    )
    if not documents:
        print(f"No documents found in {source}")
        print("Put your PDF textbooks there and run this again.")
        print("See training/README.md for where to obtain openly licensed textbooks.")
        return 1

    print(f"Found {len(documents)} document(s) in {source}\n")
    total_pages = 0
    for doc in documents:
        book = doc.stem.replace(" ", "_")
        out_dir = EXTRACTED / book
        print(f"  {doc.name}")
        if doc.suffix.lower() == ".pdf":
            pages = extract_pdf(doc, out_dir, args.min_chars)
        else:
            pages = extract_plaintext(doc, out_dir, args.min_chars)
        print(f"    -> {pages} page(s) of text")
        total_pages += pages

    print(f"\nExtracted {total_pages} pages into {EXTRACTED}")
    if total_pages == 0:
        print("\nNothing was extracted. If your PDFs are scanned images rather than")
        print("digital text, they need OCR first (for example with ocrmypdf).")
        return 1
    print("Next:  python training/2_build_corpus.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
