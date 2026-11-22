# VM validation runbook — FastNetMon 10M parallel recalc (8-core / 32 GB)

You run these on your VM; paste the results back and I'll analyse each stage and decide whether it's
safe to proceed. Everything stays local — no commits/pushes. Work stage by stage; stop on any failure.

## Files to copy to the VM (into one folder, e.g. `~/fnm-validate/`)
- `fastnetmon_10M_parallel.patch` — the full change set vs upstream base `751a596`.
- `vm_validate_master.sh` — the driver (env + init + scaling + correctness).
- `equiv_concurrency_test.cpp` — seq-vs-parallel equivalence + sanitizer target.
- `regression_detection.sh` — detection parity (parallel ON vs OFF).
- `netflow_gen.py` — traffic generator used by the detection test.

## 0. Prerequisites (once)
```bash
# tooling for richer stats + sanitizers (Debian/Ubuntu; adjust for your distro)
sudo apt-get update && sudo apt-get install -y sysstat numactl htop g++ python3 git cmake
```
You need FastNetMon's build dependencies too (boost, log4cpp, abseil, grpc, …). Use the same method
you used before to build the daemon on this VM.

## 1. Get the code with the 10M changes
Option A — you already have the repo on the VM:
```bash
cd /path/to/fastnetmon
git fetch --all
git checkout 751a5965bda6a9efba781511063b768039bf2010   # the base the patch is cut against
git checkout -b vm-10M-validate
git apply --3way /path/to/fastnetmon_10M_parallel.patch   # applies committed+uncommitted work
```
Option B — fresh clone:
```bash
git clone <your fastnetmon remote> fastnetmon && cd fastnetmon
git checkout 751a5965bda6a9efba781511063b768039bf2010
git checkout -b vm-10M-validate
git apply --3way /path/to/fastnetmon_10M_parallel.patch
```
`git apply --stat /path/to/fastnetmon_10M_parallel.patch` first to preview; it touches 5 files under
`src/`. If `git apply` complains, tell me the output — don't force it.

## 2. Build the daemon
Build the way you normally do on this VM, producing `src/build_own/fastnetmon`. Typical:
```bash
cd src && mkdir -p build_own && cd build_own
cmake .. -DLINK_WITH_ABSL=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --target fastnetmon -j"$(nproc)"
ls -la fastnetmon      # expect a ~5-6 MB binary
```
Record the exact `cmake` line and compiler version (`g++ --version`) for the report.

## 3. Run the validation (as root — binds :2055, writes /etc, /var/log)
```bash
cd ~/fnm-validate
export FNM_SRC=/path/to/fastnetmon
export FNM_BIN=$FNM_SRC/src/build_own/fastnetmon
export TESTS_DIR=~/fnm-validate
chmod +x vm_validate_master.sh regression_detection.sh

# Stage 2 — environment (also confirms 8 dedicated cores via a burn test)
sudo -E bash vm_validate_master.sh env

# Stage 4-5 — host init 1M/5M/10M via the REAL allocator (verifies count, time, RSS)
sudo -E bash vm_validate_master.sh init

# Stage 6 — recalc scalability: 1M/5M/10M x {conntrack off,on} x {1,2,4,8 workers}
#           (ban/detection ON throughout; warmup + >=5 measured samples each)
sudo -E bash vm_validate_master.sh scale

# Stage 10 core — correctness: equivalence, ASan/UBSan, counter-class TSan, perf test, detection parity
sudo -E bash vm_validate_master.sh correctness
```
Or everything in order: `sudo -E bash vm_validate_master.sh all`.

Results land in `./vm_results_<timestamp>/`: `environment.txt`, `scaling.csv`, per-run daemon logs,
and `SUMMARY.txt` (human-readable, includes the speedup/efficiency table and per-shard distribution).
**Send me `SUMMARY.txt` and `scaling.csv`.**

### What each stage proves (and what to watch)
- **env**: `burn threads=1..8 wall_ms` should stay ~flat to 8 → 8 real cores. If t8 ≈ 2× t4, the VM is
  shared/contended — tell me, it changes interpretation.
- **init**: `hosts=` must equal the configured count (1,048,576 / 5,242,880 / 10,485,760), `init_s`
  reasonable, `rss_hwm_mb` sane (10M conntrack-off ≈ 5–6 GB; conntrack-on more). No "Can't allocate".
- **scale**: the `SPEEDUP & EFFICIENCY` block is the headline. Targets: ≥3× at 4 workers, ideally ≥6×
  at 8; 10M recalc_min < 1000 ms. Each row also logs `shard_dist` — at ≥2 workers expect 64 shards,
  **cv < ~0.05 and empty_shards=0** (proves the hash spreads real keys).
- **correctness**: equivalence `byte-mismatches=0`; ASan/TSan `0`; perf test `0 misplaced`; detection
  `IDENTICAL DETECTION -> REGRESSION PASS`.

## 4. Stages I'll direct interactively after the above pass (not in the one-shot script)
These are longer/riskier, so we do them deliberately once Stage 6 looks right:
- **Stage 7 — real ingestion under load**: drive `netflow_gen.py` from a *second* VM (so the FNM VM's
  CPU numbers stay clean) while recalc runs; measure loss, contention, detection under load. I'll give
  exact generator commands sized to your result.
- **Stage 8 — stress**: step host count/rate up to the edge; I'll give safe stop thresholds (we never
  intentionally exhaust the 32 GB).
- **Stage 9 — soak**: 1 h then 4 h at 10M; watch RSS/latency drift for leaks. I'll give a monitor one-liner.
- **Stage 10 full — daemon TSan under traffic** for the conntrack sharding.

## 5. Optional Stage 3 — original vs modified A/B
To compare against the pre-dense implementation on identical config:
```bash
# build the "original" (committed sharding only, no dense/hash/conntrack fixes)
git stash   # or: git checkout the pre-patch state in a second worktree
# build into src/build_own_orig/, then point FNM_BIN at it and run `scale` into a separate OUT dir
```
Tell me if you want this; I'll give exact steps so the two builds use identical flags.

## Safety
- The script kills only `fastnetmon` processes it starts, writes only `/etc/networks_list`,
  `/etc/networks_whitelist`, `/var/log/fastnetmon.log`, `/tmp/*`, and its results dir. It does not
  touch your repo or other configs.
- If any run prints `DAEMON DIED` or a non-zero sanitizer count, it stops that line and records the
  tail — send it and I'll root-cause before we continue.
