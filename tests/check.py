#!/usr/bin/env python3
"""Small public checks of observable cache behavior (not a full grader)."""

from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BIN = Path(os.environ.get("CACHELAB_BIN", str(ROOT / "cachelab")))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def trace_requests(name):
    requests = []
    for line in (ROOT / "test_tr" / name).read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            requests.append(line.split())
    return requests


def fields(line):
    parts = line.split()
    require(parts, "empty event line")
    values = {}
    for part in parts[1:]:
        key, sep, value = part.partition("=")
        require(sep and key and value, f"malformed event field: {part!r}")
        require(key not in values, f"duplicate event field: {key}")
        values[key] = value
    return parts[0], values


def run(name, pool_size, clients, allocation, eviction):
    result = subprocess.run(
        [str(BIN), str(pool_size), f"test_tr/{name}",
         str(clients), "-a", allocation, "-e", eviction],
        cwd=ROOT, capture_output=True, text=True, timeout=15,
    )
    require(result.returncode == 0,
            f"{name} ({allocation}/{eviction}, {clients} clients) exited "
            f"{result.returncode}: {result.stderr.strip()}")

    lines = result.stdout.splitlines()
    require(lines and lines[-1].startswith("STATS "),
            f"{name}: missing final STATS line")
    kind, stats = fields(lines[-1])
    require(kind == "STATS", f"{name}: last event is not STATS")

    requests = trace_requests(name)
    groups = []
    seen = set()
    for line in lines[:-1]:
        kind, data = fields(line)
        require("req" in data and "client" in data,
                f"{name}: event has no request/client ID: {line}")
        seq = int(data["req"])
        client = int(data["client"])
        require(0 <= seq < len(requests) and 0 <= client < clients,
                f"{name}: invalid event request/client ID: {line}")
        if not groups or groups[-1][0] != seq:
            require(seq not in seen, f"{name}: request {seq} events interleave")
            seen.add(seq)
            groups.append((seq, client, []))
        require(groups[-1][1] == client,
                f"{name}: request {seq} has multiple client IDs")
        groups[-1][2].append((kind, data))

    require(seen == set(range(len(requests))),
            f"{name}: missing or duplicate requests")
    if clients == 1:
        require([group[0] for group in groups] == list(range(len(requests))),
                f"{name}: single-client requests are out of trace order")

    active = {}
    hits = misses = evictions = rejects = 0
    for seq, _client, events in groups:
        request = requests[seq]
        op, key = request[:2]
        if op == "GET":
            require(len(events) == 1 and events[0][0] in ("HIT", "MISS"),
                    f"{name}: GET {seq} should emit one HIT or MISS")
            kind, data = events[0]
            require(data.get("key") == key, f"{name}: GET {seq} wrong key")
            require((kind == "HIT") == (key in active),
                    f"{name}: GET {seq} incorrect hit/miss")
            hits += kind == "HIT"
            misses += kind == "MISS"
            continue

        size = int(request[2])
        require(events[0][0] == "PUT" and
                events[0][1].get("key") == key and
                events[0][1].get("size") == str(size),
                f"{name}: PUT {seq} missing or wrong primary event")
        old_size = active.get(key)
        updated = False
        terminal = None
        freed = False
        for index, (kind, data) in enumerate(events[1:], start=1):
            if kind == "UPDATE":
                require(not updated and key in active and data.get("key") == key,
                        f"{name}: invalid UPDATE for request {seq}")
                require(data.get("old_size") == str(active[key]) and
                        data.get("new_size") == str(size),
                        f"{name}: UPDATE {seq} sizes disagree")
                del active[key]
                updated = freed = True
            elif kind == "EVICT":
                victim = data.get("key")
                require(victim in active and
                        data.get("size") == str(active[victim]),
                        f"{name}: invalid EVICT for request {seq}")
                del active[victim]
                evictions += 1
                freed = True
            elif kind == "COALESCE":
                require(freed, f"{name}: COALESCE without a freed block")
            elif kind == "ALLOC":
                require(index == len(events) - 1 and terminal is None and
                        data.get("key") == key and
                        data.get("size") == str(size) and key not in active,
                        f"{name}: invalid ALLOC for request {seq}")
                require(0 <= int(data.get("addr", "-1")) < pool_size,
                        f"{name}: ALLOC outside the pool")
                active[key] = size
                terminal = "ALLOC"
            elif kind == "REJECT":
                require(index == len(events) - 1 and terminal is None and
                        not freed and data.get("key") == key and
                        data.get("size") == str(size),
                        f"{name}: invalid REJECT for request {seq}")
                rejects += 1
                terminal = "REJECT"
            else:
                raise AssertionError(f"{name}: unexpected {kind} in PUT {seq}")

        require(terminal in ("ALLOC", "REJECT"),
                f"{name}: PUT {seq} has no result")
        if terminal == "ALLOC":
            require(updated == (old_size is not None),
                    f"{name}: PUT {seq} has incorrect UPDATE behavior")
        else:
            require(active.get(key) == old_size,
                    f"{name}: rejected PUT {seq} changed the old value")

    expected = {"hits": hits, "misses": misses,
                "evictions": evictions, "rejects": rejects}
    for field, value in expected.items():
        require(stats.get(field) == str(value),
                f"{name}: STATS {field} should be {value}")
    require(stats.get("final_occupancy") ==
            f"{sum(active.values())}/{pool_size}",
            f"{name}: final occupancy disagrees with active objects")
    return groups


def events_for(groups, seq, kind):
    return [data for group_seq, _client, events in groups
            if group_seq == seq for event_kind, data in events
            if event_kind == kind]


def main():
    for allocation in ("first", "best"):
        for eviction in ("fifo", "clock"):
            run("basic.trace", 1024, 1, allocation, eviction)
            groups = run("frag.trace", 1024, 1, allocation, eviction)
            require(any(kind == "COALESCE" for _seq, _cid, events in groups
                        for kind, _data in events),
                    "frag.trace should produce a pool merge")

    run("empty.trace", 1024, 1, "first", "fifo")
    run("exact_max.trace", 1024, 1, "first", "fifo")
    run("tiny_remainder.trace", 1024, 1, "first", "fifo")
    groups = run("oversized_update.trace", 1024, 1, "first", "clock")
    require(events_for(groups, 2, "REJECT") and
            events_for(groups, 3, "HIT"),
            "oversized replacement must preserve the existing key")

    fifo = run("clock_test.trace", 1024, 1, "first", "fifo")
    clock = run("clock_test.trace", 1024, 1, "first", "clock")
    require([e["key"] for seq in (4, 6) for e in events_for(fifo, seq, "EVICT")]
            == ["A", "B"], "FIFO victims should be A, then B")
    require([e["key"] for seq in (4, 6) for e in events_for(clock, seq, "EVICT")]
            == ["A", "C"], "clock victims should be A, then C")

    first = run("placement.trace", 1024, 1, "first", "fifo")
    best = run("placement.trace", 1024, 1, "best", "fifo")
    initial = events_for(first, 0, "ALLOC")[0]["addr"]
    require(events_for(first, 5, "ALLOC")[0]["addr"] == initial and
            events_for(best, 7, "ALLOC")[0]["addr"] == initial and
            events_for(best, 5, "ALLOC")[0]["addr"] != initial,
            "first-fit uses A's old block; best-fit saves it for N")

    for clients in (2, 4, 8):
        run("multi.trace", 1024, clients, "best", "clock")
        run("stress.trace", 1024, clients, "best", "clock")
    print("cache checks passed")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"cache check failed: {error}", file=sys.stderr)
        sys.exit(1)
