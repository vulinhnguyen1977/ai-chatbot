"""Step 5 -- LoRA fine-tune on the dataset from step 4.

    pip install -r training/requirements-train.txt
    python training/5_finetune_lora.py --base Qwen/Qwen3-4B

REQUIRES AN NVIDIA GPU. LoRA on a 4B model needs roughly 10-12 GB of VRAM at
these settings; drop --batch-size to 1 and raise --grad-accum to fit a smaller
card. On CPU this will not finish in any useful time -- that is a property of
backpropagation, not of this script.

If you do not have a GPU, skip steps 5 and 6. Step 7 still produces a complete,
working tutor from the prompt alone; the fine-tune improves format consistency
and refusal reliability on top of that, it is not a prerequisite.

The base model here must correspond to the Ollama base used in step 7 -- an
adapter trained on Qwen3-4B cannot be applied to Llama or to a different size.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from common import ADAPTERS, DATASET, require

DEFAULT_BASE = "Qwen/Qwen3-4B"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default=DEFAULT_BASE, help=f"HF model id (default {DEFAULT_BASE})")
    parser.add_argument("--out", default=str(ADAPTERS / "mstutor-lora"))
    parser.add_argument("--epochs", type=float, default=3.0)
    parser.add_argument("--batch-size", type=int, default=2)
    parser.add_argument("--grad-accum", type=int, default=8)
    parser.add_argument("--lr", type=float, default=2e-4)
    parser.add_argument("--max-seq", type=int, default=2048)
    parser.add_argument("--rank", type=int, default=16, help="LoRA rank")
    parser.add_argument("--alpha", type=int, default=32, help="LoRA alpha")
    parser.add_argument("--load-4bit", action="store_true",
                        help="quantise the base to 4-bit (QLoRA) to fit a smaller GPU")
    args = parser.parse_args()

    train_path = DATASET / "train.jsonl"
    if not train_path.exists():
        print(f"{train_path} not found. Run 4_build_dataset.py first.")
        return 1

    torch = require("torch", "torch --index-url https://download.pytorch.org/whl/cu121")
    require("transformers", "transformers")
    require("peft", "peft")
    require("trl", "trl")
    require("datasets", "datasets")

    if not torch.cuda.is_available():
        print("No CUDA GPU detected.")
        print()
        print("Fine-tuning needs one. Options:")
        print("  * Skip steps 5-6 -- step 7 builds a complete tutor without them.")
        print("  * Rent a GPU (Colab, RunPod, Lambda) and run steps 4-6 there,")
        print("    then copy the .gguf adapter back into training/adapters/.")
        return 1

    from datasets import load_dataset
    from peft import LoraConfig
    from transformers import AutoModelForCausalLM, AutoTokenizer
    from trl import SFTConfig, SFTTrainer

    print(f"GPU: {torch.cuda.get_device_name(0)}")
    print(f"Base: {args.base}")

    tokenizer = AutoTokenizer.from_pretrained(args.base, trust_remote_code=True)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token

    load_kwargs = {"dtype": torch.bfloat16, "device_map": "auto", "trust_remote_code": True}
    if args.load_4bit:
        require("bitsandbytes", "bitsandbytes")
        from transformers import BitsAndBytesConfig
        load_kwargs["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True,
            bnb_4bit_compute_dtype=torch.bfloat16,
            bnb_4bit_quant_type="nf4",
            bnb_4bit_use_double_quant=True,
        )

    model = AutoModelForCausalLM.from_pretrained(args.base, **load_kwargs)
    model.config.use_cache = False  # incompatible with gradient checkpointing

    data_files = {"train": str(train_path)}
    val_path = DATASET / "val.jsonl"
    if val_path.exists():
        data_files["validation"] = str(val_path)
    dataset = load_dataset("json", data_files=data_files)
    print(f"Training examples: {len(dataset['train'])}")

    peft_config = LoraConfig(
        r=args.rank,
        lora_alpha=args.alpha,
        lora_dropout=0.05,
        bias="none",
        task_type="CAUSAL_LM",
        # Attention and MLP projections. Targeting both adapts style and
        # content; attention alone tends to leave the format inconsistent.
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj",
                        "gate_proj", "up_proj", "down_proj"],
    )

    config = SFTConfig(
        output_dir=args.out,
        num_train_epochs=args.epochs,
        per_device_train_batch_size=args.batch_size,
        gradient_accumulation_steps=args.grad_accum,
        learning_rate=args.lr,
        lr_scheduler_type="cosine",
        warmup_ratio=0.03,
        logging_steps=5,
        save_strategy="epoch",
        eval_strategy="epoch" if val_path.exists() else "no",
        bf16=True,
        max_length=args.max_seq,
        gradient_checkpointing=True,
        report_to=[],
        # Train on the assistant turn only. Without this the model also learns
        # to predict the (identical) system prompt and the student's question,
        # which wastes capacity and weakens the behaviour we want.
        assistant_only_loss=True,
    )

    trainer = SFTTrainer(
        model=model,
        args=config,
        train_dataset=dataset["train"],
        eval_dataset=dataset.get("validation"),
        peft_config=peft_config,
        processing_class=tokenizer,
    )

    trainer.train()

    out_dir = Path(args.out)
    trainer.save_model(str(out_dir))
    tokenizer.save_pretrained(str(out_dir))

    # Record the base model: step 6 needs it, and applying an adapter to the
    # wrong base produces confident nonsense rather than an error.
    (out_dir / "mstutor_base.json").write_text(
        json.dumps({"base_model": args.base}, indent=2), encoding="utf-8"
    )

    print(f"\nAdapter saved to {out_dir}")
    print("Next:  python training/6_export_gguf.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
