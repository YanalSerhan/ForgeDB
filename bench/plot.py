"""Charts and tables from forgedb_bench results (TODO 5.2).

    python bench/plot.py out/bench/20261102-1400            # a folder from scripts/bench.ps1
    python bench/plot.py a.json b.json --out charts           # or result files

Reads every forgedb_bench JSON file given (or in the folders given; other JSON
files, such as bench.ps1's machine.json, are skipped) and writes, per sync
mode ("unsynced", "synced"):

    throughput-<mode>.png    ops/s per workload: one panel per workload, a
                             column per engine at the median of its runs, a
                             line from the minimum to the maximum
    latency-p99-<mode>.png   the same for p99 latency (scans have none)
    write-amp-<mode>.png     write amplification (the write workloads)
    space-amp-<mode>.png     space amplification
    results.md               every number as median [min-max], per mode,
                             and the machine record if machine.json is there

Each workload's panel has its own scale: throughput runs from tens of
thousands to millions of operations per second across workloads. Needs
matplotlib (pip install matplotlib).
"""

import argparse
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no window (before pyplot is imported)
import matplotlib.pyplot as plt
from matplotlib import patches, ticker

WORKLOADS = ["fillseq", "fillrandom", "overwrite", "readrandom", "readmissing", "readseq",
             "seekrandom", "mixed"]
WRITE_WORKLOADS = ["fillseq", "fillrandom", "overwrite", "mixed"]

# Engines in a fixed order with fixed colors, so an engine looks the same in
# every chart (the first four slots of a categorical palette checked for
# color-blind separation; the engine names are on the axis too).
ENGINE_ORDER = ["forgedb", "rocksdb", "leveldb", "sqlite"]
ENGINE_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
EXTRA_COLOR = "#8a8984"  # any engine past these four

SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e4e3df"


# --- Loading ---------------------------------------------------------------------------------

def result_files(paths):
    files = []
    for path in map(Path, paths):
        if path.is_dir():
            files.extend(sorted(path.glob("*.json")))
        elif path.is_file():
            files.append(path)
        else:
            sys.exit(f"plot.py: no such file or folder: {path}")
    return files


def load(paths):
    """{mode: {(engine, workload): [result, ...]}}, the benchmark headers, machine.json."""
    runs = defaultdict(lambda: defaultdict(list))
    headers = []
    machine = None
    for file in result_files(paths):
        with open(file, encoding="utf-8-sig") as f:  # PowerShell 5 writes a BOM
            data = json.load(f)
        if file.name == "machine.json":
            machine = data
            continue
        if not isinstance(data, dict) or data.get("tool") != "forgedb_bench":
            continue
        mode = "synced" if data["options"]["sync"] else "unsynced"
        headers.append((file.name, mode, data))
        for result in data["results"]:
            runs[mode][(result["engine"], result["workload"])].append(result)
    if not headers:
        sys.exit("plot.py: no forgedb_bench results found")
    return runs, headers, machine


# --- Numbers ---------------------------------------------------------------------------------

def metric(result, name):
    if name == "ops_per_second":
        return result["ops_per_second"]
    if name == "p99_us":
        latency = result.get("latency_us")
        return latency["p99"] if latency else None
    if name == "p50_us":
        latency = result.get("latency_us")
        return latency["p50"] if latency else None
    return result.get(name)


def summary(results, name):
    """(median, min, max, count) of a metric over runs; None if no run has it."""
    values = [v for v in (metric(r, name) for r in results) if v is not None]
    if not values:
        return None
    return statistics.median(values), min(values), max(values), len(values)


def engines_in(mode_runs):
    present = {engine for engine, _ in mode_runs}
    ordered = [e for e in ENGINE_ORDER if e in present]
    return ordered + sorted(present - set(ordered))


def workloads_in(mode_runs, only=None):
    present = {workload for _, workload in mode_runs}
    candidates = only if only is not None else WORKLOADS
    ordered = [w for w in candidates if w in present]
    if only is None:
        ordered += sorted(present - set(WORKLOADS))
    return ordered


def color_of(engine):
    if engine in ENGINE_ORDER:
        return ENGINE_COLORS[ENGINE_ORDER.index(engine)]
    return EXTRA_COLOR


def compact(value):
    """1,234 / 12.3K / 1.23M for labels and ticks."""
    if value is None:
        return "n/a"
    magnitude = abs(value)
    if magnitude >= 1e6:
        return f"{value / 1e6:.3g}M"
    if magnitude >= 1e4:
        return f"{value / 1e3:.3g}K"
    if magnitude >= 100:
        return f"{value:,.0f}"
    return f"{value:.3g}"


# --- Charts ----------------------------------------------------------------------------------

def style_axes(ax):
    ax.set_facecolor(SURFACE)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=8, length=0)
    ax.yaxis.grid(True, color=GRID, linewidth=1)
    ax.set_axisbelow(True)
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: compact(v)))


def chart(mode_runs, name, title, unit, path, only=None):
    """Small multiples: one panel per workload, a column per engine. Returns
    False (and writes nothing) if no run has the metric."""
    engines = engines_in(mode_runs)
    workloads = [w for w in workloads_in(mode_runs, only)
                 if any(summary(mode_runs.get((e, w), []), name) for e in engines)]
    if not workloads:
        return False
    columns = min(4, len(workloads))
    rows = (len(workloads) + columns - 1) // columns
    fig, axes = plt.subplots(rows, columns, figsize=(3.2 * columns, 2.8 * rows + 0.7),
                             squeeze=False, facecolor=SURFACE)
    run_counts = set()
    for index, workload in enumerate(workloads):
        ax = axes[index // columns][index % columns]
        style_axes(ax)
        ax.set_title(workload, color=TEXT, fontsize=10, loc="left")
        top = 0
        for x, engine in enumerate(engines):
            stats = summary(mode_runs.get((engine, workload), []), name)
            if stats is None:
                ax.text(x, 0, "n/a", ha="center", va="bottom", color=TEXT_SECONDARY, fontsize=7)
                continue
            median, low, high, count = stats
            run_counts.add(count)
            ax.bar(x, median, width=0.62, color=color_of(engine), edgecolor=SURFACE, linewidth=2)
            if count > 1 and high > low:
                ax.plot([x, x], [low, high], color=TEXT, linewidth=1.2, solid_capstyle="butt")
                ax.plot([x - 0.08, x + 0.08], [high, high], color=TEXT, linewidth=1.2)
                ax.plot([x - 0.08, x + 0.08], [low, low], color=TEXT, linewidth=1.2)
            ax.annotate(compact(median), (x, max(median, high)), xytext=(0, 3),
                        textcoords="offset points", ha="center", va="bottom",
                        color=TEXT_SECONDARY, fontsize=7.5)
            top = max(top, high)
        ax.set_xticks(range(len(engines)), engines, fontsize=8)
        ax.set_xlim(-0.6, len(engines) - 0.4)
        ax.set_ylim(0, top * 1.15 if top > 0 else 1)
    for index in range(len(workloads), rows * columns):
        axes[index // columns][index % columns].set_visible(False)

    runs = "/".join(str(c) for c in sorted(run_counts)) or "0"
    height = fig.get_figheight()
    fig.text(0.01, 1 - 0.12 / height, title, ha="left", va="top", color=TEXT, fontsize=12)
    fig.text(0.01, 1 - 0.40 / height,
             f"{unit}. Columns: median of {runs} runs; lines: lowest to highest run.",
             ha="left", va="top", color=TEXT_SECONDARY, fontsize=8.5)
    handles = [patches.Patch(color=color_of(e), label=e) for e in engines]
    fig.legend(handles=handles, loc="upper right", bbox_to_anchor=(0.995, 1 - 0.05 / height),
               ncol=len(engines), frameon=False, fontsize=8.5, labelcolor=TEXT_SECONDARY)
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.62 / height), h_pad=1.5)
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return True


CHARTS = [
    # metric, file stem, title, unit, workloads (None: all)
    ("ops_per_second", "throughput", "Throughput", "Operations per second, higher is better", None),
    ("p99_us", "latency-p99", "p99 latency", "Microseconds per operation, lower is better", None),
    ("write_amplification", "write-amp", "Write amplification",
     "Bytes the process wrote / bytes of keys and values, lower is better", WRITE_WORKLOADS),
    ("space_amplification", "space-amp", "Space amplification",
     "Database files / live keys and values, lower is better", None),
]


# --- Tables ----------------------------------------------------------------------------------

def cell(stats):
    if stats is None:
        return "n/a"
    median, low, high, _ = stats
    return f"{compact(median)} [{compact(low)}–{compact(high)}]"


def table(mode_runs, name, only=None):
    engines = engines_in(mode_runs)
    lines = ["| workload | " + " | ".join(engines) + " |",
             "|---|" + "---|" * len(engines)]
    for workload in workloads_in(mode_runs, only):
        cells = [cell(summary(mode_runs.get((e, workload), []), name)) for e in engines]
        lines.append(f"| {workload} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def machine_lines(machine):
    lines = []
    for key, value in machine.items():
        if isinstance(value, dict):
            value = "; ".join(f"{k}: {v}" for k, v in value.items())
        elif isinstance(value, list):
            value = ", ".join(map(str, value))
        lines.append(f"- **{key}**: {value}")
    return lines


def write_markdown(runs, headers, machine, path):
    out = ["# forgedb_bench results", ""]
    if machine:
        out += ["## Machine", ""] + machine_lines(machine) + [""]
    for mode in ("unsynced", "synced"):
        if mode not in runs:
            continue
        files = [(name, data) for name, m, data in headers if m == mode]
        options = files[0][1]["options"]
        build = files[0][1]["build"]
        out += [f"## {mode.capitalize()} writes", "",
                "Files: " + ", ".join(name for name, _ in files) + ". "
                f"Options: {options['num']:,} keys of {options['key_size']} B, values of "
                f"{options['value_size']} B, {options['threads']} thread(s), "
                f"{options['cache_mb']} MiB cache, {options['memtable_mb']} MiB write buffer, "
                f"{options['bloom_bits_per_key']}-bit Bloom filters, no compression. "
                f"Build: {build['compiler']}, {build['type']}. "
                "Cells: median [lowest–highest] over the runs.", ""]
        for name, _stem, title, unit, only in CHARTS:
            out += [f"### {title}", "", f"{unit}.", "", table(runs[mode], name, only), ""]
    path.write_text("\n".join(out), encoding="utf-8")


# --- Main ------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="+", help="result folders or JSON files")
    parser.add_argument("--out", help="folder for the charts and results.md "
                        "(default: the first folder given, or the first file's folder)")
    args = parser.parse_args()

    runs, headers, machine = load(args.paths)
    first = Path(args.paths[0])
    out = Path(args.out) if args.out else (first if first.is_dir() else first.parent)
    out.mkdir(parents=True, exist_ok=True)

    written = []
    for mode, mode_runs in runs.items():
        for name, stem, title, unit, only in CHARTS:
            path = out / f"{stem}-{mode}.png"
            mode_title = f"{title}, {mode} writes"
            if chart(mode_runs, name, mode_title, unit, path, only):
                written.append(path)
    markdown = out / "results.md"
    write_markdown(runs, headers, machine, markdown)
    written.append(markdown)
    for path in written:
        print(path)


if __name__ == "__main__":
    main()
