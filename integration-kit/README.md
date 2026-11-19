# FastNetMon Daemon Integration / Load / Stress / Soak Kit

Real daemon, real NetFlow v5 pipeline, real per-host accounting and detection — driven by
synthetic traffic in an **isolated VM**. This covers the daemon-level rows of Pavel's checklist
(integration, load, stress, soak) that a standalone benchmark cannot.

**Safety boundary:** generate traffic only against your own test daemon on loopback / the VM's
own address. Never point the generator at a real or public collector, and keep mitigation
unconfigured (this kit runs detection in **detect-only** mode — no BGP/exabgp/gobgp — so a
"detected attack" just writes a log line, it never blackholes anything).

## Files
| File | Role | Verified |
|---|---|---|
| `netflow_gen.py` | byte-correct NetFlow v5 generator (matches `src/netflow_plugin/netflow_v5.hpp`); sprays millions of hosts; rate/burst/attacker control | ✅ tested: 300k flows/s, 0 malformed packets, ~1.2M distinct hosts |
| `run_integration.sh` | orchestrator: configure, start daemon, run phases | ✅ syntax-checked |
| `monitor.sh` | 5-sec time-series → CSV (RSS, CPU%, threads, load, UDP drops, detections, errors, crash flag) | ✅ tested, emits CSV + verdict |
| `README.md` | this runbook | |

> I verified the generator and monitor here. The **daemon phases themselves are NOT YET RUN** —
> they need the built daemon on the 16-core VM. Nothing below is claimed as passed until you run it.

## Prerequisites (on the VM)
Build the daemon the **documented in-tree way** (this is `tests/Dockerfile.ubuntu-24.04` — system deps +
`cmake -DLINK_WITH_ABSL=ON && make`; fast, and the binary lands at `src/build/fastnetmon`). This is the build
you already ran successfully — no need for the slow from-source `fastnetmon_build.pl`.
```bash
cd /root/fastnetmon && git checkout sharded-speed-counters && git pull
# deps (per tests/Dockerfile.ubuntu-24.04):
sudo apt-get update && sudo apt-get install -y build-essential git cmake libssl-dev libabsl-dev \
    libboost-all-dev liblog4cpp5-dev libcapnp-dev libpcap-dev
cd src && mkdir -p build && cd build && cmake .. -DLINK_WITH_ABSL=ON && make -j$(nproc)
# -> binary: /root/fastnetmon/src/build/fastnetmon   (point the kit at it:)
export FNM_BIN=/root/fastnetmon/src/build/fastnetmon
python3 --version   # stdlib only, no pip installs needed
# raise UDP buffers so the collector isn't the bottleneck under high rate:
sudo sysctl -w net.core.rmem_max=134217728 net.core.rmem_default=134217728
```
> The fully-authoritative packaging build is `fastnetmon_build.pl` (bundled GCC 12.1.0 + from-source deps,
> `-DBUILD_TESTS=ON`), used by CI to make release packages. It is functionally equivalent for this testing but
> takes hours to compile its toolchain, so the in-tree build above is the practical choice.

## Run order (maps to the checklist)

```bash
cd integration-kit
sudo ./run_integration.sh setup            # networks_list=10.0.0.0/8, netflow on (loopback), detect-only
sudo ./run_integration.sh start 16         # daemon with speed_calculation_threads=16

# 10. INTEGRATION — confirm the pipeline works end-to-end (60s)
./run_integration.sh smoke
#   PASS if: daemon stays up, hosts appear (fastnetmon_client shows a growing table),
#   workers run (grep log: "Parallel speed calculation enabled: 16 threads"),
#   results_smoke.csv shows non-zero CPU and a stable thread count.

# 5a. LOAD — expected sustained workload (30 min): 5M hosts, 1.5M flows/s
./run_integration.sh load 5000000 1500000 1800
#   PASS if: RSS plateaus, udp_rcvbuf_errors stays ~0 (no input drops), no log errors, no crash.

# 6. STRESS — beyond expected: higher rate + 50 attackers above threshold (10 min)
./run_integration.sh stress 5000000 3000000 600 50 500000
#   Watch for: the breaking point — rising udp_rcvbuf_errors (drops), CPU saturation,
#   detections firing for the 50 attackers (/var/log/fnm_attacks.log), RSS growth, any crash.
#   Report the rate at which drops/backlog begin — that's the practical limit.

# 7. SOAK — stability over time: 10M hosts, 2M flows/s, 2h then 8h
./run_integration.sh soak 10000000 2000000 7200     # 2h first
./run_integration.sh soak 10000000 2000000 28800    # 8h if the 2h is clean

sudo ./run_integration.sh stop
```

## What each CSV tells you (`results_<phase>.csv`)
- `rss_mb` — must **plateau**. Steady climb over a soak = leak → investigate.
- `cpu_pct` / `threads` — thread count constant at ~16 workers + daemon threads; CPU shows parallel use.
- `udp_rcvbuf_errors` — **input drops**. ~0 under load = the daemon keeps up; climbing = it's falling behind (backlog).
- `detections` — cumulative attack notifications; should rise only during stress (the attackers), not during plain load.
- `log_errors` — any `error/fatal/terminate/deadlock` in the daemon log; must stay 0.
- `alive` — flips to `DEAD` if the daemon crashes (the monitor says `CRASHED`).

## Per-phase pass criteria (bring Pavel these)
| Phase | PASS means |
|---|---|
| Integration | pipeline receives flows, tracks hosts, workers run, reporting works, no errors |
| Load | handles the expected rate with ~0 drops, bounded RSS, no errors |
| Stress | identifies the practical limit (drop onset / CPU saturation) without crashing or corrupting accounting; attackers are detected |
| Soak | RSS flat and sweep stable over 2–8h, zero crashes/deadlocks |

## Notes / tuning
- One generator process does ~300k flows/s. For higher rates run several in parallel (different `--seed`),
  or on a second VM pointed at the daemon's address.
- To push host count toward 10M, keep `--hosts 10000000` and let it run a few minutes (hosts accumulate as
  distinct IPs are first seen).
- Compare `speed_calculation_threads=1` vs `=16` across the same load to show the parallel benefit end-to-end.
- `fastnetmon_client` (run alongside) gives the live top-talkers view for a visual sanity check.
