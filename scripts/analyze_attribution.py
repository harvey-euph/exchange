#!/usr/bin/env python3
import os
import sys
import pandas as pd
import numpy as np
from scipy import stats

def analyze():
    csv_path = "log/latency_attribution.csv"
    if not os.path.exists(csv_path):
        print(f"Error: CSV file not found at '{csv_path}'.")
        print("Please run the eBPF tracer first: 'sudo ./app/perf/ebpf/lat-tracer' to collect data.")
        sys.exit(1)

    print(f"Loading '{csv_path}'...")
    df = pd.read_csv(csv_path)

    # Rename latency column if needed
    lat_col = 'latency(ns)'
    if lat_col not in df.columns:
        # Fallback in case of column naming issues
        for col in df.columns:
            if 'latency' in col:
                lat_col = col
                break

    groups = df.groupby(['execType', 'stage'])
    
    # We will output a clean summary report
    print("\n" + "="*80)
    print("  STAGE-BASED LATENCY ATTRIBUTION ANALYSIS REPORT")
    print("="*80)
    
    metrics = [
        'page_faults', 'ctx_switches', 'runqueue_delay', 
        'llc_miss', 'l1d_miss', 'l1i_miss', 'dtlb_miss', 
        'cpu_migrated', 'IPC', 'branch_miss'
    ]

    # Filter out columns that don't exist in CSV
    metrics = [m for m in metrics if m in df.columns]

    report_rows = []

    stage_names = {
        0: "0.Recv->ReqEntry",
        1: "1.CM-Processing",
        2: "2.Request-Queue",
        3: "3.MatchingEngine",
        4: "4.Response-Queue",
        5: "5.CM-RespHandler",
        6: "6.CM-DB-Write",
        7: "7.TCP-Send"
    }

    for (exec_type, stage), group in groups:
        if len(group) < 5:
            continue  # Skip groups with too few samples for correlation
            
        lat_data = group[lat_col]
        lat_mean_us = lat_data.mean() / 1000.0
        lat_p95_us = np.percentile(lat_data, 95) / 1000.0
        lat_p99_us = np.percentile(lat_data, 99) / 1000.0
        
        # Calculate Spearman correlation for each metric
        correlations = {}
        for m in metrics:
            # If the metric is constant in this group (e.g. 0), correlation is undefined
            if group[m].nunique() <= 1:
                continue
            
            # Spearman correlation is robust to non-linear latency spikes
            coef, _ = stats.spearmanr(lat_data, group[m])
            if not np.isnan(coef):
                correlations[m] = coef
                
        # Find highest absolute correlation
        highest_metric = "None"
        highest_coef = 0.0
        diagnosis = "Normal execution / Unknown"

        if correlations:
            highest_metric = max(correlations, key=lambda k: abs(correlations[k]))
            highest_coef = correlations[highest_metric]
            
            # Formulate Diagnosis / Attribution based on the highest correlated factor
            if abs(highest_coef) > 0.15:  # Consider correlation significant if > 0.15
                if highest_metric == 'page_faults':
                    diagnosis = "Page Fault overhead (Memory allocation/paging)"
                elif highest_metric == 'runqueue_delay':
                    diagnosis = "Scheduler Wait / CPU Contention"
                elif highest_metric == 'ctx_switches':
                    diagnosis = "Context Switch Overhead"
                elif highest_metric == 'llc_miss':
                    diagnosis = "L3 Cache Misses (Memory Access Stall)"
                elif highest_metric in ['l1d_miss', 'l1i_miss']:
                    diagnosis = "L1 Cache Misses (Cache Locality Stall)"
                elif highest_metric == 'dtlb_miss':
                    diagnosis = "dTLB Misses (Page Table Walk / Translation Stall)"
                elif highest_metric == 'cpu_migrated':
                    diagnosis = "CPU Core Migration Overhead"
                elif highest_metric == 'IPC':
                    # Negative correlation with IPC means lower IPC -> higher latency (Stall)
                    if highest_coef < 0:
                        diagnosis = "Instruction Pipeline Stall (Low IPC)"
                    else:
                        diagnosis = "CPU Heavy Processing (High IPC)"
                elif highest_metric == 'branch_miss':
                    diagnosis = "Branch Misprediction Stall"
            else:
                diagnosis = "No single strong hardware factor (Uniform latency distribution)"

        stage_name = stage_names.get(stage, f"Stage {stage}")
        report_rows.append({
            'exec_type': exec_type,
            'stage': stage_name,
            'count': len(group),
            'mean_us': lat_mean_us,
            'p95_us': lat_p95_us,
            'p99_us': lat_p99_us,
            'highest_factor': highest_metric,
            'coef': highest_coef,
            'diagnosis': diagnosis
        })

    # Print Table
    print(f"| {'ExecType':<10} | {'Stage':<18} | {'Count':<7} | {'Mean(us)':<8} | {'P95(us)':<8} | {'P99(us)':<8} | {'Highest Correlated Factor':<25} | {'Coef':<7} | {'Primary Root Cause':<42} |")
    print("|" + "-"*12 + "|" + "-"*20 + "|" + "-"*9 + "|" + "-"*10 + "|" + "-"*10 + "|" + "-"*10 + "|" + "-"*27 + "|" + "-"*9 + "|" + "-"*44 + "|")
    
    for r in sorted(report_rows, key=lambda x: (x['exec_type'], x['stage'])):
        print(f"| {r['exec_type']:<10} | {r['stage']:<18} | {r['count']:<7} | {r['mean_us']:>8.2f} | {r['p95_us']:>8.2f} | {r['p99_us']:>8.2f} | {r['highest_factor']:<25} | {r['coef']:>+7.3f} | {r['diagnosis']:<42} |")

    print("="*80)
    print("\nReport complete. All correlation analysis performed using Spearman rank correlation coefficients.")

if __name__ == "__main__":
    analyze()
