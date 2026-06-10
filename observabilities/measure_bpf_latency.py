#!/usr/bin/env python3
import sys
import json
import subprocess
import time

def get_bpftool_progs():
    res = subprocess.run(["sudo", "bpftool", "-j", "prog", "show"], capture_output=True, text=True)
    if res.returncode != 0:
        print("Error running bpftool. Make sure bpftool is installed and you have sudo access.", file=sys.stderr)
        sys.exit(1)
    return json.loads(res.stdout)

# Enable stats
subprocess.run(["sudo", "sysctl", "-w", "kernel.bpf_stats_enabled=1"], capture_output=True)

try:
    print("Enabling BPF stats tracking...")
    print("Collecting BPF execution baseline (waiting 5 seconds)...")
    
    # Baseline
    progs_base = get_bpftool_progs()
    time.sleep(5)
    progs_new = get_bpftool_progs()
finally:
    # Disable stats to prevent runtime overhead
    print("Disabling BPF stats tracking...")
    subprocess.run(["sudo", "sysctl", "-w", "kernel.bpf_stats_enabled=0"], capture_output=True)

# Parse stats
base_map = {p['id']: p for p in progs_base if 'run_cnt' in p}
new_map = {p['id']: p for p in progs_new if 'run_cnt' in p}

results = []
for pid, p_new in new_map.items():
    if pid in base_map:
        p_base = base_map[pid]
        dcnt = p_new['run_cnt'] - p_base['run_cnt']
        dtime = p_new['run_time_ns'] - p_base['run_time_ns']
        if dcnt > 0:
            avg_ns = dtime / dcnt
            results.append({
                'id': pid,
                'name': p_new.get('name', 'unknown'),
                'type': p_new.get('type', 'unknown'),
                'cnt': dcnt,
                'time_ms': dtime / 1e6,
                'avg_ns': avg_ns
            })

# Print results
print("\n" + "="*85)
print(f"{'ID':<6} | {'Program Name':<25} | {'Type':<15} | {'Count (5s)':<12} | {'Avg Latency (ns)':<18}")
print("-"*85)
for r in sorted(results, key=lambda x: x['id']):
    # Filter for our latency tracer programs
    if "tcp_" in r['name'] or "lat_" in r['name']:
        print(f"{r['id']:<6} | {r['name']:<25} | {r['type']:<15} | {r['cnt']:<12} | {r['avg_ns']:<18.2f}")
print("="*85)
