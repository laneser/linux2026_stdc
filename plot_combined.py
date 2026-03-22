# /// script
# requires-python = ">=3.12"
# dependencies = ["matplotlib", "pandas"]
# ///
"""
Plot combined cache experiment results from multiple machines.

Reads results_*.csv and sysinfo_*.txt files, generates one SVG per machine
plus a combined comparison SVG.

Usage:
  uv run plot_combined.py            # plot all available result files
  uv run plot_combined.py dir1 dir2  # specify directories
"""

import pandas as pd
import matplotlib.pyplot as plt
import re
import sys
from pathlib import Path


def parse_sysinfo(path: Path) -> dict:
    info = {"cpu": "Unknown", "hostname": "unknown", "caches": {}}
    if not path.exists():
        return info
    for line in path.read_text().splitlines():
        if line.startswith("cpu:"):
            info["cpu"] = line.split(":", 1)[1].strip()
        if line.startswith("hostname:"):
            info["hostname"] = line.split(":", 1)[1].strip()
        m = re.match(r"(L\d\w?)\s+cache:\s+(\d+)\s*(KiB|MiB)", line, re.I)
        if m:
            name, val, unit = m.group(1), int(m.group(2)), m.group(3)
            info["caches"][name] = val * 1024 if unit == "MiB" else val
    return info


def add_cache_lines(ax, caches: dict, node_size: int = 16):
    colors = {"L1d": "#4CAF50", "L2": "#FF9800", "L3": "#F44336"}
    for name, size_kb in caches.items():
        if name == "L1i":
            continue
        nodes = (size_kb * 1024) // node_size
        c = colors.get(name, "gray")
        ax.axvline(x=nodes, color=c, ls="--", alpha=0.5, lw=1)
        ax.text(nodes * 1.1, ax.get_ylim()[1] * 0.7,
                f"{name}\n{size_kb}K", fontsize=7, color=c, va="top")


ALGO_STYLE = {
    "fast":   ("fast-slow",   "o", "#2196F3"),
    "single": ("two-pass",    "s", "#F44336"),
}


def plot_single_machine(df, sysinfo, out_path):
    """Generate a 3-panel chart for one machine."""
    has_perf = df["cache_refs"].notna().any()
    ncols = 3 if has_perf else 2
    fig, axes = plt.subplots(1, ncols, figsize=(5.5 * ncols, 5))

    # Panel 1: time ratio (single / fast-slow)
    ax = axes[0]
    for mode, label, marker, color in [
        ("shuf", "shuffled", "o", "#9C27B0"),
        ("seq",  "sequential", "^", "#607D8B"),
    ]:
        piv_r = df[df["mode"] == mode].pivot(
            index="size", columns="algo", values="time_ns_per_iter")
        if "fast" in piv_r.columns and "single" in piv_r.columns:
            piv_r["ratio"] = piv_r["single"] / piv_r["fast"]
            d = piv_r["ratio"].dropna()
            ax.plot(d.index, d.values, marker=marker, color=color,
                    label=label, lw=2, ms=5)
    ax.axhline(y=1.5, color="gray", ls=":", alpha=0.5, lw=1)
    ax.text(ax.get_xlim()[0] if ax.get_xlim()[0] > 0 else 1000, 1.52,
            " 1.5x (3n/2 ÷ n)", fontsize=7, color="gray", va="bottom")
    ax.set_xscale("log")
    ax.set_xlabel("List size (nodes)")
    ax.set_ylabel("Time ratio (two-pass / fast-slow)")
    ax.set_title("Performance ratio")
    ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    add_cache_lines(ax, sysinfo["caches"])

    # Panel 2: shuffled timing
    ax = axes[1]
    for algo, (label, marker, color) in ALGO_STYLE.items():
        d = df[(df["mode"] == "shuf") & (df["algo"] == algo)]
        ax.plot(d["size"], d["time_ns_per_iter"], marker=marker, color=color,
                label=label, lw=2, ms=5)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("List size (nodes)")
    ax.set_ylabel("Time per iteration (ns)")
    ax.set_title("Shuffled links")
    ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    add_cache_lines(ax, sysinfo["caches"])

    # Ratio annotation
    piv = df[df["mode"] == "shuf"].pivot(
        index="size", columns="algo", values="time_ns_per_iter")
    if "fast" in piv.columns and "single" in piv.columns:
        piv["ratio"] = piv["single"] / piv["fast"]
        lines = [f"{int(s):>10,}: {r:.2f}x"
                 for s, r in piv["ratio"].dropna().items()]
        ax.text(0.02, 0.02, "two-pass / fast-slow:\n" + "\n".join(lines),
                transform=ax.transAxes, fontsize=6.5, fontfamily="monospace",
                va="bottom",
                bbox=dict(boxstyle="round,pad=0.3", fc="white", alpha=0.85))

    # Panel 3: cache miss rate
    if has_perf:
        ax = axes[2]
        shuf = df[df["mode"] == "shuf"].copy()
        shuf["miss_pct"] = shuf["cache_misses"] / shuf["cache_refs"] * 100
        for algo, (label, marker, color) in ALGO_STYLE.items():
            d = shuf[shuf["algo"] == algo].dropna(subset=["miss_pct"])
            if not d.empty:
                ax.plot(d["size"], d["miss_pct"], marker=marker, color=color,
                        label=label, lw=2, ms=5)
        ax.set_xscale("log")
        ax.set_xlabel("List size (nodes)")
        ax.set_ylabel("Cache miss rate (%)")
        ax.set_title("Cache miss rate (shuffled)")
        ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
        ax.set_ylim(-5, 105)
        add_cache_lines(ax, sysinfo["caches"])

    cache_desc = ", ".join(f"{k}={v}K" for k, v in sysinfo["caches"].items()
                           if k != "L1i")
    fig.suptitle(f"{sysinfo['cpu']}\n({cache_desc})",
                 fontsize=11, fontweight="bold")
    plt.tight_layout()
    plt.savefig(out_path, bbox_inches="tight")
    print(f"  Saved: {out_path}")
    plt.close()


def plot_comparison(machines, out_path):
    """Generate a comparison chart: timing ratio + miss rate side by side."""
    n = len(machines)
    fig, axes = plt.subplots(2, n, figsize=(6 * n, 9))
    if n == 1:
        axes = axes.reshape(2, 1)

    for col, (name, df, info) in enumerate(machines):
        cache_desc = ", ".join(f"{k}={v}K" for k, v in info["caches"].items()
                               if k != "L1i")

        # Top row: shuffled timing comparison
        ax = axes[0, col]
        for algo, (label, marker, color) in ALGO_STYLE.items():
            d = df[(df["mode"] == "shuf") & (df["algo"] == algo)]
            ax.plot(d["size"], d["time_ns_per_iter"], marker=marker,
                    color=color, label=label, lw=2, ms=5)
        ax.set_xscale("log"); ax.set_yscale("log")
        ax.set_xlabel("List size (nodes)")
        ax.set_ylabel("Time per iteration (ns)")
        ax.set_title(f"{name}\n{info['cpu']}\n({cache_desc})", fontsize=9)
        ax.legend(fontsize=7); ax.grid(True, alpha=0.3)
        add_cache_lines(ax, info["caches"])

        # Bottom row: cache miss rate
        ax = axes[1, col]
        has_perf = df["cache_refs"].notna().any()
        if has_perf:
            shuf = df[df["mode"] == "shuf"].copy()
            shuf["miss_pct"] = shuf["cache_misses"] / shuf["cache_refs"] * 100
            for algo, (label, marker, color) in ALGO_STYLE.items():
                d = shuf[shuf["algo"] == algo].dropna(subset=["miss_pct"])
                if not d.empty:
                    ax.plot(d["size"], d["miss_pct"], marker=marker,
                            color=color, label=label, lw=2, ms=5)
            ax.set_ylim(-5, 105)
            ax.set_ylabel("Cache miss rate (%)")
            ax.set_title("Cache miss rate (shuffled)", fontsize=9)
        else:
            # Fallback: show ratio instead
            piv = df[df["mode"] == "shuf"].pivot(
                index="size", columns="algo", values="time_ns_per_iter")
            if "fast" in piv.columns and "single" in piv.columns:
                piv["ratio"] = piv["single"] / piv["fast"]
                ax.plot(piv.index, piv["ratio"], marker="D", color="#9C27B0",
                        lw=2, ms=5)
                ax.axhline(y=1.5, color="gray", ls=":", alpha=0.5)
                ax.set_ylabel("Time ratio (two-pass / fast-slow)")
                ax.set_title("Performance ratio (shuffled)", fontsize=9)

        ax.set_xscale("log")
        ax.set_xlabel("List size (nodes)")
        ax.legend(fontsize=7); ax.grid(True, alpha=0.3)
        add_cache_lines(ax, info["caches"])

    fig.suptitle("Fast-Slow vs Two-Pass Pointer: Cross-Platform Comparison",
                 fontsize=13, fontweight="bold")
    plt.tight_layout()
    plt.savefig(out_path, bbox_inches="tight")
    print(f"  Saved: {out_path}")
    plt.close()


def load_data(directory: Path):
    csv = directory / "results.csv"
    sysinfo_path = directory / "sysinfo.txt"
    if not csv.exists():
        return None, None
    df = pd.read_csv(csv)
    for col in ["time_ns_per_iter", "cache_misses", "cache_refs"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    info = parse_sysinfo(sysinfo_path)
    return df, info


def main():
    base = Path(__file__).parent
    out_dir = base

    # Find all result directories
    if len(sys.argv) > 1:
        dirs = [Path(d) for d in sys.argv[1:]]
    else:
        # Look for results_<name>/ subdirectories and current directory
        dirs = sorted(base.glob("results_*/"))
        if (base / "results.csv").exists():
            dirs.insert(0, base)

    machines = []
    for d in dirs:
        df, info = load_data(d)
        if df is not None:
            name = info.get("hostname", d.name)
            machines.append((name, df, info))

            # Generate per-machine chart
            plot_single_machine(df, info, out_dir / f"cache_exp_{name}.svg")

    if len(machines) >= 2:
        plot_comparison(machines, out_dir / "cache_exp_combined.svg")
    elif len(machines) == 1:
        print("Only one machine found; skipping combined chart.")
    else:
        print("No results found. Run: make bench")
        sys.exit(1)


if __name__ == "__main__":
    main()
