/* 토크나이저와 재귀 하강 파서입니다. SQL 문자열 맨 앞의 문장 하나를 문장
   구조체로 바꿉니다. 실행기는 SQL 문자열을 다시 보지 않습니다. */
#ifndef SQL_H
#define SQL_H

#include <stddef.h>

#include "value.h"

enum statement_kind {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
};

/* INSERT의 values는 문장에 적은 순서 그대로입니다. SELECT는 values가 없고,
   fields가 field 목록입니다. field_count가 0이면 *입니다. where_field가
   NULL이 아니면 WHERE where_field = where_value입니다. */
struct statement {
    enum statement_kind kind;
    char *table;
    size_t value_count;
    struct value *values;
    size_t field_count;
    char **fields;
    char *where_field;
    struct value where_value;
};

enum sql_status {
    SQL_OK,
    SQL_EMPTY,
    SQL_ERROR,
};

/* 성공하면 *statement에 새 문장을, *next_sql에 ; 다음 위치를 씁니다.
   SQL_EMPTY이면 *next_sql은 문자열 끝입니다. */
enum sql_status sql_parse(const char *sql, struct statement **statement,
                          const char **next_sql, char *error,
                          size_t error_size);

void sql_statement_destroy(struct statement *statement);

#endif
