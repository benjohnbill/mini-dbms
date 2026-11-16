/* mini-dbms CLI의 진입점입니다. SQL 파일을 메모리에 읽어 DB 엔진에 문장을
   하나씩 넘기고, SELECT 결과를 stdout에 TSV로, 문장마다 메타 줄 하나를
   stderr에 씁니다.
   종료 코드: 0 성공, 1 파일·SQL·실행 오류, 2 잘못된 CLI 인자. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "engine.h"

#define EXIT_USAGE 2
#define DEFAULT_DATA_DIR "data"

/* quiet이면 INSERT의 메타 줄을 쓰지 않습니다. 100만 건 적재용입니다. */
struct cli_args {
    const char *data_dir;
    const char *sql_path;
    bool quiet;
};

static int args_parse(int argc, char *argv[], struct cli_args *args);
static char *sql_file_read(const char *path, size_t *sql_len_out);
static int sql_run(struct engine *engine, const char *sql, bool quiet);
static double elapsed_ms_get(const struct timespec *start);
static void meta_print(const struct engine_result *result, double elapsed_ms,
                       bool quiet);
static void error_print(const char *sql, const char *statement_sql,
                        const struct engine_result *result);
static void result_print(const struct engine_result *result);
static void text_print_escaped(const char *text, size_t text_len);

int main(int argc, char *argv[])
{
    struct cli_args args = {.data_dir = DEFAULT_DATA_DIR};
    char error[ENGINE_ERROR_MAX];
    struct engine *engine;
    char *sql;
    size_t sql_len;
    int status;

    if (args_parse(argc, argv, &args) != 0) {
        fprintf(stderr, "usage: %s [--data <dir>] [-q] <sql-file>\n", argv[0]);
        return EXIT_USAGE;
    }
    sql = sql_file_read(args.sql_path, &sql_len);
    if (sql == NULL) {
        fprintf(stderr, "%s: %s\n", args.sql_path, strerror(errno));
        return EXIT_FAILURE;
    }
    /* 엔진은 SQL을 NUL로 끝나는 문자열로 받으므로, 중간의 NUL 뒤는 보이지
       않습니다. 일부만 실행하지 않도록 아무 문장도 실행하지 않습니다. */
    if (memchr(sql, '\0', sql_len) != NULL) {
        fprintf(stderr, "%s: SQL 파일에 NUL 바이트가 있습니다\n",
                args.sql_path);
        free(sql);
        return EXIT_FAILURE;
    }
    engine = engine_create(args.data_dir, NULL, error, sizeof(error));
    if (engine == NULL) {
        fprintf(stderr, "%s\n", error);
        free(sql);
        return EXIT_FAILURE;
    }
    status = sql_run(engine, sql, args.quiet);
    engine_destroy(engine);
    free(sql);

    return status;
}

static int args_parse(int argc, char *argv[], struct cli_args *args)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            args->data_dir = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0) {
            args->quiet = true;
        } else if (argv[i][0] == '-' || args->sql_path != NULL) {
            return -1;
        } else {
            args->sql_path = argv[i];
        }
    }
    return args->sql_path == NULL ? -1 : 0;
}

/* 파일 전체를 NUL로 끝나는 문자열 하나로 읽고, 끝의 NUL을 뺀 길이를
   *sql_len_out에 씁니다. */
static char *sql_file_read(const char *path, size_t *sql_len_out)
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
        *sql_len_out = sql_len;
    }
    fclose(file);

    return sql;
}

/* 문장 없음이 나올 때까지 다음 문장 위치에서 다시 부릅니다. 첫 오류에서
   멈춥니다. */
static int sql_run(struct engine *engine, const char *sql, bool quiet)
{
    const char *next_sql = sql;

    for (;;) {
        const char *statement_sql = next_sql;
        struct timespec start;
        struct engine_result *result;
        double elapsed_ms;
        int status;

        clock_gettime(CLOCK_MONOTONIC, &start);
        result = engine_execute(engine, statement_sql, &next_sql);
        elapsed_ms = elapsed_ms_get(&start);

        if (result == NULL) {
            fprintf(stderr, "out of memory\n");
            return EXIT_FAILURE;
        }
        if (result->status == ENGINE_OK) {
            result_print(result);
            meta_print(result, elapsed_ms, quiet);
        } else if (result->status == ENGINE_ERROR) {
            error_print(sql, statement_sql, result);
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

static double elapsed_ms_get(const struct timespec *start)
{
    struct timespec end;

    clock_gettime(CLOCK_MONOTONIC, &end);
    return (double)(end.tv_sec - start->tv_sec) * 1000.0 +
           (double)(end.tv_nsec - start->tv_nsec) / 1e6;
}

/* "-- 1 record inserted, id 3"과 "-- 2 records, SCAN, examined 5, 0.02 ms"
   모양입니다. 시간은 파싱부터 결과 구조체를 다 채울 때까지입니다. */
static void meta_print(const struct engine_result *result, double elapsed_ms,
                       bool quiet)
{
    if (result->kind == ENGINE_STATEMENT_INSERT) {
        if (!quiet) {
            fprintf(stderr, "-- 1 record inserted, id %lld\n",
                    (long long)result->inserted_id);
        }
        return;
    }
    fprintf(stderr, "-- %zu record%s, %s, examined %llu, %.2f ms\n",
            result->record_count, result->record_count == 1 ? "" : "s",
            result->access_path == ENGINE_ACCESS_PATH_SCAN ? "SCAN" : "INDEX",
            (unsigned long long)result->records_examined, elapsed_ms);
}

/* 엔진의 줄 번호는 statement_sql부터 셉니다. 그 앞의 줄 수를 더해 SQL
   파일 안의 줄 번호로 씁니다. */
static void error_print(const char *sql, const char *statement_sql,
                        const struct engine_result *result)
{
    size_t line = result->error_line;

    for (const char *cursor = sql; cursor < statement_sql; cursor++) {
        if (*cursor == '\n') {
            line++;
        }
    }
    fprintf(stderr, "%zu번째 줄: %s\n", line, result->error_message);
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
