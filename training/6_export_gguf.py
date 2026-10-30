"""Step 6 -- convert the trained LoRA adapter to GGUF for Ollama.

    python training/6_export_gguf.py

Ollama's ADAPTER instruction takes a GGUF adapter, while step 5 produces one in
Hugging Face PEFT format. The converter lives in llama.cpp, so this script
fetches llama.cpp if it is not already present and runs
convert_lora_to_gguf.py against the adapter.

The result lands in training/adapters/*.gguf, where step 7 picks it up
automatically.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from common import ADAPTERS, TRAINING

LLAMA_CPP = TRAINING / "llama.cpp"
REPO = "https://github.com/ggml-org/llama.cpp"


def ensure_llama_cpp(path: Path) -> Path | None:
    converter = path / "convert_lora_to_gguf.py"
    if converter.exists():
        return converter

    if not shutil.which("git"):
        print("git is not installed, so llama.cpp cannot be fetched automatically.")
        print(f"Clone it manually:\n  git clone {REPO} {path}")
        return None

    print(f"Fetching llama.cpp into {path} (shallow clone)...")
    result = subprocess.run(["git", "clone", "--depth", "1", REPO, str(path)])
    if result.returncode != 0 or not converter.exists():
        print("Clone failed. Fetch it manually and re-run with --llama-cpp <path>.")
        return None
    return converter


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter", default=str(ADAPTERS / "mstutor-lora"),
                        help="PEFT adapter directory from step 5")
    parser.add_argument("--llama-cpp", default=str(LLAMA_CPP),
                        help="path to a llama.cpp checkout")
    parser.add_argument("--outtype", default="f16", choices=["f32", "f16", "q8_0"],
                        help="adapter precision (default f16)")
    args = parser.parse_args()

    adapter_dir = Path(args.adapter)
    if not adapter_dir.exists():
        print(f"{adapter_dir} not found. Run 5_finetune_lora.py first.")
        print("(No GPU? Skip steps 5-6 -- step 7 works without an adapter.)")
        return 1

    converter = ensure_llama_cpp(Path(args.llama_cpp))
    if converter is None:
        return 1

    # The base model has to be readable so the converter can match tensor names.
    base_note = adapter_dir / "mstutor_base.json"
    base_model = None
    if base_note.exists():
        base_model = json.loads(base_note.read_text(encoding="utf-8")).get("base_model")
        print(f"Adapter was trained against: {base_model}")

    ADAPTERS.mkdir(parents=True, exist_ok=True)
    output = ADAPTERS / "mstutor-lora.gguf"

    command = [
        sys.executable, str(converter), str(adapter_dir),
        "--outfile", str(output),
        "--outtype", args.outtype,
    ]
    if base_model:
        command += ["--base", base_model]

    print("Running:", " ".join(command))
    result = subprocess.run(command)
    if result.returncode != 0:
        print("\nConversion failed.")
        print("The converter needs the base model's config; make sure it is")
        print("downloadable, or pass --base <local path to the base model>.")
        return result.returncode

    if not output.exists():
        print("The converter reported success but produced no file.")
        return 1

    size_mb = output.stat().st_size / (1024 * 1024)
    print(f"\nWrote {output} ({size_mb:.1f} MB)")
    print("\nNext:  python training/7_build_ollama_model.py")
    print("  (it will pick this adapter up automatically)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
