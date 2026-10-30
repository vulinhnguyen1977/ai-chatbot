"""Step 3 -- load the corpus into the application database.

    python training/3_ingest_corpus.py

Inserts training/data/corpus.jsonl into the doc_chunks table that the running
server retrieves from. Re-running is safe: content is de-duplicated by hash, so
adding one new textbook does not re-insert the others.

Afterwards, build the vectors with:   mstutor.exe index
"""

from __future__ import annotations

import argparse
import hashlib
import sqlite3
import sys
from pathlib import Path

from common import CORPUS, ROOT, read_jsonl

DEFAULT_DB = ROOT / "data" / "mstutor.db"


def ensure_schema(conn: sqlite3.Connection) -> None:
    """Create the corpus tables when ingesting before the server's first run."""
    conn.executescript("""
    CREATE TABLE IF NOT EXISTS doc_chunks (
      id        INTEGER PRIMARY KEY AUTOINCREMENT,
      source    TEXT NOT NULL,
      subject   TEXT NOT NULL,
      grade     TEXT,
      locator   TEXT,
      content   TEXT NOT NULL,
      token_est INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS idx_chunks_subject ON doc_chunks(subject);
    CREATE TABLE IF NOT EXISTS doc_vectors (
      chunk_id INTEGER PRIMARY KEY REFERENCES doc_chunks(id) ON DELETE CASCADE,
      dim      INTEGER NOT NULL,
      model    TEXT NOT NULL,
      vec      BLOB NOT NULL
    );
    CREATE VIRTUAL TABLE IF NOT EXISTS doc_fts USING fts5(
      content, content='doc_chunks', content_rowid='id', tokenize='porter unicode61'
    );
    CREATE TRIGGER IF NOT EXISTS doc_chunks_ai AFTER INSERT ON doc_chunks BEGIN
      INSERT INTO doc_fts(rowid, content) VALUES (new.id, new.content);
    END;
    CREATE TRIGGER IF NOT EXISTS doc_chunks_ad AFTER DELETE ON doc_chunks BEGIN
      INSERT INTO doc_fts(doc_fts, rowid, content) VALUES('delete', old.id, old.content);
    END;
    CREATE TRIGGER IF NOT EXISTS doc_chunks_au AFTER UPDATE ON doc_chunks BEGIN
      INSERT INTO doc_fts(doc_fts, rowid, content) VALUES('delete', old.id, old.content);
      INSERT INTO doc_fts(rowid, content) VALUES (new.id, new.content);
    END;
    """)
    conn.commit()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=str(DEFAULT_DB), help=f"database (default {DEFAULT_DB})")
    parser.add_argument("--corpus", default=str(CORPUS), help="corpus.jsonl to ingest")
    parser.add_argument("--replace", action="store_true",
                        help="delete the existing corpus before inserting")
    args = parser.parse_args()

    corpus_path = Path(args.corpus)
    if not corpus_path.exists():
        print(f"{corpus_path} not found. Run 2_build_corpus.py first.")
        return 1

    db_path = Path(args.db)
    db_path.parent.mkdir(parents=True, exist_ok=True)

    conn = sqlite3.connect(str(db_path))
    # The schema needs FTS5. Fail with a clear message rather than a confusing
    # error part-way through the insert loop.
    try:
        ensure_schema(conn)
    except sqlite3.OperationalError as exc:
        if "fts5" in str(exc).lower():
            print("This Python's SQLite was built without FTS5, which the corpus tables need.")
            print("Start the server once to create the schema, then re-run this script.")
            return 1
        raise

    if args.replace:
        conn.execute("DELETE FROM doc_chunks")
        conn.commit()
        print("Cleared the existing corpus.")

    existing_hashes = set()
    for (content,) in conn.execute("SELECT content FROM doc_chunks"):
        existing_hashes.add(hashlib.sha256(content.lower().encode("utf-8")).hexdigest())

    inserted = duplicate = 0
    for row in read_jsonl(corpus_path):
        content = (row.get("content") or "").strip()
        if not content:
            continue
        digest = hashlib.sha256(content.lower().encode("utf-8")).hexdigest()
        if digest in existing_hashes:
            duplicate += 1
            continue
        existing_hashes.add(digest)
        conn.execute(
            "INSERT INTO doc_chunks(source, subject, grade, locator, content, token_est) "
            "VALUES(?,?,?,?,?,?)",
            (
                row.get("source", "unknown"),
                row.get("subject", "math"),
                row.get("grade", ""),
                row.get("locator", ""),
                content,
                # Rough token estimate: English averages about 0.75 words per token.
                int(len(content.split()) / 0.75),
            ),
        )
        inserted += 1

    conn.commit()
    total = conn.execute("SELECT COUNT(*) FROM doc_chunks").fetchone()[0]
    conn.close()

    print(f"Inserted {inserted} chunks ({duplicate} already present).")
    print(f"The corpus now holds {total} chunks in {db_path}")
    print("\nNext:  mstutor.exe index      (builds the embeddings)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
