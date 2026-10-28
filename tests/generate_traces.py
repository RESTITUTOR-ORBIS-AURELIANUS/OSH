#!/usr/bin/env python3
"""Reproducible online paging workloads; no third-party dependencies.

Development and holdout suites use disjoint RNG seeds.  Use holdout results only
once the algorithm/parameters have been frozen. Synthetic performance is evidence
about these workloads, not a guarantee about the teacher's hidden traces.
"""
from __future__ import annotations
import argparse
import collections
import json
import math
from pathlib import Path
import random

FRAMES = [5, 10, 20, 50, 100]
SEEDS = {"dev": 20261028, "test": 719426031}


def workloads(seed: int):
    rng = random.Random(seed)
    output = []

    def add(name, pages, family, parameters, expectation, relabel=True):
        pages = list(pages)
        if relabel:
            # IDs carry no meaning: policies must learn reference history, not
            # identify a workload by a special page number or fixed numeric gap.
            mapping = {}
            used = set()
            for p in pages:
                if p not in mapping:
                    value = rng.randrange(1, 1 << 48)
                    while value in used:
                        value = rng.randrange(1, 1 << 48)
                    mapping[p] = value
                    used.add(value)
            pages = [mapping[p] for p in pages]
        output.append((name, pages, {
            "family": family, "parameters": parameters,
            "expected_property": expectation, "randomized_page_labels": relabel,
        }))

    for k in FRAMES:
        n = k + 1
        add(f"cycle_{n}", (i % n for i in range(4000)), "cycle",
            {"working_set": n, "length": 4000},
            f"A repeated {n}-page cycle exceeds {k} frames by one; plain LRU thrashes.")
    for n in [130, 257]:
        add(f"long_cycle_{n}", (i % n for i in range(4000)), "cycle",
            {"working_set": n, "length": 4000},
            "Repeated working set exceeds every evaluated cache; tests reuse prediction.")
    for n in [17, 80, 400]:
        add(f"uniform_{n}", (rng.randrange(n) for _ in range(7500)), "uniform",
            {"pages": n, "length": 7500},
            "Independent uniform requests have no useful historical ordering signal.")
    for n, alpha in [(64, 0.9), (256, 1.3)]:
        weights = [1.0 / (i + 1) ** alpha for i in range(n)]
        add(f"zipf_{n}", rng.choices(range(n), weights=weights, k=10000), "zipf",
            {"pages": n, "exponent": alpha, "length": 10000},
            "A stationary popularity skew rewards protection of frequently used pages.")
    for hot, scan in [(4, 120), (18, 300)]:
        pages = []
        scan_id = 1000
        while len(pages) < 6000:
            pages.extend(rng.choices(range(hot), k=240))
            pages.extend(range(scan_id, scan_id + scan))
            scan_id += scan
        add(f"hot_scan_{hot}", pages[:6000], "hot_scan",
            {"hot_pages": hot, "hot_burst": 240, "scan_length": scan},
            "One-time scans compete with a stable hot set; tests scan resistance.")
    pages = []
    phase_sets = [list(range(0, 12)), list(range(200, 212)),
                  list(range(400, 440)), list(range(200, 212)), list(range(600, 625))]
    for ids in phase_sets:
        pages.extend(rng.choices(ids, weights=[1 / (i+1) for i in range(len(ids))], k=2000))
    add("phase_shift", pages, "phase_shift", {"phase_length": 2000, "phases": 5},
        "Abrupt changes and a returning hot set test forgetting and recovery.")
    pages = []
    for outer in range(80):
        for inner in range(10):
            pages.extend([0, 1, 2, 100 + outer % 31, 200 + inner, 1, 300 + inner, 0, 2, 200 + inner])
    add("nested_loop", pages, "nested_loop", {"outer": 80, "inner": 10},
        "Multiple reuse distances coexist: loop-invariant, inner-loop, and outer-loop pages.")
    pages = []
    while len(pages) < 10000:
        p = rng.randrange(180)
        pages.extend([p] * rng.randint(2, 16))
        pages.extend(rng.choices(range(12), k=rng.randint(1, 5)))
    add("bursty", pages[:10000], "bursty", {"pages": 180, "burst_range": [2, 16]},
        "Consecutive repeated references should not be confused with long-term popularity.")
    pages = []
    for i in range(10000):
        base = 0 if (i // 125) % 2 == 0 else 1000
        pages.append(base + rng.randrange(35))
    add("alternating", pages, "alternating", {"set_size": 35, "switch_every": 125},
        "Rapid alternation between disjoint localities tests adaptation speed.")
    pages = []
    while len(pages) < 6000:
        pages.extend(range(96))
        pages.extend(range(95, -1, -1))
    add("reverse_scan", pages[:6000], "reverse_scan", {"working_set": 96},
        "Bidirectional scans reverse recency order and defeat simple fixed-direction assumptions.")
    pages = []
    p = 0
    for _ in range(10000):
        if rng.random() < .035:
            p = rng.randrange(320)
        else:
            p = (p + rng.choice([-2, -1, 0, 1, 2])) % 320
        pages.append(p)
    add("random_walk", pages, "random_walk", {"pages": 320, "jump_probability": .035},
        "Short local walks with occasional long jumps reward recent locality.")
    pages = []
    pages.extend(rng.choices(range(9), k=3000))
    pages.extend(100 + i % 41 for i in range(3000))
    pages.extend(rng.randrange(300) for _ in range(3000))
    pages.extend(rng.choices(range(600, 700), weights=[1/(i+1)**1.2 for i in range(100)], k=3000))
    add("mixed_regimes", pages, "mixed_regimes", {"regime_length": 3000},
        "Hot locality, a cycle, uniform noise, and skewed popularity appear in one stream.")
    return output


def analyze_original(root: Path):
    result = {"page_number_base": 16, "page_numbers_not_addresses": True, "traces": {}}
    for name in ["sort1.txt", "lu.txt", "mm16.txt", "mm32.txt"]:
        pages = [int(x, 16) for x in (root / name).read_text().split()]
        count = collections.Counter(pages)
        counts = sorted(count.values(), reverse=True)
        last = {}
        gaps = []
        for t, p in enumerate(pages):
            if p in last:
                gaps.append(t - last[p])
            last[p] = t
        gaps.sort()
        quantile = lambda q: gaps[min(len(gaps)-1, int(q * (len(gaps)-1)))] if gaps else None
        result["traces"][name] = {
            "references": len(pages), "unique_pages": len(count),
            "compulsory_faults": len(count),
            "consecutive_repeat_fraction": sum(a == b for a,b in zip(pages,pages[1:])) / max(1,len(pages)-1),
            "most_frequent_page_share": counts[0] / len(pages),
            "top_k_reference_share": {str(k):sum(counts[:k])/len(pages) for k in FRAMES},
            "reuse_gap_reference_distance_quantiles": {"p25":quantile(.25),"p50":quantile(.5),"p75":quantile(.75),"p90":quantile(.9)},
            "unique_pages_by_quarter": [len(set(pages[len(pages)*i//4:len(pages)*(i+1)//4])) for i in range(4)],
            "note": "Reuse gap counts references, not distinct pages; it is not LRU stack distance.",
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["dev", "test", "all"], default="all")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "generated")
    args = parser.parse_args()
    suites = SEEDS if args.suite == "all" else {args.suite: SEEDS[args.suite]}
    for suite, seed in suites.items():
        folder = args.output / suite
        folder.mkdir(parents=True, exist_ok=True)
        manifest = {"suite":suite,"seed":seed,"frames":FRAMES,
                    "warning":"Do not tune on the test suite; holdout synthetic traces do not reproduce the hidden classroom test.",
                    "traces":[]}
        for name, pages, metadata in workloads(seed):
            filename = name + ".txt"
            (folder / filename).write_text("".join(f"{p:x}\n" for p in pages))
            manifest["traces"].append({"file":filename,"references":len(pages),"unique_pages":len(set(pages)),**metadata})
        manifest["total_references"] = sum(t["references"] for t in manifest["traces"])
        (folder / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+"\n")
        print(f"{suite}: {len(manifest['traces'])} traces, {manifest['total_references']} references -> {folder}")
    original_root = Path(__file__).resolve().parents[1] / "data"
    destination = Path(__file__).resolve().parent / "trace_analysis.json"
    destination.write_text(json.dumps(analyze_original(original_root), ensure_ascii=False, indent=2)+"\n")
    print(f"Original trace analysis -> {destination}")


if __name__ == "__main__":
    main()
