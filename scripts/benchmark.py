#!/usr/bin/env python3
"""Run reproducible paging benchmarks and derive tables/plots from the CSVs.

Safe default: provided + development workloads only.  The holdout suite is run
only when explicitly selected, e.g.:
    python3 scripts/benchmark.py --include-holdout --plots

Freeze the algorithm and its parameters before that command.  The resulting
synthetic holdout evidence is not a guarantee about unknown classroom traces.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
FRAMES = [5, 10, 20, 50, 100]
PROVIDED_NAMES = ["sort1.txt", "lu.txt", "mm16.txt", "mm32.txt"]
COLUMNS = ["trace", "algorithm", "frames", "references", "faults", "fault_rate"]
COLORS = {"fifo":"#9a6653", "lru":"#477db3", "clock":"#bd963a", "opt":"#343c43", "adaptive":"#11836b"}
LABELS = {"fifo":"FIFO", "lru":"LRU", "clock":"Second Chance", "opt":"OPT (offline)", "adaptive":"Adaptive"}
PLOT_ALGORITHMS = ["fifo", "lru", "clock", "opt", "adaptive"]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def selected_suites(value: str, include_holdout: bool):
    if value == "all":
        suites = ["provided", "dev", "test"]
    else:
        suites = value.split(",")
    if any(x not in ["provided", "dev", "test"] for x in suites) or len(set(suites)) != len(suites):
        raise ValueError("--suites must be all or a comma-separated selection of provided,dev,test without duplicates")
    if include_holdout and "test" not in suites:
        suites.append("test")
    return suites


def suite_inputs(suite: str):
    if suite == "provided":
        paths = [ROOT / "data" / name for name in PROVIDED_NAMES]
        metadata = {"kind":"provided", "family_by_file":{name:"provided" for name in PROVIDED_NAMES}}
    else:
        folder = ROOT / "tests" / "generated" / suite
        manifest_path = folder / "manifest.json"
        if not manifest_path.is_file():
            raise ValueError(f"Missing {manifest_path}; run python3 tests/generate_traces.py first")
        manifest = json.loads(manifest_path.read_text())
        if manifest.get("suite") != suite:
            raise ValueError(f"Manifest suite mismatch: {manifest_path}")
        paths = [folder / trace["file"] for trace in manifest["traces"]]
        metadata = {"kind":"synthetic", "seed":manifest["seed"],
                    "family_by_file":{t["file"]:t["family"] for t in manifest["traces"]},
                    "manifest":str(manifest_path), "manifest_sha256":sha256(manifest_path),
                    "holdout_limit":"Randomized workloads use an independent seed; pure cycles and other deterministic patterns may be isomorphic after page relabeling."}
    for path in paths:
        if not path.is_file():
            raise ValueError(f"Missing trace: {path}")
    metadata["trace_sha256"] = {path.name:sha256(path) for path in paths}
    return paths, metadata


def parse_csv(text: str, expected_names):
    reader = csv.DictReader(io.StringIO(text))
    if reader.fieldnames != COLUMNS:
        raise ValueError(f"Unexpected CSV header: {reader.fieldnames!r}")
    rows = []
    seen = set()
    references_by_trace = {}
    for raw in reader:
        row = {"trace":Path(raw["trace"]).name,"algorithm":raw["algorithm"],
               "frames":int(raw["frames"]),"references":int(raw["references"]),
               "faults":int(raw["faults"]),"fault_rate":float(raw["fault_rate"])}
        key = row["trace"],row["algorithm"],row["frames"]
        if key in seen:
            raise ValueError(f"Duplicate benchmark row: {key}")
        seen.add(key)
        if row["references"] <= 0 or not 0 <= row["faults"] <= row["references"]:
            raise ValueError(f"Invalid counts: {row}")
        if abs(row["fault_rate"] - row["faults"] / row["references"]) > 1e-7:
            raise ValueError(f"Fault rate disagrees with counts: {row}")
        if references_by_trace.setdefault(row["trace"],row["references"]) != row["references"]:
            raise ValueError(f"Reference count differs across algorithms: {row['trace']}")
        rows.append(row)
    if {r["trace"] for r in rows} != set(expected_names):
        raise ValueError("CSV trace set does not match the requested suite")
    algorithms = sorted({r["algorithm"] for r in rows})
    if not {"fifo","lru","clock","adaptive","opt"}.issubset(algorithms):
        raise ValueError("Required baseline/Adaptive/OPT results are missing")
    expected = {(t,a,c) for t in expected_names for a in algorithms for c in FRAMES}
    if seen != expected:
        raise ValueError("CSV does not contain exactly one row for every trace, algorithm and frame count")
    bounds = {(r["trace"],r["frames"]):r["faults"] for r in rows if r["algorithm"] == "opt"}
    for row in rows:
        if row["faults"] < bounds[row["trace"],row["frames"]]:
            raise ValueError(f"An algorithm beat the offline OPT lower bound: {row}")
    return rows


def aggregate(rows):
    """All ratios use matched cases, with no silent trace-length normalization."""
    grouped = defaultdict(list)
    for row in rows:
        grouped[row["algorithm"]].append(row)
    result = {}
    totals = {a:sum(r["faults"] for r in rs) for a,rs in grouped.items()}
    for algorithm, algorithm_rows in sorted(grouped.items()):
        n = len(algorithm_rows)
        total = totals[algorithm]
        references = sum(r["references"] for r in algorithm_rows)
        entry = {
            "cases":n, "total_faults":total,
            "mean_faults":total/n,
            "mean_fault_rate":sum(r["faults"]/r["references"] for r in algorithm_rows)/n,
            "weighted_fault_rate":total/references,
            "references_summed_over_cases":references,
        }
        for baseline in ["lru","fifo"]:
            baseline_total = totals.get(baseline)
            entry[f"relative_fault_reduction_vs_{baseline}"] = 1 - total/baseline_total if baseline_total else None
        optimum = totals.get("opt")
        entry["mean_fault_gap_vs_opt"] = (total-optimum)/n if optimum is not None else None
        entry["relative_excess_faults_vs_opt"] = total/optimum-1 if optimum else None
        lru_total = totals.get("lru")
        denominator = lru_total-optimum if lru_total is not None and optimum is not None else None
        entry["fraction_lru_opt_gap_closed"] = (lru_total-total)/denominator if denominator else None
        result[algorithm] = entry
    return result


def summarize(rows, metadata):
    by_trace = defaultdict(list)
    by_family = defaultdict(list)
    for row in rows:
        by_trace[row["trace"]].append(row)
        by_family[metadata["family_by_file"][row["trace"]]].append(row)
    return {
        **metadata,
        "trace_count":len(by_trace),
        "cases_per_algorithm":len(by_trace)*len(FRAMES),
        "algorithms":aggregate(rows),
        "per_trace":{name:{"references":trace_rows[0]["references"],
                            "family":metadata["family_by_file"][name],
                            "algorithms":aggregate(trace_rows)}
                     for name,trace_rows in sorted(by_trace.items())},
        "per_family":{name:{"trace_count":len({r["trace"] for r in family_rows}),
                             "algorithms":aggregate(family_rows)}
                      for name,family_rows in sorted(by_family.items())},
    }


def make_plots(results: Path, summary):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("Plotting skipped: matplotlib is not installed in this Python. CSV/JSON results are complete. "
              "Use the bundled Python or install matplotlib, then rerun with --summarize-only --plots.",file=sys.stderr)
        return []
    outputs = []
    plt.rcParams.update({"font.size":10,"axes.spines.top":False,"axes.spines.right":False,
                         "axes.grid":True,"grid.alpha":.2,"figure.dpi":140})
    if "provided" in summary["suites"]:
        rows = parse_csv((results/"provided.csv").read_text(),PROVIDED_NAMES)
        lookup = {(r["trace"],r["algorithm"],r["frames"]):r["faults"] for r in rows}
        fig,axes = plt.subplots(2,2,figsize=(11,7),constrained_layout=True)
        for ax,name in zip(axes.flat,PROVIDED_NAMES):
            for algorithm in PLOT_ALGORITHMS:
                y = [lookup[name,algorithm,c] for c in FRAMES]
                ax.plot(FRAMES,y,marker="o",markersize=4,
                        linewidth=2.2 if algorithm == "adaptive" else 1.4,
                        linestyle="--" if algorithm == "opt" else "-",
                        color=COLORS[algorithm],label=LABELS[algorithm])
            ax.set_title(name)
            ax.set_xscale("log")
            ax.set_xticks(FRAMES,labels=list(map(str,FRAMES)))
            ax.minorticks_off()
            ax.set_xlabel("Frames")
            ax.set_ylabel("Page faults")
            ax.set_ylim(bottom=0)
        handles,labels = axes.flat[0].get_legend_handles_labels()
        fig.legend(handles,labels,loc="lower center",bbox_to_anchor=(.5,-.075),ncol=len(PLOT_ALGORITHMS),frameon=False)
        destination = results/"provided_faults.png"
        fig.savefig(destination,bbox_inches="tight")
        plt.close(fig)
        outputs.append(str(destination))
    if "holdout" in summary["suites"]:
        families = summary["suites"]["holdout"]["per_family"]
        names = sorted(families)
        chosen = ["fifo","clock","opt","adaptive"]
        fig,ax = plt.subplots(figsize=(12,5.5),constrained_layout=True)
        width = .18
        for j,algorithm in enumerate(chosen):
            ratios = [families[name]["algorithms"][algorithm]["total_faults"] /
                      families[name]["algorithms"]["lru"]["total_faults"] for name in names]
            positions = [i+(j-(len(chosen)-1)/2)*width for i in range(len(names))]
            ax.bar(positions,ratios,width=width,color=COLORS[algorithm],label=LABELS[algorithm])
        ax.axhline(1,color=COLORS["lru"],linestyle="--",linewidth=1.3,label="LRU = 1")
        ax.set_xticks(range(len(names)),labels=[n.replace("_"," ") for n in names],rotation=30,ha="right")
        ax.set_ylabel("Total page faults / LRU total (lower is better)")
        ax.set_title("Synthetic holdout: matched traces and five frame counts per family")
        ax.legend(frameon=False,ncol=5,loc="upper center",bbox_to_anchor=(.5,1.0))
        ax.set_ylim(bottom=0)
        destination = results/"holdout_families.png"
        fig.savefig(destination,bbox_inches="tight")
        plt.close(fig)
        outputs.append(str(destination))
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary",type=Path,default=ROOT/"pager",help="compiled C++ executable")
    parser.add_argument("--results",type=Path,default=ROOT/"build"/"benchmark")
    parser.add_argument("--suites",default="provided,dev",help="provided,dev (default), test, or all; comma-separated")
    parser.add_argument("--include-holdout",action="store_true",help="explicitly include the independent-seed test suite")
    parser.add_argument("--plots",action="store_true",help="also render PNG charts when matplotlib is available")
    parser.add_argument("--summarize-only",action="store_true",help="read existing selected CSV files; do not run the executable")
    parser.add_argument("--timeout",type=float,default=180,help="maximum seconds per suite (default: 180)")
    args = parser.parse_args()
    try:
        suites = selected_suites(args.suites,args.include_holdout)
        binary = args.binary.resolve()
        results = args.results.resolve()
        if not binary.is_file():
            raise ValueError(f"Executable not found: {binary}; run make first")
        results.mkdir(parents=True,exist_ok=True)
        summary = {
            "schema_version":1,"generated_at_utc":datetime.now(timezone.utc).isoformat(),
            "binary":str(binary),"binary_sha256":sha256(binary),"frames":FRAMES,
            "aggregation":{
                "case":"One (trace, frame count) pair; all algorithms use identical cases.",
                "mean_faults":"Arithmetic mean of raw fault counts over cases; this is the primary classroom-style score.",
                "mean_fault_rate":"Arithmetic mean of faults/references over cases; gives equal weight to each trace and frame count.",
                "weighted_fault_rate":"Sum of faults divided by sum of references over cases.",
                "relative_fault_reduction_vs_baseline":"1 - sum(algorithm faults)/sum(baseline faults); multiply by 100 for percent.",
                "mean_fault_gap_vs_opt":"mean_faults - OPT mean_faults; OPT is offline and sees the future.",
                "relative_excess_faults_vs_opt":"sum(algorithm faults)/sum(OPT faults) - 1.",
                "fraction_lru_opt_gap_closed":"(sum(LRU faults)-sum(algorithm faults))/(sum(LRU faults)-sum(OPT faults)); null if denominator is zero.",
                "family_plot":"Sum all faults within a workload family, across its traces and all five frame counts, then divide by the matched LRU sum.",
            },
            "execution_note":"Online policies receive each current request only. OPT is a separately labeled offline lower bound.",
            "holdout_selected_explicitly":"test" in suites,
            "summarize_only":args.summarize_only,
            "suites":{},
        }
        for suite in suites:
            paths,metadata = suite_inputs(suite)
            output_name = "holdout" if suite == "test" else suite
            csv_path = results/(output_name+".csv")
            if args.summarize_only:
                text = csv_path.read_text()
                metadata["runtime_seconds"] = None
                metadata["provenance_note"] = "CSV was read without rerunning; the current binary hash does not certify which binary produced this existing CSV."
            else:
                command = [str(binary),"--algorithm","all","--frames",",".join(map(str,FRAMES)),
                           "--format","csv",*map(str,paths)]
                print(f"Running {output_name}: {len(paths)} traces x {len(FRAMES)} capacities",flush=True)
                start = time.monotonic()
                proc = subprocess.run(command,text=True,capture_output=True,timeout=args.timeout)
                metadata["runtime_seconds"] = time.monotonic()-start
                metadata["command"] = command
                if proc.returncode:
                    raise ValueError(f"{output_name} failed (exit {proc.returncode}):\n{proc.stderr}")
                text = proc.stdout
                if proc.stderr.strip():
                    metadata["stderr"] = proc.stderr.strip()
            rows = parse_csv(text,[p.name for p in paths])
            if not args.summarize_only:
                temporary = csv_path.with_suffix(".csv.tmp")
                temporary.write_text(text)
                temporary.replace(csv_path)
            metadata["csv"] = str(csv_path)
            metadata["csv_sha256"] = sha256(csv_path)
            summary["suites"][output_name] = summarize(rows,metadata)
            mean = summary["suites"][output_name]["algorithms"]["adaptive"]["mean_faults"]
            print(f"{output_name}: Adaptive mean faults = {mean:.3f}")
        summary["plots"] = make_plots(results,summary) if args.plots else []
        destination = results/"summary.json"
        temporary = destination.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(summary,ensure_ascii=False,indent=2,allow_nan=False)+"\n")
        temporary.replace(destination)
        print(f"Summary -> {destination}")
    except (ValueError,OSError,subprocess.TimeoutExpired,csv.Error) as error:
        parser.exit(1,f"error: {error}\n")


if __name__ == "__main__":
    main()
