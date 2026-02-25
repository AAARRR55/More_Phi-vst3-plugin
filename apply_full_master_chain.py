"""
FULL MASTERING CHAIN RE-APPLICATION
Executes all 5 steps of the mastering chain sequentially.
Ports 30001-30005.
"""
import subprocess
import time
import os

scripts = [
    "d:/morphy/master_step1_eq.py",
    "d:/morphy/master_step2_apply.py",
    "d:/morphy/master_step3_imager.py",
    "d:/morphy/master_step4_apply.py",
    "d:/morphy/master_step5_apply.py"
]

def run_script(path):
    print(f"\n>>> Running: {os.path.basename(path)}")
    try:
        result = subprocess.run(["python", path], capture_output=True, text=True)
        if result.returncode == 0:
            print(f"  [SUCCESS] {os.path.basename(path)} applied.")
            # Print a few lines of the output to confirm
            output_lines = result.stdout.splitlines()
            for line in output_lines[-5:]:
                print(f"    {line}")
        else:
            print(f"  [ERROR] {os.path.basename(path)} failed.")
            print(result.stderr)
    except Exception as e:
        print(f"  [EXCEPTION] {e}")

if __name__ == "__main__":
    print("=" * 60)
    print("  RE-APPLYING FULL MASTERING CHAIN (Steps 1-5)")
    print("=" * 60)
    
    start_time = time.time()
    
    for script in scripts:
        run_script(script)
        time.sleep(0.5) # Short grace period between plugins
        
    duration = time.time() - start_time
    print("\n" + "=" * 60)
    print(f"  FINISHED — Entire chain re-applied in {duration:.1f}s")
    print("=" * 60)
