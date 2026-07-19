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

FORBIDDEN_PRIMITIVES = {
    "pthread_mutex_lock": "POSIX Mutex Lock",
    "malloc": "Heap Memory Allocation",
    "calloc": "Heap Memory Allocation",
    "realloc": "Heap Memory Allocation",
    "free": "Heap Memory Deallocation"
}

def remove_comments_and_strings(code):
    code = re.sub(r'//.*', '', code)
    code = re.sub(r'/\*.*?\*/', '', code, flags=re.DOTALL)
    code = re.sub(r'"([^"\\]|\\.)*"', '""', code)
    return code

def parse_c_files(root_dir, search_dirs):
    functions = {}
    pattern = re.compile(
        r'(?:static\s+|inline\s+|extern\s+)?(?:[a-zA-Z0-9_]+\s+\*?)+([a-zA-Z0-9_]+)\s*\([^)]*\)\s*\{',
        re.DOTALL
    )

    for d in search_dirs:
        dir_path = os.path.join(root_dir, d)
        if not os.path.exists(dir_path):
            continue
        for fname in os.listdir(dir_path):
            if fname.endswith(".c") or fname.endswith(".h"):
                fpath = os.path.join(dir_path, fname)
                with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                
                clean = remove_comments_and_strings(content)

                for m in pattern.finditer(clean):
                    func_name = m.group(1)
                    if func_name in C_KEYWORDS or func_name.startswith("#"):
                        continue
                    
                    start_idx = m.end() - 1 # Position of opening '{'
                    brace_count = 1
                    idx = start_idx + 1
                    while idx < len(clean) and brace_count > 0:
                        if clean[idx] == '{':
                            brace_count += 1
                        elif clean[idx] == '}':
                            brace_count -= 1
                        idx += 1
                    
                    body = clean[start_idx:idx]
                    functions[func_name] = {
                        "file": f"{d}/{fname}",
                        "body": body
                    }
    return functions

def extract_calls_detailed(func_name, body):
    calls_with_mode = [] # list of (func_name, phase: 'steady'|'init'|'error'|'teardown')
    lines = body.splitlines()
    
    in_while_loop = False
    while_brace_depth = 0
    in_error_block = False
    error_brace_depth = 0
    current_brace_depth = 0
    passed_while = False

    for line in lines:
        stripped = line.strip()
        
        # Detect start of steady audio while loop
        if func_name.endswith("_run") and ("while (1)" in stripped or "while ((" in stripped or "while (" in stripped):
            in_while_loop = True
            while_brace_depth = current_brace_depth

        if re.search(r'if\s*\([^)]*(?:err|rerr|perr|failed|STOP_REASON|EOF|!ok|!got_data|!success|!core|!loop|!state|!chunk|reached_eos|uncollected|rate_change|sample_rate_watcher)[^)]*\)', stripped):
            in_error_block = True
            error_brace_depth = current_brace_depth

        calls = re.findall(r'\b([a-zA-Z0-9_]+)\s*\(', stripped)
        for c in calls:
            if c not in C_KEYWORDS and not c.startswith("LOG_") and not c.startswith("logger_"):
                if in_error_block:
                    phase = "error"
                elif func_name.endswith("_run"):
                    if in_while_loop:
                        phase = "steady"
                    elif not passed_while:
                        phase = "init"
                    else:
                        phase = "teardown"
                else:
                    phase = "steady"
                calls_with_mode.append((c, phase))

        current_brace_depth += stripped.count('{') - stripped.count('}')
        
        if in_error_block and current_brace_depth <= error_brace_depth:
            in_error_block = False

        if in_while_loop and current_brace_depth <= while_brace_depth:
            in_while_loop = False
            passed_while = True

    return calls_with_mode

def generate_mermaid_and_tree(root_func, functions):
    visited = set()
    edges = set() # (src, dst, phase)
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
            calls = extract_calls_detailed(curr, info["body"])
            for target, phase in calls:
                edges.add((curr, target, phase))
                if target in functions and target not in visited:
                    queue.append(target)
                elif target in FORBIDDEN_PRIMITIVES:
                    node_files[target] = FORBIDDEN_PRIMITIVES[target]

    mermaid_lines = ["graph TD"]
    for src, dst, phase in sorted(edges):
        label = {
            "steady": "Steady Hot Path",
            "init": "Startup Init Path",
            "error": "Error Handling Path",
            "teardown": "Teardown Exit Path"
        }.get(phase, "Steady Hot Path")
        mermaid_lines.append(f'    {src} -->|{label}| {dst}')

    primitive_paths = []
    def dfs(curr, path, phase_acc):
        if curr in FORBIDDEN_PRIMITIVES:
            primitive_paths.append({"primitive": curr, "path": path, "phase": phase_acc})
            return
        if curr in functions:
            calls = extract_calls_detailed(curr, functions[curr]["body"])
            for target, phase in calls:
                if target not in path:
                    next_phase = phase if phase_acc == "steady" else phase_acc
                    dfs(target, path + [target], next_phase)

    dfs(root_func, [root_func], "steady")

    return "\n".join(mermaid_lines), primitive_paths, visited

def main():
    root_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    functions = parse_c_files(root_dir, SEARCH_DIRS)
    roots = ["engine_capture_loop_run", "engine_processing_loop_run", "engine_playback_loop_run"]

    out = {}
    for r in roots:
        mermaid_str, primitive_paths, visited = generate_mermaid_and_tree(r, functions)
        steady_violations = [p for p in primitive_paths if p["phase"] == "steady"]
        out[r] = {
            "mermaid": mermaid_str,
            "primitive_paths": primitive_paths,
            "steady_violations_count": len(steady_violations),
            "nodes_count": len(visited)
        }

    print(json.dumps(out, indent=2))

if __name__ == "__main__":
    main()
