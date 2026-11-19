#!/usr/bin/env python3
"""
NetFlow v5 traffic generator for FastNetMon load / stress / soak testing.

Produces byte-correct NetFlow v5 packets (matching src/netflow_plugin/netflow_v5.hpp:
24-byte header + up to 30 x 48-byte flow records, big-endian) and sends them over UDP to
the daemon's collector. Sprays the *monitored-side* IP across a subnet so FastNetMon tracks
up to millions of distinct hosts, exactly as real routers would export.

ISOLATED USE ONLY: point --host at your test daemon (127.0.0.1 or the VM's own address).
Never aim it at a real/public collector.

Examples:
  # normal load: 3M hosts in 10.0.0.0/8, ~1.5M flows/s, 2 hours
  ./netflow_gen.py --host 127.0.0.1 --subnet 10.0.0.0/8 --hosts 3000000 \
      --rate 1500000 --duration 7200
  # stress burst: add 50 "attackers" blasting way above threshold
  ./netflow_gen.py --host 127.0.0.1 --subnet 10.0.0.0/8 --hosts 3000000 \
      --rate 3000000 --attackers 50 --attacker-pps 500000 --duration 600
"""
import argparse, ipaddress, os, random, socket, struct, sys, time

HDR = struct.Struct("!HHIIIIBBH")      # 24 bytes: version,flows,uptime,time,nsec,seq,eng_t,eng_id,sampling
REC = struct.Struct("!IIIHHIIIIHHBBBBHHBBH")  # 48 bytes, matches netflow5_flow_t
MAXFLOWS = 30
PROTOS = [6, 17, 1, 47]                 # tcp, udp, icmp, gre

def build_record(src, dst, pkts, octets, sport, dport, proto, flags):
    return REC.pack(src, dst, 0, 0, 0, pkts, octets, 0, 0,
                    sport, dport, 0, flags, proto, 0, 0, 0, 0, 0, 0)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2055)
    ap.add_argument("--subnet", default="10.0.0.0/8", help="monitored network to spread hosts over")
    ap.add_argument("--hosts", type=int, default=1000000, help="distinct monitored hosts to touch")
    ap.add_argument("--rate", type=int, default=500000, help="target flows/second")
    ap.add_argument("--duration", type=int, default=120, help="seconds (0 = forever)")
    ap.add_argument("--direction", choices=["incoming", "outgoing", "both"], default="incoming")
    ap.add_argument("--pkts-per-flow", type=int, default=20)
    ap.add_argument("--bytes-per-pkt", type=int, default=800)
    ap.add_argument("--attackers", type=int, default=0, help="N hosts that exceed thresholds")
    ap.add_argument("--attacker-pps", type=int, default=200000, help="flow_packets for attacker flows")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    net = ipaddress.ip_network(args.subnet, strict=False)
    base = int(net.network_address)
    span = min(args.hosts, net.num_addresses)
    rng = random.Random(args.seed)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.host, args.port)               # connectionless: don't die if collector blips

    pkts_per_sec = max(1, args.rate // MAXFLOWS)
    interval = 1.0 / pkts_per_sec
    seq = 0
    start = time.time()
    uptime0 = int(start)
    sent_flows = 0
    last_report = start

    print(f"[gen] {args.host}:{args.port}  subnet={args.subnet} hosts={span} "
          f"rate={args.rate} flows/s  attackers={args.attackers}  duration={args.duration or '∞'}s",
          flush=True)

    try:
        while True:
            now = time.time()
            if args.duration and now - start >= args.duration:
                break
            # build one packet of up to 30 flows
            flows = []
            for _ in range(MAXFLOWS):
                host_off = rng.randrange(span)
                monitored = base + host_off
                external = 0x01000000 | rng.randrange(0x00FFFFFF)   # 1.x.x.x external peer
                proto = PROTOS[host_off & 3]
                if args.attackers and host_off < args.attackers:
                    pkts = args.attacker_pps
                    octets = pkts * 1200
                else:
                    pkts = args.pkts_per_flow
                    octets = pkts * args.bytes_per_pkt
                direction = args.direction
                if direction == "both":
                    direction = "incoming" if (host_off & 1) else "outgoing"
                if direction == "incoming":
                    src, dst = external, monitored        # traffic INTO the monitored host
                else:
                    src, dst = monitored, external        # traffic OUT of the monitored host
                flows.append(build_record(src, dst, pkts, octets,
                                          rng.randrange(1024, 65535), rng.choice([80, 443, 53, 123]),
                                          proto, 0x10))
            seq += len(flows)
            hdr = HDR.pack(5, len(flows), int((now - uptime0) * 1000) & 0xFFFFFFFF,
                           int(now), 0, seq, 0, 0, 1)
            try:
                sock.sendto(hdr + b"".join(flows), dest)
            except OSError:
                pass   # fire-and-forget: a transient "no listener" must not kill the run
            sent_flows += len(flows)

            # pace to target rate
            target = start + seq * interval * (MAXFLOWS / len(flows))
            sleep = (start + (seq / MAXFLOWS) * interval) - time.time()
            if sleep > 0:
                time.sleep(sleep)

            if now - last_report >= 10:
                rate = sent_flows / (now - start)
                print(f"[gen] t={int(now-start)}s  sent_flows={sent_flows}  avg_rate={rate:,.0f} flows/s",
                      flush=True)
                last_report = now
    except KeyboardInterrupt:
        pass
    dur = time.time() - start
    print(f"[gen] done: {sent_flows} flows in {dur:.0f}s = {sent_flows/dur:,.0f} flows/s avg", flush=True)

if __name__ == "__main__":
    main()
