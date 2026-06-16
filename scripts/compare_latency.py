#!/usr/bin/env python3
import os
import sys
import argparse
import pandas as pd
import numpy as np
import scipy.stats as stats
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import seaborn as sns

def parse_args():
    parser = argparse.ArgumentParser(description="Latency Comparison Pipeline")
    parser.add_argument("--baseline", default="log/baseline.csv", help="Path to baseline CSV file")
    parser.add_argument("--candidate", default="log/candidate.csv", help="Path to candidate CSV file")
    parser.add_argument("--min-latency", type=float, default=0.0, help="Minimum valid latency (ns)")
    parser.add_argument("--max-latency", type=float, default=1e9, help="Maximum valid latency (ns)")
    parser.add_argument("--bootstrap-iter", type=int, default=1000, help="Number of bootstrap iterations")
    parser.add_argument("--heatmap-out", default="log/heatmap.png", help="Path to save the output heatmap image")
    parser.add_argument("--report-out", default="log/report.md", help="Path to save the output markdown report")
    return parser.parse_args()

def load_and_validate(filepath, min_lat, max_lat):
    if not os.path.exists(filepath):
        raise FileNotFoundError(f"Input file not found: {filepath}")
    
    df = pd.read_csv(filepath)
    
    # Required columns
    required = ["ExecType"]
    lat_cols = [f"{i}-{i+1} lat" for i in range(8)]
    for col in required + lat_cols:
        if col not in df.columns:
            raise KeyError(f"Missing required column '{col}' in {filepath}")
            
    # Clean data: remove duplicate header rows (which can happen if multiple runs are appended together)
    df = df[df["ExecType"] != "ExecType"].copy()
    
    # Ensure latency columns are numeric
    for col in lat_cols:
        df[col] = pd.to_numeric(df[col], errors="coerce")
        
    original_len = len(df)
    if original_len == 0:
        raise ValueError(f"Input file {filepath} is empty")
        
    # Check bounds for all latency columns
    # A row is valid if ALL latency columns are within [min_lat, max_lat]
    valid_mask = pd.Series(True, index=df.index)
    for col in lat_cols:
        valid_mask &= (df[col] >= min_lat) & (df[col] <= max_lat)
        
    df_cleaned = df[valid_mask].copy()
    cleaned_len = len(df_cleaned)
    
    invalid_rows = original_len - cleaned_len
    invalid_ratio = invalid_rows / original_len
    
    print(f"[{filepath}] Loaded {original_len} rows. Cleaned {invalid_rows} invalid rows ({invalid_ratio*100:.2f}%). Remaining: {cleaned_len} rows.")
    
    if invalid_ratio > 0.01:
        print(f"WARNING: [{filepath}] Invalid row ratio ({invalid_ratio*100:.2f}%) exceeds 1%!", file=sys.stderr)
        
    return df_cleaned, lat_cols

# Bootstrapping helper
def bootstrap_p50_p99_ci(baseline, candidate, iterations=1000, chunk_size=20):
    n_b = len(baseline)
    n_c = len(candidate)
    
    if n_b == 0 or n_c == 0:
        return (np.nan, np.nan), (np.nan, np.nan)
        
    p50_improvements = []
    p99_improvements = []
    
    # Set random seed for reproducibility
    np.random.seed(42)
    
    for i in range(0, iterations, chunk_size):
        curr_chunks = min(chunk_size, iterations - i)
        
        idx_b = np.random.randint(0, n_b, size=(curr_chunks, n_b))
        idx_c = np.random.randint(0, n_c, size=(curr_chunks, n_c))
        
        samples_b = baseline[idx_b]
        samples_c = candidate[idx_c]
        
        p_b = np.percentile(samples_b, [50, 99], axis=1)
        p_c = np.percentile(samples_c, [50, 99], axis=1)
        
        p50_b, p99_b = p_b[0], p_b[1]
        p50_c, p99_c = p_c[0], p_c[1]
        
        p50_imp = (p50_b - p50_c) / np.where(p50_b == 0, 1e-9, p50_b) * 100.0
        p99_imp = (p99_b - p99_c) / np.where(p99_b == 0, 1e-9, p99_b) * 100.0
        
        p50_improvements.append(p50_imp)
        p99_improvements.append(p99_imp)
        
    p50_improvements = np.concatenate(p50_improvements)
    p99_improvements = np.concatenate(p99_improvements)
    
    p50_lower = np.percentile(p50_improvements, 2.5)
    p50_upper = np.percentile(p50_improvements, 97.5)
    p99_lower = np.percentile(p99_improvements, 2.5)
    p99_upper = np.percentile(p99_improvements, 97.5)
    
    return (p50_lower, p50_upper), (p99_lower, p99_upper)

def format_latency(ns):
    if np.isnan(ns):
        return "N/A"
    us = ns / 1000.0
    if us >= 1000.0:
        val = us / 1000.0
        return f"{val:.1f} ms".replace(".0 ms", " ms")
    else:
        return f"{us:.1f} us".replace(".0 us", " us")

def format_delta(pct):
    if np.isnan(pct):
        return "N/A"
    return f"{pct:+.1f}%".replace(".0%", "%")

def format_pvalue(p):
    if np.isnan(p):
        return "N/A"
    if p < 0.001:
        return "<0.001"
    return f"{p:.3f}"

def format_cliff(d):
    if np.isnan(d):
        return "N/A"
    return f"{d:.2f}"

def main():
    args = parse_args()
    
    # Load and clean baseline and candidate
    try:
        baseline, lat_cols = load_and_validate(args.baseline, args.min_latency, args.max_latency)
        candidate, _ = load_and_validate(args.candidate, args.min_latency, args.max_latency)
    except Exception as e:
        print(f"Error loading files: {e}", file=sys.stderr)
        sys.exit(1)
        
    # Step 3: Add Total Latency
    baseline["Total"] = baseline[lat_cols].sum(axis=1)
    candidate["Total"] = candidate[lat_cols].sum(axis=1)
    
    all_stages = lat_cols + ["Total"]
    
    # Step 4: Group By ExecType
    exec_types = sorted(list(set(baseline["ExecType"].unique()) | set(candidate["ExecType"].unique())))
    
    results = []
    
    # Keep track of metrics for final summary
    improved_count = 0
    regressed_count = 0
    
    max_imp_val = -float('inf')
    max_imp_desc = "N/A"
    
    max_reg_val = float('inf')  # Most negative improvement is the largest regression
    max_reg_desc = "N/A"
    
    print("\nAnalyzing datasets...")
    total_combinations = len(exec_types) * len(all_stages)
    current_idx = 0
    for et in exec_types:
        base_et = baseline[baseline["ExecType"] == et]
        cand_et = candidate[candidate["ExecType"] == et]
        
        for stage in all_stages:
            current_idx += 1
            print(f"[Progress] Analyzing {et} / {stage} ({current_idx}/{total_combinations})...", flush=True)
            # Get samples
            base_samples = base_et[stage].to_numpy() if len(base_et) > 0 else np.array([])
            cand_samples = cand_et[stage].to_numpy() if len(cand_et) > 0 else np.array([])
            
            n_b = len(base_samples)
            n_c = len(cand_samples)
            
            if n_b == 0 or n_c == 0:
                results.append({
                    "ExecType": et,
                    "Stage": stage,
                    "Baseline P50": np.nan,
                    "Candidate P50": np.nan,
                    "P50 Δ": np.nan,
                    "Baseline P99": np.nan,
                    "Candidate P99": np.nan,
                    "P99 Δ": np.nan,
                    "p-value": np.nan,
                    "Cliff Delta": np.nan,
                    "Result": "No Change",
                    "P99_imp": np.nan
                })
                continue
                
            # Step 5: Compute metrics
            b_p50 = np.percentile(base_samples, 50)
            c_p50 = np.percentile(cand_samples, 50)
            
            b_p99 = np.percentile(base_samples, 99)
            c_p99 = np.percentile(cand_samples, 99)
            
            # Step 6: Improvement
            p50_imp = (b_p50 - c_p50) / b_p50 * 100.0 if b_p50 != 0 else 0.0
            p99_imp = (b_p99 - c_p99) / b_p99 * 100.0 if b_p99 != 0 else 0.0
            
            # Step 7: Statistical Test
            # H0: base <= cand (no improvement)
            # H1: base > cand (improvement, i.e., candidate is faster)
            try:
                res_mwu = stats.mannwhitneyu(base_samples, cand_samples, alternative="greater")
                p_val = res_mwu.pvalue
                u_base = res_mwu.statistic
            except Exception as e:
                p_val = np.nan
                u_base = np.nan
                
            # Step 8: Effect Size (Cliff's Delta)
            if not np.isnan(u_base):
                cliff_d = 1.0 - (2.0 * u_base) / (n_b * n_c)
            else:
                cliff_d = np.nan
                
            # Step 9: Bootstrap Confidence Interval
            (p50_ci_l, p50_ci_u), (p99_ci_l, p99_ci_u) = bootstrap_p50_p99_ci(
                base_samples, cand_samples, iterations=args.bootstrap_iter
            )
            
            # Step 10: Classification
            is_improved = (p99_imp > 10.0) and (not np.isnan(p_val) and p_val < 0.01) and (not np.isnan(p99_ci_l) and p99_ci_l > 0.0)
            is_regression = (p99_imp < -5.0) or (not np.isnan(p99_ci_u) and p99_ci_u < 0.0)
            
            if is_improved:
                verdict_str = "Improved"
                improved_count += 1
            elif is_regression:
                verdict_str = "Regression"
                regressed_count += 1
            else:
                verdict_str = "No Change"
                
            results.append({
                "ExecType": et,
                "Stage": stage,
                "Baseline P50": b_p50,
                "Candidate P50": c_p50,
                "P50 Δ": p50_imp,
                "Baseline P99": b_p99,
                "Candidate P99": c_p99,
                "P99 Δ": p99_imp,
                "p-value": p_val,
                "Cliff Delta": cliff_d,
                "Result": verdict_str,
                "P99_imp": p99_imp
            })
            
            # Track largest improvement / regression
            if p99_imp > max_imp_val:
                max_imp_val = p99_imp
                max_imp_desc = f"{et} / {stage} ({format_delta(p99_imp)})"
                
            if p99_imp < max_reg_val:
                max_reg_val = p99_imp
                max_reg_desc = f"{et} / {stage} ({format_delta(p99_imp)})"
                
    # Step 11: Output Table (Markdown format)
    df_results = pd.DataFrame(results)
    
    # Generate markdown table lines
    table_lines = []
    table_lines.append("| ExecType | Stage | Baseline P50 | Candidate P50 | P50 Δ | Baseline P99 | Candidate P99 | P99 Δ | p-value | Cliff Delta | Result |")
    table_lines.append("| -------- | ----- | ------------ | ------------- | ----- | ------------ | ------------- | ----- | ------- | ----------- | ------ |")
    for _, row in df_results.iterrows():
        line = (
            f"| {row['ExecType']} | {row['Stage']} | "
            f"{format_latency(row['Baseline P50'])} | {format_latency(row['Candidate P50'])} | {format_delta(row['P50 Δ'])} | "
            f"{format_latency(row['Baseline P99'])} | {format_latency(row['Candidate P99'])} | {format_delta(row['P99 Δ'])} | "
            f"{format_pvalue(row['p-value'])} | {format_cliff(row['Cliff Delta'])} | {row['Result']} |"
        )
        table_lines.append(line)
        
    table_str = "\n".join(table_lines)
    
    # Step 12: Heatmap
    heatmap_df = df_results.pivot(index="ExecType", columns="Stage", values="P99_imp")
    heatmap_df = heatmap_df[all_stages]
    
    plt.figure(figsize=(12, 6))
    max_abs_val = max(abs(heatmap_df.min().min()), abs(heatmap_df.max().max()), 1e-9)
    cmap_limit = min(max(max_abs_val, 10.0), 100.0)
    
    sns.heatmap(
        heatmap_df,
        annot=True,
        fmt=".1f",
        cmap="RdYlGn",
        center=0.0,
        vmin=-cmap_limit,
        vmax=cmap_limit,
        cbar_kws={'label': 'P99 Improvement %'}
    )
    plt.title("P99 Latency Improvement % by ExecType and Stage")
    plt.ylabel("ExecType")
    plt.xlabel("Stage")
    plt.tight_layout()
    
    os.makedirs(os.path.dirname(args.heatmap_out), exist_ok=True)
    plt.savefig(args.heatmap_out)
    plt.close()
    
    # Step 13 & 14: Final Summary & Pass/Fail Rule
    overall_base_total = baseline["Total"].to_numpy()
    overall_cand_total = candidate["Total"].to_numpy()
    
    overall_base_p99 = np.percentile(overall_base_total, 99) if len(overall_base_total) > 0 else np.nan
    overall_cand_p99 = np.percentile(overall_cand_total, 99) if len(overall_cand_total) > 0 else np.nan
    
    if not np.isnan(overall_base_p99) and overall_base_p99 != 0:
        overall_p99_imp = (overall_base_p99 - overall_cand_p99) / overall_base_p99 * 100.0
    else:
        overall_p99_imp = np.nan
        
    any_stage_regressed_gt_5 = (df_results["P99_imp"] < -5.0).any()
    
    is_pass = (overall_p99_imp > 10.0) and not any_stage_regressed_gt_5
    verdict = "PASS" if is_pass else "FAIL"
    
    summary_str = (
        "Overall:\n\n"
        f"Total P99:\n{format_delta(overall_p99_imp)}\n\n"
        f"Improved:\n{improved_count} stages\n\n"
        f"Regressed:\n{regressed_count} stages\n\n"
        f"Largest improvement:\n{max_imp_desc}\n\n"
        f"Largest regression:\n{max_reg_desc if max_reg_val < 0.0 else 'None'}\n\n"
        f"Release verdict:\n{verdict}"
    )
    
    print("\n--- METRICS TABLE ---")
    print(table_str)
    print("\n--- FINAL SUMMARY ---")
    print(summary_str)
    
    os.makedirs(os.path.dirname(args.report_out), exist_ok=True)
    with open(args.report_out, "w") as f:
        f.write("# Latency Comparison Report\n\n")
        f.write("## Metrics Table\n\n")
        f.write(table_str)
        f.write("\n\n## Final Summary\n\n")
        f.write(summary_str.replace("\n\n", "\n\n").replace("\n", "  \n"))
        f.write(f"\n\nHeatmap saved to: {args.heatmap_out}\n")
        
    print(f"\nReport saved to: {args.report_out}")
    print(f"Heatmap saved to: {args.heatmap_out}")
    
    # Exit with non-zero code if release verdict is FAIL
    sys.exit(0 if is_pass else 1)

if __name__ == "__main__":
    main()
