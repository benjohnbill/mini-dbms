#include "sql.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* TEXT 값 하나의 최대 바이트 수입니다(최종 PRD § Phase 1 타입). */
#define TEXT_MAX_BYTES ((size_t)1 << 20)

enum token_kind {
    TOKEN_END,
    TOKEN_WORD,
    TOKEN_INTEGER,
    TOKEN_STRING,
    TOKEN_SYMBOL,
};

/* TOKEN_STRING의 text는 ''를 '로 바꾼 새 메모리이고, 받은 쪽이 해제합니다.
   TOKEN_WORD의 이름은 start와 length로 SQL 문자열 안을 가리킵니다.
   line과 byte_in_line은 토큰 첫 글자의 위치입니다. */
struct token {
    enum token_kind kind;
    const char *start;
    size_t length;
    size_t line;
    size_t byte_in_line;
    int64_t integer;
    char *text;
    char symbol;
};

/* previous는 오류 메시지의 "X 다음에"에 쓰는 바로 앞 토큰입니다. text는
   비어 있습니다. */
struct parser {
    const char *next_char;
    size_t line;
    const char *line_start;
    struct token current;
    struct token previous;
    struct sql_error *error;
    bool failed;
};

static void parser_fail(struct parser *parser, enum sql_error_code code,
                        const char *format, ...);
static void parser_fail_expected(struct parser *parser, const char *expected);
static void parser_advance(struct parser *parser);
static void parser_skip_space(struct parser *parser);
static void token_read_integer(struct parser *parser);
static void token_read_string(struct parser *parser);
static size_t string_span_get(const char *next_char);
static bool parser_at_symbol(const struct parser *parser, char symbol);
static bool parser_accept_keyword(struct parser *parser, const char *keyword);
static void parser_expect_keyword(struct parser *parser, const char *keyword,
                                  const char *expected);
static void parser_expect_symbol(struct parser *parser, char symbol,
                                 const char *expected);
static char *parser_expect_name(struct parser *parser, const char *expected);
static void parser_parse_insert(struct parser *parser,
                                struct statement *statement);
static void parser_parse_values(struct parser *parser,
                                struct statement *statement);
static bool parser_take_value(struct parser *parser, struct value *value);
static bool statement_add_value(struct statement *statement,
                                struct value value);
static void parser_parse_select(struct parser *parser,
                                struct statement *statement);
static void parser_parse_fields(struct parser *parser,
                                struct statement *statement);
static bool statement_add_field(struct statement *statement, char *field);

enum sql_status sql_parse(const char *sql, struct statement **statement_out,
                          const char **next_sql, struct sql_error *error)
{
    struct parser parser = {
        .next_char = sql, .line = 1, .line_start = sql, .error = error};
    struct statement *statement;

    parser_advance(&parser);
    if (parser.current.kind == TOKEN_END && !parser.failed) {
        *next_sql = parser.next_char;
        return SQL_EMPTY;
    }
    statement = calloc(1, sizeof(*statement));
    if (statement == NULL) {
        parser_fail(&parser, SQL_ERROR_NO_MEMORY, "메모리가 부족합니다");
        free(parser.current.text);
        return SQL_ERROR;
    }
    statement->line = parser.current.line;
    statement->byte_in_line = parser.current.byte_in_line;
    if (parser_accept_keyword(&parser, "INSERT")) {
        parser_parse_insert(&parser, statement);
    } else if (parser_accept_keyword(&parser, "SELECT")) {
        parser_parse_select(&parser, statement);
    } else {
        parser_fail(&parser, SQL_ERROR_SYNTAX,
                    "문장은 INSERT나 SELECT로 시작해야 합니다");
    }
    /* ; 다음 토큰은 읽지 않습니다. 다음 문장의 오류가 이 문장의 오류가
       되지 않게 하기 위해서입니다. */
    if (!parser_at_symbol(&parser, ';')) {
        parser_fail_expected(&parser, "';'가");
    }
    free(parser.current.text);
    if (parser.failed) {
        sql_statement_destroy(statement);
        return SQL_ERROR;
    }
    *next_sql = parser.current.start + 1;
    *statement_out = statement;

    return SQL_OK;
}

void sql_statement_destroy(struct statement *statement)
{
    if (statement == NULL) {
        return;
    }
    for (size_t i = 0; i < statement->value_count; i++) {
        if (statement->values[i].type == VALUE_TEXT) {
            free((char *)statement->values[i].text);
        }
    }
    free(statement->values);
    for (size_t i = 0; i < statement->field_count; i++) {
        free(statement->fields[i]);
    }
    free(statement->fields);
    free(statement->where_field);
    if (statement->where_value.type == VALUE_TEXT) {
        free((char *)statement->where_value.text);
    }
    free(statement->table);
    free(statement);
}

/* 첫 오류만 남깁니다. 위치는 지금 토큰입니다. 오류 뒤의 토큰은 모두 끝으로
   봅니다. */
static void parser_fail(struct parser *parser, enum sql_error_code code,
                        const char *format, ...)
{
    va_list args;

    if (parser->failed) {
        return;
    }
    parser->failed = true;
    parser->error->code = code;
    parser->error->line = parser->current.line;
    parser->error->byte_in_line = parser->current.byte_in_line;
    va_start(args, format);
    vsnprintf(parser->error->message, sizeof(parser->error->message), format,
              args);
    va_end(args);
}

/* "VALUES 다음에 '('가 와야 합니다" 모양입니다. expected는 조사까지 붙인
   기대한 토큰입니다. 문자열 토큰은 길 수 있어 내용 대신 "문자열"로 적습니다. */
static void parser_fail_expected(struct parser *parser, const char *expected)
{
    const struct token *previous = &parser->previous;

    if (previous->kind == TOKEN_STRING) {
        parser_fail(parser, SQL_ERROR_SYNTAX, "문자열 다음에 %s 와야 합니다",
                    expected);
    } else {
        parser_fail(parser, SQL_ERROR_SYNTAX, "%.*s 다음에 %s 와야 합니다",
                    (int)previous->length, previous->start, expected);
    }
}

static void parser_advance(struct parser *parser)
{
    const char *next_char;

    free(parser->current.text);
    parser->current.text = NULL;
    parser->previous = parser->current;
    memset(&parser->current, 0, sizeof(parser->current));
    parser_skip_space(parser);
    next_char = parser->next_char;
    parser->current.start = next_char;
    parser->current.line = parser->line;
    parser->current.byte_in_line = (size_t)(next_char - parser->line_start) + 1;
    if (parser->failed || *next_char == '\0') {
        parser->current.kind = TOKEN_END;
    } else if (isalpha((unsigned char)*next_char) || *next_char == '_') {
        while (isalnum((unsigned char)*next_char) || *next_char == '_') {
            next_char++;
        }
        parser->current.kind = TOKEN_WORD;
        parser->current.length = (size_t)(next_char - parser->current.start);
        parser->next_char = next_char;
    } else if (isdigit((unsigned char)*next_char) ||
               (*next_char == '-' && isdigit((unsigned char)next_char[1]))) {
        token_read_integer(parser);
    } else if (*next_char == '\'') {
        token_read_string(parser);
    } else if (next_char[0] == '-' && next_char[1] == '-') {
        parser_fail(parser, SQL_ERROR_SYNTAX, "주석(--)은 지원하지 않습니다");
        parser->current.kind = TOKEN_END;
    } else {
        parser->current.kind = TOKEN_SYMBOL;
        parser->current.symbol = *next_char;
        parser->current.length = 1;
        parser->next_char = next_char + 1;
    }
}

/* 공백을 건너뛰며 줄 번호를 셉니다. */
static void parser_skip_space(struct parser *parser)
{
    while (isspace((unsigned char)*parser->next_char)) {
        if (*parser->next_char == '\n') {
            parser->line++;
            parser->line_start = parser->next_char + 1;
        }
        parser->next_char++;
    }
}

static void token_read_integer(struct parser *parser)
{
    char *end;
    long long integer;

    errno = 0;
    integer = strtoll(parser->next_char, &end, 10);
    if (errno == ERANGE) {
        parser_fail(parser, SQL_ERROR_INT_RANGE,
                    "INT 범위(64비트 부호 있는 정수)를 넘는 숫자입니다");
        parser->current.kind = TOKEN_END;
        return;
    }
    parser->current.kind = TOKEN_INTEGER;
    parser->current.integer = integer;
    parser->current.length = (size_t)(end - parser->next_char);
    parser->next_char = end;
}

/* '...' 안의 ''는 작은따옴표 하나입니다. 문자열 안의 줄바꿈도 줄 번호에
   셉니다. */
static void token_read_string(struct parser *parser)
{
    const char *next_char = parser->next_char + 1;
    size_t text_len = 0;
    char *text = malloc(string_span_get(next_char) + 1);

    if (text == NULL) {
        parser_fail(parser, SQL_ERROR_NO_MEMORY, "메모리가 부족합니다");
        parser->current.kind = TOKEN_END;
        return;
    }
    for (;;) {
        if (*next_char == '\0') {
            free(text);
            parser_fail(parser, SQL_ERROR_SYNTAX, "닫히지 않은 문자열입니다");
            parser->current.kind = TOKEN_END;
            return;
        }
        if (*next_char == '\'' && next_char[1] != '\'') {
            break;
        }
        if (*next_char == '\'') {
            next_char++;
        }
        if (text_len == TEXT_MAX_BYTES) {
            free(text);
            parser_fail(parser, SQL_ERROR_TEXT_TOO_LONG,
                        "TEXT 값이 1 MiB를 넘습니다");
            parser->current.kind = TOKEN_END;
            return;
        }
        if (*next_char == '\n') {
            parser->line++;
            parser->line_start = next_char + 1;
        }
        text[text_len++] = *next_char++;
    }
    text[text_len] = '\0';
    parser->current.kind = TOKEN_STRING;
    parser->current.text = text;
    parser->current.length = text_len;
    parser->next_char = next_char + 1;
}

/* 여는 따옴표 다음부터 닫는 따옴표 앞까지의 바이트 수이고, 읽은 TEXT 값은
   이보다 길지 않습니다. 남은 SQL 전체가 아니라 이 길이만큼 할당하므로,
   문자열마다 파일 끝까지 세던 일이 없어 100만 줄 SQL 파일도 줄 수에
   비례해 읽습니다. 상한을 넘으면 거기서 멈춥니다. */
static size_t string_span_get(const char *next_char)
{
    const char *end = next_char;

    while (*end != '\0' && (size_t)(end - next_char) <= TEXT_MAX_BYTES) {
        if (*end == '\'' && end[1] != '\'') {
            break;
        }
        end += *end == '\'' ? 2 : 1;
    }

    return (size_t)(end - next_char);
}

static bool parser_at_symbol(const struct parser *parser, char symbol)
{
    return parser->current.kind == TOKEN_SYMBOL &&
           parser->current.symbol == symbol;
}

static bool parser_accept_keyword(struct parser *parser, const char *keyword)
{
    const struct token *token = &parser->current;

    if (token->kind != TOKEN_WORD || token->length != strlen(keyword) ||
        strncasecmp(token->start, keyword, token->length) != 0) {
        return false;
    }
    parser_advance(parser);

    return true;
}

static void parser_expect_keyword(struct parser *parser, const char *keyword,
                                  const char *expected)
{
    if (!parser_accept_keyword(parser, keyword)) {
        parser_fail_expected(parser, expected);
    }
}

static void parser_expect_symbol(struct parser *parser, char symbol,
                                 const char *expected)
{
    if (!parser_at_symbol(parser, symbol)) {
        parser_fail_expected(parser, expected);
        return;
    }
    parser_advance(parser);
}

static char *parser_expect_name(struct parser *parser, const char *expected)
{
    char *name;

    if (parser->current.kind != TOKEN_WORD) {
        parser_fail_expected(parser, expected);
        return NULL;
    }
    name = strndup(parser->current.start, parser->current.length);
    if (name == NULL) {
        parser_fail(parser, SQL_ERROR_NO_MEMORY, "메모리가 부족합니다");
    }
    parser_advance(parser);

    return name;
}

/* INSERT INTO 테이블 VALUES (값, …) */
static void parser_parse_insert(struct parser *parser,
                                struct statement *statement)
{
    statement->kind = STATEMENT_INSERT;
    parser_expect_keyword(parser, "INTO", "INTO가");
    statement->table = parser_expect_name(parser, "테이블 이름이");
    parser_expect_keyword(parser, "VALUES", "VALUES가");
    parser_expect_symbol(parser, '(', "'('가");
    parser_parse_values(parser, statement);
    parser_expect_symbol(parser, ')', "')'가");
}

static void parser_parse_values(struct parser *parser,
                                struct statement *statement)
{
    for (;;) {
        struct value value;

        if (!parser_take_value(parser, &value)) {
            return;
        }
        if (!statement_add_value(statement, value)) {
            if (value.type == VALUE_TEXT) {
                free((char *)value.text);
            }
            parser_fail(parser, SQL_ERROR_NO_MEMORY, "메모리가 부족합니다");
            return;
        }
        if (!parser_at_symbol(parser, ',')) {
            return;
        }
        parser_advance(parser);
    }
}

/* 지금 토큰이 정수나 문자열이면 *value로 옮기고 다음 토큰으로 넘어갑니다.
   문자열은 *value가 주인이 됩니다. */
static bool parser_take_value(struct parser *parser, struct value *value)
{
    memset(value, 0, sizeof(*value));
    if (parser->current.kind == TOKEN_INTEGER) {
        value->type = VALUE_INT;
        value->integer = parser->current.integer;
    } else if (parser->current.kind == TOKEN_STRING) {
        value->type = VALUE_TEXT;
        value->text = parser->current.text;
        value->text_len = (uint32_t)parser->current.length;
        parser->current.text = NULL;
    } else {
        parser_fail_expected(parser, "정수나 문자열이");
        return false;
    }
    parser_advance(parser);

    return true;
}

static bool statement_add_value(struct statement *statement, struct value value)
{
    struct value *values =
        realloc(statement->values,
                (statement->value_count + 1) * sizeof(*statement->values));

    if (values == NULL) {
        return false;
    }
    statement->values = values;
    statement->values[statement->value_count++] = value;

    return true;
}

/* SELECT (* 또는 field 목록) FROM 테이블 [WHERE field = 값] */
static void parser_parse_select(struct parser *parser,
                                struct statement *statement)
{
    statement->kind = STATEMENT_SELECT;
    if (parser_at_symbol(parser, '*')) {
        parser_advance(parser);
    } else {
        parser_parse_fields(parser, statement);
    }
    parser_expect_keyword(parser, "FROM", "FROM이");
    statement->table = parser_expect_name(parser, "테이블 이름이");
    if (!parser_accept_keyword(parser, "WHERE")) {
        return;
    }
    statement->where_field = parser_expect_name(parser, "field 이름이");
    parser_expect_symbol(parser, '=', "'='가");
    parser_take_value(parser, &statement->where_value);
}

static void parser_parse_fields(struct parser *parser,
                                struct statement *statement)
{
    for (;;) {
        char *field = parser_expect_name(parser, "'*'나 field 이름이");

        if (field == NULL) {
            return;
        }
        if (!statement_add_field(statement, field)) {
            free(field);
            parser_fail(parser, SQL_ERROR_NO_MEMORY, "메모리가 부족합니다");
            return;
        }
        if (!parser_at_symbol(parser, ',')) {
            return;
        }
        parser_advance(parser);
    }
}

static bool statement_add_field(struct statement *statement, char *field)
{
    char **fields = realloc(statement->fields, (statement->field_count + 1) *
                                                   sizeof(*statement->fields));

    if (fields == NULL) {
        return false;
    }
    statement->fields = fields;
    statement->fields[statement->field_count++] = field;

    return true;
}
