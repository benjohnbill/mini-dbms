#include "engine.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"
#include "table.h"

#define SCHEMA_SUFFIX ".schema"

struct engine {
    struct table **tables;
    size_t table_count;
};

static int engine_load_tables(struct engine *engine, const char *data_dir,
                              char *error, size_t error_size);
static int engine_add_table(struct engine *engine, struct table *table);
static void result_fail(struct engine_result *result,
                        enum engine_error_code code, const char *format, ...);
static void result_set_sql_error(struct engine_result *result,
                                 const struct sql_error *sql_error);
static struct table *engine_find_table(const struct engine *engine,
                                       const char *name);
static void execute_insert(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result);
static void execute_select(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result);
static bool table_find_field(const struct table *table, const char *name,
                             size_t *source);
static int where_check(struct engine_result *result, const struct table *table,
                       const struct statement *statement, size_t *where_source);
static struct value record_field(const struct record *record, size_t source);
static const char *value_type_name(enum value_type type);
static bool value_equal(const struct value *left, const struct value *right);
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

    (void)options;
    if (engine == NULL) {
        snprintf(error, error_size, "메모리가 부족합니다");
        return NULL;
    }
    if (engine_load_tables(engine, data_dir, error, error_size) != 0) {
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
    struct table *table;

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
    /* 검증 오류는 문장의 첫 토큰을 가리킵니다. 성공하면 쓰지 않습니다. */
    result->error_line = statement->line;
    result->error_byte_in_line = statement->byte_in_line;
    table = engine_find_table(engine, statement->table);
    if (table == NULL) {
        result_fail(result, ENGINE_ERROR_NO_TABLE, "없는 테이블입니다: %s",
                    statement->table);
    } else if (statement->kind == STATEMENT_INSERT) {
        execute_insert(table, statement, result);
    } else {
        execute_select(table, statement, result);
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
    free(engine);
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

static void result_fail(struct engine_result *result,
                        enum engine_error_code code, const char *format, ...)
{
    va_list args;

    result->status = ENGINE_ERROR;
    result->error_code = code;
    va_start(args, format);
    vsnprintf(result->error_message, sizeof(result->error_message), format,
              args);
    va_end(args);
}

static void result_set_sql_error(struct engine_result *result,
                                 const struct sql_error *sql_error)
{
    static const enum engine_error_code codes[] = {
        [SQL_ERROR_SYNTAX] = ENGINE_ERROR_SYNTAX,
        [SQL_ERROR_INT_RANGE] = ENGINE_ERROR_INT_RANGE,
        [SQL_ERROR_TEXT_TOO_LONG] = ENGINE_ERROR_TEXT_TOO_LONG,
        [SQL_ERROR_NO_MEMORY] = ENGINE_ERROR_NO_MEMORY,
    };

    result_fail(result, codes[sql_error->code], "%s", sql_error->message);
    result->error_line = sql_error->line;
    result->error_byte_in_line = sql_error->byte_in_line;
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
    result->kind = ENGINE_STATEMENT_INSERT;
    if (statement->value_count != table->field_count) {
        result_fail(
            result, ENGINE_ERROR_VALUE_COUNT,
            "%s에는 값 %zu개가 와야 하는데 %zu개입니다(id는 적지 않습니다)",
            table->name, table->field_count, statement->value_count);
        return;
    }
    for (size_t i = 0; i < table->field_count; i++) {
        if (statement->values[i].type != table->fields[i].type) {
            result_fail(
                result, ENGINE_ERROR_TYPE, "%s field에는 %s 값이 와야 합니다",
                table->fields[i].name, value_type_name(table->fields[i].type));
            return;
        }
    }
    if (table_insert(table, statement->values, &result->inserted_id,
                     result->error_message,
                     sizeof(result->error_message)) != 0) {
        result->status = ENGINE_ERROR;
        result->error_code = ENGINE_ERROR_STORAGE;
        return;
    }
    result->status = ENGINE_OK;
}

static void execute_select(struct table *table,
                           const struct statement *statement,
                           struct engine_result *result)
{
    size_t source_count = statement->field_count == 0 ? table->field_count + 1
                                                      : statement->field_count;
    size_t *sources = calloc(source_count, sizeof(*sources));
    struct record_iterator *iterator = NULL;
    struct record record;
    size_t where_source = 0;
    int status;

    result->kind = ENGINE_STATEMENT_SELECT;
    result->access_path = ENGINE_ACCESS_PATH_SCAN;
    if (sources == NULL) {
        result_fail(result, ENGINE_ERROR_NO_MEMORY, "메모리가 부족합니다");
        return;
    }
    if (result_set_fields(result, table, statement, sources) == 0 &&
        where_check(result, table, statement, &where_source) == 0) {
        iterator = table_scan_create(table);
        if (iterator == NULL) {
            result_fail(result, ENGINE_ERROR_NO_MEMORY, "메모리가 부족합니다");
        }
    }
    if (iterator == NULL) {
        free(sources);
        return;
    }
    while ((status = iterator->next(iterator, &record)) == 1) {
        struct value field;

        result->records_examined++;
        if (statement->where_field != NULL) {
            field = record_field(&record, where_source);
            if (!value_equal(&field, &statement->where_value)) {
                continue;
            }
        }
        if (result_add_record(result, sources, &record) != 0) {
            status = -1;
            break;
        }
    }
    iterator->destroy(iterator);
    free(sources);
    if (status != 0) {
        result_fail(result, ENGINE_ERROR_STORAGE, "%s.tbl을 읽지 못했습니다",
                    table->name);
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
        result_fail(result, ENGINE_ERROR_NO_FIELD, "없는 field입니다: %s",
                    statement->where_field);
        return -1;
    }
    type =
        *where_source == 0 ? VALUE_INT : table->fields[*where_source - 1].type;
    if (statement->where_value.type != type) {
        result_fail(result, ENGINE_ERROR_TYPE,
                    "%s field에는 %s 값이 와야 합니다", statement->where_field,
                    value_type_name(type));
        return -1;
    }
    return 0;
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
        result_fail(result, ENGINE_ERROR_NO_MEMORY, "메모리가 부족합니다");
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
            result_fail(result, ENGINE_ERROR_NO_FIELD, "없는 field입니다: %s",
                        statement->fields[i]);
            return -1;
        } else {
            name = statement->fields[i];
        }
        result->field_names[i] = strdup(name);
        if (result->field_names[i] == NULL) {
            result_fail(result, ENGINE_ERROR_NO_MEMORY, "메모리가 부족합니다");
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
