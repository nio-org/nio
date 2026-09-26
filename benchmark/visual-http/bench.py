#!/usr/bin/env python3
"""Load benchmark for one HTTP server target, with a growing connection count.

Starts the target server, which binds port 0 and prints its port. Then runs
a schedule of stages. Each stage holds N concurrent keep-alive connections for
a fixed duration, and N grows from stage to stage. Each connection is a closed
loop: send GET, read the full response, repeat. During the load, the parent
samples the CPU% and RSS of the server process through psutil.

The client is Python, so it is the same bottleneck for every target. Thus the
comparison stays fair where the client sets the ceiling. To raise the ceiling,
connections are spread across worker processes, each with its own asyncio loop.

Output: results/<target>.json with per-second series and per-stage aggregates,
consumed by report.py.
"""

import argparse
import asyncio
import json
import multiprocessing as mp
import os
import pickle
import re
import resource
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

TARGETS = {
    "nio": {"label": "Nio", "cmd": [os.path.join(HERE, "bin", "server-nio")]},
    "go": {"label": "Go", "cmd": [os.path.join(HERE, "bin", "server-go")]},
    "go1": {
        "label": "Go (1 core)",
        "cmd": [os.path.join(HERE, "bin", "server-go")],
        "env": {"GOMAXPROCS": "1"},
    },
    "node": {"label": "Node.js", "cmd": ["node", os.path.join(HERE, "servers", "server.js")]},
    "bun": {"label": "Bun", "cmd": ["bun", os.path.join(HERE, "servers", "server.bun.js")]},
    "java": {"label": "Java", "cmd": ["java", "-cp", os.path.join(HERE, "bin", "java"), "Server"]},
}


def build_json_doc(min_bytes=50 * 1024):
    """A deterministic ~50 KB document for the POST scenario. Adds items until
    the encoded form is larger than min_bytes."""
    items = []
    i = 0
    while True:
        i += 1
        items.append({
            "id": i,
            "name": f"item-{i:06d}",
            "price": round(1 + (i * 37 % 9000) / 100, 2),
            "qty": i % 7 + 1,
            "active": i % 3 == 0,
        })
        if i % 50 == 0:
            doc = json.dumps({"items": items}, separators=(",", ":"))
            if len(doc) >= min_bytes:
                return doc.encode()


def scenarios():
    return {
        "plaintext": {"label": "GET /plaintext", "method": "GET",
                      "path": "/plaintext", "body": None},
        "json": {"label": "POST /data, 50 KB JSON", "method": "POST",
                 "path": "/data", "body": build_json_doc()},
    }


def build_request(sc):
    head = (f"{sc['method']} {sc['path']} HTTP/1.1\r\n"
            f"Host: 127.0.0.1\r\nConnection: keep-alive\r\n")
    if sc["body"] is None:
        return (head + "\r\n").encode()
    return (head + "Content-Type: application/json\r\n"
            f"Content-Length: {len(sc['body'])}\r\n\r\n").encode() + sc["body"]


def raise_fd_limit():
    """Raises the file descriptor limit. The client workers and the server,
    which inherits the limit, need more than the macOS default soft limit of
    256."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    for want in (65536, 10240, 8192):
        if hard != resource.RLIM_INFINITY and want > hard:
            continue
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
            return want
        except (ValueError, OSError):
            continue
    return soft


# ---------------------------------------------------------------- worker side

async def _read_response(reader):
    """Reads one HTTP/1.1 response framed by Content-Length. All servers in
    this benchmark send Content-Length. Returns (ok, keep_alive)."""
    header = await reader.readuntil(b"\r\n\r\n")
    low = header.lower()
    i = low.find(b"content-length:")
    if i < 0:
        raise ValueError("no content-length")
    j = low.index(b"\r\n", i)
    n = int(low[i + 15 : j])
    if n:
        await reader.readexactly(n)
    ok = header.startswith(b"HTTP/1.1 2")
    keep = b"connection: close" not in low
    return ok, keep


async def _connection(host, port, request, timeout, buckets, t0):
    reader = writer = None
    try:
        while True:
            try:
                if writer is None:
                    reader, writer = await asyncio.wait_for(
                        asyncio.open_connection(host, port), timeout
                    )
                start = time.perf_counter()
                writer.write(request)
                await writer.drain()
                ok, keep = await asyncio.wait_for(_read_response(reader), timeout)
                dt_ms = (time.perf_counter() - start) * 1000.0
                sec = int(time.time() - t0)
                b = buckets.get(sec)
                if b is None:
                    b = buckets[sec] = [0, 0, []]
                if ok:
                    b[0] += 1
                    b[2].append(dt_ms)
                else:
                    b[1] += 1
                if not keep:
                    writer.close()
                    reader = writer = None
            except asyncio.CancelledError:
                raise
            except Exception:
                sec = int(time.time() - t0)
                b = buckets.get(sec)
                if b is None:
                    b = buckets[sec] = [0, 0, []]
                b[1] += 1
                if writer is not None:
                    writer.close()
                    reader = writer = None
                await asyncio.sleep(0.05)
    finally:
        if writer is not None:
            writer.close()


async def _worker_async(wid, nworkers, host, port, request, stages, t0, timeout, buckets):
    tasks = []
    elapsed = 0.0
    for conns, dur in stages:
        share = conns // nworkers + (1 if wid < conns % nworkers else 0)
        while len(tasks) < share:
            tasks.append(
                asyncio.ensure_future(
                    _connection(host, port, request, timeout, buckets, t0)
                )
            )
        while len(tasks) > share:
            tasks.pop().cancel()
        elapsed += dur
        await asyncio.sleep(max(0.0, (t0 + elapsed) - time.time()))
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)


def worker_main(wid, nworkers, host, port, request, stages, t0, timeout, out_path):
    raise_fd_limit()
    buckets = {}
    try:
        asyncio.run(
            _worker_async(wid, nworkers, host, port, request, stages, t0, timeout, buckets)
        )
    except KeyboardInterrupt:
        pass
    for b in buckets.values():
        b[2].sort()
    with open(out_path, "wb") as f:
        pickle.dump(buckets, f, protocol=pickle.HIGHEST_PROTOCOL)


# ---------------------------------------------------------------- parent side

def percentile(sorted_vals, q):
    if not sorted_vals:
        return None
    i = min(len(sorted_vals) - 1, int(q / 100.0 * len(sorted_vals)))
    return sorted_vals[i]


def start_server(target):
    spec = TARGETS[target]
    env = dict(os.environ)
    env.update(spec.get("env", {}))
    proc = subprocess.Popen(
        spec["cmd"], stdout=subprocess.PIPE, stderr=sys.stderr, env=env, text=True
    )
    line = proc.stdout.readline().strip()
    if not line:
        proc.terminate()
        raise RuntimeError(f"{target}: server exited without printing a port")
    # Bun writes ANSI color escapes also to a pipe. Remove them.
    line = ANSI_RE.sub("", line).strip()
    try:
        port = int(line)
    except ValueError:
        proc.terminate()
        raise RuntimeError(f"{target}: expected a port on stdout, got {line!r}")
    return proc, port


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--target", required=True, choices=sorted(TARGETS))
    ap.add_argument("--scenario", default="plaintext", choices=["plaintext", "json"])
    ap.add_argument("--stages", default="10,25,50,100,250,500,1000",
                    help="comma-separated concurrent connection counts")
    ap.add_argument("--stage-duration", type=float, default=10.0,
                    help="seconds each stage holds its connection count")
    ap.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1),
                    help="client worker processes")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--idle-tail", type=float, default=10.0,
                    help="seconds of zero load after the last stage, so the "
                         "memory series shows what the runtime gives back")
    ap.add_argument("--results-dir", default=os.path.join(HERE, "results"))
    args = ap.parse_args()

    import psutil  # after argparse, so --help works without psutil

    raise_fd_limit()
    conn_counts = [int(s) for s in args.stages.split(",") if s.strip()]
    stages = [(c, args.stage_duration) for c in conn_counts]
    total = sum(d for _, d in stages)
    sc = scenarios()[args.scenario]
    request = build_request(sc)

    server, port = start_server(args.target)
    label = TARGETS[args.target]["label"]
    print(f"[{args.target}/{args.scenario}] serving on 127.0.0.1:{port}, "
          f"stages {conn_counts} x {args.stage_duration:g}s", flush=True)
    try:
        ps = psutil.Process(server.pid)
        ps.cpu_percent(None)  # the first call sets the base for the next delta

        t0 = time.time() + 0.5
        tmpdir = tempfile.mkdtemp(prefix="visual-http-")
        outs, procs = [], []
        ctx = mp.get_context("spawn")
        for w in range(args.workers):
            out = os.path.join(tmpdir, f"w{w}.pickle")
            outs.append(out)
            p = ctx.Process(
                target=worker_main,
                args=(w, args.workers, "127.0.0.1", port, request,
                      stages, t0, args.timeout, out),
            )
            p.start()
            procs.append(p)

        samples = []  # (t_rel, cpu_pct, rss_bytes)
        while any(p.is_alive() for p in procs):
            time.sleep(0.5)
            try:
                cpu = ps.cpu_percent(None)
                rss = ps.memory_info().rss
            except psutil.NoSuchProcess:
                break
            t_rel = time.time() - t0
            if 0 <= t_rel <= total + 2:
                samples.append((t_rel, cpu, rss))
        for p in procs:
            p.join(timeout=30)
            if p.is_alive():
                p.terminate()

        # The idle tail: sampling continues with no connections. It shows
        # if the runtime gives back its memory.
        idle_until = t0 + total + args.idle_tail
        while time.time() < idle_until:
            time.sleep(0.5)
            try:
                cpu = ps.cpu_percent(None)
                rss = ps.memory_info().rss
            except psutil.NoSuchProcess:
                break
            samples.append((time.time() - t0, cpu, rss))

        merged = {}
        for out in outs:
            with open(out, "rb") as f:
                for sec, (count, errs, lats) in pickle.load(f).items():
                    m = merged.get(sec)
                    if m is None:
                        merged[sec] = [count, errs, lats]
                    else:
                        m[0] += count
                        m[1] += errs
                        m[2].extend(lats)
            os.unlink(out)
        for m in merged.values():
            m[2].sort()
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()

    # Seconds past the last stage are the idle tail, with no connections.
    def conns_at(sec):
        e = 0.0
        for c, d in stages:
            e += d
            if sec < e:
                return c
        return 0

    nsec = int(total + args.idle_tail)
    per_second = {"t": [], "conns": [], "rps": [], "p50": [], "p99": [],
                  "cpu": [], "rss_mb": [], "errors": []}
    cpu_by_sec, rss_by_sec = {}, {}
    for t_rel, cpu, rss in samples:
        cpu_by_sec.setdefault(int(t_rel), []).append(cpu)
        rss_by_sec[int(t_rel)] = rss
    for sec in range(nsec):
        count, errs, lats = merged.get(sec, [0, 0, []])
        per_second["t"].append(sec)
        per_second["conns"].append(conns_at(sec))
        per_second["rps"].append(count)
        per_second["p50"].append(percentile(lats, 50))
        per_second["p99"].append(percentile(lats, 99))
        cs = cpu_by_sec.get(sec)
        per_second["cpu"].append(round(sum(cs) / len(cs), 1) if cs else None)
        rss = rss_by_sec.get(sec)
        per_second["rss_mb"].append(round(rss / 1048576, 2) if rss else None)
        per_second["errors"].append(errs)

    # Per-stage aggregates. Drop the first second of each stage, because
    # connections are still opening then.
    stages_out = []
    start = 0.0
    for c, d in stages:
        secs = range(int(start) + 1, int(start + d))
        lats, count, errs, cpus, rsss = [], 0, 0, [], []
        for sec in secs:
            m = merged.get(sec)
            if m:
                count += m[0]
                errs += m[1]
                lats.extend(m[2])
            cs = cpu_by_sec.get(sec)
            if cs:
                cpus.extend(cs)
            if sec in rss_by_sec:
                rsss.append(rss_by_sec[sec])
        lats.sort()
        n = max(1, len(secs))
        stages_out.append({
            "conns": c,
            "rps": round(count / n, 1),
            "p50": percentile(lats, 50),
            "p95": percentile(lats, 95),
            "p99": percentile(lats, 99),
            "max": lats[-1] if lats else None,
            "cpu": round(sum(cpus) / len(cpus), 1) if cpus else None,
            "rss_mb": round(max(rsss) / 1048576, 2) if rsss else None,
            "errors": errs,
        })
        start += d

    # The first and last RSS samples of the idle tail.
    idle = None
    if args.idle_tail > 0:
        tail = [rss_by_sec[s] for s in range(int(total), nsec + 1) if s in rss_by_sec]
        if tail:
            idle = {
                "seconds": args.idle_tail,
                "rss_start_mb": round(tail[0] / 1048576, 2),
                "rss_end_mb": round(tail[-1] / 1048576, 2),
            }

    out_dir = os.path.join(args.results_dir, args.scenario)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"{args.target}.json")
    with open(out_path, "w") as f:
        json.dump({
            "target": args.target,
            "label": label,
            "meta": {
                "date": time.strftime("%Y-%m-%d %H:%M:%S"),
                "scenario": args.scenario,
                "scenario_label": sc["label"],
                "body_bytes": len(sc["body"]) if sc["body"] else 0,
                "stages": conn_counts,
                "stage_duration": args.stage_duration,
                "idle_tail": args.idle_tail,
                "workers": args.workers,
                "cpu_count": os.cpu_count(),
            },
            "per_second": per_second,
            "stages": stages_out,
            "idle": idle,
        }, f, indent=1)

    peak = max((s["rps"] for s in stages_out), default=0)
    print(f"[{args.target}/{args.scenario}] done: peak {peak:,.0f} req/s -> {out_path}",
          flush=True)


if __name__ == "__main__":
    main()
