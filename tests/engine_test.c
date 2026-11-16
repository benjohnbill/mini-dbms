#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "acutest.h"
#include "bptree.h"
#include "engine.h"
#include "engine_bench.h"
#include "table.h"

#define PATH_MAX_LEN 512

#define DATA_DIR_MAX 64
/* 잠금을 기다리면 멈추는 테스트를 실패로 바꾸는 시간입니다. 순서를 맞추는
   데는 쓰지 않습니다. */
#define HANG_LIMIT_SECONDS 20

static char data_dir[DATA_DIR_MAX];

static void write_file(const char *name, const char *content);
static long file_size_get(const char *name);
static long file_size_get(const char *name)
{
    char path[PATH_MAX_LEN];
    FILE *file;
    long size;

    snprintf(path, sizeof(path), "%s/%s", data_dir, name);
    file = fopen(path, "rb");
    TEST_ASSERT(file != NULL);
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fclose(file);

    return size;
}

static unsigned char *file_bytes_create(const char *name, size_t *size);
static ssize_t part_then_fail_pwrite(int fd, const void *bytes, size_t size,
                                     off_t offset);

static int failing_ftruncate(int fd, off_t size);
static ssize_t small_interrupted_pwrite(int fd, const void *bytes, size_t size,
                                        off_t offset);
static void check_only_kim(struct engine *engine);
static void *failing_malloc(size_t size);
static struct engine *users_and_items_engine_create(void);
static void check_table_unavailable(struct engine_result *result);

static void data_dir_create(void);
static void data_dir_remove(void);
static struct engine *users_engine_create(void);
static struct engine *engine_reopen(void);
/* 같은 데이터 폴더로 엔진을 다시 만듭니다. CLI를 다시 실행한 것과
   같습니다. */
static struct engine *engine_reopen(void)
{
    char error[ENGINE_ERROR_MAX] = "";
    struct engine *engine = engine_create(data_dir, NULL, error, sizeof(error));

    TEST_ASSERT(engine != NULL);
    TEST_MSG("error: %s", error);
    return engine;
}

static struct engine_result *execute_one(struct engine *engine,
                                         const char *sql);
static struct engine *limited_engine_create(size_t max_records,
                                            size_t max_bytes);

/* limited_engine_create의 users record 하나는 길이 4, id 8, 'kim' 4 + 3,
   'k@x' 4 + 3, age 8로 34바이트입니다. record i(0부터)는 34 * i에서
   시작하고, 네 건을 넣은 테이블 파일은 136바이트입니다. */
#define LIMITED_RECORD_BYTES 34
#define LIMITED_FILE_BYTES 136

static void select_over_record_limit_returns_part_and_cursor_test(void)
{
    struct engine *engine = limited_engine_create(3, 0);
    struct engine_result *result = execute_one(engine, "SELECT * FROM users;");

    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 3);
    TEST_CHECK(result->has_next_cursor);
    TEST_CHECK(result->next_cursor == 3 * LIMITED_RECORD_BYTES);
    engine_result_destroy(result);

    result = engine_execute_from(engine, "SELECT * FROM users;",
                                 3 * LIMITED_RECORD_BYTES);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 4);
    TEST_CHECK(!result->has_next_cursor);
    engine_result_destroy(result);

    /* 정확히 상한만큼인 결과에는 커서가 없습니다. */
    result = execute_one(engine, "SELECT name FROM users WHERE age = 1;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 3);
    TEST_CHECK(!result->has_next_cursor);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void select_over_byte_limit_returns_part_and_cursor_test(void)
{
    /* 결과 record 하나는 id 8, 'kim' 3, 'k@x' 3, age 8로 22바이트입니다. */
    struct engine *engine = limited_engine_create(0, 50);
    struct engine_result *result =
        execute_one(engine, "SELECT * FROM users WHERE age = 1;");

    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 2);
    TEST_CHECK(result->has_next_cursor);
    TEST_CHECK(result->next_cursor == 2 * LIMITED_RECORD_BYTES);
    engine_result_destroy(result);

    result = engine_execute_from(engine, "SELECT * FROM users WHERE age = 1;",
                                 2 * LIMITED_RECORD_BYTES);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 3);
    TEST_CHECK(!result->has_next_cursor);
    engine_result_destroy(result);
    engine_destroy(engine);
    data_dir_remove();

    /* record 하나가 상한보다 크면 나눌 수 없으므로 result_too_large입니다. */
    engine = limited_engine_create(0, 10);
    result = execute_one(engine, "SELECT * FROM users;");
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_RESULT_TOO_LARGE);
    TEST_CHECK(result->record_count == 0);
    TEST_CHECK(result->values == NULL);
    engine_result_destroy(result);
    engine_destroy(engine);
    data_dir_remove();
}

static void check_invalid_cursor(struct engine *engine, const char *sql,
                                 uint64_t cursor);

static void invalid_cursor_is_rejected_test(void)
{
    struct engine *engine = limited_engine_create(3, 0);
    struct engine_result *result;

    check_invalid_cursor(engine, "SELECT * FROM users;", 1);
    check_invalid_cursor(engine, "SELECT * FROM users;",
                         LIMITED_RECORD_BYTES + 4);
    check_invalid_cursor(engine, "SELECT * FROM users;",
                         LIMITED_FILE_BYTES + 1);
    check_invalid_cursor(engine, "SELECT * FROM users;", UINT64_MAX);
    check_invalid_cursor(engine, "SELECT * FROM users WHERE id = 1;", 0);
    check_invalid_cursor(engine, "INSERT INTO users VALUES ('a', 'b', 1);", 0);

    /* 파일 끝은 읽을 record가 없는 올바른 커서입니다. */
    result =
        engine_execute_from(engine, "SELECT * FROM users;", LIMITED_FILE_BYTES);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 0);
    TEST_CHECK(!result->has_next_cursor);
    engine_result_destroy(result);

    /* 거절한 INSERT는 파일에 쓰지 않았습니다. */
    result = engine_execute_from(engine, "SELECT * FROM users;", 0);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 3);
    TEST_CHECK(result->next_cursor == 3 * LIMITED_RECORD_BYTES);
    engine_result_destroy(result);
    result = engine_execute_from(engine, "SELECT * FROM users;",
                                 3 * LIMITED_RECORD_BYTES);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->record_count == 1);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void insert_between_parts_does_not_wait_test(void)
{
    struct engine *engine = limited_engine_create(3, 0);
    struct engine_result *result = execute_one(engine, "SELECT * FROM users;");
    uint64_t cursor = result->next_cursor;

    TEST_CHECK(result->has_next_cursor);
    engine_result_destroy(result);

    /* 앞 부분의 읽기 잠금이 남아 있으면 같은 스레드의 쓰기 잠금이 영원히
       기다리고, alarm이 테스트를 실패로 끝냅니다. */
    alarm(HANG_LIMIT_SECONDS);
    result = execute_one(engine, "INSERT INTO users VALUES ('new', 'n@x', 1);");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == 5);
    engine_result_destroy(result);
    alarm(0);

    /* 새 record는 뒤 부분에만 나오고, 앞 부분의 record는 다시 나오지
       않습니다. */
    result = engine_execute_from(engine, "SELECT id FROM users;", cursor);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->record_count == 2);
    TEST_CHECK(result->values[0].integer == 4);
    TEST_CHECK(result->values[1].integer == 5);
    TEST_CHECK(!result->has_next_cursor);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void check_text(const struct value *value, const char *expected);

static void insert_returns_id_and_select_returns_record_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result = execute_one(
        engine, "INSERT INTO users VALUES ('kim', 'kim@example.com', 20);");

    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->kind == ENGINE_STATEMENT_INSERT);
    TEST_CHECK(result->inserted_id == 1);
    engine_result_destroy(result);

    result = execute_one(engine, "SELECT * FROM users;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->kind == ENGINE_STATEMENT_SELECT);
    TEST_ASSERT(result->field_count == 4);
    TEST_CHECK(strcmp(result->field_names[0], "id") == 0);
    TEST_CHECK(strcmp(result->field_names[1], "name") == 0);
    TEST_CHECK(strcmp(result->field_names[2], "email") == 0);
    TEST_CHECK(strcmp(result->field_names[3], "age") == 0);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 1);
    check_text(&result->values[1], "kim");
    check_text(&result->values[2], "kim@example.com");
    TEST_CHECK(result->values[3].type == VALUE_INT);
    TEST_CHECK(result->values[3].integer == 20);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void insert_ids_increase_from_one_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *first =
        execute_one(engine, "INSERT INTO users VALUES ('a', 'a@x', 1);");
    struct engine_result *second =
        execute_one(engine, "INSERT INTO users VALUES ('b', 'b@x', -2);");
    struct engine_result *select = execute_one(engine, "SELECT * FROM users;");

    TEST_CHECK(first->inserted_id == 1);
    TEST_CHECK(second->inserted_id == 2);
    TEST_ASSERT(select->record_count == 2);
    TEST_CHECK(select->values[4].integer == 2);
    check_text(&select->values[5], "b");
    TEST_CHECK(select->values[7].integer == -2);
    engine_result_destroy(first);
    engine_result_destroy(second);
    engine_result_destroy(select);

    engine_destroy(engine);
    data_dir_remove();
}

static void select_on_empty_table_returns_field_names_only_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result = execute_one(engine, "SELECT * FROM users;");

    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->field_count == 4);
    TEST_CHECK(result->record_count == 0);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void doubled_quote_in_text_is_one_quote_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result =
        execute_one(engine, "INSERT INTO users VALUES ('O''Brien', ';', 1);");

    TEST_CHECK(result->status == ENGINE_OK);
    engine_result_destroy(result);
    result = execute_one(engine, "SELECT * FROM users;");
    TEST_ASSERT(result->record_count == 1);
    check_text(&result->values[1], "O'Brien");
    check_text(&result->values[2], ";");
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void next_sql_points_after_semicolon_test(void)
{
    struct engine *engine = users_engine_create();
    const char *sql = "SELECT * FROM users;\n  select * from users;  \n";
    const char *next_sql = NULL;
    struct engine_result *result = engine_execute(engine, sql, &next_sql);

    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(next_sql == sql + strlen("SELECT * FROM users;"));
    engine_result_destroy(result);

    result = engine_execute(engine, next_sql, &next_sql);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->kind == ENGINE_STATEMENT_SELECT);
    engine_result_destroy(result);

    result = engine_execute(engine, next_sql, &next_sql);
    TEST_CHECK(result->status == ENGINE_EMPTY);
    TEST_CHECK(*next_sql == '\0');
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void broken_statement_returns_error_test(void)
{
    struct engine *engine = users_engine_create();
    const char *bad_sql[] = {
        "SELECT name FROM users",
        "INSERT INTO users VALUES ('kim', 20);",
        "INSERT INTO users VALUES (1, 'kim@x', 20);",
        "SELECT * FROM nobody;",
        "INSERT INTO users VALUES ('unterminated, 'x', 1);",
    };

    for (size_t i = 0; i < sizeof(bad_sql) / sizeof(bad_sql[0]); i++) {
        struct engine_result *result = execute_one(engine, bad_sql[i]);

        TEST_CHECK(result->status == ENGINE_ERROR);
        TEST_MSG("sql: %s", bad_sql[i]);
        TEST_CHECK(result->error_message[0] != '\0');
        engine_result_destroy(result);
    }

    engine_destroy(engine);
    data_dir_remove();
}

static void restart_keeps_records_and_continues_ids_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    for (int i = 0; i < 3; i++) {
        result = execute_one(engine, "INSERT INTO users VALUES ('a', 'b', 1);");
        TEST_CHECK(result->status == ENGINE_OK);
        engine_result_destroy(result);
    }
    engine_destroy(engine);

    engine = engine_reopen();
    result = execute_one(engine, "SELECT * FROM users;");
    TEST_CHECK(result->record_count == 3);
    engine_result_destroy(result);
    result = execute_one(engine, "INSERT INTO users VALUES ('c', 'd', 2);");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == 4);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void each_table_counts_ids_from_one_test(void)
{
    const char *inserts[] = {
        "INSERT INTO users VALUES ('a', 'b', 1);",
        "INSERT INTO items VALUES ('pen');",
        "INSERT INTO users VALUES ('c', 'd', 2);",
        "INSERT INTO items VALUES ('cup');",
    };
    const int64_t expected_ids[] = {1, 1, 2, 2};
    struct engine *engine;

    data_dir_create();
    write_file("users.schema", "name TEXT\nemail TEXT\nage INT\n");
    write_file("users.tbl", "");
    write_file("items.schema", "title TEXT\n");
    write_file("items.tbl", "");
    engine = engine_reopen();
    for (size_t i = 0; i < sizeof(inserts) / sizeof(inserts[0]); i++) {
        struct engine_result *result = execute_one(engine, inserts[i]);

        TEST_CHECK(result->status == ENGINE_OK);
        TEST_CHECK(result->inserted_id == expected_ids[i]);
        TEST_MSG("sql: %s, id: %lld", inserts[i],
                 (long long)result->inserted_id);
        engine_result_destroy(result);
    }

    engine_destroy(engine);
    data_dir_remove();
}

static void second_engine_on_locked_folder_fails_test(void)
{
    struct engine *first = users_engine_create();
    char error[ENGINE_ERROR_MAX] = "";
    struct engine *second = engine_create(data_dir, NULL, error, sizeof(error));

    TEST_CHECK(second == NULL);
    TEST_CHECK(strstr(error, "in use") != NULL);
    TEST_MSG("error: %s", error);
    engine_destroy(second);
    engine_destroy(first);

    second = engine_reopen();
    engine_destroy(second);
    data_dir_remove();
}

static void bad_schema_stops_engine_create_test(void)
{
    struct {
        const char *schema;
        const char *expected_error;
    } cases[] = {
        {"name TEXT\nbroken\n", "line 2"},
        {"name TEXT\nage INT extra\n", "line 2"},
        {"name TEXT\nname INT\n", "name"},
        {"name VARCHAR\n", "VARCHAR"},
        {"id INT\nname TEXT\n", "id"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char error[ENGINE_ERROR_MAX] = "";
        struct engine *engine;

        data_dir_create();
        write_file("users.schema", cases[i].schema);
        write_file("users.tbl", "");
        engine = engine_create(data_dir, NULL, error, sizeof(error));
        TEST_CHECK(engine == NULL);
        TEST_CHECK(strstr(error, cases[i].expected_error) != NULL);
        TEST_MSG("schema: %s, error: %s", cases[i].schema, error);
        engine_destroy(engine);
        data_dir_remove();
    }
}

static void missing_table_file_stops_engine_create_test(void)
{
    char error[ENGINE_ERROR_MAX] = "";
    char path[PATH_MAX_LEN];
    struct engine *engine;

    data_dir_create();
    write_file("users.schema", "name TEXT\n");
    engine = engine_create(data_dir, NULL, error, sizeof(error));
    TEST_CHECK(engine == NULL);
    TEST_CHECK(strstr(error, "users.tbl") != NULL);
    TEST_MSG("error: %s", error);
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    TEST_CHECK(access(path, F_OK) != 0);
    engine_destroy(engine);
    data_dir_remove();
}

static void insert_after_max_id_fails_and_keeps_file_test(void)
{
    /* age INT 테이블의 record 하나: 본문 길이 16, id = INT64_MAX - 1,
       age = 7. 모든 정수는 리틀 엔디언입니다. */
    const unsigned char record_bytes[] = {
        0x10, 0x00, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0x7f, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    char path[PATH_MAX_LEN];
    struct engine *engine;
    struct engine_result *result;
    FILE *file;

    data_dir_create();
    write_file("ages.schema", "age INT\n");
    snprintf(path, sizeof(path), "%s/ages.tbl", data_dir);
    file = fopen(path, "wb");
    TEST_ASSERT(file != NULL);
    fwrite(record_bytes, 1, sizeof(record_bytes), file);
    fclose(file);
    engine = engine_reopen();

    result = execute_one(engine, "INSERT INTO ages VALUES (8);");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == INT64_MAX);
    engine_result_destroy(result);
    TEST_CHECK(file_size_get("ages.tbl") == 40);

    result = execute_one(engine, "INSERT INTO ages VALUES (9);");
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_ID_EXHAUSTED);
    TEST_CHECK(strstr(result->error_message, "id") != NULL);
    TEST_MSG("error: %s", result->error_message);
    engine_result_destroy(result);
    TEST_CHECK(file_size_get("ages.tbl") == 40);

    engine_destroy(engine);
    data_dir_remove();
}

static void failed_write_with_rollback_keeps_file_and_id_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");
    size_t before_size;
    size_t after_size;
    unsigned char *before;
    unsigned char *after;

    engine_result_destroy(result);
    before = file_bytes_create("users.tbl", &before_size);

    table_file_functions.write_at = part_then_fail_pwrite;
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    table_file_functions.write_at = pwrite;
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_STORAGE_ERROR);
    TEST_MSG("error: %s", result->error_message);
    engine_result_destroy(result);

    after = file_bytes_create("users.tbl", &after_size);
    TEST_CHECK(after_size == before_size);
    TEST_CHECK(after_size == before_size &&
               memcmp(before, after, before_size) == 0);
    free(before);
    free(after);

    result =
        execute_one(engine, "INSERT INTO users VALUES ('park', 'p@x', 4);");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == 2);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void failed_rollback_stops_table_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    table_file_functions.write_at = part_then_fail_pwrite;
    table_file_functions.truncate = failing_ftruncate;
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    table_file_functions.write_at = pwrite;
    table_file_functions.truncate = ftruncate;
    TEST_CHECK(strstr(result->error_message, "unknown whether") != NULL);
    check_table_unavailable(result);

    check_table_unavailable(execute_one(engine, "SELECT * FROM users;"));
    check_table_unavailable(
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 2);"));

    engine_destroy(engine);
    data_dir_remove();
}

static void stopped_table_leaves_other_tables_working_test(void)
{
    struct engine *engine = users_and_items_engine_create();
    struct engine_result *result;

    table_file_functions.write_at = part_then_fail_pwrite;
    table_file_functions.truncate = failing_ftruncate;
    check_table_unavailable(
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);"));
    table_file_functions.write_at = pwrite;
    table_file_functions.truncate = ftruncate;

    result = execute_one(engine, "INSERT INTO items VALUES ('pen');");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == 1);
    engine_result_destroy(result);
    result = execute_one(engine, "SELECT * FROM items;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->record_count == 1);
    check_text(&result->values[1], "pen");
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void select_after_rollback_returns_records_before_failure_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");

    engine_result_destroy(result);
    table_file_functions.write_at = part_then_fail_pwrite;
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    table_file_functions.write_at = pwrite;
    TEST_CHECK(result->status == ENGINE_ERROR);
    engine_result_destroy(result);

    check_only_kim(engine);
    engine_destroy(engine);
    engine = engine_reopen();
    check_only_kim(engine);

    engine_destroy(engine);
    data_dir_remove();
}

static void short_and_interrupted_writes_make_one_record_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    table_file_functions.write_at = small_interrupted_pwrite;
    result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");
    table_file_functions.write_at = pwrite;
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == 1);
    engine_result_destroy(result);
    /* 길이 4 + id 8 + 'kim' 4+3 + 'k@x' 4+3 + age 8 */
    TEST_CHECK(file_size_get("users.tbl") == 34);

    check_only_kim(engine);
    engine_destroy(engine);
    engine = engine_reopen();
    check_only_kim(engine);

    engine_destroy(engine);
    data_dir_remove();
}

/* 함수 표를 바꾸지 않고 실제 pwrite가 EFBIG로 실패하게 합니다. 파일 크기
   상한을 지금 크기로 두면 다음 쓰기가 실패합니다. 상한을 넘는 쓰기는
   SIGXFSZ를 보내므로 무시합니다. */
static void real_file_size_limit_rolls_back_insert_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");
    struct rlimit old_limit;
    struct rlimit new_limit;

    engine_result_destroy(result);
    TEST_ASSERT(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    new_limit = old_limit;
    new_limit.rlim_cur = (rlim_t)file_size_get("users.tbl");
    signal(SIGXFSZ, SIG_IGN);
    TEST_ASSERT(setrlimit(RLIMIT_FSIZE, &new_limit) == 0);
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    TEST_ASSERT(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
    signal(SIGXFSZ, SIG_DFL);

    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_STORAGE_ERROR);
    TEST_MSG("error: %s", result->error_message);
    engine_result_destroy(result);
    TEST_CHECK(file_size_get("users.tbl") == 34);
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    TEST_CHECK(result->inserted_id == 2);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void failed_allocation_keeps_file_and_id_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");
    size_t before_size;
    size_t after_size;
    unsigned char *before;
    unsigned char *after;

    engine_result_destroy(result);
    before = file_bytes_create("users.tbl", &before_size);

    table_file_functions.allocate = failing_malloc;
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    table_file_functions.allocate = malloc;
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_OUT_OF_MEMORY);
    engine_result_destroy(result);

    after = file_bytes_create("users.tbl", &after_size);
    TEST_CHECK(after_size == before_size &&
               memcmp(before, after, before_size) == 0);
    free(before);
    free(after);
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    TEST_CHECK(result->inserted_id == 2);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

/* 테이블 파일 바이트를 만듭니다. 모든 정수는 리틀 엔디언입니다. */
static size_t put_le(unsigned char *bytes, uint64_t number, size_t width)
{
    for (size_t i = 0; i < width; i++) {
        bytes[i] = (unsigned char)(number >> (8 * i));
    }
    return width;
}

/* name TEXT 스키마의 record 하나를 bytes에 쓰고 쓴 바이트 수를 돌려줍니다.
   body_len과 text_len은 일부러 내용과 다르게 줄 수 있습니다. */
static size_t text_record_put(unsigned char *bytes, uint32_t body_len,
                              int64_t new_id, uint32_t text_len,
                              const char *text)
{
    size_t size = put_le(bytes, body_len, 4);

    size += put_le(bytes + size, (uint64_t)new_id, 8);
    size += put_le(bytes + size, text_len, 4);
    memcpy(bytes + size, text, strlen(text));
    return size + strlen(text);
}

static void table_file_write(const char *name, const unsigned char *bytes,
                             size_t size)
{
    char path[PATH_MAX_LEN];
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", data_dir, name);
    file = fopen(path, "wb");
    TEST_ASSERT(file != NULL);
    TEST_ASSERT(fwrite(bytes, 1, size, file) == size);
    fclose(file);
}

/* users(name TEXT) 테이블 파일을 bytes로 두고 엔진을 만들면 실패하고,
   메시지에 파일 이름, 바이트 위치, record 번호, 종류가 있고, 파일이 그대로인지
   확인합니다. */
static void broken_table_check(const unsigned char *bytes, size_t size,
                               const char *position, const char *record_no,
                               const char *kind)
{
    char error[ENGINE_ERROR_MAX] = "";
    char path[PATH_MAX_LEN];
    unsigned char after[256];
    struct engine *engine;
    FILE *file;
    size_t after_size;

    TEST_ASSERT(size <= sizeof(after));
    data_dir_create();
    write_file("users.schema", "name TEXT\n");
    table_file_write("users.tbl", bytes, size);
    engine = engine_create(data_dir, NULL, error, sizeof(error));
    TEST_CHECK(engine == NULL);
    TEST_CHECK(strstr(error, "users.tbl") != NULL);
    TEST_CHECK(strstr(error, position) != NULL);
    TEST_CHECK(strstr(error, record_no) != NULL);
    TEST_CHECK(strstr(error, kind) != NULL);
    TEST_MSG("expected %s, %s, %s; error: %s", position, record_no, kind,
             error);
    engine_destroy(engine);
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    file = fopen(path, "rb");
    TEST_ASSERT(file != NULL);
    after_size = fread(after, 1, sizeof(after), file);
    fclose(file);
    TEST_CHECK(after_size == size && memcmp(after, bytes, size) == 0);
    data_dir_remove();
}

static void length_cut_in_last_record_is_truncation_test(void)
{
    unsigned char bytes[64];
    size_t size = text_record_put(bytes, 15, 1, 3, "abc");

    for (size_t cut = 1; cut <= 3; cut++) {
        put_le(bytes + size, 15, 4);
        broken_table_check(bytes, size + cut, "19번째 바이트", "record(2번째)",
                           "잘림");
    }
}

static void body_cut_in_last_record_is_truncation_test(void)
{
    /* 둘째 record의 본문을 0바이트, id 중간, 길이 칸 중간, TEXT 중간에서
       자릅니다. */
    const size_t body_cuts[] = {0, 5, 10, 13};
    unsigned char bytes[64];
    size_t size = text_record_put(bytes, 15, 1, 3, "abc");

    text_record_put(bytes + size, 15, 2, 3, "xyz");
    for (size_t i = 0; i < sizeof(body_cuts) / sizeof(body_cuts[0]); i++) {
        broken_table_check(bytes, size + 4 + body_cuts[i], "19번째 바이트",
                           "record(2번째)", "잘림");
    }
}

static void middle_length_past_file_end_is_corruption_test(void)
{
    unsigned char bytes[64];
    size_t size = text_record_put(bytes, 115, 1, 3, "abc");

    size += text_record_put(bytes + size, 15, 2, 3, "xyz");
    broken_table_check(bytes, size, "0번째 바이트", "record(1번째)", "손상");
}

static void zero_length_record_is_corruption_test(void)
{
    unsigned char bytes[64] = {0};
    size_t size = text_record_put(bytes, 15, 1, 3, "abc");

    broken_table_check(bytes, size + 20, "19번째 바이트", "record(2번째)",
                       "손상");
}

static void fields_not_matching_body_len_is_corruption_test(void)
{
    unsigned char bytes[64];
    size_t size;

    /* TEXT 길이 2: field가 본문 15바이트 중 14바이트만 씁니다. */
    size = text_record_put(bytes, 15, 1, 2, "abc");
    size += text_record_put(bytes + size, 15, 2, 3, "xyz");
    broken_table_check(bytes, size, "0번째 바이트", "record(1번째)", "손상");

    /* TEXT 길이 4: field가 본문 15바이트보다 1바이트 더 씁니다. */
    size = text_record_put(bytes, 15, 1, 4, "abc");
    size += text_record_put(bytes + size, 15, 2, 3, "xyz");
    broken_table_check(bytes, size, "0번째 바이트", "record(1번째)", "손상");
}

static void id_not_above_previous_is_corruption_test(void)
{
    const int64_t second_ids[] = {1, 0, -3};
    unsigned char bytes[64];
    size_t size;

    for (size_t i = 0; i < sizeof(second_ids) / sizeof(second_ids[0]); i++) {
        size = text_record_put(bytes, 15, 1, 3, "abc");
        size += text_record_put(bytes + size, 15, second_ids[i], 3, "xyz");
        broken_table_check(bytes, size, "19번째 바이트", "record(2번째)",
                           "손상");
    }
    /* 첫 id는 1 이상이어야 합니다. */
    size = text_record_put(bytes, 15, 0, 3, "abc");
    broken_table_check(bytes, size, "0번째 바이트", "record(1번째)", "손상");
}

static void keywords_ignore_case_and_table_names_do_not_test(void)
{
    struct engine *engine = users_engine_create();
    const char *same_sql[] = {
        "select * from users;",
        "SELECT * FROM users;",
        "SeLeCt * FrOm users;",
    };
    struct engine_result *result;

    for (size_t i = 0; i < sizeof(same_sql) / sizeof(same_sql[0]); i++) {
        result = execute_one(engine, same_sql[i]);
        TEST_CHECK(result->status == ENGINE_OK);
        TEST_MSG("sql: %s", same_sql[i]);
        engine_result_destroy(result);
    }
    result = execute_one(engine, "SELECT * FROM Users;");
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_NO_TABLE);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void field_list_returns_only_named_fields_in_order_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");

    engine_result_destroy(result);
    result = execute_one(engine, "SELECT age, id, name FROM users;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->field_count == 3);
    TEST_CHECK(strcmp(result->field_names[0], "age") == 0);
    TEST_CHECK(strcmp(result->field_names[1], "id") == 0);
    TEST_CHECK(strcmp(result->field_names[2], "name") == 0);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 20);
    TEST_CHECK(result->values[1].integer == 1);
    check_text(&result->values[2], "kim");
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void where_returns_matching_records_or_none_test(void)
{
    struct engine *engine = users_engine_create();
    const char *inserts[] = {
        "INSERT INTO users VALUES ('kim', 'k@x', 20);",
        "INSERT INTO users VALUES ('lee', 'l@x', 31);",
        "INSERT INTO users VALUES ('park', 'p@x', 20);",
    };
    struct engine_result *result;

    for (size_t i = 0; i < sizeof(inserts) / sizeof(inserts[0]); i++) {
        engine_result_destroy(execute_one(engine, inserts[i]));
    }
    result = execute_one(engine, "SELECT name FROM users WHERE age = 20;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->record_count == 2);
    check_text(&result->values[0], "kim");
    check_text(&result->values[1], "park");
    engine_result_destroy(result);

    result = execute_one(engine, "SELECT * FROM users WHERE name = 'lee';");
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 2);
    engine_result_destroy(result);

    result = execute_one(engine, "SELECT name FROM users WHERE id = 3;");
    TEST_ASSERT(result->record_count == 1);
    check_text(&result->values[0], "park");
    engine_result_destroy(result);

    result = execute_one(engine, "SELECT * FROM users WHERE name = 'choi';");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->field_count == 4);
    TEST_CHECK(result->record_count == 0);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void syntax_error_reports_line_and_expected_token_test(void)
{
    struct engine *engine = users_engine_create();
    const struct {
        const char *sql;
        size_t line;
    } cases[] = {
        {"SELECT *\nFROM users", 2},
        {"INSERT INTO users VALUES ('kim',\n'oops, 1);", 2},
        {"\n-- 주석\nSELECT * FROM users;", 2},
    };
    struct engine_result *result;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        result = execute_one(engine, cases[i].sql);
        TEST_CHECK(result->status == ENGINE_ERROR);
        TEST_CHECK(result->error_reason == ENGINE_REASON_SYNTAX);
        TEST_CHECK(result->error_line == cases[i].line);
        TEST_MSG("sql: %s, line: %zu, message: %s", cases[i].sql,
                 result->error_line, result->error_message);
        engine_result_destroy(result);
    }
    result = execute_one(engine, "INSERT INTO users\nVALUES 'a', 'b', 1);");
    TEST_CHECK(result->error_line == 2);
    TEST_CHECK(
        strcmp(result->error_message, "VALUES 다음에 '('가 와야 합니다") == 0);
    TEST_MSG("message: %s", result->error_message);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void each_kind_of_bad_statement_has_its_own_error_reason_test(void)
{
    struct engine *engine = users_engine_create();
    const struct {
        const char *sql;
        enum engine_error_reason reason;
    } cases[] = {
        {"SELECT * FROM nobody;", ENGINE_REASON_NO_TABLE},
        {"SELECT nope FROM users;", ENGINE_REASON_NO_FIELD},
        {"SELECT * FROM users WHERE nope = 1;", ENGINE_REASON_NO_FIELD},
        {"INSERT INTO users VALUES ('kim', 20);", ENGINE_REASON_VALUE_COUNT},
        {"INSERT INTO users VALUES (1, 'kim', 'k@x', 20);",
         ENGINE_REASON_VALUE_COUNT},
        {"INSERT INTO users VALUES ('kim', 'k@x', 'twenty');",
         ENGINE_REASON_TYPE},
        {"SELECT * FROM users WHERE id = 'abc';", ENGINE_REASON_TYPE},
        {"SELECT * FROM users WHERE age = 'x';", ENGINE_REASON_TYPE},
        {"INSERT INTO users VALUES ('kim', 'k@x', 9223372036854775808);",
         ENGINE_REASON_INT_RANGE},
        {"SELECT * FROM users WHERE age = -9223372036854775809;",
         ENGINE_REASON_INT_RANGE},
    };
    struct engine_result *result;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        result = execute_one(engine, cases[i].sql);
        TEST_CHECK(result->status == ENGINE_ERROR);
        TEST_CHECK(result->error_reason == cases[i].reason);
        TEST_MSG("sql: %s, reason: %d, message: %s", cases[i].sql,
                 (int)result->error_reason, result->error_message);
        engine_result_destroy(result);
    }
    result = execute_one(
        engine,
        "INSERT INTO users VALUES ('kim', 'k@x', -9223372036854775808);");
    TEST_CHECK(result->status == ENGINE_OK);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

/* 1 MiB는 받고 1 MiB + 1바이트는 거절합니다. */
static void text_longer_than_1_mib_is_an_error_test(void)
{
    struct engine *engine = users_engine_create();
    size_t text_len = (size_t)1 << 20;
    const char *prefix = "INSERT INTO users VALUES ('";
    const char *suffix = "', 'k@x', 1);";
    size_t prefix_len = strlen(prefix);
    char *sql = malloc(prefix_len + text_len + 1 + strlen(suffix) + 1);
    struct engine_result *result;

    TEST_ASSERT(sql != NULL);
    memcpy(sql, prefix, prefix_len);
    memset(sql + prefix_len, 'a', text_len);
    strcpy(sql + prefix_len + text_len, suffix);
    result = execute_one(engine, sql);
    TEST_CHECK(result->status == ENGINE_OK);
    engine_result_destroy(result);

    sql[prefix_len + text_len] = 'a';
    strcpy(sql + prefix_len + text_len + 1, suffix);
    result = execute_one(engine, sql);
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_TEXT_TOO_LONG);
    engine_result_destroy(result);
    free(sql);

    engine_destroy(engine);
    data_dir_remove();
}

static void select_reports_scan_and_records_examined_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    engine_result_destroy(
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);"));
    engine_result_destroy(
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);"));
    result = execute_one(engine, "SELECT * FROM users WHERE name = 'lee';");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->access_path == ENGINE_ACCESS_PATH_SCAN);
    TEST_CHECK(result->record_count == 1);
    TEST_CHECK(result->records_examined == 2);
    engine_result_destroy(result);

    result = execute_one(engine, "SELECT * FROM users WHERE age = 'x';");
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->records_examined == 0);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

/* 동시성 테스트의 스레드는 acutest 매크로를 부르지 않습니다. TEST_ASSERT가
   스레드 안에서 테스트를 끝낼 수 없기 때문입니다. 스레드는 본 것을 칸에
   적고, 검사는 join 뒤에 main 스레드가 합니다. */

#define CONCURRENT_INSERTS 100
#define MIXED_WRITERS 4
#define MIXED_INSERTS_PER_WRITER 50
#define MIXED_RECORDS (MIXED_WRITERS * MIXED_INSERTS_PER_WRITER)
#define MIXED_READERS 4
#define SQL_MAX_LEN 128
#define MARK_MAX_LEN 32

/* 모든 스레드가 만들어진 뒤에 함께 출발시킵니다. pthread_barrier는 macOS에
   없어 mutex와 조건 변수로 만듭니다. */
struct start_gate {
    pthread_mutex_t mutex;
    pthread_cond_t opened;
    bool open;
};

struct concurrent_insert {
    struct engine *engine;
    struct start_gate *gate;
    int mark;
    enum engine_status status;
    int64_t inserted_id;
};

struct mixed_run {
    struct engine *engine;
    struct start_gate gate;
    pthread_mutex_t mutex;
    int writers_left;
};

struct mixed_writer {
    struct mixed_run *run;
    int first_mark;
    int64_t id_of_mark[MIXED_INSERTS_PER_WRITER];
    bool failed;
};

/* mark_of_id[id]는 그 id의 record에서 본 age입니다. 0은 아직 못 본
   것입니다. */
struct mixed_reader {
    struct mixed_run *run;
    int mark_of_id[MIXED_RECORDS + 1];
    int selects;
    char problem[ENGINE_ERROR_MAX];
};

static void start_gate_init(struct start_gate *gate)
{
    pthread_mutex_init(&gate->mutex, NULL);
    pthread_cond_init(&gate->opened, NULL);
    gate->open = false;
}

static void start_gate_destroy(struct start_gate *gate)
{
    pthread_mutex_destroy(&gate->mutex);
    pthread_cond_destroy(&gate->opened);
}

static void start_gate_open(struct start_gate *gate)
{
    pthread_mutex_lock(&gate->mutex);
    gate->open = true;
    pthread_cond_broadcast(&gate->opened);
    pthread_mutex_unlock(&gate->mutex);
}

static void start_gate_wait(struct start_gate *gate)
{
    pthread_mutex_lock(&gate->mutex);
    while (!gate->open) {
        pthread_cond_wait(&gate->opened, &gate->mutex);
    }
    pthread_mutex_unlock(&gate->mutex);
}

static void *concurrent_insert_main(void *arg)
{
    struct concurrent_insert *insert = arg;
    char sql[SQL_MAX_LEN];
    struct engine_result *result;

    snprintf(sql, sizeof(sql),
             "INSERT INTO users VALUES ('mark-%d', 'mark-%d@x', %d);",
             insert->mark, insert->mark, insert->mark);
    start_gate_wait(insert->gate);
    result = engine_execute(insert->engine, sql, NULL);
    if (result != NULL) {
        insert->status = result->status;
        insert->inserted_id = result->inserted_id;
        engine_result_destroy(result);
    }
    return NULL;
}

static void concurrent_inserts_get_ids_one_to_hundred_test(void)
{
    struct engine *engine = users_engine_create();
    struct start_gate gate;
    struct concurrent_insert inserts[CONCURRENT_INSERTS] = {0};
    pthread_t threads[CONCURRENT_INSERTS];
    bool id_seen[CONCURRENT_INSERTS + 1] = {false};

    start_gate_init(&gate);
    for (int i = 0; i < CONCURRENT_INSERTS; i++) {
        inserts[i] = (struct concurrent_insert){.engine = engine,
                                                .gate = &gate,
                                                .mark = i + 1,
                                                .status = ENGINE_ERROR};
        TEST_ASSERT(pthread_create(&threads[i], NULL, concurrent_insert_main,
                                   &inserts[i]) == 0);
    }
    start_gate_open(&gate);
    for (int i = 0; i < CONCURRENT_INSERTS; i++) {
        pthread_join(threads[i], NULL);
    }
    start_gate_destroy(&gate);

    for (int i = 0; i < CONCURRENT_INSERTS; i++) {
        int64_t new_id = inserts[i].inserted_id;

        TEST_CHECK(inserts[i].status == ENGINE_OK);
        TEST_ASSERT(new_id >= 1 && new_id <= CONCURRENT_INSERTS);
        TEST_CHECK(!id_seen[new_id]);
        TEST_MSG("id %lld was given twice", (long long)new_id);
        id_seen[new_id] = true;
    }
    for (int i = 0; i < CONCURRENT_INSERTS; i++) {
        char sql[SQL_MAX_LEN];
        char mark[MARK_MAX_LEN];
        struct engine_result *result;

        snprintf(sql, sizeof(sql), "SELECT name FROM users WHERE id = %lld;",
                 (long long)inserts[i].inserted_id);
        snprintf(mark, sizeof(mark), "mark-%d", inserts[i].mark);
        result = execute_one(engine, sql);
        TEST_CHECK(result->status == ENGINE_OK);
        TEST_ASSERT(result->record_count == 1);
        check_text(&result->values[0], mark);
        engine_result_destroy(result);
    }
    engine_destroy(engine);
    data_dir_remove();
}

static void *mixed_writer_main(void *arg)
{
    struct mixed_writer *writer = arg;
    struct mixed_run *run = writer->run;

    start_gate_wait(&run->gate);
    for (int i = 0; i < MIXED_INSERTS_PER_WRITER; i++) {
        int mark = writer->first_mark + i;
        char sql[SQL_MAX_LEN];
        struct engine_result *result;

        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('n%d', 'e%d', %d);", mark, mark,
                 mark);
        result = engine_execute(run->engine, sql, NULL);
        if (result == NULL || result->status != ENGINE_OK) {
            writer->failed = true;
        } else {
            writer->id_of_mark[i] = result->inserted_id;
        }
        engine_result_destroy(result);
    }
    pthread_mutex_lock(&run->mutex);
    run->writers_left--;
    pthread_mutex_unlock(&run->mutex);
    return NULL;
}

static bool mixed_run_is_writing(struct mixed_run *run)
{
    bool writing;

    pthread_mutex_lock(&run->mutex);
    writing = run->writers_left > 0;
    pthread_mutex_unlock(&run->mutex);
    return writing;
}

/* text 값이 prefix 뒤에 mark를 붙인 문자열인지 봅니다. */
static bool text_is_mark(const struct value *value, char prefix, int mark)
{
    char expected[MARK_MAX_LEN];
    int expected_len =
        snprintf(expected, sizeof(expected), "%c%d", prefix, mark);

    return value->type == VALUE_TEXT &&
           value->text_len == (uint32_t)expected_len &&
           memcmp(value->text, expected, (size_t)expected_len) == 0;
}

static void mixed_record_check(struct mixed_reader *reader,
                               const struct value *record_values,
                               int64_t expected_id);

/* SELECT * 결과 하나를 확인합니다. id는 1부터 빈틈없이 이어져야 합니다.
   문제가 있으면 reader->problem에 적습니다. */
static void mixed_reader_check(struct mixed_reader *reader,
                               const struct engine_result *result)
{
    for (size_t i = 0; i < result->record_count && reader->problem[0] == '\0';
         i++) {
        mixed_record_check(reader, &result->values[i * 4], (int64_t)i + 1);
    }
}

/* SELECT *의 record 하나를 확인합니다. id는 expected_id이고, 세 field는 같은
   mark로 쓴 짝이며, 앞서 본 같은 id의 mark와 같아야 합니다. */
static void mixed_record_check(struct mixed_reader *reader,
                               const struct value *record_values,
                               int64_t expected_id)
{
    int64_t found_id = record_values[0].integer;
    int mark = (int)record_values[3].integer;

    if (found_id != expected_id || found_id > MIXED_RECORDS) {
        snprintf(reader->problem, sizeof(reader->problem),
                 "expected id %lld, got %lld", (long long)expected_id,
                 (long long)found_id);
        return;
    }
    if (!text_is_mark(&record_values[1], 'n', mark) ||
        !text_is_mark(&record_values[2], 'e', mark)) {
        snprintf(reader->problem, sizeof(reader->problem),
                 "id %lld has fields from different inserts",
                 (long long)found_id);
        return;
    }
    if (reader->mark_of_id[found_id] != 0 &&
        reader->mark_of_id[found_id] != mark) {
        snprintf(reader->problem, sizeof(reader->problem),
                 "id %lld changed from mark %d to %d", (long long)found_id,
                 reader->mark_of_id[found_id], mark);
        return;
    }
    reader->mark_of_id[found_id] = mark;
}

/* WHERE id = target_id 결과 하나를 확인합니다. INDEX로 0건이나 1건이어야
   하고, 한 번 본 id가 다시 0건이 되면 안 됩니다. */
static void index_reader_check(struct mixed_reader *reader,
                               const struct engine_result *result,
                               int64_t target_id)
{
    if (result->access_path != ENGINE_ACCESS_PATH_INDEX ||
        result->record_count > 1) {
        snprintf(reader->problem, sizeof(reader->problem),
                 "id %lld: path %d, %zu records", (long long)target_id,
                 (int)result->access_path, result->record_count);
    } else if (result->record_count == 0) {
        if (reader->mark_of_id[target_id] != 0) {
            snprintf(reader->problem, sizeof(reader->problem),
                     "id %lld disappeared", (long long)target_id);
        }
    } else {
        mixed_record_check(reader, result->values, target_id);
    }
}

/* 쓰기가 도는 동안 WHERE id = 1부터 끝까지를 INDEX로 되풀이해 읽습니다. */
static void *index_reader_main(void *arg)
{
    struct mixed_reader *reader = arg;
    struct mixed_run *run = reader->run;
    bool writing = true;

    start_gate_wait(&run->gate);
    while (writing && reader->problem[0] == '\0') {
        writing = mixed_run_is_writing(run);
        for (int64_t target_id = 1;
             target_id <= MIXED_RECORDS && reader->problem[0] == '\0';
             target_id++) {
            char sql[SQL_MAX_LEN];
            struct engine_result *result;

            snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id = %lld;",
                     (long long)target_id);
            result = engine_execute(run->engine, sql, NULL);
            if (result == NULL || result->status != ENGINE_OK) {
                snprintf(reader->problem, sizeof(reader->problem),
                         "SELECT failed");
            } else {
                index_reader_check(reader, result, target_id);
            }
            engine_result_destroy(result);
        }
        reader->selects++;
    }
    return NULL;
}

static void *mixed_reader_main(void *arg)
{
    struct mixed_reader *reader = arg;
    struct mixed_run *run = reader->run;
    bool writing = true;

    start_gate_wait(&run->gate);
    /* 쓰기가 끝난 뒤에도 한 번 더 읽어, 적어도 한 번은 읽게 합니다. */
    while (writing && reader->problem[0] == '\0') {
        struct engine_result *result;

        writing = mixed_run_is_writing(run);
        result = engine_execute(run->engine, "SELECT * FROM users;", NULL);
        if (result == NULL || result->status != ENGINE_OK) {
            snprintf(reader->problem, sizeof(reader->problem), "SELECT failed");
        } else {
            mixed_reader_check(reader, result);
        }
        engine_result_destroy(result);
        reader->selects++;
    }
    return NULL;
}

/* 쓰기 스레드와 reader_main 읽기 스레드를 함께 돌리고, 쓰기가 받은 id와
   mark의 짝을 읽기가 본 짝과 견줍니다. */
static void mixed_run_check(void *(*reader_main)(void *))
{
    static struct mixed_writer writers[MIXED_WRITERS];
    static struct mixed_reader readers[MIXED_READERS];
    struct mixed_run run = {.engine = users_engine_create(),
                            .writers_left = MIXED_WRITERS};
    pthread_t writer_threads[MIXED_WRITERS];
    pthread_t reader_threads[MIXED_READERS];
    int64_t mark_count[MIXED_RECORDS + 1] = {0};
    int mark_of_id[MIXED_RECORDS + 1] = {0};

    start_gate_init(&run.gate);
    pthread_mutex_init(&run.mutex, NULL);
    for (int i = 0; i < MIXED_WRITERS; i++) {
        writers[i] = (struct mixed_writer){
            .run = &run, .first_mark = i * MIXED_INSERTS_PER_WRITER + 1};
        TEST_ASSERT(pthread_create(&writer_threads[i], NULL, mixed_writer_main,
                                   &writers[i]) == 0);
    }
    for (int i = 0; i < MIXED_READERS; i++) {
        readers[i] = (struct mixed_reader){.run = &run};
        TEST_ASSERT(pthread_create(&reader_threads[i], NULL, reader_main,
                                   &readers[i]) == 0);
    }
    start_gate_open(&run.gate);
    for (int i = 0; i < MIXED_WRITERS; i++) {
        pthread_join(writer_threads[i], NULL);
    }
    for (int i = 0; i < MIXED_READERS; i++) {
        pthread_join(reader_threads[i], NULL);
    }
    pthread_mutex_destroy(&run.mutex);
    start_gate_destroy(&run.gate);

    /* 쓰기 스레드가 받은 id와 mark의 짝입니다. */
    for (int i = 0; i < MIXED_WRITERS; i++) {
        TEST_CHECK(!writers[i].failed);
        for (int j = 0; j < MIXED_INSERTS_PER_WRITER; j++) {
            int64_t new_id = writers[i].id_of_mark[j];

            TEST_ASSERT(new_id >= 1 && new_id <= MIXED_RECORDS);
            mark_count[new_id]++;
            mark_of_id[new_id] = writers[i].first_mark + j;
        }
    }
    for (int i = 1; i <= MIXED_RECORDS; i++) {
        TEST_CHECK(mark_count[i] == 1);
        TEST_MSG("id %d was given %lld times", i, (long long)mark_count[i]);
    }
    /* 읽기 스레드가 본 짝은 모두 쓰기 스레드가 받은 짝과 같아야 합니다. */
    for (int i = 0; i < MIXED_READERS; i++) {
        TEST_CHECK(readers[i].problem[0] == '\0');
        TEST_MSG("reader %d: %s", i, readers[i].problem);
        TEST_CHECK(readers[i].selects >= 1);
        for (int k = 1; k <= MIXED_RECORDS; k++) {
            int seen = readers[i].mark_of_id[k];

            TEST_CHECK(seen == 0 || seen == mark_of_id[k]);
            TEST_MSG("reader %d saw id %d with mark %d, inserted %d", i, k,
                     seen, mark_of_id[k]);
        }
    }
    engine_destroy(run.engine);
    data_dir_remove();
}

static void scan_during_inserts_sees_only_whole_records_test(void)
{
    mixed_run_check(mixed_reader_main);
}

static void index_select_during_inserts_sees_only_whole_records_test(void)
{
    mixed_run_check(index_reader_main);
}

/* 첫 호출만 테스트가 놓아 줄 때까지 멈추는 pwrite입니다. 멈춘 동안 그
   INSERT는 자기 테이블의 쓰기 잠금을 쥐고 있습니다. */
static struct start_gate held_write_entered;
static struct start_gate held_write_release;
static bool held_write_used;

static ssize_t held_first_pwrite(int fd, const void *bytes, size_t size,
                                 off_t offset)
{
    bool first;

    pthread_mutex_lock(&held_write_entered.mutex);
    first = !held_write_used;
    held_write_used = true;
    pthread_mutex_unlock(&held_write_entered.mutex);
    if (first) {
        start_gate_open(&held_write_entered);
        start_gate_wait(&held_write_release);
    }
    return pwrite(fd, bytes, size, offset);
}

static void *held_users_insert_main(void *arg)
{
    struct engine_result *result = engine_execute(
        arg, "INSERT INTO users VALUES ('kim', 'k@x', 20);", NULL);
    enum engine_status *status = malloc(sizeof(*status));

    if (status != NULL) {
        *status = result == NULL ? ENGINE_ERROR : result->status;
    }
    engine_result_destroy(result);
    return status;
}

static void other_tables_do_not_wait_for_each_other_test(void)
{
    struct engine *engine = users_and_items_engine_create();
    struct engine_result *result;
    pthread_t users_thread;
    void *users_status = NULL;

    alarm(HANG_LIMIT_SECONDS);
    start_gate_init(&held_write_entered);
    start_gate_init(&held_write_release);
    held_write_used = false;
    table_file_functions.write_at = held_first_pwrite;
    TEST_ASSERT(pthread_create(&users_thread, NULL, held_users_insert_main,
                               engine) == 0);
    start_gate_wait(&held_write_entered);

    /* users의 INSERT가 쓰기 잠금을 쥔 채 멈춰 있어도 items는 읽고 씁니다. */
    result = execute_one(engine, "INSERT INTO items VALUES ('pen');");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->inserted_id == 1);
    engine_result_destroy(result);
    result = execute_one(engine, "SELECT * FROM items;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 1);
    engine_result_destroy(result);

    start_gate_open(&held_write_release);
    pthread_join(users_thread, &users_status);
    TEST_CHECK(users_status != NULL &&
               *(enum engine_status *)users_status == ENGINE_OK);
    free(users_status);
    table_file_functions.write_at = pwrite;
    start_gate_destroy(&held_write_entered);
    start_gate_destroy(&held_write_release);
    alarm(0);
    check_only_kim(engine);
    engine_destroy(engine);
    data_dir_remove();
}

/* users에 ('user1', 'u1@x', 1)부터 ('userN', 'uN@x', N)까지 N개를 넣습니다.
   ID는 1부터 N입니다. */
static void users_insert(struct engine *engine, int count)
{
    char sql[128];

    for (int i = 1; i <= count; i++) {
        struct engine_result *result;

        snprintf(sql, sizeof(sql),
                 "INSERT INTO users VALUES ('user%d', 'u%d@x', %d);", i, i, i);
        result = execute_one(engine, sql);
        TEST_CHECK(result->status == ENGINE_OK);
        engine_result_destroy(result);
    }
}

/* SELECT *의 결과가 INDEX로 찾은 record 하나이고, 값이 users_insert가 넣은
   expected_name, expected_email, expected_age인지 확인하고 해제합니다. */
static void check_index_record(struct engine_result *result,
                               int64_t expected_id, const char *expected_name,
                               const char *expected_email, int64_t expected_age)
{
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->access_path == ENGINE_ACCESS_PATH_INDEX);
    TEST_CHECK(result->records_examined == 1);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == expected_id);
    check_text(&result->values[1], expected_name);
    check_text(&result->values[2], expected_email);
    TEST_CHECK(result->values[3].integer == expected_age);
    engine_result_destroy(result);
}

static void where_id_reads_one_record_through_index_test(void)
{
    struct engine *engine = users_engine_create();

    users_insert(engine, 10);
    check_index_record(execute_one(engine, "SELECT * FROM users WHERE id = 7;"),
                       7, "user7", "u7@x", 7);

    engine_destroy(engine);
    data_dir_remove();
}

static void where_missing_id_reads_no_record_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    users_insert(engine, 10);
    result = execute_one(engine, "SELECT * FROM users WHERE id = 11;");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->access_path == ENGINE_ACCESS_PATH_INDEX);
    TEST_CHECK(result->record_count == 0);
    TEST_CHECK(result->records_examined == 0);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

/* 측정 전용 강제 SCAN은 같은 WHERE id = k를 SCAN으로 읽어 INDEX와 같은
   record를 찾고, 끄면 planner 규칙이 그대로 INDEX를 고릅니다. */
static void forced_scan_finds_same_record_as_index_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    users_insert(engine, 10);
    engine_bench_force_scan = true;
    result = execute_one(engine, "SELECT * FROM users WHERE id = 7;");
    engine_bench_force_scan = false;
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->access_path == ENGINE_ACCESS_PATH_SCAN);
    TEST_CHECK(result->records_examined == 10);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 7);
    check_text(&result->values[1], "user7");
    check_text(&result->values[2], "u7@x");
    TEST_CHECK(result->values[3].integer == 7);
    engine_result_destroy(result);
    check_index_record(execute_one(engine, "SELECT * FROM users WHERE id = 7;"),
                       7, "user7", "u7@x", 7);

    engine_destroy(engine);
    data_dir_remove();
}

static void where_other_field_scans_all_records_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    users_insert(engine, 10);
    result = execute_one(engine, "SELECT * FROM users WHERE email = 'u7@x';");
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->access_path == ENGINE_ACCESS_PATH_SCAN);
    TEST_CHECK(result->record_count == 1);
    TEST_CHECK(result->records_examined == 10);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void where_id_with_text_is_type_error_without_reading_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    users_insert(engine, 3);
    result = execute_one(engine, "SELECT * FROM users WHERE id = 'abc';");
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_TYPE);
    TEST_CHECK(result->record_count == 0);
    TEST_CHECK(result->records_examined == 0);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

static void inserted_id_is_found_through_index_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    users_insert(engine, 3);
    result =
        execute_one(engine, "INSERT INTO users VALUES ('new', 'n@x', 40);");
    TEST_CHECK(result->inserted_id == 4);
    engine_result_destroy(result);
    check_index_record(execute_one(engine, "SELECT * FROM users WHERE id = 4;"),
                       4, "new", "n@x", 40);

    engine_destroy(engine);
    data_dir_remove();
}

static void restart_rebuilds_index_from_table_file_test(void)
{
    struct engine *engine = users_engine_create();

    users_insert(engine, 100);
    engine_destroy(engine);

    engine = engine_reopen();
    check_index_record(execute_one(engine, "SELECT * FROM users WHERE id = 1;"),
                       1, "user1", "u1@x", 1);
    check_index_record(
        execute_one(engine, "SELECT * FROM users WHERE id = 57;"), 57, "user57",
        "u57@x", 57);
    check_index_record(
        execute_one(engine, "SELECT * FROM users WHERE id = 100;"), 100,
        "user100", "u100@x", 100);
    users_insert(engine, 1);
    check_index_record(
        execute_one(engine, "SELECT * FROM users WHERE id = 101;"), 101,
        "user1", "u1@x", 1);

    engine_destroy(engine);
    data_dir_remove();
}

/* 빈 테이블의 트리는 노드를 미리 할당해 두지 않았으므로 첫 INSERT의 미리
   할당이 실제로 노드를 할당합니다. */
static void failed_tree_reserve_keeps_file_and_tree_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result;

    bptree_allocate = failing_malloc;
    result =
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 20);");
    bptree_allocate = malloc;
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_OUT_OF_MEMORY);
    engine_result_destroy(result);
    TEST_CHECK(file_size_get("users.tbl") == 0);

    result = execute_one(engine, "SELECT * FROM users WHERE id = 1;");
    TEST_CHECK(result->access_path == ENGINE_ACCESS_PATH_INDEX);
    TEST_CHECK(result->record_count == 0);
    TEST_CHECK(result->records_examined == 0);
    engine_result_destroy(result);
    result =
        execute_one(engine, "INSERT INTO users VALUES ('lee', 'l@x', 31);");
    TEST_CHECK(result->inserted_id == 1);
    engine_result_destroy(result);
    check_index_record(execute_one(engine, "SELECT * FROM users WHERE id = 1;"),
                       1, "lee", "l@x", 31);

    engine_destroy(engine);
    data_dir_remove();
}

/* 강제 SCAN은 티켓 08이 만듭니다. 여기서는 값이 record마다 다른 email로
   같은 record를 SCAN해 견줍니다. */
static void index_and_scan_return_same_record_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *index_result;
    struct engine_result *scan_result;

    users_insert(engine, 50);
    index_result = execute_one(engine, "SELECT * FROM users WHERE id = 33;");
    scan_result =
        execute_one(engine, "SELECT * FROM users WHERE email = 'u33@x';");
    TEST_CHECK(scan_result->access_path == ENGINE_ACCESS_PATH_SCAN);
    TEST_CHECK(scan_result->records_examined == 50);
    TEST_ASSERT(scan_result->record_count == 1);
    TEST_CHECK(scan_result->values[0].integer == 33);
    check_text(&scan_result->values[1], "user33");
    check_text(&scan_result->values[2], "u33@x");
    TEST_CHECK(scan_result->values[3].integer == 33);
    engine_result_destroy(scan_result);
    check_index_record(index_result, 33, "user33", "u33@x", 33);

    engine_destroy(engine);
    data_dir_remove();
}

/* API 모드(next_sql NULL)는 문장 하나만 받습니다. 둘째 문장이 있으면 첫
   문장도 실행하지 않습니다. 뒤의 공백은 문장이 아닙니다. */
static void api_mode_rejects_second_statement_before_running_test(void)
{
    struct engine *engine = users_engine_create();
    struct engine_result *result = engine_execute(
        engine,
        "INSERT INTO users VALUES ('a', 'a@x', 1); SELECT * FROM users;", NULL);

    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_MULTIPLE_STATEMENTS);
    engine_result_destroy(result);

    result = engine_execute(engine, "SELECT * FROM users; x", NULL);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->error_reason == ENGINE_REASON_MULTIPLE_STATEMENTS);
    engine_result_destroy(result);

    result = engine_execute(engine, "SELECT * FROM users; \n\t ", NULL);
    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_OK);
    TEST_CHECK(result->record_count == 0);
    engine_result_destroy(result);

    engine_destroy(engine);
    data_dir_remove();
}

TEST_LIST = {
    {"[T6] INSERT는 ID 1을 받고 SELECT *가 그 record를 돌려준다",
     insert_returns_id_and_select_returns_record_test},
    {"INSERT의 ID는 1부터 1씩 오른다", insert_ids_increase_from_one_test},
    {"빈 테이블의 SELECT *는 field 이름만 돌려준다",
     select_on_empty_table_returns_field_names_only_test},
    {"문자열 안의 ''는 작은따옴표 하나이고 ;는 문장을 끊지 않는다",
     doubled_quote_in_text_is_one_quote_test},
    {"next_sql은 ; 다음을 가리키고 공백만 남으면 문장 없음이다",
     next_sql_points_after_semicolon_test},
    {"문법, 값 개수, 타입, 테이블이 틀린 문장은 오류를 돌려준다",
     broken_statement_returns_error_test},
    {"엔진을 다시 만들어도 record가 남고 ID는 가장 큰 ID 다음부터 이어진다",
     restart_keeps_records_and_continues_ids_test},
    {"[T20] 두 테이블에 번갈아 INSERT하면 테이블마다 ID가 1부터 따로 "
     "오른다",
     each_table_counts_ids_from_one_test},
    {"잠긴 데이터 폴더로 만든 두 번째 엔진은 실패하고, 첫 엔진을 해제하면 "
     "만들어진다",
     second_engine_on_locked_folder_fails_test},
    {"[T21] 형식이 틀린 줄, 같은 field 이름, 지원하지 않는 타입, 스키마의 "
     "id는 엔진 만들기를 멈추고, 형식 오류는 줄 번호를 알린다",
     bad_schema_stops_engine_create_test},
    {"[T22] 테이블 파일이 없는 테이블이 있으면 엔진 만들기가 실패하고 "
     "파일을 "
     "만들지 않는다",
     missing_table_file_stops_engine_create_test},
    {"다음 ID가 INT 최댓값을 넘으면 INSERT는 오류이고 테이블 파일이 "
     "바뀌지 않는다",
     insert_after_max_id_fails_and_keeps_file_test},
    {"[T80] 쓰기가 실패하고 되돌리기가 성공하면 테이블 파일이 그대로이고 "
     "다음 INSERT가 실패한 INSERT의 ID를 받는다",
     failed_write_with_rollback_keeps_file_and_id_test},
    {"[T81] 되돌리기도 실패하면 그 INSERT와 이후 같은 테이블의 SELECT, "
     "INSERT가 모두 table_unavailable이다",
     failed_rollback_stops_table_test},
    {"[T82] 한 테이블이 멈춰도 다른 테이블의 INSERT와 SELECT는 성공한다",
     stopped_table_leaves_other_tables_working_test},
    {"[T83] 쓰기가 실패하고 되돌린 뒤의 SELECT는 재시작 전후 모두 실패 "
     "전과 "
     "같은 record를 돌려준다",
     select_after_rollback_returns_records_before_failure_test},
    {"[T84] 일부만 쓰이거나 EINTR로 끊긴 쓰기는 이어 써서 완전한 record "
     "하나가 된다",
     short_and_interrupted_writes_make_one_record_test},
    {"[T85] 파일 크기 상한에 걸린 실제 쓰기 실패도 INSERT를 되돌리고 ID를 "
     "쓰지 않는다",
     real_file_size_limit_rolls_back_insert_test},
    {"[T86] record 버퍼 할당이 실패하면 INSERT는 out_of_memory이고 테이블 "
     "파일과 ID가 그대로다",
     failed_allocation_keeps_file_and_id_test},
    {"[T60] 마지막 길이 칸 중간(1~3바이트)에서 끝난 테이블 파일은 잘림으로 "
     "엔진 만들기를 멈추고 파일을 고치지 않는다",
     length_cut_in_last_record_is_truncation_test},
    {"[T61] 마지막 record의 본문 중간에서 끝난 테이블 파일은 잘림으로 엔진 "
     "만들기를 멈춘다",
     body_cut_in_last_record_is_truncation_test},
    {"[T62] 길이 칸이 파일 끝을 넘지만 field는 다 읽히는 중간 record는 "
     "잘림이 "
     "아니라 손상이다",
     middle_length_past_file_end_is_corruption_test},
    {"[T63] 길이가 0인 record(0으로 채워진 끝)는 손상이다",
     zero_length_record_is_corruption_test},
    {"[T64] field가 본문 길이보다 적게 또는 많게 쓰는 record는 손상이다",
     fields_not_matching_body_len_is_corruption_test},
    {"[T65] 앞 record의 id보다 작거나 같은 id와 1보다 작은 첫 id는 "
     "손상이다",
     id_not_above_previous_is_corruption_test},
    {"[T7] 키워드는 대소문자를 가리지 않고 테이블 이름은 가린다",
     keywords_ignore_case_and_table_names_do_not_test},
    {"[T8] field 목록 SELECT는 요청한 field만 그 순서로 돌려준다",
     field_list_returns_only_named_fields_in_order_test},
    {"[T9] WHERE는 값이 같은 record만 돌려주고 없으면 0건이다",
     where_returns_matching_records_or_none_test},
    {"[T10] 문법 오류는 줄 번호와 기대한 토큰을 알린다",
     syntax_error_reports_line_and_expected_token_test},
    {"[T11] 없는 테이블, 없는 field, 값 개수, 타입, INT 범위가 각각 다른 "
     "오류이다",
     each_kind_of_bad_statement_has_its_own_error_reason_test},
    {"[T12] 1 MiB를 넘는 TEXT 값은 오류이고 1 MiB는 받는다",
     text_longer_than_1_mib_is_an_error_test},
    {"SELECT는 접근 경로 SCAN과 읽은 레코드 수를 돌려주고 타입 오류는 레코드를 "
     "읽지 않는다",
     select_reports_scan_and_records_examined_test},
    {"[T123] 빈 테이블에 스레드 100개가 INSERT를 하나씩 보내면 ID 1~100을 "
     "하나씩 받고, 각 INSERT의 표식이 그 ID로 조회된다",
     concurrent_inserts_get_ids_one_to_hundred_test},
    {"[T124] SCAN과 INSERT를 섞어 돌려도 SELECT는 반쯤 쓰인 record를 보지 "
     "않고, 본 id와 데이터는 INSERT가 받은 짝과 같다",
     scan_during_inserts_sees_only_whole_records_test},
    {"[T125] 한 테이블의 INSERT가 쓰기 잠금을 쥐고 멈춰 있어도 다른 테이블의 "
     "INSERT와 SELECT는 기다리지 않는다",
     other_tables_do_not_wait_for_each_other_test},
    {"[T126] INSERT가 도는 동안 WHERE id = k는 INDEX로 0건이나 온전한 record "
     "하나를 돌려주고, 한 번 찾은 id는 사라지지 않으며 INSERT가 받은 짝과 "
     "같다",
     index_select_during_inserts_sees_only_whole_records_test},
    {"[T100] WHERE id = 7은 INDEX로 그 record 하나만 읽는다",
     where_id_reads_one_record_through_index_test},
    {"[T101] ID가 1~N인 테이블에서 WHERE id = N+1은 INDEX이고 0건이며 "
     "record를 읽지 않는다",
     where_missing_id_reads_no_record_test},
    {"[T140] 측정 전용 강제 SCAN은 WHERE id = k를 SCAN으로 읽어 INDEX와 "
     "같은 record를 찾고, 끄면 다시 INDEX다",
     forced_scan_finds_same_record_as_index_test},
    {"[T102] id가 아닌 field의 WHERE는 SCAN이고 모든 record를 읽는다",
     where_other_field_scans_all_records_test},
    {"[T103] WHERE id = 'abc'는 타입 오류이고 record를 읽지 않는다",
     where_id_with_text_is_type_error_without_reading_test},
    {"[T104] INSERT 직후의 WHERE id = 새 ID가 INDEX로 그 record를 찾는다",
     inserted_id_is_found_through_index_test},
    {"[T105] 엔진을 다시 만들면 테이블 파일에서 트리를 다시 만들어 WHERE id "
     "= k가 INDEX로 같은 record를 찾는다",
     restart_rebuilds_index_from_table_file_test},
    {"[T106] 트리 미리 할당이 실패하면 INSERT는 out_of_memory이고 테이블 "
     "파일과 트리가 그대로다",
     failed_tree_reserve_keeps_file_and_tree_test},
    {"[T107] 같은 record를 찾는 INDEX 결과와 SCAN 결과가 같다",
     index_and_scan_return_same_record_test},
    {"[T160] API 모드에서 문장 뒤에 다른 문장이 있으면 multiple_statements "
     "오류이고 첫 문장도 실행하지 않는다",
     api_mode_rejects_second_statement_before_running_test},
    {"[T200] 결과 상한이 record 3건인 엔진에서 4건을 돌려줄 SELECT는 3건과 "
     "넷째 record의 위치인 next_cursor를 받고, 그 커서로 다시 부르면 남은 "
     "1건과 커서 없음을 받으며, 정확히 3건인 결과에는 커서가 없다",
     select_over_record_limit_returns_part_and_cursor_test},
    {"[T201] 결과 상한이 50바이트인 엔진에서 22바이트 record 세 건은 두 건과 "
     "next_cursor로 나뉘고, record 하나가 상한보다 크면 result_too_large이다",
     select_over_byte_limit_returns_part_and_cursor_test},
    {"[T240] record 시작이 아닌 위치, 파일 크기보다 큰 위치, INDEX 질의와 "
     "INSERT에 붙은 커서는 invalid_cursor이고, 파일 끝의 커서는 0건이다",
     invalid_cursor_is_rejected_test},
    {"[T241] 앞 부분을 받은 뒤의 INSERT는 기다리지 않고 들어가고, 새 record는 "
     "뒤 부분에만 나온다",
     insert_between_parts_does_not_wait_test},
    {NULL, NULL},
};

static void write_file(const char *name, const char *content)
{
    char path[PATH_MAX_LEN];
    FILE *file;

    snprintf(path, sizeof(path), "%s/%s", data_dir, name);
    file = fopen(path, "w");
    TEST_ASSERT(file != NULL);
    fputs(content, file);
    fclose(file);
}

static void data_dir_create(void)
{
    snprintf(data_dir, sizeof(data_dir), "/tmp/mini-dbms-test-XXXXXX");
    TEST_ASSERT(mkdtemp(data_dir) != NULL);
}

/* 데이터 폴더 안의 파일(스키마, 테이블, 잠금 파일)을 모두 지우고 폴더를
   지웁니다. */
static void data_dir_remove(void)
{
    DIR *dir = opendir(data_dir);
    struct dirent *entry;
    char path[PATH_MAX_LEN];

    TEST_ASSERT(dir != NULL);
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", data_dir, entry->d_name);
        unlink(path);
    }
    closedir(dir);
    rmdir(data_dir);
}

/* users(name TEXT, email TEXT, age INT) 스키마와 빈 테이블 파일을 만든
   데이터 폴더로 엔진을 엽니다. */
static struct engine *users_engine_create(void)
{
    char error[ENGINE_ERROR_MAX] = "";
    struct engine *engine;

    data_dir_create();
    write_file("users.schema", "name TEXT\nemail TEXT\nage INT\n");
    write_file("users.tbl", "");
    engine = engine_create(data_dir, NULL, error, sizeof(error));
    TEST_ASSERT(engine != NULL);
    TEST_MSG("error: %s", error);

    return engine;
}

/* users에 age 1인 record 세 건과 age 2인 record 한 건을 넣은 뒤, 결과 상한을
   준 엔진으로 다시 엽니다. 0은 그 상한을 두지 않는다는 뜻입니다. */
static struct engine *limited_engine_create(size_t max_records,
                                            size_t max_bytes)
{
    struct engine_options options = {.max_records = max_records,
                                     .max_bytes = max_bytes};
    char error[ENGINE_ERROR_MAX] = "";
    struct engine *engine = users_engine_create();

    for (int i = 0; i < 3; i++) {
        engine_result_destroy(
            execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 1);"));
    }
    engine_result_destroy(
        execute_one(engine, "INSERT INTO users VALUES ('kim', 'k@x', 2);"));
    engine_destroy(engine);
    engine = engine_create(data_dir, &options, error, sizeof(error));
    TEST_ASSERT(engine != NULL);
    TEST_MSG("error: %s", error);

    return engine;
}

static struct engine_result *execute_one(struct engine *engine, const char *sql)
{
    const char *next_sql = NULL;
    struct engine_result *result = engine_execute(engine, sql, &next_sql);

    TEST_ASSERT(result != NULL);
    return result;
}

static void check_invalid_cursor(struct engine *engine, const char *sql,
                                 uint64_t cursor)
{
    struct engine_result *result = engine_execute_from(engine, sql, cursor);

    TEST_ASSERT(result != NULL);
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_INVALID_CURSOR);
    TEST_MSG("sql: %s, cursor: %llu", sql, (unsigned long long)cursor);
    TEST_CHECK(result->record_count == 0);
    engine_result_destroy(result);
}

static void check_text(const struct value *value, const char *expected)
{
    TEST_CHECK(value->type == VALUE_TEXT);
    TEST_CHECK(value->text_len == strlen(expected));
    TEST_CHECK(memcmp(value->text, expected, value->text_len) == 0);
    TEST_MSG("expected: %s, got: %.*s", expected, (int)value->text_len,
             value->text);
}

/* 데이터 폴더의 파일 전체를 읽어 새 버퍼로 돌려줍니다. */
static unsigned char *file_bytes_create(const char *name, size_t *size)
{
    long file_size = file_size_get(name);
    char path[PATH_MAX_LEN];
    unsigned char *bytes = malloc((size_t)file_size + 1);
    FILE *file;

    TEST_ASSERT(bytes != NULL);
    snprintf(path, sizeof(path), "%s/%s", data_dir, name);
    file = fopen(path, "rb");
    TEST_ASSERT(file != NULL);
    *size = fread(bytes, 1, (size_t)file_size, file);
    fclose(file);

    return bytes;
}

/* 첫 호출은 요청의 절반만 실제로 쓰고, 그다음 호출은 디스크가 가득 찬 것처럼
   실패합니다. 반쯤 쓰인 record가 파일에 남은 상태를 만듭니다. */
static ssize_t part_then_fail_pwrite(int fd, const void *bytes, size_t size,
                                     off_t offset)
{
    static bool wrote_part;

    if (!wrote_part) {
        wrote_part = true;
        return pwrite(fd, bytes, size / 2, offset);
    }
    errno = ENOSPC;
    return -1;
}

static int failing_ftruncate(int fd, off_t size)
{
    (void)fd;
    (void)size;
    errno = EIO;
    return -1;
}

/* users와 items(title TEXT) 두 테이블이 있는 데이터 폴더로 엔진을 엽니다. */
static struct engine *users_and_items_engine_create(void)
{
    data_dir_create();
    write_file("users.schema", "name TEXT\nemail TEXT\nage INT\n");
    write_file("users.tbl", "");
    write_file("items.schema", "title TEXT\n");
    write_file("items.tbl", "");

    return engine_reopen();
}

/* result가 table_unavailable 오류인지 확인하고 해제합니다. */
static void check_table_unavailable(struct engine_result *result)
{
    TEST_CHECK(result->status == ENGINE_ERROR);
    TEST_CHECK(result->error_reason == ENGINE_REASON_TABLE_UNAVAILABLE);
    TEST_MSG("reason: %d, error: %s", (int)result->error_reason,
             result->error_message);
    engine_result_destroy(result);
}

/* 한 번에 3바이트까지만 쓰고, 그 사이 호출마다 EINTR로 끊깁니다. */
static ssize_t small_interrupted_pwrite(int fd, const void *bytes, size_t size,
                                        off_t offset)
{
    static bool interrupt_next = true;

    if (interrupt_next) {
        interrupt_next = false;
        errno = EINTR;
        return -1;
    }
    interrupt_next = true;
    return pwrite(fd, bytes, size < 3 ? size : 3, offset);
}

/* SELECT *가 ('kim', 'k@x', 20) record 하나만 돌려주는지 확인합니다. */
static void check_only_kim(struct engine *engine)
{
    struct engine_result *result = execute_one(engine, "SELECT * FROM users;");

    TEST_CHECK(result->status == ENGINE_OK);
    TEST_ASSERT(result->record_count == 1);
    TEST_CHECK(result->values[0].integer == 1);
    check_text(&result->values[1], "kim");
    check_text(&result->values[2], "k@x");
    TEST_CHECK(result->values[3].integer == 20);
    engine_result_destroy(result);
}

static void *failing_malloc(size_t size)
{
    (void)size;
    return NULL;
}
