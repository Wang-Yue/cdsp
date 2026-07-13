import sys
import os
import subprocess
import concurrent.futures
import time

def run_one_case(bin_path, case_name=None):
    bin_name = os.path.basename(bin_path)
    display_name = f"{bin_name}:{case_name}" if case_name else bin_name
    print(f"[START] {display_name}", flush=True)
    
    cmd = [bin_path]
    if case_name:
        cmd.extend(["--run", case_name])
        
    start_time = time.time()
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8")
    elapsed = time.time() - start_time
    
    return {
        "bin": bin_path,
        "case": case_name,
        "stdout": result.stdout,
        "returncode": result.returncode,
        "elapsed": elapsed
    }

def main():
    if len(sys.argv) < 2:
        print("Usage: run_tests.py <test_binary1> <test_binary2> ...")
        sys.exit(1)
        
    bins = sys.argv[1:]
    
    # We can use os.cpu_count() or a reasonable number.
    max_workers = os.cpu_count() or 4
    
    all_tasks = []
    
    # 1. Discover all test cases
    for b in bins:
        try:
            res = subprocess.run([b, "--list"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", timeout=5)
            if res.returncode == 0:
                cases = [line.strip() for line in res.stdout.splitlines() if line.strip()]
                if cases:
                    for c in cases:
                        all_tasks.append((b, c))
                    continue
        except Exception:
            pass
        # Fallback: run the binary as a single task
        all_tasks.append((b, None))
        
    failed = False
    
    print(f"Running {len(all_tasks)} test cases in parallel (workers={max_workers})...\n", flush=True)
    
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        # Submit all tasks
        future_to_task = {executor.submit(run_one_case, b, c): (b, c) for b, c in all_tasks}
        
        for future in concurrent.futures.as_completed(future_to_task):
            res = future.result()
            bin_name = os.path.basename(res["bin"])
            case_name = res["case"]
            display_name = f"{bin_name}:{case_name}" if case_name else bin_name
            
            # Print test output block
            print(f"\n=== {display_name} (elapsed: {res['elapsed']:.2f}s) ===", flush=True)
            if res["stdout"]:
                print(res["stdout"].strip(), flush=True)
            
            if res["returncode"] != 0:
                print(f"❌ {display_name} failed with exit code {res['returncode']}\n", flush=True)
                failed = True
            else:
                print(f"✅ {display_name} passed\n", flush=True)
                
    if failed:
        print("❌ Some tests failed!", flush=True)
        sys.exit(1)
    else:
        print("✅ All C unit tests passed!", flush=True)
        sys.exit(0)

if __name__ == "__main__":
    main()
