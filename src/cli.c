/* mini-dbms CLI의 진입점입니다. SQL 파일을 메모리에 읽어 DB 엔진에 문장을
   하나씩 넘기고, SELECT 결과를 stdout에 TSV로 씁니다. `dump <테이블>`은
   바이너리 테이블 파일을 같은 TSV로 씁니다.
   종료 코드: 0 성공, 1 파일·SQL·실행 오류, 2 잘못된 CLI 인자. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

#define EXIT_USAGE 2
#define DEFAULT_DATA_DIR "data"
#define DUMP_COMMAND "dump"
#define DUMP_SQL_BYTES 1024

/* dump_table이 NULL이 아니면 dump 명령이고, 아니면 sql_path를 실행합니다. */
struct cli_args {
    const char *data_dir;
    const char *sql_path;
    const char *dump_table;
};

static int args_parse(int argc, char *argv[], struct cli_args *args);
static int table_dump(struct engine *engine, const char *table_name);
static char *sql_file_read(const char *path);
static int sql_run(struct engine *engine, const char *sql);
static void result_print(const struct engine_result *result);
static void text_print_escaped(const char *text, size_t text_len);

int main(int argc, char *argv[])
{
    struct cli_args args = {.data_dir = DEFAULT_DATA_DIR};
    char error[ENGINE_ERROR_MAX];
    struct engine *engine;
    char *sql;
    int status;

    if (args_parse(argc, argv, &args) != 0) {
        fprintf(stderr,
                "usage: %s [--data <dir>] <sql-file>\n"
                "       %s [--data <dir>] dump <table>\n",
                argv[0], argv[0]);
        return EXIT_USAGE;
    }
    if (args.dump_table != NULL) {
        engine = engine_create(args.data_dir, NULL, error, sizeof(error));
        if (engine == NULL) {
            fprintf(stderr, "%s\n", error);
            return EXIT_FAILURE;
        }
        status = table_dump(engine, args.dump_table);
        engine_destroy(engine);
        return status;
    }
    sql = sql_file_read(args.sql_path);
    if (sql == NULL) {
        fprintf(stderr, "%s: %s\n", args.sql_path, strerror(errno));
        return EXIT_FAILURE;
    }
    engine = engine_create(args.data_dir, NULL, error, sizeof(error));
    if (engine == NULL) {
        fprintf(stderr, "%s\n", error);
        free(sql);
        return EXIT_FAILURE;
    }
    status = sql_run(engine, sql);
    engine_destroy(engine);
    free(sql);

    return status;
}

/* 위치 인자가 하나면 SQL 파일이고, `dump <테이블>` 두 개면 dump 명령입니다.
   그래서 이름이 dump인 SQL 파일도 실행할 수 있습니다. */
static int args_parse(int argc, char *argv[], struct cli_args *args)
{
    const char *positional[2];
    int positional_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            args->data_dir = argv[++i];
        } else if (argv[i][0] == '-' || positional_count == 2) {
            return -1;
        } else {
            positional[positional_count++] = argv[i];
        }
    }
    if (positional_count == 1) {
        args->sql_path = positional[0];
        return 0;
    }
    if (positional_count == 2 && strcmp(positional[0], DUMP_COMMAND) == 0) {
        args->dump_table = positional[1];
        return 0;
    }
    return -1;
}

/* 테이블 파일의 모든 record를 SELECT *와 같은 TSV로 씁니다. 엔진의 SCAN을
   그대로 쓰므로 두 출력은 같습니다. 테이블 이름 뒤에 다른 문장이 붙지 않게
   문장 하나로 끝나는지 확인합니다. */
static int table_dump(struct engine *engine, const char *table_name)
{
    char sql[DUMP_SQL_BYTES];
    const char *next_sql = NULL;
    struct engine_result *result;
    int status = EXIT_SUCCESS;

    snprintf(sql, sizeof(sql), "SELECT * FROM %s;", table_name);
    result = engine_execute(engine, sql, &next_sql);
    if (result == NULL) {
        fprintf(stderr, "out of memory\n");
        return EXIT_FAILURE;
    }
    if (result->status != ENGINE_OK) {
        fprintf(stderr, "error: %s\n", result->error_message);
        status = EXIT_FAILURE;
    } else if (*next_sql != '\0') {
        fprintf(stderr, "error: bad table name: %s\n", table_name);
        status = EXIT_FAILURE;
    } else {
        result_print(result);
    }
    engine_result_destroy(result);

    return status;
}

/* 파일 전체를 NUL로 끝나는 문자열 하나로 읽습니다. */
static char *sql_file_read(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *sql = NULL;
    size_t sql_len = 0;
    size_t capacity = 0;
    size_t read_len;

    if (file == NULL) {
        return NULL;
    }
    do {
        if (sql_len + 1 >= capacity) {
            char *grown;

            capacity = capacity == 0 ? 4096 : capacity * 2;
            grown = realloc(sql, capacity);
            if (grown == NULL) {
                free(sql);
                fclose(file);
                return NULL;
            }
            sql = grown;
        }
        read_len = fread(sql + sql_len, 1, capacity - sql_len - 1, file);
        sql_len += read_len;
    } while (read_len > 0);
    if (ferror(file)) {
        free(sql);
        sql = NULL;
    } else {
        sql[sql_len] = '\0';
    }
    fclose(file);

    return sql;
}

/* 문장 없음이 나올 때까지 다음 문장 위치에서 다시 부릅니다. 첫 오류에서
   멈춥니다. */
static int sql_run(struct engine *engine, const char *sql)
{
    for (;;) {
        struct engine_result *result = engine_execute(engine, sql, &sql);
        int status;

        if (result == NULL) {
            fprintf(stderr, "out of memory\n");
            return EXIT_FAILURE;
        }
        if (result->status == ENGINE_OK) {
            result_print(result);
        } else if (result->status == ENGINE_ERROR) {
            fprintf(stderr, "error: %s\n", result->error_message);
        }
        status = result->status;
        engine_result_destroy(result);
        if (status == ENGINE_EMPTY) {
            return EXIT_SUCCESS;
        }
        if (status == ENGINE_ERROR) {
            return EXIT_FAILURE;
        }
    }
}

/* SELECT만 출력합니다. 첫 줄은 field 이름이고 그 뒤 record마다 한 줄입니다. */
static void result_print(const struct engine_result *result)
{
    if (result->kind != ENGINE_STATEMENT_SELECT) {
        return;
    }
    for (size_t j = 0; j < result->field_count; j++) {
        printf(j == 0 ? "%s" : "\t%s", result->field_names[j]);
    }
    putchar('\n');
    for (size_t i = 0; i < result->record_count; i++) {
        for (size_t j = 0; j < result->field_count; j++) {
            const struct value *value =
                &result->values[i * result->field_count + j];

            if (j > 0) {
                putchar('\t');
            }
            if (value->type == VALUE_INT) {
                printf("%lld", (long long)value->integer);
            } else {
                text_print_escaped(value->text, value->text_len);
            }
        }
        putchar('\n');
    }
}

/* TSV의 칸과 줄을 지키려고 탭, 줄바꿈, 역슬래시를 \t, \n, \\로 씁니다. */
static void text_print_escaped(const char *text, size_t text_len)
{
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] == '\t') {
            fputs("\\t", stdout);
        } else if (text[i] == '\n') {
            fputs("\\n", stdout);
        } else if (text[i] == '\\') {
            fputs("\\\\", stdout);
        } else {
            putchar(text[i]);
        }
    }
}
