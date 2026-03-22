# /// script
# requires-python = ">=3.12"
# dependencies = ["matplotlib", "pandas"]
# ///
"""
Plot cache experiment results from results.csv.

Generates cache_exp.svg with:
  - Left panel:  sequential vs shuffled timing comparison
  - Right panel: cache miss rate by list size (if perf data available)
  - Annotated cache size boundaries from sysinfo.txt
"""

import pandas as pd
import matplotlib.pyplot as plt
import re
import sys
from pathlib import Path


def parse_sysinfo(path: Path) -> dict:
    """Parse sysinfo.txt into a dict with CPU name and cache sizes."""
    info = {"cpu": "Unknown", "caches": {}}
    if not path.exists():
        return info
    text = path.read_text()
    for line in text.splitlines():
        if line.startswith("cpu:"):
            info["cpu"] = line.split(":", 1)[1].strip()
        # Parse cache lines like "L1d cache: 48 KiB (2 instances)"
        m = re.match(r"(L\d\w?)\s+cache:\s+(\d+)\s*(KiB|MiB)", line, re.I)
        if m:
            name = m.group(1)
            size_val = int(m.group(2))
            unit = m.group(3)
            size_kb = size_val * 1024 if unit == "MiB" else size_val
            info["caches"][name] = size_kb
    return info


def add_cache_boundaries(ax, caches: dict, node_size: int = 16):
    """Add vertical lines at cache size boundaries."""
    colors = {"L1d": "#4CAF50", "L1i": "#8BC34A",
              "L2": "#FF9800", "L3": "#F44336"}
    for name, size_kb in caches.items():
        if name == "L1i":
            continue  # instruction cache not relevant
        nodes_fit = (size_kb * 1024) // node_size
        color = colors.get(name, "gray")
        ax.axvline(x=nodes_fit, color=color, linestyle="--",
                   alpha=0.5, linewidth=1)
        ax.text(nodes_fit, ax.get_ylim()[1], f" {name}\n {size_kb}K",
                fontsize=7, color=color, va="top", ha="left")


def main():
    base = Path(__file__).parent
    csv_path = base / "results.csv"
    if not csv_path.exists():
        print(f"Error: {csv_path} not found. Run: make bench")
        sys.exit(1)

    df = pd.read_csv(csv_path)
    df["time_ns_per_iter"] = pd.to_numeric(df["time_ns_per_iter"],
                                           errors="coerce")
    df["cache_misses"] = pd.to_numeric(df["cache_misses"], errors="coerce")
    df["cache_refs"] = pd.to_numeric(df["cache_refs"], errors="coerce")
    df["iters"] = pd.to_numeric(df["iters"], errors="coerce")

    sysinfo = parse_sysinfo(base / "sysinfo.txt")
    has_perf = df["cache_refs"].notna().any()

    ncols = 3 if has_perf else 2
    fig, axes = plt.subplots(1, ncols, figsize=(6 * ncols, 5.5))

    algo_style = {
        "fast":   ("fast-slow pointer",        "o", "#2196F3"),
        "single": ("single pointer (two-pass)", "s", "#F44336"),
    }

    # --- Panel 1: Time ratio (two-pass / fast-slow) ---
    ax = axes[0]
    for mode, label, marker, color in [
        ("shuf", "shuffled", "o", "#9C27B0"),
        ("seq",  "sequential", "^", "#607D8B"),
    ]:
        piv = df[df["mode"] == mode].pivot(
            index="size", columns="algo", values="time_ns_per_iter")
        if "fast" in piv.columns and "single" in piv.columns:
            piv["ratio"] = piv["single"] / piv["fast"]
            d = piv["ratio"].dropna()
            ax.plot(d.index, d.values, marker=marker, color=color,
                    label=label, linewidth=2, markersize=5)
    ax.axhline(y=1.0, color="gray", ls="-", alpha=0.3, lw=1)
    ax.axhline(y=1.5, color="gray", ls=":", alpha=0.5, lw=1)
    ax.text(df["size"].min(), 1.52, " 1.5x", fontsize=7, color="gray",
            va="bottom")
    ax.set_xscale("log")
    ax.set_xlabel("List size (nodes)")
    ax.set_ylabel("Time ratio (two-pass / fast-slow)")
    ax.set_title("Performance ratio\n(>1 = fast-slow wins)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    add_cache_boundaries(ax, sysinfo["caches"])

    # --- Panel 2: Shuffled timing ---
    ax = axes[1]
    for algo, (label, marker, color) in algo_style.items():
        d = df[(df["mode"] == "shuf") & (df["algo"] == algo)]
        ax.plot(d["size"], d["time_ns_per_iter"], marker=marker, color=color,
                label=label, linewidth=2, markersize=5)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("List size (nodes)")
    ax.set_ylabel("Time per iteration (ns)")
    ax.set_title("Shuffled links\n(poor spatial locality)")
    ax.legend(fontsize=8)
    ax.grid(True, alpha=0.3)
    add_cache_boundaries(ax, sysinfo["caches"])

    # --- Panel 3: Cache miss rate (if perf data available) ---
    if has_perf:
        ax = axes[2]
        shuf_perf = df[df["mode"] == "shuf"].copy()
        shuf_perf["miss_rate"] = (shuf_perf["cache_misses"]
                                  / shuf_perf["cache_refs"] * 100)
        for algo, (label, marker, color) in algo_style.items():
            d = shuf_perf[shuf_perf["algo"] == algo]
            d = d.dropna(subset=["miss_rate"])
            if not d.empty:
                ax.plot(d["size"], d["miss_rate"], marker=marker, color=color,
                        label=label, linewidth=2, markersize=5)
        ax.set_xscale("log")
        ax.set_xlabel("List size (nodes)")
        ax.set_ylabel("Cache miss rate (%)")
        ax.set_title("Cache miss rate (shuffled)\n(perf stat)")
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)
        ax.set_ylim(-5, 105)
        add_cache_boundaries(ax, sysinfo["caches"])

    plt.suptitle(
        f"Cache Behavior: Fast-Slow vs Single Pointer — {sysinfo['cpu']}",
        fontsize=11, fontweight="bold")
    plt.tight_layout()

    out_path = base / "cache_exp.svg"
    plt.savefig(out_path, bbox_inches="tight")
    print(f"Chart saved to {out_path}")
    plt.close()


if __name__ == "__main__":
    main()
