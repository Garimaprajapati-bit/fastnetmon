# Build & test instructions (independently reproducible)

All commands assume the repo at `$FNM_SRC` (e.g. `/root/fastnetmon`) on `sharded-speed-counters`, and this
handover folder copied to the VM (it carries the tests, the fix patch, and the harness).

## A. Build the daemon (the documented way)
```bash
# deps (Ubuntu 24.04, from tests/Dockerfile.ubuntu-24.04):
apt-get install -y --no-install-recommends build-essential cmake libssl-dev capnproto libcapnp-dev \
  libelf-dev libbpf-dev libpcap-dev libgrpc-dev libgrpc++-dev libprotobuf-dev protobuf-compiler \
  libprotoc-dev protobuf-compiler-grpc libboost-dev libboost-serialization-dev libboost-thread-dev \
  libboost-regex-dev libboost-program-options-dev libmongoc-dev liblog4cpp5-dev libncurses-dev libabsl-dev

# the top-k reporting fix is the only uncommitted change; apply it if not already committed:
cd "$FNM_SRC/src" && git apply /path/to/handover/get_top_k_reporting_fix.diff   # skip if already on branch

cd "$FNM_SRC/src" && mkdir -p build_own && cd build_own
cmake .. -DLINK_WITH_ABSL=ON && make -j"$(nproc)" fastnetmon
# binary: $FNM_SRC/src/build_own/fastnetmon
```

## B. Enable the feature (opt-in)
In `/etc/fastnetmon.conf`:
```
speed_calculation_threads = 8     # 1 = original single-threaded behaviour (default)
speed_calculation_shards  = 0     # 0 = auto (threads*16); or set explicitly
```
With both absent/defaulted, the daemon behaves exactly as upstream.

## C. Run every test (one command)
```bash
FNM_SRC=/root/fastnetmon bash /path/to/handover/validate_on_vm.sh
# writes results_<timestamp>/ with all logs + a summary
```

## D. Run tests individually
```bash
# 1) correctness: equivalence (seq==parallel, byte-identical) + concurrency
g++ -O2 -std=c++20 -I "$FNM_SRC/src" -I "$FNM_SRC/src/tests" handover/equiv_concurrency_test.cpp -o /tmp/eqc -lpthread -llog4cpp && /tmp/eqc
# ThreadSanitizer:
g++ -O2 -g -std=c++20 -fsanitize=thread  <same includes> -o /tmp/eqc_tsan ... && /tmp/eqc_tsan
# ASan + UBSan:
g++ -O1 -g -std=c++20 -fsanitize=address,undefined <same includes> -o /tmp/eqc_asan ... && /tmp/eqc_asan

# 2) detection regression: parallel ON vs OFF identical (real daemon)
FNM_SRC=/root/fastnetmon bash handover/regression_detection.sh

# 3) isolated layout benchmark (1M + 10M, 1/2/4/8 threads, per-candidate RSS, perf):
FNM_SRC=/root/fastnetmon THREADS=1,2,4,8 REP=7 bash handover/run_bench.sh

# 4) integration load / stress / soak (real daemon, real NetFlow):
cd "$FNM_SRC/integration-kit"
export FNM_BIN="$FNM_SRC/src/build_own/fastnetmon" FNM_SRC SUBNET=10.0.0.0/12
./run_integration.sh setup
./run_integration.sh start 8
./run_integration.sh load   1048576 200000 70      # 1M hosts, 70s
./run_integration.sh stress 1048576 300000 600 50 500000   # with attackers
./run_integration.sh soak   10000000 2000000 28800         # 8h, needs a big VM
grep -c "Traffic was calculated in" /var/log/fastnetmon.log   # watchdog over-budget count
```

## E. Isolated-VM safety
The NetFlow generator sends only to `127.0.0.1:2055`. Run on an isolated VM; it generates no external traffic.

## F. What needs the 8-core VM (vs. what was validated in a 4-core dev container)
- 8-thread scaling numbers and 10M-host load/stress/soak need the VM (RAM + cores).
- Correctness/concurrency/equivalence and the 1M integration regression were validated in a 4-core/15 GB
  container; `validate_on_vm.sh` re-runs them on the VM and extends to 8 threads / 10M.
