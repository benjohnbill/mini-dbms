#!/usr/bin/env python3
"""make bench-cursor의 절차입니다. 부분 하나의 크기(서버의 RESULT_RECORDS_MAX)마다
SELECT 부분 하나의 응답 시간, 전체를 커서로 받는 시간, 서버의 최대 메모리를 잽니다.

사용법: python3 scripts/bench-cursor.py <측정 실행 파일 폴더> <record 수> <부분 크기>...

부분 크기 n마다 <폴더>/server-<n>을 씁니다. 테이블 파일은 scripts/generate-users.py와
같은 모양(name user<7자리>, email user<7자리>@example.com)의 record를 바로 씁니다.
서버는 크기마다 두 번 띄웁니다. 한 번은 작은 질의 하나만 보내 기준 메모리를 재고, 한 번은
부분을 받습니다. 메모리는 서버가 끝난 뒤 wait4가 돌려주는 최대 RSS입니다. 틀린 결과가
하나라도 있으면 종료 코드 1입니다.
"""

import http.client
import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time

HOST = "127.0.0.1"
FIRST_PART_REPEATS = 20
START_TIMEOUT = 30.0
REQUEST_TIMEOUT = 120.0
SQL = b"SELECT * FROM users;"


def users_record_bytes(new_id):
    body = new_id.to_bytes(8, "little", signed=True)
    for text in (f"user{new_id:07d}", f"user{new_id:07d}@example.com"):
        encoded = text.encode()
        body += len(encoded).to_bytes(4, "little") + encoded
    body += (new_id % 100).to_bytes(8, "little", signed=True)
    return len(body).to_bytes(4, "little") + body


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


def server_start(binary, data_dir):
    port = free_port()
    process = subprocess.Popen([binary, "--data", data_dir, "--port", str(port)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + START_TIMEOUT
    while time.monotonic() < deadline:
        try:
            socket.create_connection((HOST, port), timeout=0.2).close()
            return process, port
        except OSError:
            time.sleep(0.02)
    process.kill()
    process.wait()
    sys.exit(f"bench-cursor: {binary} did not start")


def server_stop(process):
    """서버를 끄고 최대 RSS를 바이트로 돌려줍니다."""
    process.send_signal(signal.SIGTERM)
    _, _, usage = os.wait4(process.pid, 0)
    process.returncode = 0
    # Linux의 ru_maxrss는 KiB, macOS는 바이트입니다.
    return usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024


def post(port, path, body):
    """(걸린 초, 응답 바이트 수, JSON)을 돌려줍니다."""
    connection = http.client.HTTPConnection(HOST, port, timeout=REQUEST_TIMEOUT)
    try:
        started = time.perf_counter()
        connection.request("POST", path, body=body)
        response = connection.getresponse()
        raw = response.read()
        elapsed = time.perf_counter() - started
        if response.status != 200:
            sys.exit(f"bench-cursor: {path} returned {response.status}: {raw[:200]!r}")
        return elapsed, len(raw), json.loads(raw)
    finally:
        connection.close()


def measure(binary, data_dir, records, part_size):
    process, port = server_start(binary, data_dir)
    try:
        post(port, "/query", b"SELECT * FROM users WHERE id = 1;")
    finally:
        base_rss = server_stop(process)

    process, port = server_start(binary, data_dir)
    try:
        first_times = []
        for _ in range(FIRST_PART_REPEATS):
            elapsed, first_bytes, body = post(port, "/query", SQL)
            first_times.append(elapsed)
        if len(body["records"]) != min(part_size, records):
            sys.exit(f"bench-cursor: first part has {len(body['records'])} records")
        part_times = []
        seen = 0
        path = "/query"
        started = time.perf_counter()
        while True:
            elapsed, _, body = post(port, path, SQL)
            part_times.append(elapsed)
            for record in body["records"]:
                seen += 1
                if record[0] != seen:
                    sys.exit(f"bench-cursor: record {seen} came as id {record[0]}")
            if "next_cursor" not in body:
                break
            path = f"/query?cursor={body['next_cursor']}"
        total = time.perf_counter() - started
    finally:
        peak_rss = server_stop(process)
    if seen != records:
        sys.exit(f"bench-cursor: got {seen} of {records} records")
    return {
        "part": part_size,
        "first_ms": statistics.median(first_times) * 1000,
        "first_max_ms": max(first_times) * 1000,
        "first_bytes": first_bytes,
        "parts": len(part_times),
        "part_median_ms": statistics.median(part_times) * 1000,
        "total_s": total,
        "base_mib": base_rss / (1 << 20),
        "peak_mib": peak_rss / (1 << 20),
    }


def main(argv):
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    bin_dir, records, part_sizes = argv[1], int(argv[2]), [int(size) for size in argv[3:]]
    data_dir = tempfile.mkdtemp(prefix="mini-dbms-bench-cursor-")
    try:
        with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as schema:
            schema.write("name TEXT\nemail TEXT\nage INT\n")
        with open(os.path.join(data_dir, "users.tbl"), "wb") as table:
            for new_id in range(1, records + 1):
                table.write(users_record_bytes(new_id))
        print(f"records {records}, table file {os.path.getsize(os.path.join(data_dir, 'users.tbl'))} bytes")
        print("| 부분 크기 | 첫 부분 중앙값 ms | 첫 부분 최대 ms | 첫 부분 응답 바이트 | 부분 수 | "
              "부분 중앙값 ms | 전체 s | 기준 RSS MiB | 최대 RSS MiB | 차이 MiB |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for part_size in part_sizes:
            row = measure(os.path.join(bin_dir, f"server-{part_size}"), data_dir, records, part_size)
            print(f"| {row['part']} | {row['first_ms']:.1f} | {row['first_max_ms']:.1f} | "
                  f"{row['first_bytes']} | {row['parts']} | {row['part_median_ms']:.1f} | "
                  f"{row['total_s']:.2f} | {row['base_mib']:.1f} | {row['peak_mib']:.1f} | "
                  f"{row['peak_mib'] - row['base_mib']:.1f} |", flush=True)
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
