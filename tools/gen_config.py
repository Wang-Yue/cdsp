#!/usr/bin/env python3
"""
Entry point for CamillaDSP / cdsp C configuration code generation.
Pure Python 3 standard library.
"""

import os
import sys
import argparse

sys.dont_write_bytecode = True

# Add workspace to path
workspace_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if workspace_dir not in sys.path:
    sys.path.insert(0, workspace_dir)

from tools.codegen.gen_engine import CodegenEngine
from src.config.schema.config_schema import ALL_SCHEMAS

def main():
    default_out_dir = os.path.join(workspace_dir, "src", "config")
    parser = argparse.ArgumentParser(description="Generate C config parser and structures.")
    parser.add_argument("--out-dir", default=default_out_dir, help="Output directory for generated files")
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    engine = CodegenEngine(ALL_SCHEMAS)
    header_code = engine.generate_header()
    source_code = engine.generate_source()

    header_path = os.path.join(args.out_dir, "config_gen.h")
    source_path = os.path.join(args.out_dir, "config_gen.c")

    with open(header_path, "w", encoding="utf-8") as f:
        f.write(header_code)

    with open(source_path, "w", encoding="utf-8") as f:
        f.write(source_code)

    print(f"[codegen] Generated {header_path} and {source_path}")

if __name__ == "__main__":
    main()
