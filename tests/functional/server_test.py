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
# 종료 테스트의 긴 SCAN이 읽을 record 수입니다. SCAN 하나가 1초쯤 걸려, SIGINT를 보내고 서버의
# 반응을 확인하는 동안 작업이 아직 실행 중입니다.
FILLER_RECORDS = 1_000_000
LONG_SCAN = "SELECT name FROM users WHERE age = 7;"
# 서버가 보낸 바이트를 읽었는지, 작업 스레드가 작업을 꺼냈는지는 바깥에서 볼 수 없어 이만큼
# 기다립니다. 긴 SCAN이 이 시간보다 몇 배 깁니다.
SETTLE = 0.2
SHUTDOWN_WAIT_MS = 1000
STOPPED_ACCEPTING = "stopped accepting connections"
SANITIZER_MARKS = ("WARNING: ThreadSanitizer", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")


def users_record_bytes(new_id, name, email, age):
    """테이블 파일 레이아웃의 users record 하나입니다(GLOSSARY.md의 데이터 파일)."""
    body = new_id.to_bytes(8, "little", signed=True)
    for text in (name, email):
        encoded = text.encode("utf-8")
        body += len(encoded).to_bytes(4, "little") + encoded
    body += age.to_bytes(8, "little", signed=True)
    return len(body).to_bytes(4, "little") + body


def make_data_dir(records=(), filler_count=0):
    """records는 테이블 파일에 미리 써 둘 (name, email, age)들이고, ID는 1부터 붙습니다. 그 뒤에
    ('n', 'e', 2) record를 filler_count개 붙입니다."""
    data_dir = tempfile.mkdtemp(prefix="mini-dbms-server-")
    with open(os.path.join(data_dir, "users.schema"), "w", encoding="utf-8") as f:
        f.write(USERS_SCHEMA)
    with open(os.path.join(data_dir, "users.tbl"), "wb") as f:
        f.write(b"".join(users_record_bytes(i, *record) for i, record in enumerate(records, 1)))
        filler = users_record_bytes(0, "n", "e", 2)
        f.write(b"".join(filler[:4] + i.to_bytes(8, "little") + filler[12:]
                         for i in range(len(records) + 1, len(records) + filler_count + 1)))
    return data_dir


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


class Server:
    """with 문 안에서만 서버가 돕니다. 나갈 때 서버를 끄고 데이터 폴더를 지웁니다."""

    def __init__(self, extra_args=(), preexec_fn=None, records=(), filler_count=0, data_dir=None):
        """data_dir을 주면 그 폴더로 띄우고, 나갈 때 지우지 않습니다."""
        self.owns_data_dir = data_dir is None
        self.data_dir = make_data_dir(records, filler_count) if data_dir is None else data_dir
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
        if self.owns_data_dir:
            shutil.rmtree(self.data_dir, ignore_errors=True)
        output = self.output()
        self.log.close()
        if exc[0] is None:
            assert not any(mark in output for mark in SANITIZER_MARKS), output

    def output(self):
        self.log.seek(0)
        return self.log.read().decode("utf-8", "replace")

    def interrupt(self):
        self.process.send_signal(signal.SIGINT)

    def wait_output(self, text):
        """서버 출력에 text가 나올 때까지 기다립니다."""
        deadline = time.monotonic() + STOP_TIMEOUT
        while text not in self.output():
            assert time.monotonic() < deadline, f"서버 출력에 {text!r}가 없습니다: {self.output()}"
            time.sleep(0.02)

    def wait_exit(self):
        """서버가 끝나기를 기다려 종료 코드를 돌려줍니다."""
        try:
            return self.process.wait(timeout=STOP_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise AssertionError(f"서버가 {STOP_TIMEOUT}초 안에 끝나지 않았습니다") from None

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


def result_over_10000_records_is_400_result_too_large():
    records = [("n", "e", i) for i in range(1, 10001)] + [("last", "e", 10001)]
    with Server(records=records) as server:
        status, body = server.query("SELECT * FROM users;")
        assert (status, body["error"]) == (400, "result_too_large"), (status, body)
        status, body = server.query("SELECT id FROM users WHERE name = 'n';")
        assert status == 200 and len(body["records"]) == 10000, status


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


def interrupted_server_finishes_received_requests_and_restarts():
    data_dir = make_data_dir([("a", "e", 7), ("b", "e", 7)], FILLER_RECORDS)
    try:
        with Server(extra_args=["--threads", "1"], data_dir=data_dir) as server:
            with server.raw() as scan_client, server.raw() as insert_client:
                scan_client.send(post(LONG_SCAN))
                time.sleep(SETTLE)
                insert_client.send(post("INSERT INTO users VALUES ('c', 'e', 7);"))
                time.sleep(SETTLE)
                server.interrupt()
                status, _, body = scan_client.response()
                assert (status, body["records"]) == (200, [["a"], ["b"]]), body
                assert body["records_examined"] == FILLER_RECORDS + 2, body
                status, _, body = insert_client.response()
                assert (status, body) == (200, {"inserted": 1, "id": FILLER_RECORDS + 3}), body
            assert server.wait_exit() == 0, server.output()
        with Server(data_dir=data_dir) as server:
            status, body = server.query("SELECT name FROM users WHERE age = 7;")
            assert body["records"] == [["a"], ["b"], ["c"]], body
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)


def interrupted_server_refuses_new_connections_and_keep_alive_requests():
    with Server(extra_args=["--threads", "1"], filler_count=FILLER_RECORDS) as server:
        with server.raw() as kept_client, server.raw() as scan_client:
            kept_client.send(post("INSERT INTO users VALUES ('k', 'e', 7);"))
            assert kept_client.response()[0] == 200
            scan_client.send(post(LONG_SCAN))
            time.sleep(SETTLE)
            server.interrupt()
            server.wait_output(STOPPED_ACCEPTING)
            try:
                socket.create_connection((HOST, server.port), timeout=1.0).close()
                raise AssertionError("SIGINT 뒤에 새 연결이 받아졌습니다")
            except ConnectionRefusedError:
                pass
            kept_client.send(post("INSERT INTO users VALUES ('z', 'e', 7);"))
            status, headers, body = kept_client.response()
            assert (status, body["error"]) == (503, "shutting_down"), body
            assert headers.get("connection") == "close", headers
            assert kept_client.is_closed()
            assert scan_client.response()[2]["records"] == [["k"]]
        assert server.wait_exit() == 0, server.output()


def unread_reply_delays_shutdown_only_for_the_wait():
    records = [("x" * 1000, "e", 1)] * 5000
    with Server(extra_args=["--shutdown-wait", str(SHUTDOWN_WAIT_MS)], records=records) as server:
        sock = socket.socket()
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        sock.connect((HOST, server.port))
        try:
            sock.sendall(post("SELECT * FROM users;"))
            # 응답의 첫 바이트가 오면 작업은 끝났고 응답은 송신 버퍼에 있습니다.
            assert sock.recv(1) == b"H"
            started = time.monotonic()
            server.interrupt()
            assert server.wait_exit() == 0, server.output()
            elapsed = time.monotonic() - started
        finally:
            sock.close()
        assert SHUTDOWN_WAIT_MS / 1000 <= elapsed < SHUTDOWN_WAIT_MS / 1000 + 2, elapsed


def second_interrupt_keeps_running_insert_and_drops_queued_one():
    data_dir = make_data_dir(filler_count=FILLER_RECORDS)
    try:
        with Server(extra_args=["--threads", "2"], data_dir=data_dir) as server:
            with server.raw() as scan_client, server.raw() as running_client, \
                    server.raw() as queued_client:
                # 긴 SCAN이 읽기 잠금을 쥔 동안 두 번째 스레드의 INSERT는 쓰기 잠금을 기다리며
                # 실행 중이고, 스레드가 없는 마지막 INSERT는 대기열에만 있습니다.
                scan_client.send(post(LONG_SCAN))
                time.sleep(SETTLE)
                running_client.send(post("INSERT INTO users VALUES ('running', 'e', 7);"))
                time.sleep(SETTLE)
                queued_client.send(post("INSERT INTO users VALUES ('queued', 'e', 7);"))
                time.sleep(SETTLE)
                server.interrupt()
                server.wait_output(STOPPED_ACCEPTING)
                server.interrupt()
                assert server.wait_exit() == 0, server.output()
                assert queued_client.response() is None
        with Server(data_dir=data_dir) as server:
            status, body = server.query("SELECT name FROM users WHERE age = 7;")
            assert body["records"] == [["running"]], body
    finally:
        shutil.rmtree(data_dir, ignore_errors=True)


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
    ("[T206] 1만 1건을 돌려줄 SELECT는 400 result_too_large이고, 1만 건은 200이다",
     result_over_10000_records_is_400_result_too_large),
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
    ("[T221] SIGINT 때 실행 중이던 SELECT는 전체 응답을, 대기열의 INSERT는 200을 받고, 서버는 "
     "종료 코드 0으로 끝나며, 같은 데이터 폴더로 다시 뜬 서버에 그 INSERT가 남아 있다",
     interrupted_server_finishes_received_requests_and_restarts),
    ("[T222] SIGINT 뒤의 새 TCP 연결은 거절되고, 열린 keep-alive 연결의 새 요청은 "
     "503 shutting_down과 Connection: close를 받은 뒤 닫힌다",
     interrupted_server_refuses_new_connections_and_keep_alive_requests),
    ("[T223] 응답을 읽지 않는 클라이언트는 마지막 작업이 끝난 뒤 --shutdown-wait만큼만 종료를 늦춘다",
     unread_reply_delays_shutdown_only_for_the_wait),
    ("[T224] 두 번째 SIGINT 때 실행 중이던 INSERT는 완전한 record로 남고, 대기열에만 있던 INSERT는 "
     "남지 않으며, 다음 시작이 성공한다",
     second_interrupt_keeps_running_insert_and_drops_queued_one),
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
