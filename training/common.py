"""Shared helpers for the training and ingestion pipeline.

The single most important thing in here is `read_tutor_system_prompt()`. The
teaching contract lives in src/prompts.h because the C++ server needs it at
runtime; the fine-tune and the Ollama model both have to be built from that
exact same text or the model will be trained against one contract and prompted
with another. Parsing the header keeps one copy.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRAINING = ROOT / "training"
DATA = TRAINING / "data"

TEXTBOOKS = DATA / "textbooks"      # drop source PDFs here
EXTRACTED = DATA / "extracted"      # per-page plain text
CORPUS = DATA / "corpus.jsonl"      # retrieval chunks
DATASET = DATA / "dataset"          # instruction-tuning data
ADAPTERS = TRAINING / "adapters"    # LoRA output

MATH_TERMS = {
    "fraction", "decimal", "percent", "percentage", "ratio", "proportion", "integer",
    "exponent", "algebra", "equation", "inequality", "expression", "simplify", "factor",
    "quadratic", "linear", "coordinate", "graph", "slope", "geometry", "triangle",
    "rectangle", "circle", "polygon", "angle", "parallel", "perpendicular", "congruent",
    "perimeter", "area", "volume", "circumference", "radius", "diameter", "pythagoras",
    "mean", "median", "mode", "probability", "statistics", "prime", "numerator",
    "denominator", "arithmetic", "multiply", "divide", "subtract", "sequence", "symmetry",
}

PHYSICS_TERMS = {
    "velocity", "acceleration", "speed", "displacement", "momentum", "newton", "force",
    "friction", "gravity", "weight", "mass", "inertia", "work", "power", "joule", "watt",
    "energy", "kinetic", "potential", "lever", "pulley", "density", "pressure", "buoyancy",
    "heat", "temperature", "thermal", "conduction", "convection", "radiation", "wave",
    "wavelength", "frequency", "amplitude", "sound", "light", "reflection", "refraction",
    "lens", "mirror", "magnet", "magnetic", "electric", "circuit", "current", "voltage",
    "resistance", "ohm", "ampere", "volt", "conductor", "insulator",
}


def read_tutor_system_prompt() -> str:
    """Extract kTutorSystem from src/prompts.h.

    Raises rather than falling back to a hard-coded copy: a silent divergence
    between the served prompt and the trained prompt is exactly the bug this
    function exists to prevent.
    """
    header = ROOT / "src" / "prompts.h"
    text = header.read_text(encoding="utf-8")
    match = re.search(r'kTutorSystem\s*=\s*R"\((.*?)\)";', text, re.DOTALL)
    if not match:
        raise SystemExit(f"Could not find kTutorSystem in {header}")
    return match.group(1).strip()


def guess_subject(text: str) -> str:
    """Label a passage 'math' or 'physics' by vocabulary overlap."""
    lowered = text.lower()
    math_hits = sum(1 for term in MATH_TERMS if term in lowered)
    physics_hits = sum(1 for term in PHYSICS_TERMS if term in lowered)
    if physics_hits > math_hits:
        return "physics"
    return "math"


def clean_text(text: str) -> str:
    """Normalise PDF text extraction artefacts."""
    # Join words split across a line break by hyphenation.
    text = re.sub(r"(\w)-\n(\w)", r"\1\2", text)
    # Collapse single newlines inside a paragraph, keep blank-line breaks.
    text = re.sub(r"(?<!\n)\n(?!\n)", " ", text)
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    # Drop the private-use and control characters PDF fonts often leave behind.
    text = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f-]", "", text)
    return text.strip()


def chunk_text(text: str, target_words: int = 220, overlap_words: int = 40):
    """Split into overlapping word windows, respecting paragraph boundaries.

    Overlap matters for retrieval: a worked example split down the middle is
    useless to both halves, and the overlap keeps at least one copy intact.
    """
    paragraphs = [p.strip() for p in text.split("\n\n") if p.strip()]
    chunks, current, count = [], [], 0

    for paragraph in paragraphs:
        words = paragraph.split()
        if not words:
            continue
        if count + len(words) > target_words and current:
            chunks.append(" ".join(current))
            tail = " ".join(current).split()[-overlap_words:] if overlap_words else []
            current = list(tail)
            count = len(current)
        current.extend(words)
        count += len(words)

    if current:
        chunks.append(" ".join(current))
    # Fragments shorter than this carry no usable teaching content.
    return [c for c in chunks if len(c.split()) >= 25]


def write_jsonl(path: Path, rows) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
            written += 1
    return written


def read_jsonl(path: Path):
    if not path.exists():
        return
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                yield json.loads(line)


def require(module: str, install: str):
    """Import a module or exit with the exact install command."""
    try:
        return __import__(module)
    except ImportError:
        sys.exit(
            f"This step needs the '{module}' package.\n"
            f"Install it with:  pip install {install}"
        )
