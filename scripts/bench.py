#!/usr/bin/env python3
"""make bench의 절차입니다. 생성, 적재, 개수 확인, INDEX 대 SCAN, 노드 크기 순서로 실행합니다.

사용법: python3 scripts/bench.py <측정 실행 파일 폴더> <record 수> <시드> <노드 크기>...

측정마다 임시 폴더의 빈 users 테이블에서 시작하고, 끝나면 그 폴더를 지웁니다.
적재는 Phase 1의 CLI(-q)가 일반 INSERT 경로로 합니다. 하나라도 틀리면 종료 코드 1입니다.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENERATOR = os.path.join(ROOT, "scripts", "generate-users.py")
USERS_SCHEMA = "name TEXT\nemail TEXT\nage INT\n"
COUNT_META = re.compile(rb"^-- (\d+) records?, SCAN, examined (\d+),")


def run_checked(args, **kwargs):
    proc = subprocess.run(args, **kwargs)
    if proc.returncode != 0:
        sys.exit(f"bench: {' '.join(args)} failed with {proc.returncode}")
    return proc


def main(argv):
    if len(argv) < 5:
        print(__doc__, file=sys.stderr)
        return 2
    bin_dir, records, seed, node_sizes = argv[1], int(argv[2]), argv[3], argv[4:]
    cli = os.path.join(bin_dir, "mini-dbms")
    work_dir = tempfile.mkdtemp(prefix="mini-dbms-bench-")
    try:
        sql_path = os.path.join(work_dir, "users.sql")
        started = time.monotonic()
        run_checked([sys.executable, "-B", GENERATOR, str(records), seed, sql_path])
        print(f"generate: {records} records, seed {seed}, "
              f"{os.path.getsize(sql_path)} bytes, {time.monotonic() - started:.2f} s")

        data_dir = os.path.join(work_dir, "data")
        os.mkdir(data_dir)
        with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as f:
            f.write(USERS_SCHEMA)
        open(os.path.join(data_dir, "users.tbl"), "wb").close()
        started = time.monotonic()
        run_checked([cli, "--data", data_dir, "-q", sql_path])
        print(f"load: CLI -q, {time.monotonic() - started:.2f} s, table file "
              f"{os.path.getsize(os.path.join(data_dir, 'users.tbl'))} bytes")

        count_path = os.path.join(work_dir, "count.sql")
        with open(count_path, "w", encoding="utf-8") as f:
            f.write("SELECT * FROM users;\n")
        proc = run_checked([cli, "--data", data_dir, count_path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        match = COUNT_META.match(proc.stderr)
        if match is None or int(match.group(1)) != records:
            sys.exit(f"bench: SELECT counted {proc.stderr!r}, expected {records}")
        print(f"count: SELECT * returned {match.group(1).decode()} records")

        sys.stdout.flush()
        run_checked([os.path.join(bin_dir, "bench"), "engine", data_dir, str(records)])
        for size in node_sizes:
            sys.stdout.flush()
            run_checked([os.path.join(bin_dir, f"bench-{size}"), "nodes", str(records)])
    finally:
        shutil.rmtree(work_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
