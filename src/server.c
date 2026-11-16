/* mini-dbms API 서버의 진입점입니다. `POST /query`의 본문에 담긴 SQL 문장
   하나를 DB 엔진으로 실행하고 결과를 JSON으로 돌려줍니다.

   스레드는 두 종류입니다. 이벤트 루프(main 스레드) 하나가 Mongoose로 연결을
   받고, 받은 바이트에서 요청을 잘라 검사하고, SQL만 복사해 작업 대기열에
   넣고, 응답을 보냅니다. 작업 스레드들은 대기열에서 요청을 꺼내 엔진을 API
   모드로 부르고, JSON을 만들어 완료 대기열에 넣은 뒤 mg_wakeup()으로 이벤트
   루프를 깨우기만 합니다. Mongoose의 연결은 이벤트 루프만 만지므로, 작업
   스레드는 연결 대신 연결 번호만 들고 다닙니다. 응답을 보낼 때 그 번호의
   연결이 이미 닫혔으면 응답을 버립니다.

   요청을 자르는 일(본문 길이, 상한, keep-alive)은 Mongoose의 HTTP 처리기 대신
   이 파일이 합니다. Mongoose의 HTTP 처리기는 Content-Length와
   Transfer-Encoding이 함께 오거나 길이 표기가 틀리면 응답 없이 연결을 닫고,
   응답을 미뤄 보내는 연결의 Connection: close를 지키지 않기 때문입니다.
   그래서 연결은 mg_listen으로 받고, Mongoose에서는 헤더 파서
   (mg_http_parse)와 응답 쓰기(mg_http_reply)만 씁니다.

   종료 코드: 0 정상 종료, 1 시작 실패, 2 잘못된 인자. */
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "mongoose.h"
#include "work_queue.h"

#define EXIT_USAGE 2
#define DEFAULT_DATA_DIR "data"
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_THREAD_COUNT 8
#define DEFAULT_QUEUE_CAPACITY 128
#define DEFAULT_IDLE_TIMEOUT_MS 30000
#define PORT_MAX 65535
#define THREAD_COUNT_MAX 1024
#define QUEUE_CAPACITY_MAX 1000000
#define IDLE_TIMEOUT_MS_MAX 86400000
#define LISTEN_URL_BYTES 300
/* 작업 스레드가 mg_wakeup()으로 깨우므로 이 시간은 응답 지연이 아니라, 깨우는
   신호를 놓쳤을 때 완료 대기열을 다시 보기까지의 상한입니다. 쉬는 연결을
   닫는 시각도 이 간격만큼 늦을 수 있습니다. */
#define POLL_INTERVAL_MS 100

/* 요청 하나의 헤더(요청 줄 포함)와 본문의 상한입니다. 둘의 합은 Mongoose의
   연결당 수신 버퍼 한도(MG_MAX_RECV_SIZE, 3 MiB)보다 작아야 합니다. 그
   한도에 먼저 걸리면 Mongoose가 응답 없이 연결을 닫기 때문입니다. 응답을
   기다리는 동안에는 그 연결에서 읽지 않으므로(is_full) 버퍼에는 요청 하나와
   그 앞뒤의 일부만 쌓입니다. */
#define HEADER_BYTES_MAX (8 * 1024)
#define BODY_BYTES_MAX (1024 * 1024)
_Static_assert(HEADER_BYTES_MAX + BODY_BYTES_MAX < MG_MAX_RECV_SIZE,
               "요청 상한은 Mongoose 수신 버퍼 한도보다 작아야 합니다");
/* 결과 상한입니다. 엔진은 record 수와 값의 누적 바이트로 멈추고, 서버는 JSON
   응답 바이트로 한 번 더 확인합니다. 이스케이프로 JSON이 값보다 커질 수
   있기 때문입니다. */
#define RESULT_RECORDS_MAX 10000
#define RESULT_BYTES_MAX (16 * 1024 * 1024)
#define RESPONSE_BYTES_MAX (32 * 1024 * 1024)

#define QUERY_PATH "/query"
#define JSON_HEADERS "Content-Type: application/json\r\n"
#define REPLY_HEADERS_BYTES 160
#define ERROR_JSON_BYTES 256
#define HTTP_OK 200
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_METHOD_NOT_ALLOWED 405
#define HTTP_LENGTH_REQUIRED 411
#define HTTP_PAYLOAD_TOO_LARGE 413
#define HTTP_HEADER_TOO_LARGE 431
#define HTTP_INTERNAL_ERROR 500
#define HTTP_UNAVAILABLE 503
/* JSON을 만들 메모리도 없을 때 보내는 본문입니다. 할당 없이 보냅니다. */
#define OUT_OF_MEMORY_JSON                                                     \
    "{\"error\": \"out_of_memory\", \"message\": \"메모리가 부족합니다\"}"
#define RESPONSE_TOO_LARGE_JSON                                                \
    "{\"error\": \"result_too_large\", \"message\": \"응답 JSON이 상한을 "     \
    "넘습니다\"}"

struct server_args {
    const char *data_dir;
    const char *host;
    long port;
    long thread_count;
    long queue_capacity;
    long idle_timeout_ms;
};

/* 요청 하나입니다. 이벤트 루프가 만들어 작업 대기열에 넣고, 작업 스레드가
   status와 json을 채워 같은 구조체를 완료 대기열에 넣습니다. 응답을 보낸
   이벤트 루프가 해제합니다. json이 NULL이면 OUT_OF_MEMORY_JSON을 보냅니다. */
struct request {
    struct request *next; /* 완료 대기열 안의 다음 요청 */
    unsigned long connection_id;
    char *sql;
    int status;
    char *json;
};

/* 완료 대기열입니다. 작업 스레드가 끝에 넣고 이벤트 루프가 통째로 꺼냅니다. */
struct done_queue {
    pthread_mutex_t mutex;
    struct request *head;
    struct request *tail;
};

struct server {
    struct mg_mgr manager;
    struct engine *engine;
    struct work_queue *work_queue;
    struct done_queue done;
    uint64_t idle_timeout_ms;
};

/* 받은 연결 하나의 상태입니다. 이벤트 루프만 읽고 씁니다. 연결의 data 칸에
   이 구조체의 주소를 넣어 두고, 연결이 닫힐 때 해제합니다.
   closes_after_reply는 요청을 받을 때 정하고 그 요청의 응답을 보낼 때
   씁니다. */
struct client {
    uint64_t last_active_ms; /* 마지막으로 바이트를 받거나 보낸 시각 */
    bool awaits_reply; /* 요청 하나가 작업 대기열이나 작업 스레드에 있음 */
    bool closes_after_reply;
    bool announces_keep_alive; /* keep-alive를 요청한 HTTP/1.0 */
};

/* 헤더의 본문 길이 표기를 읽은 결과입니다. */
enum body_length_status {
    BODY_LENGTH_DECLARED, /* Content-Length 하나, 숫자만 */
    BODY_LENGTH_MISSING,  /* Content-Length도 Transfer-Encoding도 없음 */
    BODY_LENGTH_CHUNKED,  /* Content-Length 없이 Transfer-Encoding */
    BODY_LENGTH_INVALID,  /* 둘이 함께 옴, 여러 번 옴, 숫자가 아님 */
};

static int args_parse(int argc, char *argv[], struct server_args *args);
static bool number_parse(const char *text, long min, long max, long *number);
static int server_run(const struct server_args *args);
static void connection_event_handle(struct mg_connection *connection, int event,
                                    void *event_data);
static struct client *client_find(const struct mg_connection *connection);
static void client_attach(struct mg_connection *connection);
static void client_close_if_idle(const struct server *server,
                                 struct mg_connection *connection,
                                 uint64_t now_ms);
static void requests_handle(struct server *server,
                            struct mg_connection *connection);
static size_t request_take(struct server *server,
                           struct mg_connection *connection,
                           struct client *client);
static enum body_length_status body_length_parse(struct mg_str head,
                                                 size_t *body_len);
static bool header_has_token(struct mg_http_message *message, const char *name,
                             const char *token);
static void request_route(struct server *server,
                          struct mg_connection *connection,
                          struct client *client,
                          const struct mg_http_message *message,
                          enum body_length_status length_status);
static void request_submit(struct server *server,
                           struct mg_connection *connection,
                           struct client *client,
                           const struct mg_http_message *message);
static void connection_refuse(struct mg_connection *connection, int status,
                              const char *reason, const char *message);
static void error_reply(struct mg_connection *connection, int status,
                        const char *headers, const char *reason,
                        const char *message);
static void reply_send(struct mg_connection *connection, int status,
                       const char *headers, const char *json);
static void request_execute(void *job, void *context);
static void done_queue_push(struct done_queue *done, struct request *request);
static void done_replies_send(struct server *server);
static struct mg_connection *connection_find(struct mg_mgr *manager,
                                             unsigned long connection_id);
static void request_destroy(struct request *request);
static int result_status_get(const struct engine_result *result);
static const char *error_reason_name(enum engine_error_reason reason);
static char *result_json_create(const struct engine_result *result,
                                bool *is_too_large);
static bool json_result_write(FILE *out, const struct engine_result *result);
static void json_value_write(FILE *out, const struct value *value);
static void json_string_write(FILE *out, const char *text, size_t text_len);

int main(int argc, char *argv[])
{
    struct server_args args = {
        .data_dir = DEFAULT_DATA_DIR,
        .host = DEFAULT_HOST,
        .port = DEFAULT_PORT,
        .thread_count = DEFAULT_THREAD_COUNT,
        .queue_capacity = DEFAULT_QUEUE_CAPACITY,
        .idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS,
    };

    if (args_parse(argc, argv, &args) != 0) {
        fprintf(stderr,
                "usage: %s [--data <dir>] [--host <addr>] [--port <n>]\n"
                "          [--threads <n>] [--queue <n>] [--idle-timeout "
                "<ms>]\n"
                "  --data     데이터 폴더 (기본 %s)\n"
                "  --host     묶을 주소 (기본 %s). 시연용입니다. 서버에는 "
                "인증이 없으므로\n"
                "             127.0.0.1이 아닌 주소를 열면 그 주소에 닿는 "
                "누구나 SQL을 실행합니다\n"
                "  --port     포트 (기본 %d)\n"
                "  --threads  작업 스레드 수 (기본 %d)\n"
                "  --queue    작업 대기열 칸 수 (기본 %d). 0이면 모든 SQL "
                "요청을 503으로 거절합니다\n"
                "  --idle-timeout  주고받은 바이트가 없는 연결을 닫기까지의 "
                "밀리초 (기본 %d)\n",
                argv[0], DEFAULT_DATA_DIR, DEFAULT_HOST, DEFAULT_PORT,
                DEFAULT_THREAD_COUNT, DEFAULT_QUEUE_CAPACITY,
                DEFAULT_IDLE_TIMEOUT_MS);
        return EXIT_USAGE;
    }
    return server_run(&args);
}

/* 옵션마다 값 하나를 받습니다. 위치 인자는 없습니다. */
static int args_parse(int argc, char *argv[], struct server_args *args)
{
    for (int i = 1; i < argc; i++) {
        const char *option = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        bool valid = value != NULL;

        if (!valid) {
            return -1;
        }
        if (strcmp(option, "--data") == 0) {
            args->data_dir = value;
        } else if (strcmp(option, "--host") == 0) {
            args->host = value;
        } else if (strcmp(option, "--port") == 0) {
            valid = number_parse(value, 1, PORT_MAX, &args->port);
        } else if (strcmp(option, "--threads") == 0) {
            valid =
                number_parse(value, 1, THREAD_COUNT_MAX, &args->thread_count);
        } else if (strcmp(option, "--queue") == 0) {
            valid = number_parse(value, 0, QUEUE_CAPACITY_MAX,
                                 &args->queue_capacity);
        } else if (strcmp(option, "--idle-timeout") == 0) {
            valid = number_parse(value, 1, IDLE_TIMEOUT_MS_MAX,
                                 &args->idle_timeout_ms);
        } else {
            valid = false;
        }
        if (!valid) {
            return -1;
        }
        i++;
    }
    return 0;
}

static bool number_parse(const char *text, long min, long max, long *number)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < min ||
        parsed > max) {
        return false;
    }
    *number = parsed;
    return true;
}

/* 엔진, Mongoose 매니저, 작업 스레드 순서로 만들고 이벤트 루프를 돕니다.
   작업 스레드는 매니저의 깨우기 통로를 쓰므로 매니저 뒤에 만듭니다. */
static int server_run(const struct server_args *args)
{
    static struct server server;
    const struct engine_options options = {.max_records = RESULT_RECORDS_MAX,
                                           .max_bytes = RESULT_BYTES_MAX};
    char error[ENGINE_ERROR_MAX];
    char listen_url[LISTEN_URL_BYTES];

    server.engine =
        engine_create(args->data_dir, &options, error, sizeof(error));
    if (server.engine == NULL) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    server.idle_timeout_ms = (uint64_t)args->idle_timeout_ms;
    pthread_mutex_init(&server.done.mutex, NULL);
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&server.manager);
    snprintf(listen_url, sizeof(listen_url), "http://%s:%ld", args->host,
             args->port);
    if (!mg_wakeup_init(&server.manager) ||
        mg_listen(&server.manager, listen_url, connection_event_handle,
                  &server) == NULL) {
        fprintf(stderr, "%s: 연결을 받을 수 없습니다\n", listen_url);
        mg_mgr_free(&server.manager);
        engine_destroy(server.engine);
        return EXIT_FAILURE;
    }
    server.work_queue = work_queue_create((size_t)args->thread_count,
                                          (size_t)args->queue_capacity,
                                          request_execute, &server);
    if (server.work_queue == NULL) {
        fprintf(stderr, "작업 스레드를 만들 수 없습니다\n");
        mg_mgr_free(&server.manager);
        engine_destroy(server.engine);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "listening on %s (threads %ld, queue %ld)\n", listen_url,
            args->thread_count, args->queue_capacity);
    for (;;) {
        mg_mgr_poll(&server.manager, POLL_INTERVAL_MS);
        done_replies_send(&server);
    }
}

/* 이벤트 루프에서만 불립니다. 듣는 연결의 이벤트는 넘기고, 받은 연결마다
   상태를 붙이고, 바이트가 오면 요청을 꺼내고, 매 poll마다 쉬는 시간을
   잽니다. MG_EV_WAKEUP은 mg_mgr_poll을 깨우는 일로 끝나고, 응답은 poll 뒤의
   done_replies_send가 보냅니다. */
static void connection_event_handle(struct mg_connection *connection, int event,
                                    void *event_data)
{
    struct server *server = connection->fn_data;
    struct client *client = client_find(connection);

    if (connection->is_listening) {
        return;
    }
    if (event == MG_EV_ACCEPT) {
        client_attach(connection);
    } else if (client == NULL) {
        return;
    } else if (event == MG_EV_READ || event == MG_EV_WRITE) {
        client->last_active_ms = mg_millis();
        if (event == MG_EV_READ) {
            requests_handle(server, connection);
        }
    } else if (event == MG_EV_POLL) {
        client_close_if_idle(server, connection, *(uint64_t *)event_data);
    } else if (event == MG_EV_CLOSE) {
        free(client);
    }
}

/* 연결의 data 칸에는 struct client의 주소가 들어 있습니다. 칸의 정렬을 알 수
   없으므로 memcpy로 읽습니다. 붙이기 전이면 NULL입니다. */
static struct client *client_find(const struct mg_connection *connection)
{
    struct client *client;

    memcpy(&client, connection->data, sizeof(client));
    return client;
}

static void client_attach(struct mg_connection *connection)
{
    struct client *client = calloc(1, sizeof(*client));

    if (client == NULL) {
        connection->is_closing = 1;
        return;
    }
    client->last_active_ms = mg_millis();
    memcpy(connection->data, &client, sizeof(client));
}

/* 응답을 기다리는 연결은 서버가 일하는 중이라 닫지 않습니다. 보낼 응답이
   남았는데 클라이언트가 받지 않는 연결도 쉬는 연결로 보고 닫습니다. */
static void client_close_if_idle(const struct server *server,
                                 struct mg_connection *connection,
                                 uint64_t now_ms)
{
    const struct client *client = client_find(connection);

    if (!client->awaits_reply &&
        now_ms - client->last_active_ms >= server->idle_timeout_ms) {
        connection->is_closing = 1;
    }
}

/* 받은 바이트에서 요청을 하나씩 꺼내 처리합니다. 같은 연결의 다음 요청은 앞
   요청의 응답을 송신 버퍼에 넣은 뒤에 꺼냅니다. 닫기로 한 연결의 뒤 요청은
   꺼내지 않습니다. */
static void requests_handle(struct server *server,
                            struct mg_connection *connection)
{
    struct client *client = client_find(connection);
    size_t request_len;

    while (!client->awaits_reply && !connection->is_draining &&
           !connection->is_closing && connection->recv.len > 0) {
        request_len = request_take(server, connection, client);
        if (request_len == 0) {
            return;
        }
        mg_iobuf_del(&connection->recv, 0, request_len);
    }
}

/* 버퍼 맨 앞의 요청 하나를 검사하고 처리합니다. 처리한 요청의 바이트 수를
   돌려주고, 더 받아야 하거나 연결을 닫기로 했으면 0입니다. 헤더 단계에서
   본문 길이를 검사하므로, 선언한 본문을 다 받기 전에 끊긴 요청은 실행되지
   않습니다. 길이를 믿을 수 없는 요청은 다음 요청의 시작도 알 수 없으므로
   연결을 닫습니다. */
static size_t request_take(struct server *server,
                           struct mg_connection *connection,
                           struct client *client)
{
    const char *buffer = (const char *)connection->recv.buf;
    int head_len =
        mg_http_get_request_len(connection->recv.buf, connection->recv.len);
    struct mg_http_message message;
    enum body_length_status length_status;
    size_t body_len = 0;

    if (head_len == 0 && connection->recv.len <= HEADER_BYTES_MAX) {
        return 0;
    }
    if (head_len == 0 || head_len > HEADER_BYTES_MAX) {
        connection_refuse(connection, HTTP_HEADER_TOO_LARGE, "header_too_large",
                          "헤더는 8 KiB까지 받습니다");
        return 0;
    }
    length_status =
        head_len < 0
            ? BODY_LENGTH_INVALID
            : body_length_parse(mg_str_n(buffer, (size_t)head_len), &body_len);
    if (length_status == BODY_LENGTH_INVALID) {
        connection_refuse(connection, HTTP_BAD_REQUEST, "invalid_body_length",
                          "Content-Length 하나에 숫자만 적고, "
                          "Transfer-Encoding과 함께 보내지 않습니다");
        return 0;
    }
    if (length_status == BODY_LENGTH_CHUNKED) {
        connection_refuse(connection, HTTP_LENGTH_REQUIRED, "length_required",
                          "chunked 대신 Content-Length로 보냅니다");
        return 0;
    }
    if (body_len > BODY_BYTES_MAX) {
        connection_refuse(connection, HTTP_PAYLOAD_TOO_LARGE, "body_too_large",
                          "본문은 1 MiB까지 받습니다");
        return 0;
    }
    if (mg_http_parse(buffer, (size_t)head_len, &message) <= 0) {
        connection_refuse(connection, HTTP_BAD_REQUEST, "invalid_request",
                          "HTTP 요청으로 읽을 수 없습니다");
        return 0;
    }
    if (connection->recv.len - (size_t)head_len < body_len) {
        return 0;
    }
    message.body = mg_str_n(buffer + head_len, body_len);
    client->announces_keep_alive =
        mg_strcasecmp(message.proto, mg_str("HTTP/1.0")) == 0 &&
        header_has_token(&message, "Connection", "keep-alive");
    client->closes_after_reply =
        header_has_token(&message, "Connection", "close") ||
        (mg_strcasecmp(message.proto, mg_str("HTTP/1.0")) == 0 &&
         !client->announces_keep_alive);
    request_route(server, connection, client, &message, length_status);

    return (size_t)head_len + body_len;
}

/* 헤더의 Content-Length와 Transfer-Encoding을 직접 셉니다. mg_http_parse는
   이 경우들을 구분하지 않고 실패로만 돌려주기 때문입니다. 1 MiB를 넘는
   길이는 정확한 값 대신 BODY_BYTES_MAX + 1로 둡니다. */
static enum body_length_status body_length_parse(struct mg_str head,
                                                 size_t *body_len)
{
    struct mg_str line;
    struct mg_str rest = head;
    int length_count = 0;
    int encoding_count = 0;
    bool is_number = true;

    *body_len = 0;
    mg_span(rest, NULL, &rest, '\n'); /* 요청 줄 */
    while (mg_span(rest, &line, &rest, '\n')) {
        struct mg_str name;
        struct mg_str value;

        if (!mg_span(line, &name, &value, ':')) {
            continue;
        }
        if (mg_strcasecmp(name, mg_str("Transfer-Encoding")) == 0) {
            encoding_count++;
        } else if (mg_strcasecmp(name, mg_str("Content-Length")) == 0) {
            size_t digit_count = 0;

            length_count++;
            for (size_t i = 0; i < value.len; i++) {
                char letter = value.buf[i];

                if (letter >= '0' && letter <= '9') {
                    digit_count++;
                    *body_len = *body_len > BODY_BYTES_MAX
                                    ? *body_len
                                    : *body_len * 10 + (size_t)(letter - '0');
                } else if (letter != ' ' && letter != '\t' && letter != '\r') {
                    is_number = false;
                }
            }
            is_number = is_number && digit_count > 0;
        }
    }
    if (length_count > 1 || (length_count == 1 && encoding_count > 0) ||
        (length_count == 1 && !is_number)) {
        return BODY_LENGTH_INVALID;
    }
    if (encoding_count > 0) {
        return BODY_LENGTH_CHUNKED;
    }
    return length_count == 1 ? BODY_LENGTH_DECLARED : BODY_LENGTH_MISSING;
}

/* 쉼표로 나눈 헤더 값 중에 token이 있는지 봅니다(대소문자 무시). */
static bool header_has_token(struct mg_http_message *message, const char *name,
                             const char *token)
{
    struct mg_str *value = mg_http_get_header(message, name);
    struct mg_str rest = value == NULL ? mg_str("") : *value;
    struct mg_str item;

    while (mg_span(rest, &item, &rest, ',')) {
        while (item.len > 0 && (item.buf[0] == ' ' || item.buf[0] == '\t')) {
            item.buf++;
            item.len--;
        }
        while (item.len > 0 && (item.buf[item.len - 1] == ' ' ||
                                item.buf[item.len - 1] == '\t')) {
            item.len--;
        }
        if (mg_strcasecmp(item, mg_str(token)) == 0) {
            return true;
        }
    }
    return false;
}

/* 경로, 메서드, 본문을 차례로 보고 거절하거나 작업 대기열에 넣습니다. 여기서
   거절한 요청은 길이를 믿을 수 있으므로 연결을 계속 씁니다. */
static void request_route(struct server *server,
                          struct mg_connection *connection,
                          struct client *client,
                          const struct mg_http_message *message,
                          enum body_length_status length_status)
{
    if (mg_strcmp(message->uri, mg_str(QUERY_PATH)) != 0) {
        error_reply(connection, HTTP_NOT_FOUND, "", "route_not_found",
                    "경로는 /query 하나입니다");
    } else if (mg_strcmp(message->method, mg_str("POST")) != 0) {
        error_reply(connection, HTTP_METHOD_NOT_ALLOWED, "Allow: POST\r\n",
                    "method_not_allowed", "/query는 POST만 받습니다");
    } else if (length_status == BODY_LENGTH_MISSING) {
        error_reply(connection, HTTP_LENGTH_REQUIRED, "", "length_required",
                    "본문 길이를 Content-Length로 보냅니다");
    } else if (memchr(message->body.buf, '\0', message->body.len) != NULL) {
        error_reply(connection, HTTP_BAD_REQUEST, "", "invalid_sql_body",
                    "본문에 NUL 바이트가 있습니다");
    } else {
        request_submit(server, connection, client, message);
    }
}

/* 본문의 SQL을 NUL로 끝나는 새 문자열로 복사해 작업 대기열에 넣습니다.
   요청의 바이트는 이 함수가 끝나면 수신 버퍼에서 지워지기 때문입니다. 응답을
   보낼 때까지 이 연결에서 읽지 않습니다(is_full). */
static void request_submit(struct server *server,
                           struct mg_connection *connection,
                           struct client *client,
                           const struct mg_http_message *message)
{
    struct request *request = calloc(1, sizeof(*request));

    if (request != NULL) {
        request->sql = malloc(message->body.len + 1);
    }
    if (request == NULL || request->sql == NULL) {
        free(request);
        reply_send(connection, HTTP_INTERNAL_ERROR, "", OUT_OF_MEMORY_JSON);
        return;
    }
    memcpy(request->sql, message->body.buf, message->body.len);
    request->sql[message->body.len] = '\0';
    request->connection_id = connection->id;
    if (!work_queue_submit(server->work_queue, request)) {
        request_destroy(request);
        error_reply(connection, HTTP_UNAVAILABLE, "", "queue_full",
                    "작업 대기열이 가득 찼습니다");
        return;
    }
    client->awaits_reply = true;
    connection->is_full = 1;
}

/* 길이를 믿을 수 없는 요청에 답하고 연결을 닫습니다. 남은 바이트는 읽지
   않습니다. */
static void connection_refuse(struct mg_connection *connection, int status,
                              const char *reason, const char *message)
{
    client_find(connection)->closes_after_reply = true;
    error_reply(connection, status, "", reason, message);
    connection->recv.len = 0;
}

/* 이벤트 루프가 직접 만드는 오류 응답입니다. reason과 message는 고정된
   문자열이라 이스케이프하지 않습니다. */
static void error_reply(struct mg_connection *connection, int status,
                        const char *headers, const char *reason,
                        const char *message)
{
    char json[ERROR_JSON_BYTES];

    snprintf(json, sizeof(json), "{\"error\": \"%s\", \"message\": \"%s\"}",
             reason, message);
    reply_send(connection, status, headers, json);
}

/* 응답을 송신 버퍼에 넣습니다. 요청을 받을 때 닫기로 정한 연결이면
   Connection: close를 붙이고, 보낸 뒤 닫습니다(is_draining). */
static void reply_send(struct mg_connection *connection, int status,
                       const char *headers, const char *json)
{
    const struct client *client = client_find(connection);
    char reply_headers[REPLY_HEADERS_BYTES];
    const char *connection_header = "";

    if (client->closes_after_reply) {
        connection_header = "Connection: close\r\n";
    } else if (client->announces_keep_alive) {
        connection_header = "Connection: keep-alive\r\n";
    }
    snprintf(reply_headers, sizeof(reply_headers), "%s%s%s", JSON_HEADERS,
             headers, connection_header);
    mg_http_reply(connection, status, reply_headers, "%s", json);
    if (client->closes_after_reply) {
        connection->is_draining = 1;
    }
}

/* 작업 스레드에서 돕니다. 엔진을 API 모드(next_sql NULL)로 불러 문장
   하나만 실행합니다. 엔진의 잠금은 engine_execute 안에서 풀리므로, JSON
   만들기는 잠금 밖에서 합니다. 연결은 만지지 않고 이벤트 루프를 깨우기만
   합니다. */
static void request_execute(void *job, void *context)
{
    struct request *request = job;
    struct server *server = context;
    /* 완료 대기열에 넣은 뒤에는 이벤트 루프가 request를 해제할 수 있으므로
       깨울 연결 번호를 먼저 복사해 둡니다. */
    unsigned long connection_id = request->connection_id;
    struct engine_result *result =
        engine_execute(server->engine, request->sql, NULL);
    bool is_too_large = false;

    request->status = result_status_get(result);
    request->json =
        result == NULL ? NULL : result_json_create(result, &is_too_large);
    if (is_too_large) {
        request->status = HTTP_BAD_REQUEST;
        request->json = strdup(RESPONSE_TOO_LARGE_JSON);
    }
    if (request->json == NULL) {
        request->status = HTTP_INTERNAL_ERROR;
    }
    engine_result_destroy(result);
    done_queue_push(&server->done, request);
    /* 결과는 완료 대기열로만 전합니다. 깨우기에는 아무것도 싣지 않습니다. */
    mg_wakeup(&server->manager, connection_id, "", 0);
}

static void done_queue_push(struct done_queue *done, struct request *request)
{
    pthread_mutex_lock(&done->mutex);
    request->next = NULL;
    if (done->tail == NULL) {
        done->head = request;
    } else {
        done->tail->next = request;
    }
    done->tail = request;
    pthread_mutex_unlock(&done->mutex);
}

/* 이벤트 루프에서 매 poll 뒤에 불립니다. 완료 대기열을 통째로 꺼내, 연결이
   남아 있으면 응답을 송신 버퍼에 넣고, 끊겼으면 결과만 해제합니다. 끊긴
   연결의 INSERT도 엔진에서는 이미 끝났습니다. */
static void done_replies_send(struct server *server)
{
    struct request *request;

    pthread_mutex_lock(&server->done.mutex);
    request = server->done.head;
    server->done.head = NULL;
    server->done.tail = NULL;
    pthread_mutex_unlock(&server->done.mutex);

    while (request != NULL) {
        struct request *next = request->next;
        struct mg_connection *connection =
            connection_find(&server->manager, request->connection_id);

        if (connection != NULL) {
            client_find(connection)->awaits_reply = false;
            connection->is_full = 0;
            reply_send(connection, request->status, "",
                       request->json != NULL ? request->json
                                             : OUT_OF_MEMORY_JSON);
            /* 응답을 기다리는 동안 받아 둔 같은 연결의 다음 요청입니다. */
            requests_handle(server, connection);
        }
        request_destroy(request);
        request = next;
    }
}

static struct mg_connection *connection_find(struct mg_mgr *manager,
                                             unsigned long connection_id)
{
    for (struct mg_connection *connection = manager->conns; connection != NULL;
         connection = connection->next) {
        if (connection->id == connection_id) {
            return connection;
        }
    }
    return NULL;
}

static void request_destroy(struct request *request)
{
    free(request->sql);
    free(request->json);
    free(request);
}

/* 오류 이유마다 상태 코드를 고릅니다. 요청 쪽 잘못은 400, 서버 쪽 실패는
   500입니다. 문장 없음(공백뿐인 본문)도 요청 쪽 잘못입니다. */
static int result_status_get(const struct engine_result *result)
{
    if (result == NULL) {
        return HTTP_INTERNAL_ERROR;
    }
    if (result->status == ENGINE_OK) {
        return HTTP_OK;
    }
    if (result->status == ENGINE_EMPTY) {
        return HTTP_BAD_REQUEST;
    }
    switch (result->error_reason) {
    case ENGINE_REASON_SYNTAX:
    case ENGINE_REASON_NO_TABLE:
    case ENGINE_REASON_NO_FIELD:
    case ENGINE_REASON_VALUE_COUNT:
    case ENGINE_REASON_TYPE:
    case ENGINE_REASON_INT_RANGE:
    case ENGINE_REASON_TEXT_TOO_LONG:
    case ENGINE_REASON_MULTIPLE_STATEMENTS:
    case ENGINE_REASON_RESULT_TOO_LARGE:
        return HTTP_BAD_REQUEST;
    default:
        return HTTP_INTERNAL_ERROR;
    }
}

/* 응답의 error 값입니다. 클라이언트가 읽는 고정된 영어 문자열입니다.
   이유가 없는 엔진 오류(ID 소진)는 서버 쪽 실패로 storage_error입니다. */
static const char *error_reason_name(enum engine_error_reason reason)
{
    switch (reason) {
    case ENGINE_REASON_SYNTAX:
        return "syntax_error";
    case ENGINE_REASON_NO_TABLE:
        return "table_not_found";
    case ENGINE_REASON_NO_FIELD:
        return "field_not_found";
    case ENGINE_REASON_VALUE_COUNT:
        return "value_count_mismatch";
    case ENGINE_REASON_TYPE:
        return "type_mismatch";
    case ENGINE_REASON_INT_RANGE:
        return "int_out_of_range";
    case ENGINE_REASON_TEXT_TOO_LONG:
        return "text_too_long";
    case ENGINE_REASON_MULTIPLE_STATEMENTS:
        return "multiple_statements";
    case ENGINE_REASON_RESULT_TOO_LARGE:
        return "result_too_large";
    case ENGINE_REASON_OUT_OF_MEMORY:
        return "out_of_memory";
    case ENGINE_REASON_TABLE_UNAVAILABLE:
        return "table_unavailable";
    default:
        return "storage_error";
    }
}

/* 결과 하나를 JSON 문자열 하나로 만듭니다. 메모리가 없거나 JSON이
   RESPONSE_BYTES_MAX를 넘으면 NULL이고, 넘었을 때는 *is_too_large가
   참입니다. */
static char *result_json_create(const struct engine_result *result,
                                bool *is_too_large)
{
    char *json = NULL;
    size_t json_len = 0;
    FILE *out = open_memstream(&json, &json_len);

    if (out == NULL) {
        return NULL;
    }
    *is_too_large = !json_result_write(out, result);
    if (ferror(out) || *is_too_large) {
        fclose(out);
        free(json);
        return NULL;
    }
    fclose(out);

    return json;
}

/* JSON을 쓰다가 RESPONSE_BYTES_MAX를 넘으면 멈추고 false입니다. 엔진의 결과
   상한 안이라도 이스케이프가 JSON을 키울 수 있으므로 record마다 봅니다. */
static bool json_result_write(FILE *out, const struct engine_result *result)
{
    if (result->status == ENGINE_EMPTY) {
        fputs("{\"error\": \"no_statement\", \"message\": \"실행할 SQL 문장이 "
              "없습니다\"}",
              out);
    } else if (result->status == ENGINE_ERROR) {
        fprintf(out, "{\"error\": \"%s\", \"message\": ",
                error_reason_name(result->error_reason));
        json_string_write(out, result->error_message,
                          strlen(result->error_message));
        fputc('}', out);
    } else if (result->kind == ENGINE_STATEMENT_INSERT) {
        fprintf(out, "{\"inserted\": 1, \"id\": %" PRId64 "}",
                result->inserted_id);
    } else {
        fputs("{\"fields\": [", out);
        for (size_t j = 0; j < result->field_count; j++) {
            fputs(j == 0 ? "" : ", ", out);
            json_string_write(out, result->field_names[j],
                              strlen(result->field_names[j]));
        }
        fputs("], \"records\": [", out);
        for (size_t i = 0; i < result->record_count; i++) {
            fputs(i == 0 ? "[" : ", [", out);
            for (size_t j = 0; j < result->field_count; j++) {
                fputs(j == 0 ? "" : ", ", out);
                json_value_write(out,
                                 &result->values[i * result->field_count + j]);
            }
            fputc(']', out);
            if (ftell(out) > RESPONSE_BYTES_MAX) {
                return false;
            }
        }
        fprintf(
            out,
            "], \"access_path\": \"%s\", \"records_examined\": %" PRIu64 "}",
            result->access_path == ENGINE_ACCESS_PATH_INDEX ? "INDEX" : "SCAN",
            result->records_examined);
    }
    return true;
}

/* INT는 double을 거치지 않고 64비트 정수 그대로 씁니다. */
static void json_value_write(FILE *out, const struct value *value)
{
    if (value->type == VALUE_INT) {
        fprintf(out, "%" PRId64, value->integer);
    } else {
        json_string_write(out, value->text, value->text_len);
    }
}

/* JSON 문자열에서 그대로 쓸 수 없는 따옴표, 역슬래시, 제어 문자(0x00부터 0x1f)만
   이스케이프합니다. 다른 바이트는 UTF-8이라고 보고 그대로 씁니다. */
static void json_string_write(FILE *out, const char *text, size_t text_len)
{
    fputc('"', out);
    for (size_t i = 0; i < text_len; i++) {
        unsigned char byte = (unsigned char)text[i];

        if (byte == '"' || byte == '\\') {
            fputc('\\', out);
            fputc(byte, out);
        } else if (byte < 0x20) {
            fprintf(out, "\\u%04x", byte);
        } else {
            fputc(byte, out);
        }
    }
    fputc('"', out);
}
