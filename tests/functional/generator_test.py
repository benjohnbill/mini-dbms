#!/usr/bin/env python3
"""측정용 SQL 생성 스크립트를 실행하고, 만든 파일을 CLI로 넣어 확인합니다.

사용법: python3 tests/functional/generator_test.py <CLI 경로>
"""

import os
import shutil
import subprocess
import sys
import tempfile

CLI = os.path.abspath(sys.argv[1])
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GENERATOR = os.path.join(ROOT, "scripts", "generate-users.py")
USERS_SCHEMA = "name TEXT\nemail TEXT\nage INT\n"


def generate(records, seed, out_path):
    proc = subprocess.run(
        [sys.executable, "-B", GENERATOR, str(records), str(seed), out_path],
        capture_output=True)
    assert proc.returncode == 0, proc.stderr
    with open(out_path, "rb") as f:
        return f.read()


def make_data_dir():
    data_dir = tempfile.mkdtemp(prefix="mini-dbms-gen-")
    with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write(USERS_SCHEMA)
    open(os.path.join(data_dir, "users.tbl"), "wb").close()
    return data_dir


def run_cli(data_dir, sql_path, quiet=False):
    args = [CLI, "--data", data_dir] + (["-q"] if quiet else []) + [sql_path]
    return subprocess.run(args, capture_output=True)


def run_sql(data_dir, sql):
    sql_path = os.path.join(data_dir, "query.sql")
    with open(sql_path, "w", encoding="utf-8") as f:
        f.write(sql)
    return run_cli(data_dir, sql_path)


def same_seed_makes_same_bytes():
    work_dir = tempfile.mkdtemp(prefix="mini-dbms-gen-")
    try:
        first = generate(1000, 7, os.path.join(work_dir, "first.sql"))
        second = generate(1000, 7, os.path.join(work_dir, "second.sql"))
        assert first == second
        assert first.count(b"\n") == 1000, first.count(b"\n")
    finally:
        shutil.rmtree(work_dir)


def other_seed_changes_only_ages():
    work_dir = tempfile.mkdtemp(prefix="mini-dbms-gen-")
    try:
        first = generate(1000, 7, os.path.join(work_dir, "first.sql"))
        other = generate(1000, 8, os.path.join(work_dir, "other.sql"))
        assert first != other
        first_names = [line.rsplit(b",", 1)[0] for line in first.splitlines()]
        other_names = [line.rsplit(b",", 1)[0] for line in other.splitlines()]
        assert first_names == other_names
    finally:
        shutil.rmtree(work_dir)


def numbered_name_and_email_follow_insert_order():
    work_dir = tempfile.mkdtemp(prefix="mini-dbms-gen-")
    try:
        lines = generate(3, 1, os.path.join(work_dir, "users.sql")).splitlines()
        assert len(lines) == 3, lines
        assert lines[0].startswith(
            b"INSERT INTO users VALUES ('user0000001', "
            b"'user0000001@example.com', "), lines[0]
        assert lines[2].startswith(
            b"INSERT INTO users VALUES ('user0000003', "
            b"'user0000003@example.com', "), lines[2]
        for line in lines:
            assert line.endswith(b");"), line
    finally:
        shutil.rmtree(work_dir)


def loaded_file_counts_all_and_id_and_email_find_same_record():
    data_dir = make_data_dir()
    try:
        sql_path = os.path.join(data_dir, "users.sql")
        generate(2000, 3, sql_path)
        proc = run_cli(data_dir, sql_path, quiet=True)
        assert proc.returncode == 0, proc.stderr
        assert proc.stderr == b"", proc.stderr

        proc = run_sql(data_dir, "SELECT * FROM users;\n")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout.count(b"\n") == 2001, proc.stdout.count(b"\n")
        assert proc.stderr.startswith(b"-- 2000 records, SCAN, examined 2000,"), proc.stderr

        proc = run_sql(data_dir, "SELECT * FROM users WHERE id = 1500;\n")
        assert proc.returncode == 0, proc.stderr
        by_id = proc.stdout
        assert proc.stderr.startswith(b"-- 1 record, INDEX, examined 1,"), proc.stderr
        proc = run_sql(data_dir,
                       "SELECT * FROM users WHERE email = 'user0001500@example.com';\n")
        assert proc.returncode == 0, proc.stderr
        assert proc.stderr.startswith(b"-- 1 record, SCAN, examined 2000,"), proc.stderr
        assert proc.stdout == by_id, (proc.stdout, by_id)
        assert by_id.splitlines()[1].startswith(
            b"1500\tuser0001500\tuser0001500@example.com\t"), by_id
    finally:
        shutil.rmtree(data_dir)


TESTS = [
    ("[T142] 같은 시드로 두 번 만든 SQL 파일은 바이트까지 같다",
     same_seed_makes_same_bytes),
    ("시드가 다르면 age만 달라지고 name과 email은 같다",
     other_seed_changes_only_ages),
    ("name과 email은 INSERT 순서의 번호를 7자리로 담는다",
     numbered_name_and_email_follow_insert_order),
    ("[T143] 만든 SQL 파일을 CLI -q로 넣으면 SELECT가 모든 record를 세고, "
     "같은 번호의 id 조회(INDEX)와 email 조회(SCAN)가 같은 record를 돌려준다",
     loaded_file_counts_all_and_id_and_email_find_same_record),
]


def main():
    failed = 0
    for name, test in TESTS:
        try:
            test()
            print(f"Test {name}... [ OK ]")
        except AssertionError as error:
            failed += 1
            print(f"Test {name}... [ FAILED ]\n  {error!r}")
    if failed:
        print(f"FAILED: {failed} of {len(TESTS)} tests failed.")
        return 1
    print("SUCCESS: No functional tests have failed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
