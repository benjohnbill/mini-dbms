#!/usr/bin/env python3
"""API 서버 실행 파일을 띄우고 실제 HTTP 요청을 보내 상태 코드와 JSON을 확인합니다.

사용법: python3 -B tests/functional/server_test.py <서버 경로>

테스트마다 임시 데이터 폴더와 빈 포트로 서버를 새로 띄우고, 테스트가 실패해도 마지막에
서버를 끕니다. python3 표준 라이브러리만 씁니다.
"""

import http.client
import json
import os
import resource
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SERVER = os.path.abspath(sys.argv[1])
HOST = "127.0.0.1"
USERS_SCHEMA = "name TEXT\nemail TEXT\nage INT\n"
START_TIMEOUT = 10.0
STOP_TIMEOUT = 10.0
REQUEST_TIMEOUT = 10.0
CONCURRENT_INSERTS = 100
SANITIZER_MARKS = ("WARNING: ThreadSanitizer", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")


def make_data_dir():
    data_dir = tempfile.mkdtemp(prefix="mini-dbms-server-")
    with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write(USERS_SCHEMA)
    open(os.path.join(data_dir, "users.tbl"), "wb").close()
    return data_dir


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


class Server:
    """with 문 안에서만 서버가 돕니다. 나갈 때 서버를 끄고 데이터 폴더를 지웁니다."""

    def __init__(self, extra_args=(), preexec_fn=None):
        self.data_dir = make_data_dir()
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [SERVER, "--data", self.data_dir, "--port", str(self.port), *extra_args],
            stdout=self.log, stderr=self.log, preexec_fn=preexec_fn)

    def __enter__(self):
        deadline = time.monotonic() + START_TIMEOUT
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.__exit__(None, None, None)
                raise AssertionError(f"서버가 시작 중에 끝났습니다: {self.output()}")
            try:
                socket.create_connection((HOST, self.port), timeout=0.2).close()
                return self
            except OSError:
                time.sleep(0.02)
        self.__exit__(None, None, None)
        raise AssertionError(f"서버가 {START_TIMEOUT}초 안에 연결을 받지 않습니다")

    def __exit__(self, *exc):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=STOP_TIMEOUT)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        shutil.rmtree(self.data_dir, ignore_errors=True)
        output = self.output()
        self.log.close()
        if exc[0] is None:
            assert not any(mark in output for mark in SANITIZER_MARKS), output

    def output(self):
        self.log.seek(0)
        return self.log.read().decode("utf-8", "replace")

    def request(self, method, path, body=None):
        """(상태 코드, 헤더 dict, 본문 JSON)을 돌려줍니다."""
        connection = http.client.HTTPConnection(HOST, self.port, timeout=REQUEST_TIMEOUT)
        try:
            connection.request(method, path, body=body)
            response = connection.getresponse()
            raw = response.read()
            headers = {k.lower(): v for k, v in response.getheaders()}
            assert headers.get("content-type") == "application/json", headers
            return response.status, headers, json.loads(raw)
        finally:
            connection.close()

    def query(self, sql):
        status, _, body = self.request("POST", "/query", sql.encode("utf-8"))
        return status, body


def insert_returns_id_and_select_by_id_uses_index():
    with Server() as server:
        assert server.query("INSERT INTO users VALUES ('kim', 'k@x', 20);") == \
            (200, {"inserted": 1, "id": 1})
        assert server.query("INSERT INTO users VALUES ('lee', 'l@x', 31);") == \
            (200, {"inserted": 1, "id": 2})
        status, body = server.query("SELECT * FROM users WHERE id = 2;")
        assert status == 200, body
        assert body == {"fields": ["id", "name", "email", "age"],
                        "records": [[2, "lee", "l@x", 31]],
                        "access_path": "INDEX", "records_examined": 1}, body


def select_by_other_field_scans_every_record():
    with Server() as server:
        for name in ("a", "b", "c"):
            server.query(f"INSERT INTO users VALUES ('{name}', 'x', 1);")
        status, body = server.query("SELECT name FROM users WHERE name = 'b';")
        assert status == 200, body
        assert body == {"fields": ["name"], "records": [["b"]],
                        "access_path": "SCAN", "records_examined": 3}, body


def empty_select_returns_200_and_empty_records():
    with Server() as server:
        status, body = server.query("SELECT * FROM users;")
        assert status == 200, body
        assert body["records"] == [], body
        assert body["fields"] == ["id", "name", "email", "age"], body


def text_and_big_int_are_json_strings_and_exact_numbers():
    with Server() as server:
        status, body = server.query(
            "INSERT INTO users VALUES ('O''Brien \"q\" \\ tab\t;', '한글', 9223372036854775807);")
        assert status == 200, body
        status, body = server.query("SELECT name, email, age FROM users WHERE id = 1;")
        assert status == 200, body
        # json.loads는 정수를 int로 읽으므로 2^63 - 1이 double로 깎이지 않았는지 확인합니다.
        assert body["records"] == [["O'Brien \"q\" \\ tab\t;", "한글", 9223372036854775807]], body


SQL_ERRORS = [
    ("SELEC * FROM users;", "syntax_error"),
    ("SELECT * FROM nobody;", "table_not_found"),
    ("SELECT nope FROM users;", "field_not_found"),
    ("SELECT * FROM users WHERE id = 'abc';", "type_mismatch"),
    ("INSERT INTO users VALUES ('a', 'b');", "value_count_mismatch"),
    ("SELECT * FROM users; SELECT * FROM users;", "multiple_statements"),
]


def each_sql_error_is_400_with_its_reason():
    with Server() as server:
        for sql, reason in SQL_ERRORS:
            status, body = server.query(sql)
            assert status == 400, (sql, status, body)
            assert body["error"] == reason, (sql, body)
            assert isinstance(body["message"], str) and body["message"], (sql, body)
            assert set(body) == {"error", "message"}, body


def two_statements_run_neither():
    with Server() as server:
        status, body = server.query(
            "INSERT INTO users VALUES ('a', 'a@x', 1); INSERT INTO users VALUES ('b', 'b@x', 2);")
        assert (status, body["error"]) == (400, "multiple_statements"), body
        status, body = server.query("SELECT * FROM users;")
        assert body["records"] == [], body
        assert server.query("INSERT INTO users VALUES ('c', 'c@x', 3);")[1]["id"] == 1


def other_path_is_404_and_other_method_is_405():
    with Server() as server:
        status, _, body = server.request("POST", "/other", b"SELECT * FROM users;")
        assert (status, body["error"]) == (404, "route_not_found"), body
        status, headers, body = server.request("GET", "/query")
        assert (status, body["error"]) == (405, "method_not_allowed"), body
        assert headers.get("allow") == "POST", headers


def limit_file_size_to_zero():
    # 서버 프로세스의 파일 크기 상한을 0으로 두면 빈 테이블 파일에 record를 쓰는
    # pwrite가 EFBIG로 실패합니다. SIGXFSZ를 무시해야 서버가 죽지 않습니다.
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (0, resource.RLIM_INFINITY))


def failed_write_is_500_storage_error():
    with Server(preexec_fn=limit_file_size_to_zero) as server:
        status, body = server.query("INSERT INTO users VALUES ('a', 'a@x', 1);")
        assert (status, body["error"]) == (500, "storage_error"), (status, body)
        assert os.path.getsize(os.path.join(server.data_dir, "users.tbl")) == 0


def concurrent_inserts_and_selects_keep_ids_and_data():
    with Server() as server:
        results = [None] * CONCURRENT_INSERTS
        scans = []

        def insert(i):
            results[i] = server.query(f"INSERT INTO users VALUES ('m{i}', 'e{i}', {i});")

        def scan():
            scans.append(server.query("SELECT * FROM users;"))

        threads = [threading.Thread(target=insert, args=(i,)) for i in range(CONCURRENT_INSERTS)]
        threads += [threading.Thread(target=scan) for _ in range(20)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        ids = {}
        for i, (status, body) in enumerate(results):
            assert status == 200, body
            ids[body["id"]] = i
        assert sorted(ids) == list(range(1, CONCURRENT_INSERTS + 1)), sorted(ids)
        for new_id, i in ids.items():
            status, body = server.query(f"SELECT * FROM users WHERE id = {new_id};")
            assert body["records"] == [[new_id, f"m{i}", f"e{i}", i]], body
        for status, body in scans:
            assert status == 200, body
            for record_id, name, email, age in body["records"]:
                assert ids[record_id] == age and name == f"m{age}" and email == f"e{age}", body


def outbound_address():
    """이 머신의 루프백이 아닌 주소입니다. 없으면 None입니다."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("192.0.2.1", 9))  # TEST-NET-1: 패킷은 나가지 않습니다
            address = probe.getsockname()[0]
        except OSError:
            return None
    return None if address.startswith("127.") or address == "0.0.0.0" else address


def default_server_refuses_non_loopback_address():
    address = outbound_address()
    with Server() as server:
        assert server.query("SELECT * FROM users;")[0] == 200
        if address is None:
            print("  (루프백이 아닌 주소가 없어 접속 거절은 확인하지 못했습니다)")
            return
        try:
            socket.create_connection((address, server.port), timeout=1.0).close()
        except OSError:
            return
        raise AssertionError(f"{address}:{server.port}로 접속되었습니다")


def bad_arguments_exit_2():
    for args in (["--port"], ["--threads", "0"], ["--queue", "x"], ["extra"]):
        proc = subprocess.run([SERVER, *args], capture_output=True, timeout=STOP_TIMEOUT)
        assert proc.returncode == 2, (args, proc.returncode, proc.stderr)
        assert b"usage" in proc.stderr, proc.stderr


TESTS = [
    ("[T163] INSERT는 200과 발급된 id를 받고, 같은 id의 SELECT는 INDEX로 그 record를 돌려준다",
     insert_returns_id_and_select_by_id_uses_index),
    ("[T164] 다른 field 조건의 SELECT는 SCAN과 전체 record 수를 돌려준다",
     select_by_other_field_scans_every_record),
    ("[T165] 결과 0건인 SELECT는 200과 빈 records이다",
     empty_select_returns_200_and_empty_records),
    ("TEXT의 따옴표, 역슬래시, 탭, 한글은 JSON 문자열로, INT 최댓값은 깎이지 않은 JSON 숫자로 온다",
     text_and_big_int_are_json_strings_and_exact_numbers),
    ("[T166] SQL 오류는 이유마다 400과 그 오류 이유, 사람이 읽는 message를 돌려준다",
     each_sql_error_is_400_with_its_reason),
    ("[T167] 문장 두 개가 든 본문은 400 multiple_statements이고 첫 문장도 실행되지 않는다",
     two_statements_run_neither),
    ("[T168] /query가 아닌 경로는 404 route_not_found, POST가 아닌 메서드는 Allow: POST와 "
     "405 method_not_allowed이다",
     other_path_is_404_and_other_method_is_405),
    ("[T171] 테이블 파일 쓰기가 실패한 INSERT는 500 storage_error이고 파일은 그대로다",
     failed_write_is_500_storage_error),
    ("[T169] 클라이언트 100개의 동시 INSERT와 SELECT는 서버를 거쳐도 ID 1~100을 하나씩 받고 "
     "반쯤 쓰인 record를 보지 않는다",
     concurrent_inserts_and_selects_keep_ids_and_data),
    ("[T170] 기본 실행의 서버는 127.0.0.1이 아닌 주소로는 접속을 받지 않는다",
     default_server_refuses_non_loopback_address),
    ("잘못된 서버 인자는 사용법을 알리고 종료 코드 2로 끝난다", bad_arguments_exit_2),
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
