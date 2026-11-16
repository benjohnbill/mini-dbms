#include "engine.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "sql.h"
#include "table.h"

#define SCHEMA_SUFFIX ".schema"
#define LOCK_FILE_NAME ".lock"
#define LOCK_PATH_BYTES 1024

/* lock_fd는 잠금 파일을 연 fd이고, 닫으면 잠금이 풀립니다. */
struct engine {
    int lock_fd;
    struct table **tables;
    size_t table_count;
};

static int engine_lock_data_dir(struct engine *engine, const char *data_dir,
                                char *error, size_t error_size);
static int engine_load_tables(struct engine *engine, const char *data_dir,
                              char *error, size_t error_size);
static int engine_add_table(struct engine *engine, struct table *table);
static void result_fail(struct engine_result *result, const char *format, ...);
static struct table *engine_find_table(const struct engine *engine,
                                       const char *name);
static void execute_insert(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result);
static enum engine_error_reason
insert_error_reason(enum table_insert_status insert_status);
static void execute_select(struct table *table, struct engine_result *result);
static int result_set_fields(struct engine_result *result,
                             const struct table *table);
static int result_add_record(struct engine_result *result,
                             const struct table *table,
                             const struct record *record);

struct engine *engine_create(const char *data_dir,
                             const struct engine_options *options, char *error,
                             size_t error_size)
{
    struct engine *engine = calloc(1, sizeof(*engine));

    (void)options;
    if (engine == NULL) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    engine->lock_fd = -1;
    /* 다른 프로세스가 같은 파일을 쓰는 중에 읽지 않도록, 스키마와 테이블
       파일을 읽기 전에 잠급니다. */
    if (engine_lock_data_dir(engine, data_dir, error, error_size) != 0 ||
        engine_load_tables(engine, data_dir, error, error_size) != 0) {
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
    enum sql_status sql_status;
    struct table *table;

    if (result == NULL) {
        return NULL;
    }
    sql_status =
        sql_parse(sql, &statement, &statement_end, result->error_message,
                  sizeof(result->error_message));
    if (next_sql != NULL) {
        *next_sql = statement_end;
    }
    if (sql_status == SQL_EMPTY) {
        result->status = ENGINE_EMPTY;
        return result;
    }
    if (sql_status == SQL_ERROR) {
        result->status = ENGINE_ERROR;
        return result;
    }
    table = engine_find_table(engine, statement->table);
    if (table == NULL) {
        result_fail(result, "no such table: %s", statement->table);
    } else if (!table->available) {
        /* 되돌리기가 실패한 테이블은 파일 상태를 알 수 없으므로 읽지도
           쓰지도 않습니다. 재시작하면 시작할 때 읽기가 파일을 판정합니다. */
        result_fail(result, "%s: table is unavailable until restart",
                    statement->table);
        result->error_reason = ENGINE_REASON_TABLE_UNAVAILABLE;
    } else if (statement->kind == STATEMENT_INSERT) {
        execute_insert(table, statement, result);
    } else {
        execute_select(table, result);
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
    for (size_t i = 0; i < result->record_count * result->field_count; i++) {
        if (result->values[i].type == VALUE_TEXT) {
            free((char *)result->values[i].text);
        }
    }
    free(result->field_names);
    free(result->values);
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

static void result_fail(struct engine_result *result, const char *format, ...)
{
    va_list args;

    result->status = ENGINE_ERROR;
    va_start(args, format);
    vsnprintf(result->error_message, sizeof(result->error_message), format,
              args);
    va_end(args);
}

static struct table *engine_find_table(const struct engine *engine,
                                       const char *name)
{
    for (size_t i = 0; i < engine->table_count; i++) {
        if (strcmp(engine->tables[i]->name, name) == 0) {
            return engine->tables[i];
        }
    }
    return NULL;
}

/* 값 개수와 타입을 스키마와 먼저 맞춘 뒤에 파일에 씁니다. */
static void execute_insert(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result)
{
    enum table_insert_status insert_status;

    result->kind = ENGINE_STATEMENT_INSERT;
    if (statement->value_count != table->field_count) {
        result_fail(result, "%s has %zu fields but got %zu values", table->name,
                    table->field_count, statement->value_count);
        return;
    }
    for (size_t i = 0; i < table->field_count; i++) {
        if (statement->values[i].type != table->fields[i].type) {
            result_fail(result, "type mismatch for field %s",
                        table->fields[i].name);
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

static void execute_select(struct table *table, struct engine_result *result)
{
    struct record_iterator *iterator;
    struct record record;
    int status;

    result->kind = ENGINE_STATEMENT_SELECT;
    if (result_set_fields(result, table) != 0) {
        result_fail(result, "out of memory");
        return;
    }
    iterator = table_scan_create(table);
    if (iterator == NULL) {
        result_fail(result, "out of memory");
        return;
    }
    while ((status = iterator->next(iterator, &record)) == 1) {
        if (result_add_record(result, table, &record) != 0) {
            status = -1;
            break;
        }
    }
    iterator->destroy(iterator);
    if (status != 0) {
        result_fail(result, "%s.tbl: cannot read records", table->name);
        return;
    }
    result->status = ENGINE_OK;
}

/* 첫 field는 id이고, 그 뒤는 스키마 순서입니다. */
static int result_set_fields(struct engine_result *result,
                             const struct table *table)
{
    size_t field_count = table->field_count + 1;

    result->field_names = calloc(field_count, sizeof(*result->field_names));
    if (result->field_names == NULL) {
        return -1;
    }
    result->field_count = field_count;
    result->field_names[0] = strdup("id");
    for (size_t i = 0; i < table->field_count; i++) {
        result->field_names[i + 1] = strdup(table->fields[i].name);
    }
    for (size_t i = 0; i < field_count; i++) {
        if (result->field_names[i] == NULL) {
            return -1;
        }
    }
    return 0;
}

/* record를 결과로 복사합니다. iterator의 text는 다음 next에서 바뀌기
   때문입니다. */
static int result_add_record(struct engine_result *result,
                             const struct table *table,
                             const struct record *record)
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
    record_values[0].type = VALUE_INT;
    record_values[0].integer = record->id;
    result->record_count++;
    for (size_t i = 0; i < table->field_count; i++) {
        struct value *copy = &record_values[i + 1];
        const struct value *source = &record->values[i];
        char *text;

        *copy = *source;
        if (source->type != VALUE_TEXT) {
            continue;
        }
        text = malloc((size_t)source->text_len + 1);
        copy->text = text;
        if (text == NULL) {
            return -1;
        }
        memcpy(text, source->text, source->text_len);
        text[source->text_len] = '\0';
    }
    return 0;
}
