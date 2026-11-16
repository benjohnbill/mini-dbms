/* DB 엔진의 공개 인터페이스입니다. CLI, API 서버, 테스트는 이 네 함수만
   부릅니다. 결과 구조체에는 칸을 더할 수 있지만 함수 모양은 바꾸지 않습니다. */
#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "value.h"

#define ENGINE_ERROR_MAX 256

struct engine;

/* 지금은 칸이 없습니다. 결과 상한 같은 옵션이 여기에 더해집니다.
   NULL을 넘기면 기본값을 씁니다. */
struct engine_options {
    int reserved;
};

enum engine_status {
    ENGINE_OK,
    ENGINE_EMPTY, /* 문장 없음: 남은 SQL이 공백뿐입니다 */
    ENGINE_ERROR,
};

enum engine_statement_kind {
    ENGINE_STATEMENT_NONE,
    ENGINE_STATEMENT_INSERT,
    ENGINE_STATEMENT_SELECT,
};

/* 실행 결과 하나입니다. 받은 쪽이 engine_result_destroy()로 해제합니다.
   record i의 field j는 values[i * field_count + j]에 있습니다. */
struct engine_result {
    enum engine_status status;
    enum engine_statement_kind kind;
    char error_message[ENGINE_ERROR_MAX];
    int64_t inserted_id;
    size_t field_count;
    char **field_names;
    size_t record_count;
    struct value *values;
};

/* data_dir의 스키마 파일과 테이블 파일을 엽니다. 실패하면 NULL을 돌려주고
   이유를 error에 씁니다. */
struct engine *engine_create(const char *data_dir,
                             const struct engine_options *options, char *error,
                             size_t error_size);

/* sql 맨 앞의 문장 하나만 실행합니다. next_sql이 NULL이 아니면 다음 문장이
   시작하는 곳을 *next_sql에 씁니다(CLI 모드). 결과 구조체를 만들 메모리가
   없을 때만 NULL을 돌려줍니다. */
struct engine_result *engine_execute(struct engine *engine, const char *sql,
                                     const char **next_sql);

void engine_result_destroy(struct engine_result *result);

void engine_destroy(struct engine *engine);

#endif
