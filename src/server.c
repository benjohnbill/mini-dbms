/* mini-dbms API 서버의 진입점입니다. `POST /query`의 본문에 담긴 SQL 문장
   하나를 DB 엔진으로 실행하고 결과를 JSON으로 돌려줍니다.

   스레드는 두 종류입니다. 이벤트 루프(main 스레드) 하나가 Mongoose로 연결을
   받고, 요청의 SQL만 복사해 작업 대기열에 넣고, 응답을 보냅니다. 작업
   스레드들은 대기열에서 요청을 꺼내 엔진을 API 모드로 부르고, JSON을 만들어
   완료 대기열에 넣은 뒤 mg_wakeup()으로 이벤트 루프를 깨우기만 합니다.
   Mongoose의 연결은 이벤트 루프만 만지므로, 작업 스레드는 연결 대신 연결
   번호만 들고 다닙니다. 응답을 보낼 때 그 번호의 연결이 이미 닫혔으면
   응답을 버립니다.

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
#define PORT_MAX 65535
#define THREAD_COUNT_MAX 1024
#define QUEUE_CAPACITY_MAX 1000000
#define LISTEN_URL_BYTES 300
/* 작업 스레드가 mg_wakeup()으로 깨우므로 이 시간은 응답 지연이 아니라, 깨우는
   신호를 놓쳤을 때 완료 대기열을 다시 보기까지의 상한입니다. */
#define POLL_INTERVAL_MS 100

#define QUERY_PATH "/query"
#define JSON_HEADERS "Content-Type: application/json\r\n"
#define HTTP_OK 200
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_METHOD_NOT_ALLOWED 405
#define HTTP_INTERNAL_ERROR 500
#define HTTP_UNAVAILABLE 503
/* JSON을 만들 메모리도 없을 때 보내는 본문입니다. 할당 없이 보냅니다. */
#define OUT_OF_MEMORY_JSON                                                     \
    "{\"error\": \"out_of_memory\", \"message\": \"메모리가 부족합니다\"}"

struct server_args {
    const char *data_dir;
    const char *host;
    long port;
    long thread_count;
    long queue_capacity;
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
};

static int args_parse(int argc, char *argv[], struct server_args *args);
static bool number_parse(const char *text, long min, long max, long *number);
static int server_run(const struct server_args *args);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void request_accept(struct server *server,
                           struct mg_connection *connection,
                           const struct mg_http_message *message);
static void error_reply(struct mg_connection *connection, int status,
                        const char *headers, const char *reason,
                        const char *message);
static void request_execute(void *job, void *context);
static void done_queue_push(struct done_queue *done, struct request *request);
static void done_replies_send(struct server *server);
static struct mg_connection *connection_find(struct mg_mgr *manager,
                                             unsigned long connection_id);
static void request_destroy(struct request *request);
static int result_status_get(const struct engine_result *result);
static const char *error_reason_name(enum engine_error_reason reason);
static char *result_json_create(const struct engine_result *result);
static void json_result_write(FILE *out, const struct engine_result *result);
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
    };

    if (args_parse(argc, argv, &args) != 0) {
        fprintf(stderr,
                "usage: %s [--data <dir>] [--host <addr>] [--port <n>]\n"
                "          [--threads <n>] [--queue <n>]\n"
                "  --data     데이터 폴더 (기본 %s)\n"
                "  --host     묶을 주소 (기본 %s). 시연용입니다. 서버에는 "
                "인증이 없으므로\n"
                "             127.0.0.1이 아닌 주소를 열면 그 주소에 닿는 "
                "누구나 SQL을 실행합니다\n"
                "  --port     포트 (기본 %d)\n"
                "  --threads  작업 스레드 수 (기본 %d)\n"
                "  --queue    작업 대기열 칸 수 (기본 %d)\n",
                argv[0], DEFAULT_DATA_DIR, DEFAULT_HOST, DEFAULT_PORT,
                DEFAULT_THREAD_COUNT, DEFAULT_QUEUE_CAPACITY);
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
            valid = number_parse(value, 1, QUEUE_CAPACITY_MAX,
                                 &args->queue_capacity);
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
    char error[ENGINE_ERROR_MAX];
    char listen_url[LISTEN_URL_BYTES];

    server.engine = engine_create(args->data_dir, NULL, error, sizeof(error));
    if (server.engine == NULL) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    pthread_mutex_init(&server.done.mutex, NULL);
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&server.manager);
    snprintf(listen_url, sizeof(listen_url), "http://%s:%ld", args->host,
             args->port);
    if (!mg_wakeup_init(&server.manager) ||
        mg_http_listen(&server.manager, listen_url, http_event_handle,
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

/* 이벤트 루프에서만 불립니다. 헤더와 본문을 다 받은 요청만 다룹니다.
   MG_EV_WAKEUP은 mg_mgr_poll을 깨우는 일로 끝나고, 응답은 poll 뒤의
   done_replies_send가 보냅니다. */
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data)
{
    struct mg_http_message *message = event_data;

    if (event != MG_EV_HTTP_MSG) {
        return;
    }
    if (mg_strcmp(message->uri, mg_str(QUERY_PATH)) != 0) {
        error_reply(connection, HTTP_NOT_FOUND, JSON_HEADERS, "route_not_found",
                    "경로는 /query 하나입니다");
    } else if (mg_strcmp(message->method, mg_str("POST")) != 0) {
        error_reply(connection, HTTP_METHOD_NOT_ALLOWED,
                    JSON_HEADERS "Allow: POST\r\n", "method_not_allowed",
                    "/query는 POST만 받습니다");
    } else {
        request_accept(connection->fn_data, connection, message);
    }
}

/* 본문의 SQL을 NUL로 끝나는 새 문자열로 복사해 작업 대기열에 넣습니다.
   Mongoose는 이 함수가 끝나면 받은 요청의 버퍼를 지우기 때문입니다. */
static void request_accept(struct server *server,
                           struct mg_connection *connection,
                           const struct mg_http_message *message)
{
    struct request *request = calloc(1, sizeof(*request));

    if (request != NULL) {
        request->sql = malloc(message->body.len + 1);
    }
    if (request == NULL || request->sql == NULL) {
        free(request);
        mg_http_reply(connection, HTTP_INTERNAL_ERROR, JSON_HEADERS, "%s",
                      OUT_OF_MEMORY_JSON);
        return;
    }
    memcpy(request->sql, message->body.buf, message->body.len);
    request->sql[message->body.len] = '\0';
    request->connection_id = connection->id;
    if (!work_queue_submit(server->work_queue, request)) {
        request_destroy(request);
        error_reply(connection, HTTP_UNAVAILABLE, JSON_HEADERS, "queue_full",
                    "작업 대기열이 가득 찼습니다");
    }
}

/* 이벤트 루프가 직접 만드는 오류 응답입니다. reason과 message는 고정된
   문자열이라 이스케이프하지 않습니다. */
static void error_reply(struct mg_connection *connection, int status,
                        const char *headers, const char *reason,
                        const char *message)
{
    mg_http_reply(connection, status, headers,
                  "{\"error\": \"%s\", \"message\": \"%s\"}", reason, message);
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

    request->status = result_status_get(result);
    request->json = result == NULL ? NULL : result_json_create(result);
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
   남아 있으면 응답을 송신 버퍼에 넣고, 끊겼으면 결과만 해제합니다. */
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
            mg_http_reply(connection, request->status, JSON_HEADERS, "%s",
                          request->json != NULL ? request->json
                                                : OUT_OF_MEMORY_JSON);
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
    case ENGINE_REASON_OUT_OF_MEMORY:
        return "out_of_memory";
    case ENGINE_REASON_TABLE_UNAVAILABLE:
        return "table_unavailable";
    default:
        return "storage_error";
    }
}

/* 결과 하나를 JSON 문자열 하나로 만듭니다. 메모리가 없으면 NULL입니다. */
static char *result_json_create(const struct engine_result *result)
{
    char *json = NULL;
    size_t json_len = 0;
    FILE *out = open_memstream(&json, &json_len);

    if (out == NULL) {
        return NULL;
    }
    json_result_write(out, result);
    if (ferror(out)) {
        fclose(out);
        free(json);
        return NULL;
    }
    fclose(out);

    return json;
}

static void json_result_write(FILE *out, const struct engine_result *result)
{
    if (result->status == ENGINE_EMPTY) {
        fputs("{\"error\": \"empty_body\", \"message\": \"실행할 SQL 문장이 "
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
        }
        fprintf(
            out,
            "], \"access_path\": \"%s\", \"records_examined\": %" PRIu64 "}",
            result->access_path == ENGINE_ACCESS_PATH_INDEX ? "INDEX" : "SCAN",
            result->records_examined);
    }
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
