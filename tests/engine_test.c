#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "engine.h"

#define PATH_MAX_LEN 512

#define DATA_DIR_MAX 64

static char data_dir[DATA_DIR_MAX];

static void write_file(const char *name, const char *content);
static void data_dir_create(void);
static void data_dir_remove(void);
static struct engine *users_engine_create(void);
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
    TEST_CHECK(result->error_code == ENGINE_ERROR_NO_TABLE);
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
        TEST_CHECK(result->error_code == ENGINE_ERROR_SYNTAX);
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

static void each_kind_of_bad_statement_has_its_own_error_code_test(void)
{
    struct engine *engine = users_engine_create();
    const struct {
        const char *sql;
        enum engine_error_code code;
    } cases[] = {
        {"SELECT * FROM nobody;", ENGINE_ERROR_NO_TABLE},
        {"SELECT nope FROM users;", ENGINE_ERROR_NO_FIELD},
        {"SELECT * FROM users WHERE nope = 1;", ENGINE_ERROR_NO_FIELD},
        {"INSERT INTO users VALUES ('kim', 20);", ENGINE_ERROR_VALUE_COUNT},
        {"INSERT INTO users VALUES (1, 'kim', 'k@x', 20);",
         ENGINE_ERROR_VALUE_COUNT},
        {"INSERT INTO users VALUES ('kim', 'k@x', 'twenty');",
         ENGINE_ERROR_TYPE},
        {"SELECT * FROM users WHERE id = 'abc';", ENGINE_ERROR_TYPE},
        {"SELECT * FROM users WHERE age = 'x';", ENGINE_ERROR_TYPE},
        {"INSERT INTO users VALUES ('kim', 'k@x', 9223372036854775808);",
         ENGINE_ERROR_INT_RANGE},
        {"SELECT * FROM users WHERE age = -9223372036854775809;",
         ENGINE_ERROR_INT_RANGE},
    };
    struct engine_result *result;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        result = execute_one(engine, cases[i].sql);
        TEST_CHECK(result->status == ENGINE_ERROR);
        TEST_CHECK(result->error_code == cases[i].code);
        TEST_MSG("sql: %s, code: %d, message: %s", cases[i].sql,
                 (int)result->error_code, result->error_message);
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
    TEST_CHECK(result->error_code == ENGINE_ERROR_TEXT_TOO_LONG);
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
    result = execute_one(engine, "SELECT * FROM users WHERE id = 2;");
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
     each_kind_of_bad_statement_has_its_own_error_code_test},
    {"[T12] 1 MiB를 넘는 TEXT 값은 오류이고 1 MiB는 받는다",
     text_longer_than_1_mib_is_an_error_test},
    {"SELECT는 접근 경로 SCAN과 읽은 레코드 수를 돌려주고 타입 오류는 레코드를 "
     "읽지 않는다",
     select_reports_scan_and_records_examined_test},
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

static void data_dir_remove(void)
{
    char path[PATH_MAX_LEN];

    snprintf(path, sizeof(path), "%s/users.schema", data_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/users.tbl", data_dir);
    unlink(path);
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
