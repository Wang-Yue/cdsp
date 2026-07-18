#!/usr/bin/env python3
import os
import re
import sys
import json
from collections import defaultdict, deque

SEARCH_DIRS = ["Engine", "Audio", "DoP", "Pipeline", "Resampler", "Filters", "Mixer", "Utils", "Backend", "Logging"]

C_KEYWORDS = {
    "if", "while", "for", "switch", "return", "sizeof", "typeof", "alignof",
    "atomic_load_explicit", "atomic_store_explicit", "atomic_init",
    "atomic_compare_exchange_weak_explicit", "atomic_compare_exchange_strong_explicit",
    "atomic_exchange", "atomic_load", "atomic_store", "cast", "defined",
    "__attribute__", "NULL", "true", "false", "int", "char", "bool", "double", "float", "size_t"
}

def remove_comments_and_strings(code):
    code = re.sub(r'//.*', '', code)
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.DOTALL)
    code = re.sub(r'"([^"\\]|\\.)*"', '""', code)
    return code

def parse_c_files(root_dir, search_dirs):
    functions = {}
    for d in search_dirs:
        dir_path = os.path.join(root_dir, d)
        if not os.path.exists(dir_path):
            continue
        for fname in os.listdir(dir_path):
            if fname.endswith(".c") or fname.endswith(".h"):
                fpath = os.path.join(dir_path, fname)
                with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                
                clean_content = remove_comments_and_strings(content)
                lines = clean_content.splitlines()

                i = 0
                while i < len(lines):
                    line = lines[i]
                    m = re.match(r'^(?:static\s+|inline\s+|extern\s+)?(?:[a-zA-Z0-9_]+\s+\*?)+([a-zA-Z0-9_]+)\s*\([^)]*\)\s*\{', line.strip())
                    if m and not line.strip().startswith("#"):
                        func_name = m.group(1)
                        if func_name not in C_KEYWORDS:
                            start_line = i + 1
                            brace_count = 1
                            body_lines = [line]
                            i += 1
                            while i < len(lines) and brace_count > 0:
                                b_line = lines[i]
                                brace_count += b_line.count('{') - b_line.count('}')
                                body_lines.append(b_line)
                                i += 1
                            body = "\n".join(body_lines)
                            functions[func_name] = {
                                "file": f"{d}/{fname}",
                                "start_line": start_line,
                                "body": body
                            }
                            continue
                    i += 1
    return functions

def extract_calls_detailed(body):
    calls_with_mode = [] # list of (func_name, is_error_path)
    lines = body.splitlines()
    in_error_block = False
    error_brace_depth = 0
    current_brace_depth = 0

    for line in lines:
        stripped = line.strip()
        if re.search(r'if\s*\([^)]*(?:err|rerr|perr|failed|STOP_REASON|EOF|!ok|!got_data|!success|!core|!loop|!state|!chunk|reached_eos)[^)]*\)', stripped):
            in_error_block = True
            error_brace_depth = current_brace_depth

        calls = re.findall(r'\b([a-zA-Z0-9_]+)\s*\(', stripped)
        for c in calls:
            if c not in C_KEYWORDS and not c.startswith("LOG_") and not c.startswith("logger_"):
                calls_with_mode.append((c, in_error_block))

        current_brace_depth += stripped.count('{') - stripped.count('}')
        if in_error_block and current_brace_depth <= error_brace_depth:
            in_error_block = False

    return calls_with_mode

def generate_mermaid_and_tree(root_func, functions):
    visited = set()
    edges = set()
    node_files = {}

    queue = deque([root_func])

    while queue:
        curr = queue.popleft()
        if curr in visited:
            continue
        visited.add(curr)

        if curr in functions:
            info = functions[curr]
            node_files[curr] = info["file"]
            calls = extract_calls_detailed(info["body"])
            for target, is_err in calls:
                edges.add((curr, target, is_err))
                if target in functions and target not in visited:
                    queue.append(target)
                elif target == "pthread_mutex_lock":
                    node_files[target] = "POSIX Mutex"

    mermaid_lines = ["graph TD"]
    for src, dst, is_err in sorted(edges):
        if is_err:
            mermaid_lines.append(f'    {src} -->|Error/Teardown Path| {dst}')
        else:
            mermaid_lines.append(f'    {src} -->|Steady Hot Path| {dst}')

    lock_paths = []
    def dfs(curr, path, err_mode):
        if curr == "pthread_mutex_lock":
            lock_paths.append((path, err_mode))
            return
        if curr in functions:
            calls = extract_calls_detailed(functions[curr]["body"])
            for target, is_err in calls:
                if target not in path:
                    dfs(target, path + [target], err_mode or is_err)

    dfs(root_func, [root_func], False)

    return "\n".join(mermaid_lines), lock_paths, visited

def main():
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    functions = parse_c_files(root_dir, SEARCH_DIRS)
    roots = ["engine_capture_loop_run", "engine_processing_loop_run", "engine_playback_loop_run"]

    out = {}
    for r in roots:
        mermaid_str, lock_paths, visited = generate_mermaid_and_tree(r, functions)
        out[r] = {
            "mermaid": mermaid_str,
            "lock_paths": lock_paths,
            "nodes_count": len(visited)
        }

    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()
