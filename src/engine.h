/* DB 엔진의 공개 인터페이스입니다. CLI, API 서버, 테스트는 이 네 함수만
   부릅니다. 결과 구조체에는 칸을 더할 수 있지만 함수 모양은 바꾸지 않습니다. */
#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "value.h"

#define ENGINE_ERROR_MAX 256

struct engine;

/* 엔진을 만들 때 정하는 옵션입니다. NULL을 넘기면 모든 칸이 0입니다.
   결과 상한은 SELECT 하나가 모을 수 있는 record 수와 값의 누적 바이트(INT는
   8, TEXT는 바이트 길이)입니다. 넘으면 ENGINE_REASON_RESULT_TOO_LARGE로
   끝납니다. 0은 상한을 두지 않는다는 뜻이고, CLI는 0을 씁니다. */
struct engine_options {
    size_t max_records;
    size_t max_bytes;
};

enum engine_status {
    ENGINE_OK,
    ENGINE_EMPTY, /* 문장 없음: 남은 SQL이 공백뿐입니다 */
    ENGINE_ERROR,
};

/* 기계가 읽는 오류 이유입니다. API 서버가 상태 코드와 응답의 error 값을
   고를 때 씁니다. 성공과 문장 없음은 ENGINE_REASON_NONE입니다. */
enum engine_error_reason {
    ENGINE_REASON_NONE,
    ENGINE_REASON_SYNTAX,
    ENGINE_REASON_NO_TABLE,
    ENGINE_REASON_NO_FIELD,
    ENGINE_REASON_VALUE_COUNT,
    ENGINE_REASON_TYPE,
    ENGINE_REASON_INT_RANGE,
    ENGINE_REASON_TEXT_TOO_LONG,
    ENGINE_REASON_OUT_OF_MEMORY,
    ENGINE_REASON_STORAGE_ERROR, /* 테이블 파일 읽기와 쓰기 */
    ENGINE_REASON_TABLE_UNAVAILABLE,
    ENGINE_REASON_MULTIPLE_STATEMENTS, /* API 모드에서 문장 뒤에 다른 글자 */
    ENGINE_REASON_RESULT_TOO_LARGE,    /* SELECT 결과가 결과 상한을 넘음 */
    ENGINE_REASON_ID_EXHAUSTED,        /* 테이블의 ID가 INT 최댓값에 닿음 */
};

enum engine_statement_kind {
    ENGINE_STATEMENT_NONE,
    ENGINE_STATEMENT_INSERT,
    ENGINE_STATEMENT_SELECT,
};

/* SELECT가 record를 찾은 방법입니다. 조건이 id = 정수이면 INDEX, 그 밖은
   SCAN입니다. */
enum engine_access_path {
    ENGINE_ACCESS_PATH_SCAN,
    ENGINE_ACCESS_PATH_INDEX,
};

/* 실행 결과 하나입니다. 받은 쪽이 engine_result_destroy()로 해제합니다.
   record i의 field j는 values[i * field_count + j]에 있습니다. */
struct engine_result {
    enum engine_status status;
    enum engine_statement_kind kind;
    enum engine_error_reason error_reason;
    /* 오류 위치입니다. engine_execute가 받은 sql의 첫 줄을 1로 셉니다.
       파일 안의 줄 번호는 부르는 쪽이 앞의 줄 수를 더해 구합니다. */
    size_t error_line;
    size_t error_byte_in_line;
    char error_message[ENGINE_ERROR_MAX];
    int64_t inserted_id;
    size_t field_count;
    char **field_names;
    size_t record_count;
    struct value *values;
    enum engine_access_path access_path;
    uint64_t records_examined; /* 조건을 보려고 읽은 record 수 */
};

/* data_dir의 스키마 파일과 테이블 파일을 엽니다. 실패하면 NULL을 돌려주고
   이유를 error에 씁니다. */
struct engine *engine_create(const char *data_dir,
                             const struct engine_options *options, char *error,
                             size_t error_size);

/* sql 맨 앞의 문장 하나만 실행합니다. next_sql이 NULL이 아니면 다음 문장이
   시작하는 곳을 *next_sql에 씁니다(CLI 모드). next_sql이 NULL이면 API
   모드이고, 문장 뒤에 공백 말고 다른 글자가 있으면 실행하지 않고
   ENGINE_REASON_MULTIPLE_STATEMENTS로 끝냅니다. 결과 구조체를 만들 메모리가
   없을 때만 NULL을 돌려줍니다. */
struct engine_result *engine_execute(struct engine *engine, const char *sql,
                                     const char **next_sql);

void engine_result_destroy(struct engine_result *result);

void engine_destroy(struct engine *engine);

#endif
