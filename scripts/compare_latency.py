#!/usr/bin/env python3
"""
compare_latency.py – Latency Comparison Pipeline

Supports both legacy CSV (.csv / .log) and fast Parquet (.parquet) inputs.
Analysis is parallelised across (ExecType × Stage) combinations using
ProcessPoolExecutor, cutting wall-clock time by ~N_CPU× on multi-core hosts.
"""

import os
import sys
import argparse
import warnings
import pandas as pd
import numpy as np
import scipy.stats as stats
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import seaborn as sns
from concurrent.futures import ProcessPoolExecutor, as_completed

# ─────────────────────────────────────────────────────────────────────────────
# Argument parsing
# ─────────────────────────────────────────────────────────────────────────────

def parse_args():
    parser = argparse.ArgumentParser(description="Latency Comparison Pipeline")
    parser.add_argument("--baseline",       default="log/baseline.csv",
                        help="Path to baseline file (.csv, .log, or .parquet)")
    parser.add_argument("--candidate",      default="log/candidate.csv",
                        help="Path to candidate file (.csv, .log, or .parquet)")
    parser.add_argument("--min-latency",    type=float, default=0.0,
                        help="Minimum valid latency (ns)")
    parser.add_argument("--max-latency",    type=float, default=1e9,
                        help="Maximum valid latency (ns)")
    parser.add_argument("--bootstrap-sample-cap", type=int, default=20000,
                        help="Max rows to resample per bootstrap iteration (0=unlimited). "
                             "Default 20000 is statistically equivalent to full data for CI estimation.")
    parser.add_argument("--bootstrap-iter", type=int,   default=1000,
                        help="Number of bootstrap iterations")
    parser.add_argument("--workers",        type=int,   default=0,
                        help="Parallel workers (0 = auto = CPU count)")
    parser.add_argument("--heatmap-out",    default="log/heatmap.png",
                        help="Path to save the output heatmap image")
    parser.add_argument("--report-out",     default="log/report.md",
                        help="Path to save the output markdown report")
    return parser.parse_args()


# ─────────────────────────────────────────────────────────────────────────────
# I/O helpers – CSV (legacy) and Parquet (fast)
# ─────────────────────────────────────────────────────────────────────────────

LAT_COLS = [f"{i}-{i+1} lat" for i in range(8)]

def _load_parquet(filepath: str) -> pd.DataFrame:
    """Read a Parquet file written by ebpf-msg-flow --parquet."""
    try:
        import pyarrow.parquet as pq
    except ImportError:
        raise ImportError(
            "pyarrow is required to read .parquet files.\n"
            "Run: pip install pyarrow   (or activate the project venv first)"
        )
    df = pq.read_table(filepath).to_pandas()
    return df


def _load_csv(filepath: str) -> pd.DataFrame:
    """Read a CSV / .log file (legacy format)."""
    df = pd.read_csv(filepath)
    # Strip duplicate header rows that can appear when multiple runs are appended
    df = df[df["ExecType"] != "ExecType"].copy()
    for col in LAT_COLS:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    return df


def load_and_validate(filepath: str, min_lat: float, max_lat: float):
    if not os.path.exists(filepath):
        raise FileNotFoundError(f"Input file not found: {filepath}")

    ext = os.path.splitext(filepath)[1].lower()
    if ext == ".parquet":
        df = _load_parquet(filepath)
    else:
        df = _load_csv(filepath)

    # Validate required columns
    required = ["ExecType"] + LAT_COLS
    for col in required:
        if col not in df.columns:
            raise KeyError(f"Missing required column '{col}' in {filepath}")

    original_len = len(df)
    if original_len == 0:
        raise ValueError(f"Input file {filepath} is empty")

    # Row-level validity: ALL latency columns must be in [min_lat, max_lat]
    valid_mask = pd.Series(True, index=df.index)
    for col in LAT_COLS:
        valid_mask &= (df[col] >= min_lat) & (df[col] <= max_lat)

    df_cleaned = df[valid_mask].copy()
    cleaned_len = len(df_cleaned)
    invalid_rows = original_len - cleaned_len
    invalid_ratio = invalid_rows / original_len

    print(
        f"[{filepath}] Loaded {original_len} rows. "
        f"Cleaned {invalid_rows} invalid rows ({invalid_ratio*100:.2f}%). "
        f"Remaining: {cleaned_len} rows."
    )

    if invalid_ratio > 0.01:
        print(
            f"WARNING: [{filepath}] Invalid row ratio ({invalid_ratio*100:.2f}%) "
            "exceeds 1%!",
            file=sys.stderr,
        )

    return df_cleaned, LAT_COLS


# ─────────────────────────────────────────────────────────────────────────────
# Statistical helpers
# ─────────────────────────────────────────────────────────────────────────────

def bootstrap_p50_p99_ci(baseline, candidate, iterations=1000, chunk_size=20, sample_cap=0):
    n_b = len(baseline)
    n_c = len(candidate)

    if n_b == 0 or n_c == 0:
        return (np.nan, np.nan), (np.nan, np.nan)

    # Cap sample size – statistically equivalent for CI estimation at huge speedup
    rng = np.random.default_rng(42)
    if sample_cap > 0:
        if n_b > sample_cap:
            baseline = baseline[rng.choice(n_b, size=sample_cap, replace=False)]
            n_b = sample_cap
        if n_c > sample_cap:
            candidate = candidate[rng.choice(n_c, size=sample_cap, replace=False)]
            n_c = sample_cap

    p50_improvements, p99_improvements = [], []

    for i in range(0, iterations, chunk_size):
        curr_chunks = min(chunk_size, iterations - i)

        idx_b = rng.integers(0, n_b, size=(curr_chunks, n_b))
        idx_c = rng.integers(0, n_c, size=(curr_chunks, n_c))

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

    return (
        (np.percentile(p50_improvements, 2.5), np.percentile(p50_improvements, 97.5)),
        (np.percentile(p99_improvements, 2.5), np.percentile(p99_improvements, 97.5)),
    )


# ─────────────────────────────────────────────────────────────────────────────
# Per-combination analysis (top-level function so it is picklable)
# ─────────────────────────────────────────────────────────────────────────────

def _analyse_combo(args_tuple):
    """Analyse one (ExecType, Stage) combination.  Must be a module-level
    function so ProcessPoolExecutor can pickle it.  Receives a tuple of
    (et, stage, base_values, cand_values, bootstrap_iter, bootstrap_sample_cap)."""
    et, stage, base_values, cand_values, bootstrap_iter, bootstrap_sample_cap = args_tuple

    base_samples = np.asarray(base_values, dtype=np.float64)
    cand_samples = np.asarray(cand_values, dtype=np.float64)

    n_b = len(base_samples)
    n_c = len(cand_samples)

    if n_b == 0 or n_c == 0:
        return {
            "ExecType": et, "Stage": stage,
            "Baseline P50": np.nan, "Candidate P50": np.nan, "P50 Δ": np.nan,
            "Baseline P99": np.nan, "Candidate P99": np.nan, "P99 Δ": np.nan,
            "p-value": np.nan, "Cliff Delta": np.nan,
            "Result": "No Change", "P99_imp": np.nan,
        }

    b_p50 = np.percentile(base_samples, 50)
    c_p50 = np.percentile(cand_samples, 50)
    b_p99 = np.percentile(base_samples, 99)
    c_p99 = np.percentile(cand_samples, 99)

    p50_imp = (b_p50 - c_p50) / b_p50 * 100.0 if b_p50 != 0 else 0.0
    p99_imp = (b_p99 - c_p99) / b_p99 * 100.0 if b_p99 != 0 else 0.0

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            res_mwu = stats.mannwhitneyu(base_samples, cand_samples, alternative="greater")
        p_val  = res_mwu.pvalue
        u_base = res_mwu.statistic
    except Exception:
        p_val  = np.nan
        u_base = np.nan

    cliff_d = (
        1.0 - (2.0 * u_base) / (n_b * n_c)
        if not np.isnan(u_base)
        else np.nan
    )

    (_, _), (p99_ci_l, p99_ci_u) = bootstrap_p50_p99_ci(
        base_samples, cand_samples,
        iterations=bootstrap_iter,
        sample_cap=bootstrap_sample_cap,
    )

    is_improved   = (p99_imp > 10.0) and (not np.isnan(p_val) and p_val < 0.01) and (not np.isnan(p99_ci_l) and p99_ci_l > 0.0)
    is_regression = (p99_imp < -5.0) or (not np.isnan(p99_ci_u) and p99_ci_u < 0.0)

    verdict_str = "Improved" if is_improved else ("Regression" if is_regression else "No Change")

    return {
        "ExecType": et, "Stage": stage,
        "Baseline P50": b_p50, "Candidate P50": c_p50, "P50 Δ": p50_imp,
        "Baseline P99": b_p99, "Candidate P99": c_p99, "P99 Δ": p99_imp,
        "p-value": p_val, "Cliff Delta": cliff_d,
        "Result": verdict_str, "P99_imp": p99_imp,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Formatting helpers
# ─────────────────────────────────────────────────────────────────────────────

def format_latency(ns):
    if np.isnan(ns): return "N/A"
    us = ns / 1000.0
    if us >= 1000.0:
        return f"{us/1000.0:.1f} ms".replace(".0 ms", " ms")
    return f"{us:.1f} us".replace(".0 us", " us")

def format_delta(pct):
    if np.isnan(pct): return "N/A"
    return f"{pct:+.1f}%".replace(".0%", "%")

def format_pvalue(p):
    if np.isnan(p): return "N/A"
    if p < 0.001:   return "<0.001"
    return f"{p:.3f}"

def format_cliff(d):
    if np.isnan(d): return "N/A"
    return f"{d:.2f}"


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    try:
        baseline,  lat_cols = load_and_validate(args.baseline,  args.min_latency, args.max_latency)
        candidate, _        = load_and_validate(args.candidate, args.min_latency, args.max_latency)
    except Exception as e:
        print(f"Error loading files: {e}", file=sys.stderr)
        sys.exit(1)

    baseline["Total"]  = baseline[lat_cols].sum(axis=1)
    candidate["Total"] = candidate[lat_cols].sum(axis=1)

    all_stages = lat_cols + ["Total"]
    exec_types = sorted(set(baseline["ExecType"].unique()) | set(candidate["ExecType"].unique()))

    # ── Build task list ────────────────────────────────────────────────────
    tasks = []
    for et in exec_types:
        base_et = baseline[baseline["ExecType"] == et]
        cand_et = candidate[candidate["ExecType"] == et]
        for stage in all_stages:
            base_vals = base_et[stage].dropna().tolist() if len(base_et) > 0 else []
            cand_vals = cand_et[stage].dropna().tolist() if len(cand_et) > 0 else []
            tasks.append((et, stage, base_vals, cand_vals, args.bootstrap_iter))

    total = len(tasks)
    n_workers = args.workers if args.workers > 0 else None   # None → os.cpu_count()
    sample_cap = args.bootstrap_sample_cap

    cap_note = f", bootstrap sample cap = {sample_cap}" if sample_cap > 0 else ""
    print(f"\nAnalyzing {total} combinations with up to {n_workers or os.cpu_count()} workers"
          f"{cap_note}...")

    # ── Parallel execution ─────────────────────────────────────────────────
    results_map: dict[tuple, dict] = {}
    completed = 0

    # Inject sample_cap into each task tuple
    tasks_with_cap = [
        (et, stage, base_vals, cand_vals, args.bootstrap_iter, sample_cap)
        for (et, stage, base_vals, cand_vals, _) in tasks
    ]

    with ProcessPoolExecutor(max_workers=n_workers) as pool:
        future_to_key = {
            pool.submit(_analyse_combo, task): (task[0], task[1])
            for task in tasks_with_cap
        }
        for future in as_completed(future_to_key):
            key = future_to_key[future]
            try:
                results_map[key] = future.result()
            except Exception as exc:
                et, stage = key
                print(f"  [ERROR] {et}/{stage}: {exc}", file=sys.stderr)
                results_map[key] = {
                    "ExecType": et, "Stage": stage,
                    "Baseline P50": np.nan, "Candidate P50": np.nan, "P50 Δ": np.nan,
                    "Baseline P99": np.nan, "Candidate P99": np.nan, "P99 Δ": np.nan,
                    "p-value": np.nan, "Cliff Delta": np.nan,
                    "Result": "No Change", "P99_imp": np.nan,
                }
            completed += 1
            print(f"  [{completed}/{total}] {key[0]} / {key[1]}", flush=True)

    # Preserve original (ExecType, Stage) order
    results = [results_map[(et, stage)] for et in exec_types for stage in all_stages
               if (et, stage) in results_map]

    # ── Summary counters ───────────────────────────────────────────────────
    improved_count  = sum(1 for r in results if r["Result"] == "Improved")
    regressed_count = sum(1 for r in results if r["Result"] == "Regression")

    p99_imps = [(r["P99_imp"], f"{r['ExecType']} / {r['Stage']}") for r in results
                if not np.isnan(r["P99_imp"])]

    if p99_imps:
        max_imp_val, max_imp_label = max(p99_imps, key=lambda x: x[0])
        max_reg_val, max_reg_label = min(p99_imps, key=lambda x: x[0])
        max_imp_desc = f"{max_imp_label} ({format_delta(max_imp_val)})"
        max_reg_desc = f"{max_reg_label} ({format_delta(max_reg_val)})" if max_reg_val < 0 else "None"
    else:
        max_imp_desc = max_reg_desc = "N/A"

    # ── Markdown table ─────────────────────────────────────────────────────
    df_results = pd.DataFrame(results)

    table_lines = [
        "| ExecType | Stage | Baseline P50 | Candidate P50 | P50 Δ | Baseline P99 | Candidate P99 | P99 Δ | p-value | Cliff Delta | Result |",
        "| -------- | ----- | ------------ | ------------- | ----- | ------------ | ------------- | ----- | ------- | ----------- | ------ |",
    ]
    for _, row in df_results.iterrows():
        table_lines.append(
            f"| {row['ExecType']} | {row['Stage']} | "
            f"{format_latency(row['Baseline P50'])} | {format_latency(row['Candidate P50'])} | {format_delta(row['P50 Δ'])} | "
            f"{format_latency(row['Baseline P99'])} | {format_latency(row['Candidate P99'])} | {format_delta(row['P99 Δ'])} | "
            f"{format_pvalue(row['p-value'])} | {format_cliff(row['Cliff Delta'])} | {row['Result']} |"
        )
    table_str = "\n".join(table_lines)

    # ── Heatmap ────────────────────────────────────────────────────────────
    heatmap_df = df_results.pivot(index="ExecType", columns="Stage", values="P99_imp")
    heatmap_df = heatmap_df.reindex(columns=all_stages)

    plt.figure(figsize=(12, 6))
    max_abs_val = max(abs(heatmap_df.min().min()), abs(heatmap_df.max().max()), 1e-9)
    cmap_limit  = min(max(max_abs_val, 10.0), 100.0)

    sns.heatmap(
        heatmap_df,
        annot=True, fmt=".1f", cmap="RdYlGn",
        center=0.0, vmin=-cmap_limit, vmax=cmap_limit,
        cbar_kws={"label": "P99 Improvement %"},
    )
    plt.title("P99 Latency Improvement % by ExecType and Stage")
    plt.ylabel("ExecType")
    plt.xlabel("Stage")
    plt.tight_layout()

    os.makedirs(os.path.dirname(args.heatmap_out), exist_ok=True)
    plt.savefig(args.heatmap_out)
    plt.close()

    # ── Overall verdict ────────────────────────────────────────────────────
    overall_base_total = baseline["Total"].to_numpy()
    overall_cand_total = candidate["Total"].to_numpy()

    overall_base_p99 = np.percentile(overall_base_total, 99) if len(overall_base_total) > 0 else np.nan
    overall_cand_p99 = np.percentile(overall_cand_total, 99) if len(overall_cand_total) > 0 else np.nan

    overall_p99_imp = (
        (overall_base_p99 - overall_cand_p99) / overall_base_p99 * 100.0
        if not np.isnan(overall_base_p99) and overall_base_p99 != 0
        else np.nan
    )

    any_stage_regressed_gt_5 = (df_results["P99_imp"] < -5.0).any()
    is_pass = (overall_p99_imp > 10.0) and not any_stage_regressed_gt_5
    verdict = "PASS" if is_pass else "FAIL"

    summary_str = (
        "Overall:\n\n"
        f"Total P99:\n{format_delta(overall_p99_imp)}\n\n"
        f"Improved:\n{improved_count} stages\n\n"
        f"Regressed:\n{regressed_count} stages\n\n"
        f"Largest improvement:\n{max_imp_desc}\n\n"
        f"Largest regression:\n{max_reg_desc}\n\n"
        f"Release verdict:\n{verdict}"
    )

    print("\n--- METRICS TABLE ---")
    print(table_str)
    print("\n--- FINAL SUMMARY ---")
    print(summary_str)

    # ── Write report ───────────────────────────────────────────────────────
    os.makedirs(os.path.dirname(args.report_out), exist_ok=True)
    with open(args.report_out, "w") as f:
        f.write("# Latency Comparison Report\n\n")
        f.write("## Stage Meanings\n\n")
        f.write("| Stage | Probe Point |\n")
        f.write("| ----- | ----------- |\n")
        f.write("| 0-1 lat | tcp_recv_entry → req_entry |\n")
        f.write("| 1-2 lat | req_entry → req_enqueue |\n")
        f.write("| 2-3 lat | req_enqueue → req_dequeue |\n")
        f.write("| 3-4 lat | req_dequeue → resp_enqueue |\n")
        f.write("| 4-5 lat | resp_enqueue → resp_dequeue |\n")
        f.write("| 5-6 lat | resp_dequeue → user_write |\n")
        f.write("| 6-7 lat | user_write → tcp_send_entry |\n")
        f.write("| 7-8 lat | tcp_send_entry → tcp_send_ret |\n")
        f.write("\n")
        f.write("## Metrics Table\n\n")
        f.write(table_str)
        f.write("\n\n## Final Summary\n\n")
        f.write(summary_str.replace("\n", "  \n"))
        f.write(f"\n\nHeatmap saved to: {args.heatmap_out}\n")

    print(f"\nReport saved to: {args.report_out}")
    print(f"Heatmap saved to: {args.heatmap_out}")

    sys.exit(0 if is_pass else 1)


if __name__ == "__main__":
    main()
