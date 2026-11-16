#!/usr/bin/env python3
"""CLI 실행 파일을 바깥에서 실행해 stdout, 테이블 파일 바이트, 종료 코드를 확인합니다.

사용법: python3 tests/functional/cli_test.py <CLI 경로>
"""

import fcntl
import os
import shutil
import subprocess
import sys
import tempfile

CLI = os.path.abspath(sys.argv[1])
USERS_SCHEMA = "name TEXT\nemail TEXT\nage INT\n"


def make_data_dir():
    data_dir = tempfile.mkdtemp(prefix="mini-dbms-cli-")
    with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write(USERS_SCHEMA)
    open(os.path.join(data_dir, "users.tbl"), "wb").close()
    return data_dir


def run_cli(args, cwd=None):
    return subprocess.run([CLI, *args], capture_output=True, cwd=cwd)


def run_sql(data_dir, sql):
    sql_path = os.path.join(data_dir, "input.sql")
    with open(sql_path, "w", encoding="utf-8") as f:
        f.write(sql)
    return run_cli(["--data", data_dir, sql_path])


def insert_two_and_select_prints_ids_1_and_2():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('kim', 'kim@example.com', 20);\n"
                       "INSERT INTO users VALUES ('lee', 'lee@example.com', 31);\n"
                       "SELECT * FROM users;\n")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == (b"id\tname\temail\tage\n"
                               b"1\tkim\tkim@example.com\t20\n"
                               b"2\tlee\tlee@example.com\t31\n"), proc.stdout
    finally:
        shutil.rmtree(data_dir)


def insert_two_leaves_table_file_bytes_in_layout():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('kim', 'kim@example.com', 20);\n"
                       "INSERT INTO users VALUES ('lee', 'lee@example.com', 31);\n")
        assert proc.returncode == 0, proc.stderr
        with open(os.path.join(data_dir, "users.tbl"), "rb") as f:
            actual = f.read()
        # 본문 길이 42 = id 8 + name(4 + 3) + email(4 + 15) + age 8
        expected = bytes.fromhex(
            "2a000000" "0100000000000000"
            "03000000" "6b696d"
            "0f000000" "6b696d406578616d706c652e636f6d"
            "1400000000000000"
            "2a000000" "0200000000000000"
            "03000000" "6c6565"
            "0f000000" "6c6565406578616d706c652e636f6d"
            "1f00000000000000")
        assert actual == expected, actual.hex()
    finally:
        shutil.rmtree(data_dir)


def tab_newline_backslash_in_text_are_escaped():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('a\tb', 'c\nd', 1);\n"
                       "INSERT INTO users VALUES ('e\\f', 'O''Brien', 2);\n"
                       "SELECT * FROM users;")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == (b"id\tname\temail\tage\n"
                               b"1\ta\\tb\tc\\nd\t1\n"
                               b"2\te\\\\f\tO'Brien\t2\n"), proc.stdout
    finally:
        shutil.rmtree(data_dir)


def select_on_empty_table_prints_field_names_only():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir, "select * from users;")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == b"id\tname\temail\tage\n", proc.stdout
    finally:
        shutil.rmtree(data_dir)


def data_option_defaults_to_data_folder():
    work_dir = tempfile.mkdtemp(prefix="mini-dbms-cwd-")
    try:
        data_dir = make_data_dir()
        shutil.move(data_dir, os.path.join(work_dir, "data"))
        with open(os.path.join(work_dir, "input.sql"), "w", encoding="utf-8") as f:
            f.write("SELECT * FROM users;")
        proc = run_cli(["input.sql"], cwd=work_dir)
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == b"id\tname\temail\tage\n", proc.stdout
    finally:
        shutil.rmtree(work_dir)


def bad_cli_arguments_exit_2():
    for args in ([], ["--data"], ["--bogus", "x.sql"], ["a.sql", "b.sql"]):
        proc = run_cli(args)
        assert proc.returncode == 2, (args, proc.returncode)
        assert proc.stderr, args


def unreadable_sql_file_exits_1():
    data_dir = make_data_dir()
    try:
        proc = run_cli(["--data", data_dir, os.path.join(data_dir, "nope.sql")])
        assert proc.returncode == 1, proc.returncode
        assert proc.stderr
    finally:
        shutil.rmtree(data_dir)


def sql_error_exits_1():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir, "SELECT * FROM nobody;")
        assert proc.returncode == 1, proc.returncode
        assert proc.stderr
    finally:
        shutil.rmtree(data_dir)


def rerun_keeps_records_and_next_id_is_4():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('a', 'a@x', 1);\n"
                       "INSERT INTO users VALUES ('b', 'b@x', 2);\n"
                       "INSERT INTO users VALUES ('c', 'c@x', 3);\n")
        assert proc.returncode == 0, proc.stderr
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('d', 'd@x', 4);\n"
                       "SELECT * FROM users;\n")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == (b"id\tname\temail\tage\n"
                               b"1\ta\ta@x\t1\n"
                               b"2\tb\tb@x\t2\n"
                               b"3\tc\tc@x\t3\n"
                               b"4\td\td@x\t4\n"), proc.stdout
    finally:
        shutil.rmtree(data_dir)


def locked_data_dir_exits_1_until_lock_is_released():
    data_dir = make_data_dir()
    try:
        # 첫 프로세스 자리를 테스트가 맡습니다. 엔진과 같은 잠금 파일에 같은 flock을 쥡니다.
        with open(os.path.join(data_dir, ".lock"), "w") as lock_file:
            fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
            proc = run_sql(data_dir, "SELECT * FROM users;")
            assert proc.returncode == 1, proc.returncode
            assert b"in use" in proc.stderr, proc.stderr
        proc = run_sql(data_dir, "SELECT * FROM users;")
        assert proc.returncode == 0, proc.stderr
    finally:
        shutil.rmtree(data_dir)


def dump_prints_same_tsv_as_select_star():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('a\tb', 'O''Brien', 1);\n"
                       "INSERT INTO users VALUES ('c', 'd\ne', -2);\n")
        assert proc.returncode == 0, proc.stderr
        select = run_sql(data_dir, "SELECT * FROM users;")
        dump = run_cli(["--data", data_dir, "dump", "users"])
        assert dump.returncode == 0, dump.stderr
        assert dump.stdout == select.stdout, (dump.stdout, select.stdout)
        assert dump.stdout.count(b"\n") == 3, dump.stdout
    finally:
        shutil.rmtree(data_dir)


def dump_of_unknown_table_exits_1():
    data_dir = make_data_dir()
    try:
        proc = run_cli(["--data", data_dir, "dump", "nobody"])
        assert proc.returncode == 1, proc.returncode
        assert proc.stderr
    finally:
        shutil.rmtree(data_dir)


TESTS = [
    ("[T1] 빈 데이터 폴더에서 INSERT 두 개와 SELECT *는 ID 1, 2인 record를 TSV로 출력한다",
     insert_two_and_select_prints_ids_1_and_2),
    ("[T2] INSERT 두 개 뒤 테이블 파일의 바이트가 레이아웃과 같다",
     insert_two_leaves_table_file_bytes_in_layout),
    ("[T3] TEXT 안의 탭, 줄바꿈, 역슬래시는 \\t, \\n, \\\\로 출력된다",
     tab_newline_backslash_in_text_are_escaped),
    ("빈 테이블의 SELECT *는 field 이름 줄만 출력한다",
     select_on_empty_table_prints_field_names_only),
    ("--data를 생략하면 data/ 폴더를 쓴다", data_option_defaults_to_data_folder),
    ("[T4] 잘못된 CLI 인자는 종료 코드 2로 끝난다", bad_cli_arguments_exit_2),
    ("[T5] 열 수 없는 SQL 파일은 종료 코드 1로 끝난다", unreadable_sql_file_exits_1),
    ("SQL 실행 오류는 종료 코드 1로 끝난다", sql_error_exits_1),
    ("INSERT 세 개 뒤 CLI를 다시 실행하면 세 record가 조회되고 다음 INSERT는 ID 4를 받는다",
     rerun_keeps_records_and_next_id_is_4),
    ("다른 프로세스가 데이터 폴더를 잠근 동안 CLI는 종료 코드 1로 끝나고, 잠금이 풀리면 실행된다",
     locked_data_dir_exits_1_until_lock_is_released),
    ("dump는 같은 테이블의 SELECT *와 같은 TSV를 출력한다",
     dump_prints_same_tsv_as_select_star),
    ("없는 테이블의 dump는 종료 코드 1로 끝난다", dump_of_unknown_table_exits_1),
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
