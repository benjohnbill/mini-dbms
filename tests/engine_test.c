#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "engine.h"
#include "table.h"

#define PATH_MAX_LEN 512

#define DATA_DIR_MAX 64

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
    {"[T20] 두 테이블에 번갈아 INSERT하면 테이블마다 ID가 1부터 따로 오른다",
     each_table_counts_ids_from_one_test},
    {"잠긴 데이터 폴더로 만든 두 번째 엔진은 실패하고, 첫 엔진을 해제하면 "
     "만들어진다",
     second_engine_on_locked_folder_fails_test},
    {"[T21] 형식이 틀린 줄, 같은 field 이름, 지원하지 않는 타입, 스키마의 "
     "id는 엔진 만들기를 멈추고, 형식 오류는 줄 번호를 알린다",
     bad_schema_stops_engine_create_test},
    {"[T22] 테이블 파일이 없는 테이블이 있으면 엔진 만들기가 실패하고 파일을 "
     "만들지 않는다",
     missing_table_file_stops_engine_create_test},
    {"다음 ID가 INT 최댓값을 넘으면 INSERT는 오류이고 테이블 파일이 "
     "바뀌지 않는다",
     insert_after_max_id_fails_and_keeps_file_test},
    {"[T80] 쓰기가 실패하고 되돌리기가 성공하면 테이블 파일이 그대로이고 "
     "다음 INSERT가 실패한 INSERT의 ID를 받는다",
     failed_write_with_rollback_keeps_file_and_id_test},
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

static struct engine_result *execute_one(struct engine *engine, const char *sql)
{
    const char *next_sql = NULL;
    struct engine_result *result = engine_execute(engine, sql, &next_sql);

    TEST_ASSERT(result != NULL);
    return result;
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
