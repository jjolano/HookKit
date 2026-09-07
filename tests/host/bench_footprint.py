#!/usr/bin/env python3
"""Linux host suite repetitions, not hook-count scaling or device performance.

Derives the compiler invocation from existing Makefile targets, without editing
the Makefile. Allocation requests include fixtures and realloc's full requested
size; shared-library internals, allocator overhead and RSS are not measured.
"""
import argparse
import pathlib
import shlex
import statistics
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--trials", type=int, default=5)
parser.add_argument("targets", nargs="*", default=[
    "test-plan-analyze", "test-plan-prepare", "test-plan-commit",
    "test-provider-vtable",
])
args = parser.parse_args()
if not 1 <= args.trials <= 100:
    parser.error("--trials must be between 1 and 100")
output = ROOT / ".theos/obj"
output.mkdir(parents=True, exist_ok=True)
print("target,repetitions,median_us,min_us,max_us,allocation_calls,requested_bytes,free_calls", flush=True)
for target in args.targets:
    if target not in {"test-plan-analyze", "test-plan-prepare", "test-plan-commit",
                       "test-provider-vtable", "test-reloc-inline-wired"}:
        parser.error(f"unsupported host target: {target}")
    recipe = subprocess.check_output(
        ["make", "--no-print-directory", "-n", target], cwd=ROOT, text=True)
    command = shlex.split(recipe.split("&&")[1])
    source = next(arg for arg in command if arg.startswith("tests/host/test_"))
    command[command.index(source)] = "tests/host/bench_footprint.c"
    binary = output / ("bench_" + target.removeprefix("test-"))
    command[command.index("-o") + 1] = str(binary)
    command += [f'-DHK_BENCH_SOURCE="{pathlib.Path(source).name}"',
                "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=aligned_alloc,--wrap=free"]
    subprocess.run(command, cwd=ROOT, check=True)
    for count in (1, 10, 100, 1000):
        rows = []
        for _ in range(args.trials):
            result = subprocess.run([str(binary), str(count)], cwd=ROOT,
                                    capture_output=True, text=True, check=True)
            rows.append(result.stderr.strip().split(","))
        times = [float(row[1]) for row in rows]
        metrics = {tuple(row[2:]) for row in rows}
        assert len(metrics) == 1, (target, count, metrics)
        print(f"{target},{count},{statistics.median(times):.3f},"
              f"{min(times):.3f},{max(times):.3f}," + ",".join(rows[0][2:]), flush=True)
