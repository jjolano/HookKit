# HookKit Benchmarks

Host + device benches for the plan lifecycle (large-scale 1–1000 hooks), resolvers, relocator and providers. One binary per bench, JSON lines to stdout.

## Host (no device)

```sh
make bench                          # all host benches
make bench-plan BENCH_ARGS="--iters 500"    # 1/10/100/1000 hooks × symbol/objc/address + mixed 1000
make bench-resolvers                # export trie, macho peek, catalog 100/1000, symbol candidates
make bench-reloc                    # hk_arm64_relocate, branch near/far, has_* scans
make bench-provider                 # enumerate_backends, runtime create/release
```

Each bench prints a human line and a `{"bench":...,"mean":...}` JSON line.
Generated logs belong under `.theos/bench/`.

Sources: `tools/bench/bench_common.h` (timing/stats);
`tools/bench/bench_plan.c` uses `tests/host/fake_engines.h`.

## Device (jailbroken, HookKit.framework installed)

```sh
# 1. Build device_bench (real dyld catalog, enumerate, e2e 1/10/100)
make device-bench
# 2. Run on device (rm before scp is load-bearing — code-sign cache)
bash tools/bench/run_device_bench.sh mobile@10.0.1.160 --iters-e2e 100
# 3. Opt into one cold 1,000-target memory-patch sample (can take minutes)
bash tools/bench/run_device_bench.sh mobile@10.0.1.160 --include-1000
# logs to .theos/bench/device_bench_*.log

# Custom iters
bash tools/bench/run_device_bench.sh $DEVICE_SSH --iters 5000 --iters-e2e 50 --warmup 10
```

`tests/device/device_bench.c` links `HookKit.framework` like
`tests/device/device_lifecycle_smoke.c`; signposts use `os_signpost` with the
`dev.hookkit` subsystem.

## Instruments

```sh
# macOS only — Time Profiler with signposts
bash tools/bench/run_instruments.sh <UDID> --iters-e2e 20
# Linux hosts: signposts remain in device_bench output; use ssh sampling instead
```

`tools/bench/run_instruments.sh` wraps
`xcrun xctrace record --template 'Time Profiler'` (see
`Makefile:bench-instruments`). Traces are written under `.theos/bench/`.

## Adding a new bench

1. Include `tools/bench/bench_common.h`, implement a `void fn(void*)`, call `hk_bench_run`.
2. Add a `bench-foo` target in `Makefile` reusing `BENCH_CFLAGS` / `HK_PLATFORM_ENGINE_SOURCES`.

Skipped: Google Benchmark / hyperfine — use when `clock_gettime` variance proves insufficient.
