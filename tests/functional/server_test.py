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
BODY_MAX = 1 << 20
IDLE_TIMEOUT_MS = 300
SANITIZER_MARKS = ("WARNING: ThreadSanitizer", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")


def users_record_bytes(new_id, name, email, age):
    """테이블 파일 레이아웃의 users record 하나입니다(GLOSSARY.md의 데이터 파일)."""
    body = new_id.to_bytes(8, "little", signed=True)
    for text in (name, email):
        encoded = text.encode("utf-8")
        body += len(encoded).to_bytes(4, "little") + encoded
    body += age.to_bytes(8, "little", signed=True)
    return len(body).to_bytes(4, "little") + body


def make_data_dir(records=()):
    """records는 테이블 파일에 미리 써 둘 (name, email, age)들이고, ID는 1부터 붙습니다."""
    data_dir = tempfile.mkdtemp(prefix="mini-dbms-server-")
    with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write(USERS_SCHEMA)
    with open(os.path.join(data_dir, "users.tbl"), "wb") as f:
        f.write(b"".join(users_record_bytes(i, *record) for i, record in enumerate(records, 1)))
    return data_dir


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


class Server:
    """with 문 안에서만 서버가 돕니다. 나갈 때 서버를 끄고 데이터 폴더를 지웁니다."""

    def __init__(self, extra_args=(), preexec_fn=None, records=()):
        self.data_dir = make_data_dir(records)
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

    def records(self):
        status, body = self.query("SELECT * FROM users;")
        assert status == 200, body
        return body["records"]

    def raw(self):
        return RawClient(self.port)


class RawClient:
    """소켓 하나로 바이트를 그대로 보내고 응답을 하나씩 읽습니다. keep-alive와 잘못된
    요청을 보내려고 http.client 대신 씁니다."""

    def __init__(self, port):
        self.sock = socket.create_connection((HOST, port), timeout=REQUEST_TIMEOUT)
        self.buffer = b""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.sock.close()

    def send(self, data):
        self.sock.sendall(data)

    def fill(self):
        """더 받으면 True, 서버가 연결을 닫았으면 False입니다."""
        try:
            chunk = self.sock.recv(65536)
        except ConnectionResetError:
            return False
        self.buffer += chunk
        return bool(chunk)

    def response(self):
        """다음 응답 (상태 코드, 헤더 dict, 본문 JSON)입니다. 응답 전에 닫히면 None입니다."""
        while b"\r\n\r\n" not in self.buffer:
            if not self.fill():
                assert self.buffer == b"", self.buffer
                return None
        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = head.decode("utf-8").split("\r\n")
        headers = {k.strip().lower(): v.strip() for k, v in (line.split(":", 1) for line in lines[1:])}
        length = int(headers["content-length"])
        while len(self.buffer) < length:
            assert self.fill(), "본문을 다 받기 전에 연결이 닫혔습니다"
        body, self.buffer = self.buffer[:length], self.buffer[length:]
        return int(lines[0].split()[1]), headers, json.loads(body)

    def is_closed(self):
        """서버가 더 보내지 않고 연결을 닫았는지 봅니다."""
        return not self.fill() and self.buffer == b""


def post(body, headers="Content-Length: {length}\r\n", version="HTTP/1.1"):
    """POST /query 요청 바이트입니다. headers의 {length}는 본문 길이로 바뀝니다."""
    if isinstance(body, str):
        body = body.encode("utf-8")
    head = f"POST /query {version}\r\nHost: test\r\n" + headers.format(length=len(body))
    return head.encode("utf-8") + b"\r\n" + body


def insert(name):
    return f"INSERT INTO users VALUES ('{name}', 'e', 1);"


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


GUARDED_REQUESTS = [
    (post(insert("a"), "Content-Length: abc\r\n"), 400, "invalid_body_length"),
    (post(insert("a"), "Content-Length: -1\r\n"), 400, "invalid_body_length"),
    (post(insert("a"), "Content-Length: {length}\r\nContent-Length: {length}\r\n"),
     400, "invalid_body_length"),
    (post(insert("a"), "Content-Length: {length}\r\nTransfer-Encoding: chunked\r\n"),
     400, "invalid_body_length"),
    (post(b"%x\r\n%s\r\n0\r\n\r\n" % (len(insert("a")), insert("a").encode()),
          "Transfer-Encoding: chunked\r\n"), 411, "length_required"),
    (post(insert("a"), ""), 411, "length_required"),
    (post(b"", f"Content-Length: {BODY_MAX + 1}\r\n"), 413, "body_too_large"),
    (post("INSERT INTO users VALUES ('a\0b', 'e', 1);"), 400, "invalid_sql_body"),
    (post(b""), 400, "no_statement"),
    (post(" \n\t "), 400, "no_statement"),
    (post(b" " * BODY_MAX), 400, "no_statement"),
]


def guarded_requests_get_status_and_reason_without_running_sql():
    with Server() as server:
        for request, status, reason in GUARDED_REQUESTS:
            with server.raw() as client:
                client.send(request)
                response = client.response()
                assert response is not None, request[:120]
                assert (response[0], response[2]["error"]) == (status, reason), (request[:120], response)
        assert server.records() == []


def header_over_8_kib_is_refused_and_server_keeps_serving():
    pad = "X-Pad: " + "a" * (9 * 1024) + "\r\n"
    with Server() as server:
        with server.raw() as client:
            client.send(post(insert("a"), pad + "Content-Length: {length}\r\n"))
            response = client.response()
            assert response is None or response[0] == 431, response
        with server.raw() as client:
            client.send(("POST /query HTTP/1.1\r\n" + pad).encode())
            response = client.response()
            assert response is None or response[0] == 431, response
        with server.raw() as client:
            client.send(post(insert("b"), "X-Pad: " + "a" * 7000 + "\r\nContent-Length: {length}\r\n"))
            assert client.response()[0] == 200
        assert [record[1] for record in server.records()] == ["b"]


def body_shorter_than_declared_is_not_run():
    with Server() as server:
        with server.raw() as client:
            body = insert("a").encode()
            client.send(post(body, f"Content-Length: {len(body) + 10}\r\n"))
        assert server.query(insert("b"))[0] == 200
        assert [record[1] for record in server.records()] == ["b"]


def select_parts(server, sql):
    """next_cursor가 없을 때까지 같은 SQL을 커서와 함께 다시 보내고, 부분마다 records를 모읍니다."""
    parts = []
    path = "/query"
    while True:
        status, _, body = server.request("POST", path, sql.encode("utf-8"))
        assert status == 200, (status, body)
        parts.append(body["records"])
        if "next_cursor" not in body:
            return parts
        path = f"/query?cursor={body['next_cursor']}"


def result_over_10000_records_comes_in_two_parts():
    records = [("n", "e", i) for i in range(1, 10001)] + [("last", "e", 10001)]
    with Server(records=records) as server:
        parts = select_parts(server, "SELECT id FROM users;")
        assert [len(part) for part in parts] == [10000, 1], [len(part) for part in parts]
        assert parts[1] == [[10001]], parts[1]
        status, body = server.query("SELECT id FROM users WHERE name = 'n';")
        assert status == 200 and len(body["records"]) == 10000, status
        assert "next_cursor" not in body, body.keys()


def joined_parts_equal_whole_result():
    records = [(f"n{i}", f"e{i}", i % 2) for i in range(1, 25001)]
    with Server(records=records) as server:
        parts = select_parts(server, "SELECT * FROM users;")
        assert [len(part) for part in parts] == [10000, 10000, 5000], [len(part) for part in parts]
        expected = [[i, f"n{i}", f"e{i}", i % 2] for i in range(1, 25001)]
        assert sum(parts, []) == expected
        parts = select_parts(server, "SELECT id FROM users WHERE age = 1;")
        assert [len(part) for part in parts] == [10000, 2500], [len(part) for part in parts]
        assert sum(parts, []) == [[i] for i in range(1, 25001, 2)]


def insert_between_parts_appears_only_in_later_part():
    records = [("n", "e", i) for i in range(1, 10002)]
    with Server(records=records) as server:
        sql = "SELECT id, name FROM users WHERE name = 'n';"
        status, _, first = server.request("POST", "/query", sql.encode())
        assert status == 200 and len(first["records"]) == 10000, status
        # 앞 부분의 읽기 잠금이 남아 있으면 이 INSERT가 REQUEST_TIMEOUT 안에 끝나지 않습니다.
        assert server.query(insert("n"))[1] == {"inserted": 1, "id": 10002}
        assert server.query(insert("x"))[1] == {"inserted": 1, "id": 10003}
        status, _, second = server.request(
            "POST", f"/query?cursor={first['next_cursor']}", sql.encode())
        assert status == 200, second
        assert second["records"] == [[10001, "n"], [10002, "n"]], second["records"]
        assert "next_cursor" not in second


def each_invalid_cursor_is_400_invalid_cursor():
    records = [("n", "e", 1), ("m", "e", 2)]
    file_size = len(users_record_bytes(1, *records[0])) + len(users_record_bytes(2, *records[1]))
    cases = [
        ("1", "SELECT * FROM users;"),
        (str(file_size + 1), "SELECT * FROM users;"),
        ("18446744073709551616", "SELECT * FROM users;"),
        ("-1", "SELECT * FROM users;"),
        ("abc", "SELECT * FROM users;"),
        ("1.5", "SELECT * FROM users;"),
        ("", "SELECT * FROM users;"),
        ("0&x=1", "SELECT * FROM users;"),
        ("0", "SELECT * FROM users WHERE id = 1;"),
        ("0", insert("z")),
    ]
    with Server(records=records) as server:
        for cursor, sql in cases:
            status, _, body = server.request("POST", f"/query?cursor={cursor}", sql.encode())
            assert (status, body["error"]) == (400, "invalid_cursor"), (cursor, sql, status, body)
        status, _, body = server.request("POST", "/query?page=1", b"SELECT * FROM users;")
        assert (status, body["error"]) == (400, "invalid_cursor"), body
        status, _, body = server.request("POST", f"/query?cursor={file_size}", b"SELECT * FROM users;")
        assert (status, body["records"]) == (200, []), body
        assert [record[1] for record in server.records()] == ["n", "m"]


def json_over_response_limit_is_cut_into_parts():
    # 제어 문자는 JSON에서 \u0001 여섯 바이트가 됩니다. record 하나의 JSON이 약 6 MB이므로
    # 값은 7 MB로 결과 상한(16 MiB) 안이지만 JSON은 응답 상한(32 MiB)을 넘습니다.
    text = "\x01" * 1_000_000
    records = [(text, "e", i) for i in range(1, 8)]
    with Server(records=records) as server:
        parts = select_parts(server, "SELECT id, name FROM users;")
        assert len(parts) == 2, [len(part) for part in parts]
        assert [record[0] for record in sum(parts, [])] == list(range(1, 8))
        assert all(record[1] == text for record in sum(parts, []))


def full_queue_is_503_queue_full_without_running_sql():
    with Server(extra_args=["--queue", "0"]) as server:
        status, body = server.query(insert("a"))
        assert (status, body["error"]) == (503, "queue_full"), body
        assert os.path.getsize(os.path.join(server.data_dir, "users.tbl")) == 0


def three_requests_on_one_connection_get_responses_in_order():
    with Server() as server, server.raw() as client:
        client.send(post(insert("a")) + post(insert("b")) + post(insert("c")))
        for expected_id in (1, 2, 3):
            status, headers, body = client.response()
            assert (status, body) == (200, {"inserted": 1, "id": expected_id}), body
            assert headers.get("connection") != "close", headers
        client.send(post("SELECT name FROM users;"))
        assert client.response()[2]["records"] == [["a"], ["b"], ["c"]]


def connection_close_runs_no_later_sql_on_that_connection():
    cases = [
        (post(insert("a"), "Connection: close\r\nContent-Length: {length}\r\n"), True),
        (post(insert("a"), version="HTTP/1.0"), True),
        (post(insert("a"), "Connection: keep-alive\r\nContent-Length: {length}\r\n", "HTTP/1.0"), False),
    ]
    for first, closes in cases:
        with Server() as server:
            with server.raw() as client:
                client.send(first + post(insert("b")))
                status, headers, _ = client.response()
                assert status == 200
                if closes:
                    assert headers.get("connection") == "close", headers
                    assert client.is_closed()
                else:
                    assert client.response()[0] == 200
            expected = ["a"] if closes else ["a", "b"]
            assert [record[1] for record in server.records()] == expected, first


def idle_connection_is_closed_after_timeout():
    with Server(extra_args=["--idle-timeout", str(IDLE_TIMEOUT_MS)]) as server:
        with server.raw() as client:
            started = time.monotonic()
            assert client.is_closed()
            assert time.monotonic() - started >= IDLE_TIMEOUT_MS / 1000 * 0.9
        with server.raw() as client:
            client.send(post(insert("a")))
            assert client.response()[0] == 200
            assert client.is_closed()


def insert_whose_client_left_is_still_stored():
    with Server() as server:
        with server.raw() as client:
            client.send(post(insert("a")))
        deadline = time.monotonic() + REQUEST_TIMEOUT
        while not server.records():
            assert time.monotonic() < deadline, "끊긴 연결의 INSERT가 저장되지 않았습니다"
            time.sleep(0.02)
        assert [record[1] for record in server.records()] == ["a"]


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
    ("[T203] 잘못된 본문 길이, chunked, 1 MiB 초과, NUL 바이트, 빈 본문은 상태 코드와 오류 이유로 "
     "거절되고 SQL은 실행되지 않으며, 정확히 1 MiB인 본문은 받는다",
     guarded_requests_get_status_and_reason_without_running_sql),
    ("[T204] 8 KiB를 넘는 헤더는 431을 받거나 연결이 닫히고, 서버는 7 KiB 헤더의 다음 요청을 처리한다",
     header_over_8_kib_is_refused_and_server_keeps_serving),
    ("[T205] 선언한 길이보다 본문을 덜 보내고 끊은 요청은 실행되지 않는다",
     body_shorter_than_declared_is_not_run),
    ("[T206] 1만 1건을 돌려줄 SELECT는 1만 건과 next_cursor를 받고 두 번째 부분은 1건이며, "
     "정확히 1만 건인 결과에는 next_cursor가 없다",
     result_over_10000_records_comes_in_two_parts),
    ("[T242] 부분 사이에 INSERT가 없으면 커서로 받은 부분들을 이은 결과가 SCAN 한 번의 결과와 같다",
     joined_parts_equal_whole_result),
    ("[T243] 부분 사이의 INSERT는 기다리지 않고 들어가고, 조건에 맞는 새 record는 뒤 부분에만 "
     "나오며 앞서 받은 record는 다시 나오거나 빠지지 않는다",
     insert_between_parts_appears_only_in_later_part),
    ("[T244] record 시작이 아닌 위치, 파일 크기보다 큰 위치, 0 이상의 정수가 아닌 값, INDEX 질의와 "
     "INSERT에 붙은 커서는 400 invalid_cursor이고, 파일 끝의 커서는 0건이다",
     each_invalid_cursor_is_400_invalid_cursor),
    ("[T245] JSON이 응답 상한을 넘을 결과는 상한 앞에서 next_cursor로 잘리고, 부분들을 이으면 "
     "모든 record가 나온다",
     json_over_response_limit_is_cut_into_parts),
    ("[T207] 작업 대기열이 가득 차면 SQL을 실행하지 않고 503 queue_full로 바로 답한다",
     full_queue_is_503_queue_full_without_running_sql),
    ("[T208] 한 연결에 요청 세 개를 연달아 보내면 보낸 순서대로 응답을 받고 연결을 다시 쓴다",
     three_requests_on_one_connection_get_responses_in_order),
    ("[T209] Connection: close 요청과 keep-alive가 없는 HTTP/1.0 요청은 응답에 Connection: close를 "
     "받고 같은 연결의 두 번째 SQL은 실행되지 않으며, keep-alive를 요청한 HTTP/1.0은 연결을 다시 쓴다",
     connection_close_runs_no_later_sql_on_that_connection),
    ("[T210] 아무것도 보내지 않는 연결과 응답 뒤 조용한 연결은 --idle-timeout 뒤 서버가 닫는다",
     idle_connection_is_closed_after_timeout),
    ("[T211] 응답을 받기 전에 연결을 끊은 INSERT는 파일에 남는다",
     insert_whose_client_left_is_still_stored),
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
