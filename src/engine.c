#include "engine.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "rwlock.h"
#include "sql.h"
#include "table.h"

#define SCHEMA_SUFFIX ".schema"
#define LOCK_FILE_NAME ".lock"
#define LOCK_PATH_BYTES 1024

/* lock_fd는 잠금 파일을 연 fd이고, 닫으면 잠금이 풀립니다.
   table_locks[i]는 tables[i]의 쓰기 우선 잠금입니다. 테이블 목록은 시작
   뒤에 바뀌지 않으므로 목록 자체에는 잠금이 없습니다. */
struct engine {
    struct engine_options options;
    int lock_fd;
    struct table **tables;
    size_t table_count;
    struct rwlock *table_locks;
    size_t table_lock_count;
};

static int engine_lock_data_dir(struct engine *engine, const char *data_dir,
                                char *error, size_t error_size);
static int engine_load_tables(struct engine *engine, const char *data_dir,
                              char *error, size_t error_size);
static int engine_add_table(struct engine *engine, struct table *table);
static int engine_init_table_locks(struct engine *engine, char *error,
                                   size_t error_size);
static void execute_locked(const struct engine *engine, size_t table_index,
                           const struct statement *statement,
                           struct engine_result *result);
static void result_fail(struct engine_result *result,
                        enum engine_error_reason code, const char *format, ...);
static void result_set_sql_error(struct engine_result *result,
                                 const struct sql_error *sql_error);
static bool sql_rest_is_blank(const char *sql);
static bool engine_find_table(const struct engine *engine, const char *name,
                              size_t *table_index);
static void execute_insert(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result);
static void execute_select(struct table *table,
                           const struct engine_options *options,
                           const struct statement *statement,
                           struct engine_result *result);
static bool result_is_over_limit(const struct engine_result *result,
                                 const struct engine_options *options,
                                 size_t result_bytes);
static size_t record_bytes_get(const struct engine_result *result,
                               size_t record_index);
static void result_records_clear(struct engine_result *result);
static bool table_find_field(const struct table *table, const char *name,
                             size_t *source);
static int where_check(struct engine_result *result, const struct table *table,
                       const struct statement *statement, size_t *where_source);
static struct value record_field(const struct record *record, size_t source);
static enum engine_access_path select_plan(const struct statement *statement,
                                           size_t where_source);
static const char *value_type_name(enum value_type type);
static bool value_equal(const struct value *left, const struct value *right);
static enum engine_error_reason
insert_error_reason(enum table_insert_status insert_status);
static int result_set_fields(struct engine_result *result,
                             const struct table *table,
                             const struct statement *statement,
                             size_t *sources);
static int result_add_record(struct engine_result *result,
                             const size_t *sources,
                             const struct record *record);

struct engine *engine_create(const char *data_dir,
                             const struct engine_options *options, char *error,
                             size_t error_size)
{
    struct engine *engine = calloc(1, sizeof(*engine));

    if (engine == NULL) {
        snprintf(error, error_size, "메모리가 부족합니다");
        return NULL;
    }
    if (options != NULL) {
        engine->options = *options;
    }
    engine->lock_fd = -1;
    /* 다른 프로세스가 같은 파일을 쓰는 중에 읽지 않도록, 스키마와 테이블
       파일을 읽기 전에 잠급니다. */
    if (engine_lock_data_dir(engine, data_dir, error, error_size) != 0 ||
        engine_load_tables(engine, data_dir, error, error_size) != 0 ||
        engine_init_table_locks(engine, error, error_size) != 0) {
        engine_destroy(engine);
        return NULL;
    }
    return engine;
}

struct engine_result *engine_execute(struct engine *engine, const char *sql,
                                     const char **next_sql)
{
    struct engine_result *result = calloc(1, sizeof(*result));
    struct statement *statement = NULL;
    const char *statement_end = sql;
    struct sql_error sql_error;
    enum sql_status sql_status;
    size_t table_index;

    if (result == NULL) {
        return NULL;
    }
    sql_status = sql_parse(sql, &statement, &statement_end, &sql_error);
    if (next_sql != NULL) {
        *next_sql = statement_end;
    }
    if (sql_status == SQL_EMPTY) {
        result->status = ENGINE_EMPTY;
        return result;
    }
    if (sql_status == SQL_ERROR) {
        result_set_sql_error(result, &sql_error);
        return result;
    }
    /* API 모드는 요청 하나에 문장 하나입니다. 일부만 실행하지 않도록 첫
       문장도 실행하기 전에 거절합니다. */
    if (next_sql == NULL && !sql_rest_is_blank(statement_end)) {
        result_fail(result, ENGINE_REASON_MULTIPLE_STATEMENTS,
                    "요청 하나에는 SQL 문장 하나만 보냅니다");
        sql_statement_destroy(statement);
        return result;
    }
    /* 검증 오류는 문장의 첫 토큰을 가리킵니다. 성공하면 쓰지 않습니다. */
    result->error_line = statement->line;
    result->error_byte_in_line = statement->byte_in_line;
    if (engine_find_table(engine, statement->table, &table_index)) {
        execute_locked(engine, table_index, statement, result);
    } else {
        result_fail(result, ENGINE_REASON_NO_TABLE, "없는 테이블입니다: %s",
                    statement->table);
    }
    sql_statement_destroy(statement);

    return result;
}

void engine_result_destroy(struct engine_result *result)
{
    if (result == NULL) {
        return;
    }
    for (size_t i = 0; i < result->field_count; i++) {
        free(result->field_names[i]);
    }
    free(result->field_names);
    result_records_clear(result);
    free(result);
}

void engine_destroy(struct engine *engine)
{
    if (engine == NULL) {
        return;
    }
    for (size_t i = 0; i < engine->table_count; i++) {
        table_destroy(engine->tables[i]);
    }
    free(engine->tables);
    for (size_t i = 0; i < engine->table_lock_count; i++) {
        rwlock_destroy(&engine->table_locks[i]);
    }
    free(engine->table_locks);
    if (engine->lock_fd >= 0) {
        close(engine->lock_fd);
    }
    free(engine);
}

/* 데이터 폴더의 잠금 파일에 비차단 배타 flock을 겁니다. flock은 열린 파일마다
   잠그므로 같은 프로세스의 두 번째 엔진도 실패하고, 프로세스가 끝나면 커널이
   잠금을 풉니다. */
static int engine_lock_data_dir(struct engine *engine, const char *data_dir,
                                char *error, size_t error_size)
{
    char path[LOCK_PATH_BYTES];

    snprintf(path, sizeof(path), "%s/%s", data_dir, LOCK_FILE_NAME);
    engine->lock_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (engine->lock_fd < 0) {
        snprintf(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (flock(engine->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            snprintf(error, error_size,
                     "%s: data folder is in use by another process", data_dir);
        } else {
            snprintf(error, error_size, "%s: %s", path, strerror(errno));
        }
        return -1;
    }
    return 0;
}

/* 데이터 폴더의 <테이블>.schema마다 테이블 하나를 엽니다. */
static int engine_load_tables(struct engine *engine, const char *data_dir,
                              char *error, size_t error_size)
{
    DIR *dir = opendir(data_dir);
    struct dirent *entry;
    size_t suffix_len = strlen(SCHEMA_SUFFIX);
    int status = 0;

    if (dir == NULL) {
        snprintf(error, error_size, "%s: %s", data_dir, strerror(errno));
        return -1;
    }
    while (status == 0 && (entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        char *table_name;
        struct table *table;

        if (name_len <= suffix_len ||
            strcmp(entry->d_name + name_len - suffix_len, SCHEMA_SUFFIX) != 0) {
            continue;
        }
        table_name = strndup(entry->d_name, name_len - suffix_len);
        table = table_name == NULL
                    ? NULL
                    : table_create(data_dir, table_name, error, error_size);
        free(table_name);
        if (table == NULL || engine_add_table(engine, table) != 0) {
            table_destroy(table);
            status = -1;
        }
    }
    closedir(dir);

    return status;
}

static int engine_add_table(struct engine *engine, struct table *table)
{
    struct table **tables = realloc(
        engine->tables, (engine->table_count + 1) * sizeof(*engine->tables));

    if (tables == NULL) {
        return -1;
    }
    engine->tables = tables;
    engine->tables[engine->table_count++] = table;

    return 0;
}

/* 테이블마다 쓰기 우선 잠금 하나를 만듭니다. 테이블을 다 읽은 뒤에 한 번에
   할당합니다. 테이블 배열처럼 realloc하면 이미 초기화한 mutex가 옮겨지기
   때문입니다. */
static int engine_init_table_locks(struct engine *engine, char *error,
                                   size_t error_size)
{
    if (engine->table_count == 0) {
        return 0;
    }
    engine->table_locks =
        calloc(engine->table_count, sizeof(*engine->table_locks));
    if (engine->table_locks == NULL) {
        snprintf(error, error_size, "메모리가 부족합니다");
        return -1;
    }
    for (size_t i = 0; i < engine->table_count; i++) {
        if (rwlock_init(&engine->table_locks[i]) != 0) {
            snprintf(error, error_size, "테이블 잠금을 만들지 못했습니다");
            return -1;
        }
        engine->table_lock_count++;
    }
    return 0;
}

/* 실행 단계 전체를 테이블의 잠금 안에서 합니다. SELECT는 읽기 잠금,
   INSERT는 쓰기 잠금입니다. ID 발급, 파일 쓰기, 트리 등록, 실패 시
   되돌리기가 모두 이 쓰기 잠금 안에 들어가므로, 그 아래 함수는 잠금을 다시
   잡지 않습니다. available도 되돌리기가 쓰기 잠금 안에서 바꾸므로 잠금
   안에서 읽습니다. */
static void execute_locked(const struct engine *engine, size_t table_index,
                           const struct statement *statement,
                           struct engine_result *result)
{
    struct table *table = engine->tables[table_index];
    struct rwlock *lock = &engine->table_locks[table_index];
    bool writes = statement->kind == STATEMENT_INSERT;

    if (writes) {
        rwlock_write_lock(lock);
    } else {
        rwlock_read_lock(lock);
    }
    if (!table->available) {
        /* 되돌리기가 실패한 테이블은 파일 상태를 알 수 없으므로 읽지도
           쓰지도 않습니다. 재시작하면 시작할 때 읽기가 파일을 판정합니다. */
        result_fail(result, ENGINE_REASON_TABLE_UNAVAILABLE,
                    "%s: table is unavailable until restart", table->name);
    } else if (writes) {
        execute_insert(table, statement, result);
    } else {
        execute_select(table, &engine->options, statement, result);
    }
    if (writes) {
        rwlock_write_unlock(lock);
    } else {
        rwlock_read_unlock(lock);
    }
}

static void result_fail(struct engine_result *result,
                        enum engine_error_reason code, const char *format, ...)
{
    va_list args;

    result->status = ENGINE_ERROR;
    result->error_reason = code;
    va_start(args, format);
    vsnprintf(result->error_message, sizeof(result->error_message), format,
              args);
    va_end(args);
}

static void result_set_sql_error(struct engine_result *result,
                                 const struct sql_error *sql_error)
{
    static const enum engine_error_reason codes[] = {
        [SQL_ERROR_SYNTAX] = ENGINE_REASON_SYNTAX,
        [SQL_ERROR_INT_RANGE] = ENGINE_REASON_INT_RANGE,
        [SQL_ERROR_TEXT_TOO_LONG] = ENGINE_REASON_TEXT_TOO_LONG,
        [SQL_ERROR_NO_MEMORY] = ENGINE_REASON_OUT_OF_MEMORY,
    };

    result_fail(result, codes[sql_error->code], "%s", sql_error->message);
    result->error_line = sql_error->line;
    result->error_byte_in_line = sql_error->byte_in_line;
}

/* 파서가 문장 사이에서 건너뛰는 공백과 같은 isspace로 봅니다. */
static bool sql_rest_is_blank(const char *sql)
{
    while (isspace((unsigned char)*sql)) {
        sql++;
    }
    return *sql == '\0';
}

static bool engine_find_table(const struct engine *engine, const char *name,
                              size_t *table_index)
{
    for (size_t i = 0; i < engine->table_count; i++) {
        if (strcmp(engine->tables[i]->name, name) == 0) {
            *table_index = i;
            return true;
        }
    }
    return false;
}

/* 값 개수와 타입을 스키마와 먼저 맞춘 뒤에 파일에 씁니다. */
static void execute_insert(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result)
{
    enum table_insert_status insert_status;

    result->kind = ENGINE_STATEMENT_INSERT;
    if (statement->value_count != table->field_count) {
        result_fail(
            result, ENGINE_REASON_VALUE_COUNT,
            "%s에는 값 %zu개가 와야 하는데 %zu개입니다(id는 적지 않습니다)",
            table->name, table->field_count, statement->value_count);
        return;
    }
    for (size_t i = 0; i < table->field_count; i++) {
        if (statement->values[i].type != table->fields[i].type) {
            result_fail(
                result, ENGINE_REASON_TYPE, "%s field에는 %s 값이 와야 합니다",
                table->fields[i].name, value_type_name(table->fields[i].type));
            return;
        }
    }
    insert_status =
        table_insert(table, statement->values, &result->inserted_id,
                     result->error_message, sizeof(result->error_message));
    if (insert_status != TABLE_INSERT_OK) {
        result->status = ENGINE_ERROR;
        result->error_reason = insert_error_reason(insert_status);
        return;
    }
    result->status = ENGINE_OK;
}

static enum engine_error_reason
insert_error_reason(enum table_insert_status insert_status)
{
    switch (insert_status) {
    case TABLE_INSERT_OUT_OF_MEMORY:
        return ENGINE_REASON_OUT_OF_MEMORY;
    case TABLE_INSERT_STORAGE_ERROR:
        return ENGINE_REASON_STORAGE_ERROR;
    case TABLE_INSERT_UNAVAILABLE:
        return ENGINE_REASON_TABLE_UNAVAILABLE;
    default:
        return ENGINE_REASON_NONE;
    }
}

static void execute_select(struct table *table,
                           const struct engine_options *options,
                           const struct statement *statement,
                           struct engine_result *result)
{
    size_t result_bytes = 0;
    size_t source_count = statement->field_count == 0 ? table->field_count + 1
                                                      : statement->field_count;
    size_t *sources = calloc(source_count, sizeof(*sources));
    struct record_iterator *iterator = NULL;
    struct record record;
    size_t where_source = 0;
    int status;

    result->kind = ENGINE_STATEMENT_SELECT;
    if (sources == NULL) {
        result_fail(result, ENGINE_REASON_OUT_OF_MEMORY, "메모리가 부족합니다");
        return;
    }
    if (result_set_fields(result, table, statement, sources) == 0 &&
        where_check(result, table, statement, &where_source) == 0) {
        result->access_path = select_plan(statement, where_source);
        iterator =
            result->access_path == ENGINE_ACCESS_PATH_INDEX
                ? table_index_create(table, statement->where_value.integer)
                : table_scan_create(table);
        if (iterator == NULL) {
            result_fail(result, ENGINE_REASON_OUT_OF_MEMORY,
                        "메모리가 부족합니다");
        }
    }
    if (iterator == NULL) {
        free(sources);
        return;
    }
    while ((status = iterator->next(iterator, &record)) == 1) {
        struct value field;

        result->records_examined++;
        /* INDEX로 찾은 record는 조건과 다시 비교하지 않습니다. 정확성은 B+
           트리 단위 테스트가 보증합니다. */
        if (result->access_path == ENGINE_ACCESS_PATH_SCAN &&
            statement->where_field != NULL) {
            field = record_field(&record, where_source);
            if (!value_equal(&field, &statement->where_value)) {
                continue;
            }
        }
        if (result_add_record(result, sources, &record) != 0) {
            status = -1;
            break;
        }
        result_bytes += record_bytes_get(result, result->record_count - 1);
        if (result_is_over_limit(result, options, result_bytes)) {
            break;
        }
    }
    iterator->destroy(iterator);
    free(sources);
    /* 상한을 넘으면 모으기를 멈추고, 모은 record를 해제해 큰 결과가 메모리에
       남지 않게 합니다. */
    if (status == 1) {
        result_records_clear(result);
        result_fail(result, ENGINE_REASON_RESULT_TOO_LARGE,
                    "결과가 상한(record %zu건, %zu바이트)을 넘습니다",
                    options->max_records, options->max_bytes);
        return;
    }
    if (status != 0) {
        result_fail(result, ENGINE_REASON_STORAGE_ERROR,
                    "%s.tbl을 읽지 못했습니다", table->name);
        return;
    }
    result->status = ENGINE_OK;
}

/* field 이름을 record 안의 위치로 바꿉니다. 0은 id이고 i + 1은 스키마의 i번째
   field입니다. 없으면 false입니다. */
static bool table_find_field(const struct table *table, const char *name,
                             size_t *source)
{
    if (strcmp(name, "id") == 0) {
        *source = 0;
        return true;
    }
    for (size_t i = 0; i < table->field_count; i++) {
        if (strcmp(table->fields[i].name, name) == 0) {
            *source = i + 1;
            return true;
        }
    }
    return false;
}

/* WHERE의 field가 있고 값의 타입이 그 field와 같은지 확인합니다. 통과하면
   field의 위치를 *where_source에 씁니다. */
static int where_check(struct engine_result *result, const struct table *table,
                       const struct statement *statement, size_t *where_source)
{
    enum value_type type;

    if (statement->where_field == NULL) {
        return 0;
    }
    if (!table_find_field(table, statement->where_field, where_source)) {
        result_fail(result, ENGINE_REASON_NO_FIELD, "없는 field입니다: %s",
                    statement->where_field);
        return -1;
    }
    type =
        *where_source == 0 ? VALUE_INT : table->fields[*where_source - 1].type;
    if (statement->where_value.type != type) {
        result_fail(result, ENGINE_REASON_TYPE,
                    "%s field에는 %s 값이 와야 합니다", statement->where_field,
                    value_type_name(type));
        return -1;
    }
    return 0;
}

/* where_check를 통과한 SELECT의 접근 경로입니다. 조건이 id = 정수이면
   INDEX, 그 밖은 SCAN입니다. */
static enum engine_access_path select_plan(const struct statement *statement,
                                           size_t where_source)
{
    if (statement->where_field != NULL && where_source == 0) {
        return ENGINE_ACCESS_PATH_INDEX;
    }
    return ENGINE_ACCESS_PATH_SCAN;
}

static const char *value_type_name(enum value_type type)
{
    return type == VALUE_INT ? "INT" : "TEXT";
}

static bool value_equal(const struct value *left, const struct value *right)
{
    if (left->type == VALUE_INT) {
        return left->integer == right->integer;
    }
    return left->text_len == right->text_len &&
           memcmp(left->text, right->text, left->text_len) == 0;
}

static struct value record_field(const struct record *record, size_t source)
{
    struct value value = {.type = VALUE_INT, .integer = record->id};

    return source == 0 ? value : record->values[source - 1];
}

/* 결과의 field 이름과 각 field가 record의 어디에서 오는지 정합니다. *는 id와
   스키마 순서의 모든 field입니다. sources는 결과 field 수만큼 있어야 합니다.
   실패하면 result에 오류를 씁니다. */
static int result_set_fields(struct engine_result *result,
                             const struct table *table,
                             const struct statement *statement, size_t *sources)
{
    size_t field_count = statement->field_count == 0 ? table->field_count + 1
                                                     : statement->field_count;

    result->field_names = calloc(field_count, sizeof(*result->field_names));
    if (result->field_names == NULL) {
        result_fail(result, ENGINE_REASON_OUT_OF_MEMORY, "메모리가 부족합니다");
        return -1;
    }
    result->field_count = field_count;
    for (size_t i = 0; i < field_count; i++) {
        const char *name;

        if (statement->field_count == 0) {
            sources[i] = i;
            name = i == 0 ? "id" : table->fields[i - 1].name;
        } else if (!table_find_field(table, statement->fields[i],
                                     &sources[i])) {
            result_fail(result, ENGINE_REASON_NO_FIELD, "없는 field입니다: %s",
                        statement->fields[i]);
            return -1;
        } else {
            name = statement->fields[i];
        }
        result->field_names[i] = strdup(name);
        if (result->field_names[i] == NULL) {
            result_fail(result, ENGINE_REASON_OUT_OF_MEMORY,
                        "메모리가 부족합니다");
            return -1;
        }
    }
    return 0;
}

/* record를 결과로 복사합니다. iterator의 text는 다음 next에서 바뀌기
   때문입니다. */
static int result_add_record(struct engine_result *result,
                             const size_t *sources, const struct record *record)
{
    size_t field_count = result->field_count;
    struct value *values =
        realloc(result->values, (result->record_count + 1) * field_count *
                                    sizeof(*result->values));
    struct value *record_values;

    if (values == NULL) {
        return -1;
    }
    result->values = values;
    record_values = &values[result->record_count * field_count];
    memset(record_values, 0, field_count * sizeof(*record_values));
    result->record_count++;
    for (size_t i = 0; i < field_count; i++) {
        struct value source = record_field(record, sources[i]);
        char *text;

        record_values[i] = source;
        if (source.type != VALUE_TEXT) {
            continue;
        }
        text = malloc((size_t)source.text_len + 1);
        record_values[i].text = text;
        if (text == NULL) {
            return -1;
        }
        memcpy(text, source.text, source.text_len);
        text[source.text_len] = '\0';
    }
    return 0;
}

/* 상한 0은 그 상한을 두지 않는다는 뜻입니다. */
static bool result_is_over_limit(const struct engine_result *result,
                                 const struct engine_options *options,
                                 size_t result_bytes)
{
    return (options->max_records != 0 &&
            result->record_count > options->max_records) ||
           (options->max_bytes != 0 && result_bytes > options->max_bytes);
}

/* 결과 상한이 세는 record 하나의 바이트입니다. INT는 8, TEXT는 바이트
   길이입니다. */
static size_t record_bytes_get(const struct engine_result *result,
                               size_t record_index)
{
    const struct value *values =
        &result->values[record_index * result->field_count];
    size_t record_bytes = 0;

    for (size_t i = 0; i < result->field_count; i++) {
        record_bytes +=
            values[i].type == VALUE_TEXT ? values[i].text_len : sizeof(int64_t);
    }
    return record_bytes;
}

static void result_records_clear(struct engine_result *result)
{
    for (size_t i = 0; i < result->record_count * result->field_count; i++) {
        if (result->values[i].type == VALUE_TEXT) {
            free((char *)result->values[i].text);
        }
    }
    free(result->values);
    result->values = NULL;
    result->record_count = 0;
}
