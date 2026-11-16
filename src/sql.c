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
   TOKEN_WORD의 text는 SQL 문자열 안을 가리킵니다. */
struct token {
    enum token_kind kind;
    const char *start;
    size_t length;
    int64_t integer;
    char *text;
    char symbol;
};

struct parser {
    const char *cursor;
    struct token current;
    char *error;
    size_t error_size;
    bool failed;
};

static void parser_fail(struct parser *parser, const char *format, ...);
static void parser_advance(struct parser *parser);
static void token_read_integer(struct parser *parser);
static void token_read_string(struct parser *parser);
static bool parser_accept_keyword(struct parser *parser, const char *keyword);
static void parser_expect_keyword(struct parser *parser, const char *keyword);
static void parser_expect_symbol(struct parser *parser, char symbol);
static char *parser_expect_name(struct parser *parser);
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
                          const char **next_sql, char *error, size_t error_size)
{
    struct parser parser = {
        .cursor = sql, .error = error, .error_size = error_size};
    struct statement *statement;

    parser_advance(&parser);
    if (parser.current.kind == TOKEN_END && !parser.failed) {
        *next_sql = parser.cursor;
        return SQL_EMPTY;
    }
    statement = calloc(1, sizeof(*statement));
    if (statement == NULL) {
        parser_fail(&parser, "out of memory");
        return SQL_ERROR;
    }
    if (parser_accept_keyword(&parser, "INSERT")) {
        parser_parse_insert(&parser, statement);
    } else if (parser_accept_keyword(&parser, "SELECT")) {
        parser_parse_select(&parser, statement);
    } else {
        parser_fail(&parser, "expected INSERT or SELECT");
    }
    /* ; 다음 토큰은 읽지 않습니다. 다음 문장의 오류가 이 문장의 오류가
       되지 않게 하기 위해서입니다. */
    if (parser.current.kind != TOKEN_SYMBOL || parser.current.symbol != ';') {
        parser_fail(&parser, "expected ';'");
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

/* 첫 오류만 남깁니다. 오류 뒤의 토큰은 모두 끝으로 봅니다. */
static void parser_fail(struct parser *parser, const char *format, ...)
{
    va_list args;

    if (parser->failed) {
        return;
    }
    parser->failed = true;
    va_start(args, format);
    vsnprintf(parser->error, parser->error_size, format, args);
    va_end(args);
}

static void parser_advance(struct parser *parser)
{
    const char *cursor = parser->cursor;

    free(parser->current.text);
    memset(&parser->current, 0, sizeof(parser->current));
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    parser->current.start = cursor;
    parser->cursor = cursor;
    if (parser->failed || *cursor == '\0') {
        parser->current.kind = TOKEN_END;
    } else if (isalpha((unsigned char)*cursor) || *cursor == '_') {
        while (isalnum((unsigned char)*cursor) || *cursor == '_') {
            cursor++;
        }
        parser->current.kind = TOKEN_WORD;
        parser->current.length = (size_t)(cursor - parser->current.start);
        parser->cursor = cursor;
    } else if (isdigit((unsigned char)*cursor) || *cursor == '-') {
        token_read_integer(parser);
    } else if (*cursor == '\'') {
        token_read_string(parser);
    } else {
        parser->current.kind = TOKEN_SYMBOL;
        parser->current.symbol = *cursor;
        parser->current.length = 1;
        parser->cursor = cursor + 1;
    }
}

static void token_read_integer(struct parser *parser)
{
    char *end;
    long long integer;

    errno = 0;
    integer = strtoll(parser->cursor, &end, 10);
    if (end == parser->cursor || errno == ERANGE) {
        parser_fail(parser, "invalid integer");
        parser->current.kind = TOKEN_END;
        return;
    }
    parser->current.kind = TOKEN_INTEGER;
    parser->current.integer = integer;
    parser->current.length = (size_t)(end - parser->cursor);
    parser->cursor = end;
}

/* '...' 안의 ''는 작은따옴표 하나입니다. */
static void token_read_string(struct parser *parser)
{
    const char *cursor = parser->cursor + 1;
    size_t text_len = 0;
    char *text = malloc(strlen(cursor) + 1);

    if (text == NULL) {
        parser_fail(parser, "out of memory");
        parser->current.kind = TOKEN_END;
        return;
    }
    for (;;) {
        if (*cursor == '\0') {
            free(text);
            parser_fail(parser, "unterminated string");
            parser->current.kind = TOKEN_END;
            return;
        }
        if (*cursor == '\'' && cursor[1] != '\'') {
            break;
        }
        if (*cursor == '\'') {
            cursor++;
        }
        if (text_len == TEXT_MAX_BYTES) {
            free(text);
            parser_fail(parser, "string is longer than 1 MiB");
            parser->current.kind = TOKEN_END;
            return;
        }
        text[text_len++] = *cursor++;
    }
    text[text_len] = '\0';
    parser->current.kind = TOKEN_STRING;
    parser->current.text = text;
    parser->current.length = text_len;
    parser->cursor = cursor + 1;
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

static void parser_expect_keyword(struct parser *parser, const char *keyword)
{
    if (!parser_accept_keyword(parser, keyword)) {
        parser_fail(parser, "expected %s", keyword);
    }
}

static void parser_expect_symbol(struct parser *parser, char symbol)
{
    if (parser->current.kind != TOKEN_SYMBOL ||
        parser->current.symbol != symbol) {
        parser_fail(parser, "expected '%c'", symbol);
        return;
    }
    parser_advance(parser);
}

static char *parser_expect_name(struct parser *parser)
{
    char *name;

    if (parser->current.kind != TOKEN_WORD) {
        parser_fail(parser, "expected a table name");
        return NULL;
    }
    name = strndup(parser->current.start, parser->current.length);
    if (name == NULL) {
        parser_fail(parser, "out of memory");
    }
    parser_advance(parser);

    return name;
}

/* INSERT INTO 테이블 VALUES (값, …) */
static void parser_parse_insert(struct parser *parser,
                                struct statement *statement)
{
    statement->kind = STATEMENT_INSERT;
    parser_expect_keyword(parser, "INTO");
    statement->table = parser_expect_name(parser);
    parser_expect_keyword(parser, "VALUES");
    parser_expect_symbol(parser, '(');
    parser_parse_values(parser, statement);
    parser_expect_symbol(parser, ')');
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
            parser_fail(parser, "out of memory");
            return;
        }
        if (parser->current.kind != TOKEN_SYMBOL ||
            parser->current.symbol != ',') {
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
        parser_fail(parser, "expected an integer or a string");
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
    if (parser->current.kind == TOKEN_SYMBOL && parser->current.symbol == '*') {
        parser_advance(parser);
    } else {
        parser_parse_fields(parser, statement);
    }
    parser_expect_keyword(parser, "FROM");
    statement->table = parser_expect_name(parser);
    if (!parser_accept_keyword(parser, "WHERE")) {
        return;
    }
    statement->where_field = parser_expect_name(parser);
    parser_expect_symbol(parser, '=');
    parser_take_value(parser, &statement->where_value);
}

static void parser_parse_fields(struct parser *parser,
                                struct statement *statement)
{
    for (;;) {
        char *field = parser_expect_name(parser);

        if (field == NULL) {
            return;
        }
        if (!statement_add_field(statement, field)) {
            free(field);
            parser_fail(parser, "out of memory");
            return;
        }
        if (parser->current.kind != TOKEN_SYMBOL ||
            parser->current.symbol != ',') {
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
