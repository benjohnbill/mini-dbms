#!/usr/bin/env python3
"""CLI 실행 파일을 바깥에서 실행해 stdout, 테이블 파일 바이트, 종료 코드를 확인합니다.

사용법: python3 tests/functional/cli_test.py <CLI 경로>
"""

import fcntl
import os
import re
import resource
import signal
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


def quote_and_semicolon_in_text_are_stored_as_written():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('O''Brien', 'a;b', 1);\n"
                       "SELECT name, email FROM users;\n")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == b"name\temail\nO'Brien\ta;b\n", proc.stdout
    finally:
        shutil.rmtree(data_dir)


def syntax_error_names_line_in_file():
    cases = [
        ("SELECT * FROM users;\n\nSELECT * FROM users", "3번째 줄"),
        ("SELECT * FROM users;\nINSERT INTO users VALUES (\n'kim', 'oops, 1);\n",
         "3번째 줄"),
        ("SELECT * FROM users;\n\n-- 주석\nSELECT * FROM users;\n", "3번째 줄"),
    ]
    data_dir = make_data_dir()
    try:
        for sql, line in cases:
            proc = run_sql(data_dir, sql)
            stderr = proc.stderr.decode("utf-8")
            assert proc.returncode == 1, (sql, proc.returncode)
            assert line in stderr, (sql, stderr)
    finally:
        shutil.rmtree(data_dir)


def error_keeps_earlier_inserts_and_skips_later_statements():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('kim', 'k@x', 20);\n"
                       "INSERT INTO users VALUES ('lee', 'l@x', 31);\n"
                       "INSERT INTO users VALUES ('bad');\n"
                       "INSERT INTO users VALUES ('park', 'p@x', 40);\n")
        assert proc.returncode == 1, proc.returncode
        assert "3번째 줄" in proc.stderr.decode("utf-8"), proc.stderr
        proc = run_sql(data_dir, "SELECT name FROM users;")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == b"name\nkim\nlee\n", proc.stdout
    finally:
        shutil.rmtree(data_dir)


def nul_byte_in_sql_file_exits_1():
    data_dir = make_data_dir()
    try:
        sql_path = os.path.join(data_dir, "input.sql")
        with open(sql_path, "wb") as f:
            f.write(b"SELECT * FROM users;\0\nINSERT INTO users VALUES ('a', 'b', 1);\n")
        proc = run_cli(["--data", data_dir, sql_path])
        assert proc.returncode == 1, proc.returncode
        assert proc.stderr
        assert proc.stdout == b"", proc.stdout
        assert os.path.getsize(os.path.join(data_dir, "users.tbl")) == 0
    finally:
        shutil.rmtree(data_dir)


META_INSERT = re.compile(r"^-- 1 record inserted, id [0-9]+$")
META_SELECT = re.compile(r"^-- [0-9]+ records?, (SCAN|INDEX), examined [0-9]+, [0-9]+\.[0-9]{2} ms$")


def meta_lines_go_to_stderr_one_per_statement():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('kim', 'k@x', 20);\n"
                       "INSERT INTO users VALUES ('lee', 'l@x', 31);\n"
                       "SELECT * FROM users WHERE id = 2;\n"
                       "SELECT * FROM users;\n")
        assert proc.returncode == 0, proc.stderr
        lines = proc.stderr.decode("utf-8").splitlines()
        assert len(lines) == 4, lines
        assert lines[0] == "-- 1 record inserted, id 1", lines
        assert lines[1] == "-- 1 record inserted, id 2", lines
        assert META_SELECT.match(lines[2]), lines
        assert lines[2].startswith("-- 1 record, INDEX, examined 1, "), lines
        assert lines[3].startswith("-- 2 records, SCAN, examined 2, "), lines
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


def rerun_finds_same_record_by_id_through_index():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir,
                       "INSERT INTO users VALUES ('a', 'a@x', 1);\n"
                       "INSERT INTO users VALUES ('b', 'b@x', 2);\n"
                       "INSERT INTO users VALUES ('c', 'c@x', 3);\n")
        assert proc.returncode == 0, proc.stderr
        proc = run_sql(data_dir,
                       "SELECT * FROM users WHERE id = 2;\n"
                       "SELECT * FROM users WHERE id = 4;\n")
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == (b"id\tname\temail\tage\n"
                               b"2\tb\tb@x\t2\n"
                               b"id\tname\temail\tage\n"), proc.stdout
        lines = proc.stderr.decode("utf-8").splitlines()
        assert len(lines) == 2, lines
        assert lines[0].startswith("-- 1 record, INDEX, examined 1, "), lines
        assert lines[1].startswith("-- 0 records, INDEX, examined 0, "), lines
    finally:
        shutil.rmtree(data_dir)


def quiet_option_hides_insert_meta_lines_only():
    data_dir = make_data_dir()
    try:
        sql_path = os.path.join(data_dir, "input.sql")
        with open(sql_path, "w", encoding="utf-8") as f:
            f.write("INSERT INTO users VALUES ('kim', 'k@x', 20);\n"
                    "SELECT name FROM users;\n")
        proc = run_cli(["--data", data_dir, "-q", sql_path])
        assert proc.returncode == 0, proc.stderr
        assert proc.stdout == b"name\nkim\n", proc.stdout
        lines = proc.stderr.decode("utf-8").splitlines()
        assert not any(META_INSERT.match(line) for line in lines), lines
        assert len(lines) == 1 and META_SELECT.match(lines[0]), lines
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
            assert "다른 프로세스가 쓰는".encode("utf-8") in proc.stderr, proc.stderr
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


def failed_write_exits_1_and_keeps_table_file():
    data_dir = make_data_dir()
    table_path = os.path.join(data_dir, "users.tbl")
    try:
        assert run_sql(data_dir, "INSERT INTO users VALUES ('kim', 'k@x', 20);\n").returncode == 0
        with open(table_path, "rb") as f:
            before = f.read()

        def limit_file_size():
            # 테이블 파일의 지금 크기를 상한으로 두면 다음 쓰기가 EFBIG로 실패합니다.
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (len(before), resource.RLIM_INFINITY))

        sql_path = os.path.join(data_dir, "input.sql")
        with open(sql_path, "w", encoding="utf-8") as f:
            f.write("INSERT INTO users VALUES ('lee', 'l@x', 31);\n")
        proc = subprocess.run([CLI, "--data", data_dir, sql_path],
                              capture_output=True, preexec_fn=limit_file_size)
        assert proc.returncode == 1, proc.returncode
        assert b"users.tbl" in proc.stderr, proc.stderr
        with open(table_path, "rb") as f:
            assert f.read() == before
    finally:
        shutil.rmtree(data_dir)


def truncated_table_file_exits_1_and_keeps_file():
    data_dir = make_data_dir()
    try:
        proc = run_sql(data_dir, "INSERT INTO users VALUES ('kim', 'k@x', 20);\n")
        assert proc.returncode == 0, proc.stderr
        table_path = os.path.join(data_dir, "users.tbl")
        with open(table_path, "ab") as f:
            f.write(b"\x20\x00")
        with open(table_path, "rb") as f:
            before = f.read()
        proc = run_sql(data_dir, "SELECT * FROM users;")
        assert proc.returncode == 1, proc.returncode
        message = proc.stderr.decode("utf-8")
        # 첫 record는 길이 칸 4 + id 8 + "kim" 4 + 3 + "k@x" 4 + 3 + age 8 = 34바이트입니다.
        for expected in ("users.tbl", "34번째 바이트", "record(2번째)", "잘림"):
            assert expected in message, message
        with open(table_path, "rb") as f:
            assert f.read() == before
    finally:
        shutil.rmtree(data_dir)


def users_record_bytes(new_id, name, email, age):
    """테이블 파일 레이아웃의 users record 하나입니다(GLOSSARY.md의 데이터 파일)."""
    body = new_id.to_bytes(8, "little", signed=True)
    for text in (name, email):
        encoded = text.encode("utf-8")
        body += len(encoded).to_bytes(4, "little") + encoded
    body += age.to_bytes(8, "little", signed=True)
    return len(body).to_bytes(4, "little") + body


def cli_prints_all_10001_records_without_result_limit():
    data_dir = make_data_dir()
    try:
        with open(os.path.join(data_dir, "users.tbl"), "wb") as f:
            f.write(b"".join(users_record_bytes(i, "n", "e", i) for i in range(1, 10002)))
        proc = run_sql(data_dir, "SELECT * FROM users;\n")
        assert proc.returncode == 0, proc.stderr
        lines = proc.stdout.split(b"\n")
        assert len(lines) == 10003 and lines[-1] == b"", len(lines)
        assert lines[1] == b"1\tn\te\t1" and lines[10001] == b"10001\tn\te\t10001", lines[10001]
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
    ("[T13] 문자열 안의 ''와 ;는 적은 그대로 저장되고 조회된다",
     quote_and_semicolon_in_text_are_stored_as_written),
    ("[T14] 마지막 ;가 빠진 파일, 닫히지 않은 문자열, -- 주석은 파일의 줄 번호가 있는 문법 오류이다",
     syntax_error_names_line_in_file),
    ("[T15] 세 번째 문장이 틀린 파일은 앞의 INSERT 두 개를 남기고 네 번째 문장을 실행하지 않는다",
     error_keeps_earlier_inserts_and_skips_later_statements),
    ("[T16] NUL 바이트가 든 SQL 파일은 문장을 실행하지 않고 종료 코드 1로 끝난다",
     nul_byte_in_sql_file_exits_1),
    ("[T17] 문장마다 stderr에 메타 줄 하나를 쓴다",
     meta_lines_go_to_stderr_one_per_statement),
    ("[T18] -q는 INSERT 메타 줄만 끄고 SELECT 결과와 메타 줄은 그대로 둔다",
     quiet_option_hides_insert_meta_lines_only),
    ("[T87] 테이블 파일 쓰기가 실패하면 CLI는 stderr에 오류를 쓰고 종료 코드 1로 끝나며 테이블 파일은 그대로다",
     failed_write_exits_1_and_keeps_table_file),
    ("INSERT 세 개 뒤 CLI를 다시 실행하면 세 record가 조회되고 다음 INSERT는 ID 4를 받는다",
     rerun_keeps_records_and_next_id_is_4),
    ("[T108] CLI를 다시 실행해도 WHERE id = k는 INDEX로 같은 record를 찾고, 없는 ID는 INDEX로 0건이다",
     rerun_finds_same_record_by_id_through_index),
    ("다른 프로세스가 데이터 폴더를 잠근 동안 CLI는 종료 코드 1로 끝나고, 잠금이 풀리면 실행된다",
     locked_data_dir_exits_1_until_lock_is_released),
    ("dump는 같은 테이블의 SELECT *와 같은 TSV를 출력한다",
     dump_prints_same_tsv_as_select_star),
    ("없는 테이블의 dump는 종료 코드 1로 끝난다", dump_of_unknown_table_exits_1),
    ("[T66] 잘린 테이블 파일이면 CLI는 파일 이름, 바이트 위치, record 번호, 잘림을 "
     "알리고 종료 코드 1로 끝나며 파일을 고치지 않는다",
     truncated_table_file_exits_1_and_keeps_file),
    ("[T202] 결과 상한이 없는 CLI는 1만 1건을 돌려줄 SELECT의 record를 모두 출력한다",
     cli_prints_all_10001_records_without_result_limit),
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
