#!/usr/bin/env python3
"""Independent black-box checks for the pager CLI (Python standard library).

Run: python3 tests/test_cli.py --binary ./pager
The baseline references below do not share C++ implementation code. Small random cases also enable --audit to check resident capacity and legal
on-demand transitions at every access. Final counters alone do not prove
causality; the C++ API supplies only the current reference to each online policy.
"""
from __future__ import annotations
import argparse
from collections import deque
import csv
import functools
import io
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ONLINE = ["fifo", "lru", "clock", "lfu", "mru", "arc", "decay", "predict", "adaptive"]
ALGORITHMS = ONLINE + ["opt"]


def fifo(seq, k):
    resident, order, faults = set(), deque(), 0
    for p in seq:
        if p in resident:
            continue
        faults += 1
        if len(resident) == k:
            resident.remove(order.popleft())
        resident.add(p)
        order.append(p)
    return faults


def lru(seq, k):
    order, faults = [], 0
    for p in seq:
        if p in order:
            order.remove(p)
        else:
            faults += 1
            if len(order) == k:
                del order[0]
        order.append(p)
    return faults


def clock(seq, k):
    slots, bits, hand, faults = [], [], 0, 0
    for p in seq:
        if p in slots:
            bits[slots.index(p)] = 1
            continue
        faults += 1
        if len(slots) < k:
            slots.append(p)
            bits.append(1)
            continue
        while bits[hand]:
            bits[hand] = 0
            hand = (hand + 1) % k
        slots[hand] = p
        bits[hand] = 1
        hand = (hand + 1) % k
    return faults


def opt(seq, k):
    resident, faults = set(), 0
    for i, p in enumerate(seq):
        if p in resident:
            continue
        faults += 1
        if len(resident) == k:
            def next_use(v):
                try:
                    return seq.index(v, i + 1)
                except ValueError:
                    return len(seq)
            resident.remove(max(resident, key=next_use))
        resident.add(p)
    return faults


def exact_minimum(seq, k):
    """Exhaustively enumerate all legal demand-paging choices on tiny sequences."""
    @functools.lru_cache(None)
    def solve(i, resident):
        if i == len(seq):
            return 0
        p = seq[i]
        if p in resident:
            return solve(i + 1, resident)
        if len(resident) < k:
            return 1 + solve(i + 1, tuple(sorted((*resident, p))))
        return 1 + min(solve(i + 1, tuple(sorted(set(resident) - {v} | {p}))) for v in resident)
    return solve(0, ())


BINARY = Path(__file__).resolve().parents[1] / "pager"


class PagerCLI(unittest.TestCase):
    def run_pager(self, content, algorithm="all", frames=(5,10,20,50,100), base=16, extra=()):
        return subprocess.run([str(BINARY), "--algorithm", algorithm, "--frames",
                               ",".join(map(str, frames)), "--format", "csv", "--base", str(base),
                               *extra, "-"], input=content, text=True, capture_output=True, timeout=30)

    def rows(self, seq, algorithm="all", frames=(5,10,20,50,100), audit=False):
        data = "\n".join(f"{p:x}" for p in seq) + "\n"
        proc = self.run_pager(data, algorithm, frames, extra=("--audit",) if audit else ())
        self.assertEqual(proc.returncode, 0, proc.stderr + proc.stdout)
        reader = csv.DictReader(io.StringIO(proc.stdout))
        self.assertEqual(reader.fieldnames, ["trace","algorithm","frames","references","faults","fault_rate"])
        rows = list(reader)
        wanted = ALGORITHMS if algorithm == "all" else [algorithm]
        self.assertEqual(len(rows), len(wanted) * len(frames))
        self.assertEqual({r["algorithm"] for r in rows}, set(wanted))
        self.assertEqual({int(r["frames"]) for r in rows}, set(frames))
        for r in rows:
            self.assertEqual(int(r["references"]), len(seq))
            f = int(r["faults"])
            self.assertGreaterEqual(f, len(set(seq)))
            self.assertLessEqual(f, len(seq))
            self.assertAlmostEqual(float(r["fault_rate"]), f / len(seq), delta=1e-6)
        return {(r["algorithm"],int(r["frames"])):int(r["faults"]) for r in rows}

    def test_classic_reference(self):
        seq = [7,0,1,2,0,3,0,4,2,3,0,3,2]
        values = self.rows(seq, frames=(1,2,3,5,8))
        for k in (1,2,3,5,8):
            for name, ref in [("fifo",fifo),("lru",lru),("clock",clock),("opt",opt)]:
                self.assertEqual(values[name,k], ref(seq,k), (name,k))
            for name in ONLINE:
                self.assertGreaterEqual(values[name,k], values["opt",k], (name,k))
        self.assertEqual(values["fifo",3],10)
        self.assertEqual(values["lru",3],9)
        self.assertEqual(values["opt",3],7)

    def test_random_against_independent_references(self):
        rng = random.Random(875226)
        for case in range(35):
            seq = [rng.randrange(rng.randint(2,15)) for _ in range(rng.randint(1,100))]
            frames = (1,2,3,5)
            values = self.rows(seq, frames=frames, audit=True)
            for k in frames:
                for name, ref in [("fifo",fifo),("lru",lru),("clock",clock),("opt",opt)]:
                    self.assertEqual(values[name,k], ref(seq,k), (case,name,k,seq))
                for name in ONLINE:
                    self.assertGreaterEqual(values[name,k], values["opt",k], (case,name,k))

    def test_optimal_against_exhaustive_legal_choices(self):
        rng = random.Random(98744)
        for case in range(25):
            seq = [rng.randrange(5) for _ in range(9)]
            values = self.rows(seq, algorithm="opt", frames=(1,2,3))
            for k in (1,2,3):
                self.assertEqual(values["opt",k], exact_minimum(seq,k), (case,k,seq))

    def test_one_frame_no_prefetch_count(self):
        # With one frame every legal on-demand policy has exactly this count.
        seq = [9,9,3,3,2,2,3,0,0,9,2,2]
        expected = 1 + sum(a != b for a,b in zip(seq,seq[1:]))
        for value in self.rows(seq, frames=(1,)).values():
            self.assertEqual(value, expected)

    def test_fit_in_memory_and_cold_unique_stream(self):
        for seq in ([0]*50, [8,9,10]*50, list(range(150))):
            for (name,k),value in self.rows(seq, frames=(3,200)).items():
                self.assertEqual(value,len(set(seq)),(name,k))

    def test_page_relabeling_invariance_baselines(self):
        # Baselines compare identity; no address bits or page-size conversion.
        seq = [1,2,1,3,4,2,5,4,3,2,1,6,4,2,7,1]
        mapping = {i: 0xFEDCBA9876543210 + i for i in set(seq)}
        mapped = [mapping[x] for x in seq]
        for name in ["fifo","lru","clock","opt"]:
            self.assertEqual(self.rows(seq,name,(2,4)),self.rows(mapped,name,(2,4)))

    def test_hexadecimal_aliases_decimal_and_uint64(self):
        proc = self.run_pager("a 0A 000a 0xa\nFFFFFFFFFFFFFFFF ffffffffffffffff 0Xffffffffffffffff\n", frames=(2,))
        self.assertEqual(proc.returncode,0,proc.stderr)
        for r in csv.DictReader(io.StringIO(proc.stdout)):
            self.assertEqual(int(r["references"]),7)
            self.assertEqual(int(r["faults"]),2)
        a = self.run_pager("10 16 10 16", frames=(1,2), base=10)
        b = self.run_pager("a 10 a 10", frames=(1,2), base=16)
        self.assertEqual(a.returncode,0,a.stderr)
        self.assertEqual(b.returncode,0,b.stderr)
        counts = lambda p: [(r["algorithm"],r["frames"],r["faults"]) for r in csv.DictReader(io.StringIO(p.stdout))]
        self.assertEqual(counts(a),counts(b))

    def test_multiple_files(self):
        with tempfile.TemporaryDirectory(prefix="pager-cli-") as folder:
            paths = [Path(folder)/"one trace.txt", Path(folder)/"two.txt"]
            paths[0].write_text("a a b\n")
            paths[1].write_text("f e d c\n")
            proc = subprocess.run([str(BINARY),"--algorithm","lru","--frames","5,10","--format","csv",*map(str,paths)],
                                  text=True,capture_output=True,timeout=30)
            self.assertEqual(proc.returncode,0,proc.stderr)
            rows = list(csv.DictReader(io.StringIO(proc.stdout)))
            self.assertEqual(len(rows),4)
            self.assertEqual(sorted(int(r["faults"]) for r in rows),[2,2,4,4])

    def test_reject_invalid_inputs(self):
        for text in ["", " \n\t", "xyz\n", "1G\n", "-1\n", "1.2\n", "10000000000000000\n", "0x\n"]:
            with self.subTest(text=text):
                proc = self.run_pager(text,algorithm="lru",frames=(3,))
                self.assertNotEqual(proc.returncode,0)
                self.assertTrue(proc.stderr.strip())
        for args in [
            ["--frames","0"], ["--frames","-1"], ["--frames",""],
            ["--frames","3,,5"], ["--base","3"], ["--algorithm","does-not-exist"],
            ["--unknown"], ["--frames"], ["--format","invalid"],
            ["/a-file-that-does-not-exist/pager-trace"],
        ]:
            with self.subTest(args=args):
                proc = subprocess.run([str(BINARY),*args],text=True,capture_output=True,timeout=30)
                self.assertNotEqual(proc.returncode,0)
                self.assertTrue(proc.stderr.strip())

    def test_help(self):
        proc = subprocess.run([str(BINARY),"--help"],text=True,capture_output=True,timeout=30)
        self.assertEqual(proc.returncode,0,proc.stderr)
        self.assertIn("--frames",proc.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,default=BINARY)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    if not BINARY.is_file():
        parser.error(f"Build the executable first: {BINARY}")
    unittest.main(argv=[sys.argv[0],*remaining],verbosity=2)
