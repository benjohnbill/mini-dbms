#include "table.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 테이블 파일의 레이아웃입니다. record = 본문 길이(u32) + 본문,
   본문 = id(i64) + 스키마 순서의 field 값. INT는 i64, TEXT는 길이(u32) +
   UTF-8 바이트입니다. 모든 정수는 리틀 엔디언입니다. */
#define LENGTH_BYTES 4
#define INTEGER_BYTES 8
#define PATH_BYTES 1024
/* TEXT 값 하나의 상한입니다. src/sql.c의 TEXT_MAX_BYTES와 같은 값입니다. */
#define TEXT_LIMIT_BYTES ((uint64_t)1 << 20)

/* SCAN iterator입니다. iterator가 첫 칸이라 struct record_iterator *를 이
   구조체로 바꿔 쓸 수 있습니다. */
struct table_scan {
    struct record_iterator iterator;
    const struct table *table;
    uint64_t offset;
    char *body;
    size_t body_capacity;
    struct value *values;
};

/* 시작할 때 읽기에서 찾은 깨진 record입니다. kind가 NULL이면 없습니다.
   offset은 0부터 센 바이트 위치, record_number는 1부터 센 순서입니다. */
struct damage {
    const char *kind; /* "잘림" 또는 "손상" */
    const char *reason;
    uint64_t offset;
    uint64_t record_number;
};

static void error_set(char *error, size_t error_size, const char *format, ...);
static int schema_load(struct table *table, const char *path, char *error,
                       size_t error_size);
static int schema_add_field(struct table *table, const char *line,
                            int line_number, char *error, size_t error_size);
static int schema_type_parse(const char *type_name, enum value_type *type);
static bool schema_has_field(const struct table *table, const char *name);
static int table_records_load(struct table *table, const char *path,
                              char *error, size_t error_size);
static int records_check(const struct table *table, struct damage *damage,
                         int64_t *last_id);
static void damage_set(struct damage *damage, const char *kind,
                       const char *reason);
static void body_len_range(const struct table *table, uint64_t *min_body,
                           uint64_t *max_body);
static int body_fields_size(const struct table *table,
                            const unsigned char *body, uint64_t available,
                            uint64_t *used);
static size_t record_size(const struct table *table,
                          const struct value *values);
static void record_encode(const struct table *table, int64_t new_id,
                          const struct value *values, unsigned char *bytes);
static unsigned char *put_u32(unsigned char *bytes, uint32_t number);
static unsigned char *put_i64(unsigned char *bytes, int64_t number);
static int write_full(int fd, const unsigned char *bytes, size_t size,
                      uint64_t offset);
static int scan_next(struct record_iterator *iterator, struct record *record);
static void scan_destroy(struct record_iterator *iterator);
static int read_full(int fd, void *bytes, size_t size, uint64_t offset);
static int scan_decode_body(struct table_scan *scan, uint32_t body_len,
                            struct record *record);
static uint32_t get_u32(const unsigned char *bytes);
static int64_t get_i64(const unsigned char *bytes);

struct table *table_create(const char *data_dir, const char *name, char *error,
                           size_t error_size)
{
    char path[PATH_BYTES];
    struct table *table = calloc(1, sizeof(*table));
    struct stat file_stat;

    if (table == NULL) {
        error_set(error, error_size, "out of memory");
        return NULL;
    }
    table->fd = -1;
    table->name = strdup(name);
    snprintf(path, sizeof(path), "%s/%s.schema", data_dir, name);
    if (table->name == NULL ||
        schema_load(table, path, error, error_size) != 0) {
        table_destroy(table);
        return NULL;
    }
    /* O_APPEND를 쓰지 않습니다. Linux에서는 O_APPEND로 연 파일에 pwrite하면
       위치 인자를 무시하고 파일 끝에 쓰기 때문입니다. */
    snprintf(path, sizeof(path), "%s/%s.tbl", data_dir, name);
    table->fd = open(path, O_RDWR);
    if (table->fd < 0 || fstat(table->fd, &file_stat) != 0) {
        error_set(error, error_size, "%s: %s", path, strerror(errno));
        table_destroy(table);
        return NULL;
    }
    table->committed_size = (uint64_t)file_stat.st_size;
    if (table_records_load(table, path, error, error_size) != 0) {
        table_destroy(table);
        return NULL;
    }

    return table;
}

void table_destroy(struct table *table)
{
    if (table == NULL) {
        return;
    }
    if (table->fd >= 0) {
        close(table->fd);
    }
    for (size_t i = 0; i < table->field_count; i++) {
        free(table->fields[i].name);
    }
    free(table->fields);
    free(table->name);
    free(table);
}

int table_insert(struct table *table, const struct value *values,
                 int64_t *inserted_id, char *error, size_t error_size)
{
    size_t size = record_size(table, values);
    unsigned char *bytes;
    int64_t new_id;

    if (table->last_id == INT64_MAX) {
        error_set(error, error_size, "%s: id reached the INT maximum",
                  table->name);
        return -1;
    }
    new_id = table->last_id + 1;
    bytes = malloc(size);
    if (bytes == NULL) {
        error_set(error, error_size, "out of memory");
        return -1;
    }
    record_encode(table, new_id, values, bytes);
    if (write_full(table->fd, bytes, size, table->committed_size) != 0) {
        error_set(error, error_size, "%s.tbl: %s", table->name,
                  strerror(errno));
        free(bytes);
        return -1;
    }
    free(bytes);
    /* 쓰기가 모두 끝난 뒤에만 파일 끝 위치와 ID를 올립니다. */
    table->committed_size += size;
    table->last_id = new_id;
    *inserted_id = new_id;

    return 0;
}

struct record_iterator *table_scan_create(struct table *table)
{
    struct table_scan *scan = calloc(1, sizeof(*scan));

    if (scan == NULL) {
        return NULL;
    }
    scan->values = calloc(table->field_count + 1, sizeof(*scan->values));
    if (scan->values == NULL) {
        free(scan);
        return NULL;
    }
    scan->iterator.next = scan_next;
    scan->iterator.destroy = scan_destroy;
    scan->table = table;

    return &scan->iterator;
}

static void error_set(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

/* 한 줄에 `name TEXT`처럼 field 하나입니다. 빈 줄은 건너뜁니다. */
static int schema_load(struct table *table, const char *path, char *error,
                       size_t error_size)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t line_capacity = 0;
    int line_number = 0;
    int status = 0;

    if (file == NULL) {
        error_set(error, error_size, "%s: %s", path, strerror(errno));
        return -1;
    }
    while (status == 0 && getline(&line, &line_capacity, file) != -1) {
        line_number++;
        status = schema_add_field(table, line, line_number, error, error_size);
    }
    free(line);
    fclose(file);
    if (status == 0 && table->field_count == 0) {
        error_set(error, error_size, "%s: no fields", path);
        status = -1;
    }

    return status;
}

/* 한 줄은 `이름 타입` 두 낱말입니다. id는 엔진이 붙이므로 스키마에 적을 수
   없습니다. */
static int schema_add_field(struct table *table, const char *line,
                            int line_number, char *error, size_t error_size)
{
    char name[PATH_BYTES];
    char type_name[16];
    char extra[2];
    int matched = sscanf(line, "%1023s %15s %1s", name, type_name, extra);
    struct schema_field *fields;
    enum value_type type;

    if (matched == EOF) {
        return 0;
    }
    if (matched != 2) {
        error_set(error, error_size, "%s.schema: line %d: expected `name TYPE`",
                  table->name, line_number);
        return -1;
    }
    if (schema_type_parse(type_name, &type) != 0) {
        error_set(error, error_size, "%s.schema: line %d: unsupported type %s",
                  table->name, line_number, type_name);
        return -1;
    }
    if (strcmp(name, "id") == 0) {
        error_set(error, error_size,
                  "%s.schema: line %d: id is added by the engine", table->name,
                  line_number);
        return -1;
    }
    if (schema_has_field(table, name)) {
        error_set(error, error_size, "%s.schema: line %d: duplicate field %s",
                  table->name, line_number, name);
        return -1;
    }
    fields = realloc(table->fields,
                     (table->field_count + 1) * sizeof(*table->fields));
    if (fields == NULL) {
        error_set(error, error_size, "out of memory");
        return -1;
    }
    table->fields = fields;
    fields[table->field_count].name = strdup(name);
    fields[table->field_count].type = type;
    if (fields[table->field_count].name == NULL) {
        error_set(error, error_size, "out of memory");
        return -1;
    }
    table->field_count++;

    return 0;
}

static int schema_type_parse(const char *type_name, enum value_type *type)
{
    if (strcmp(type_name, "INT") == 0) {
        *type = VALUE_INT;
    } else if (strcmp(type_name, "TEXT") == 0) {
        *type = VALUE_TEXT;
    } else {
        return -1;
    }
    return 0;
}

static bool schema_has_field(const struct table *table, const char *name)
{
    for (size_t i = 0; i < table->field_count; i++) {
        if (strcmp(table->fields[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* 시작할 때 테이블 파일을 처음부터 끝까지 한 번 읽어 가장 큰 ID를 찾고,
   깨진 record가 있으면 잘림인지 손상인지 알리고 멈춥니다. 파일은 고치지
   않습니다. 주석의 규칙 번호는 docs/prd/issues/04-detect-damaged-table-file.md
   § 판정 규칙의 번호입니다. */
static int table_records_load(struct table *table, const char *path,
                              char *error, size_t error_size)
{
    struct damage damage = {0};
    int64_t last_id = 0;
    int status = records_check(table, &damage, &last_id);

    if (status != 0) {
        error_set(error, error_size, "%s: %s", path,
                  errno != 0 ? strerror(errno) : "cannot read records");
        return -1;
    }
    if (damage.kind != NULL) {
        error_set(error, error_size,
                  "%s: %" PRIu64 "번째 바이트의 record(%" PRIu64
                  "번째)가 깨졌습니다: %s (%s)",
                  path, damage.offset, damage.record_number, damage.kind,
                  damage.reason);
        return -1;
    }
    table->last_id = last_id;

    return 0;
}

/* committed_size까지 record를 차례로 확인합니다. 깨진 record를 찾으면
   *damage를 채우고 0을 돌려줍니다. 읽기 오류와 메모리 부족은 -1입니다. */
static int records_check(const struct table *table, struct damage *damage,
                         int64_t *last_id)
{
    uint64_t offset = 0;
    uint64_t record_number = 0;
    uint64_t min_body;
    uint64_t max_body;
    unsigned char *body = NULL;
    int status = 0;

    body_len_range(table, &min_body, &max_body);
    errno = 0;
    /* 규칙 1: 읽는 위치가 파일 크기에 닿으면 끝입니다. */
    while (offset < table->committed_size && damage->kind == NULL) {
        uint64_t remaining = table->committed_size - offset;
        unsigned char header[LENGTH_BYTES];
        uint64_t body_len;
        uint64_t available;
        uint64_t used = 0;
        bool fields_fit;
        unsigned char *grown;

        record_number++;
        damage->offset = offset;
        damage->record_number = record_number;
        if (remaining < LENGTH_BYTES) { /* 규칙 2 */
            damage_set(damage, "잘림", "길이 칸 중간에서 파일이 끝남");
            break;
        }
        if (read_full(table->fd, header, LENGTH_BYTES, offset) != 0) {
            status = -1;
            break;
        }
        body_len = get_u32(header);
        if (body_len < min_body || body_len > max_body) { /* 규칙 3 */
            damage_set(damage, "손상", "본문 길이가 스키마로 가능한 범위 밖임");
            break;
        }
        /* 규칙 3을 지났으므로 버퍼는 max_body를 넘지 않습니다. */
        available = remaining - LENGTH_BYTES;
        if (available > body_len) {
            available = body_len;
        }
        grown = realloc(body, available > 0 ? (size_t)available : 1);
        if (grown == NULL) {
            status = -1;
            break;
        }
        body = grown;
        if (read_full(table->fd, body, (size_t)available,
                      offset + LENGTH_BYTES) != 0) {
            status = -1;
            break;
        }
        fields_fit = body_fields_size(table, body, available, &used) == 0;
        if (available < body_len) { /* 규칙 4 */
            if (fields_fit) {
                damage_set(damage, "손상",
                           "길이 칸이 파일 끝을 넘는데 field는 다 읽힘");
            } else {
                damage_set(damage, "잘림", "record 중간에서 파일이 끝남");
            }
        } else if (!fields_fit || used != body_len) { /* 규칙 5 */
            damage_set(damage, "손상", "field가 본문 길이와 맞지 않음");
        } else if (get_i64(body) <= *last_id) { /* 규칙 6 */
            damage_set(damage, "손상", "id가 앞 record의 id보다 크지 않음");
        } else {
            *last_id = get_i64(body);
            offset += LENGTH_BYTES + body_len; /* 규칙 7 */
        }
    }
    free(body);

    return status;
}

static void damage_set(struct damage *damage, const char *kind,
                       const char *reason)
{
    damage->kind = kind;
    damage->reason = reason;
}

/* 본문 길이로 가능한 범위입니다. min_body는 모든 TEXT가 빈 문자열일 때,
   max_body는 모든 TEXT가 최대 길이일 때입니다. */
static void body_len_range(const struct table *table, uint64_t *min_body,
                           uint64_t *max_body)
{
    uint64_t text_count = 0;

    *min_body = INTEGER_BYTES;
    for (size_t i = 0; i < table->field_count; i++) {
        if (table->fields[i].type == VALUE_INT) {
            *min_body += INTEGER_BYTES;
        } else {
            *min_body += LENGTH_BYTES;
            text_count++;
        }
    }
    *max_body = *min_body + TEXT_LIMIT_BYTES * text_count;
}

/* body의 앞 available 바이트 안에서 id와 스키마 순서의 field를 읽어 봅니다.
   모두 읽히면 0이고 쓴 바이트 수를 *used에 씁니다. 모자라면 -1입니다. */
static int body_fields_size(const struct table *table,
                            const unsigned char *body, uint64_t available,
                            uint64_t *used)
{
    uint64_t cursor = INTEGER_BYTES;

    if (available < cursor) {
        return -1;
    }
    for (size_t i = 0; i < table->field_count; i++) {
        if (table->fields[i].type == VALUE_INT) {
            cursor += INTEGER_BYTES;
        } else if (available - cursor >= LENGTH_BYTES) {
            cursor += LENGTH_BYTES + get_u32(body + cursor);
        } else {
            return -1;
        }
        if (cursor > available) {
            return -1;
        }
    }
    *used = cursor;

    return 0;
}

static size_t record_size(const struct table *table, const struct value *values)
{
    size_t size = LENGTH_BYTES + INTEGER_BYTES;

    for (size_t i = 0; i < table->field_count; i++) {
        if (values[i].type == VALUE_INT) {
            size += INTEGER_BYTES;
        } else {
            size += LENGTH_BYTES + values[i].text_len;
        }
    }

    return size;
}

static void record_encode(const struct table *table, int64_t new_id,
                          const struct value *values, unsigned char *bytes)
{
    size_t body_len = record_size(table, values) - LENGTH_BYTES;
    unsigned char *cursor = put_u32(bytes, (uint32_t)body_len);

    cursor = put_i64(cursor, new_id);
    for (size_t i = 0; i < table->field_count; i++) {
        if (values[i].type == VALUE_INT) {
            cursor = put_i64(cursor, values[i].integer);
        } else {
            cursor = put_u32(cursor, values[i].text_len);
            memcpy(cursor, values[i].text, values[i].text_len);
            cursor += values[i].text_len;
        }
    }
}

/* CPU의 바이트 순서와 상관없이 리틀 엔디언으로 씁니다. */
static unsigned char *put_u32(unsigned char *bytes, uint32_t number)
{
    for (int i = 0; i < LENGTH_BYTES; i++) {
        bytes[i] = (unsigned char)(number >> (8 * i));
    }
    return bytes + LENGTH_BYTES;
}

static unsigned char *put_i64(unsigned char *bytes, int64_t number)
{
    uint64_t bits = (uint64_t)number;

    for (int i = 0; i < INTEGER_BYTES; i++) {
        bytes[i] = (unsigned char)(bits >> (8 * i));
    }
    return bytes + INTEGER_BYTES;
}

/* pwrite는 요청보다 적게 쓸 수 있으므로 다 쓸 때까지 이어 씁니다. */
static int write_full(int fd, const unsigned char *bytes, size_t size,
                      uint64_t offset)
{
    while (size > 0) {
        ssize_t written = pwrite(fd, bytes, size, (off_t)offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return -1;
        }
        bytes += written;
        size -= (size_t)written;
        offset += (uint64_t)written;
    }
    return 0;
}

static int scan_next(struct record_iterator *iterator, struct record *record)
{
    struct table_scan *scan = (struct table_scan *)iterator;
    unsigned char header[LENGTH_BYTES];
    uint32_t body_len;

    if (scan->offset >= scan->table->committed_size) {
        return 0;
    }
    if (read_full(scan->table->fd, header, LENGTH_BYTES, scan->offset) != 0) {
        return -1;
    }
    body_len = get_u32(header);
    if (body_len > scan->body_capacity) {
        char *body = realloc(scan->body, body_len);

        if (body == NULL) {
            return -1;
        }
        scan->body = body;
        scan->body_capacity = body_len;
    }
    if (read_full(scan->table->fd, scan->body, body_len,
                  scan->offset + LENGTH_BYTES) != 0 ||
        scan_decode_body(scan, body_len, record) != 0) {
        return -1;
    }
    scan->offset += LENGTH_BYTES + (uint64_t)body_len;

    return 1;
}

static void scan_destroy(struct record_iterator *iterator)
{
    struct table_scan *scan = (struct table_scan *)iterator;

    free(scan->body);
    free(scan->values);
    free(scan);
}

static int read_full(int fd, void *bytes, size_t size, uint64_t offset)
{
    unsigned char *cursor = bytes;

    while (size > 0) {
        ssize_t read_len = pread(fd, cursor, size, (off_t)offset);

        if (read_len < 0 && errno == EINTR) {
            continue;
        }
        if (read_len <= 0) {
            return -1;
        }
        cursor += read_len;
        size -= (size_t)read_len;
        offset += (uint64_t)read_len;
    }
    return 0;
}

/* 본문이 스키마와 맞지 않으면 -1입니다. 자세한 판정은 시작할 때 읽기가
   맡습니다(티켓 04). */
static int scan_decode_body(struct table_scan *scan, uint32_t body_len,
                            struct record *record)
{
    const unsigned char *cursor = (const unsigned char *)scan->body;
    const unsigned char *end = cursor + body_len;

    if (body_len < INTEGER_BYTES) {
        return -1;
    }
    record->id = get_i64(cursor);
    cursor += INTEGER_BYTES;
    for (size_t i = 0; i < scan->table->field_count; i++) {
        struct value *value = &scan->values[i];

        value->type = scan->table->fields[i].type;
        if (value->type == VALUE_INT) {
            if (end - cursor < INTEGER_BYTES) {
                return -1;
            }
            value->integer = get_i64(cursor);
            cursor += INTEGER_BYTES;
            continue;
        }
        if (end - cursor < LENGTH_BYTES) {
            return -1;
        }
        value->text_len = get_u32(cursor);
        cursor += LENGTH_BYTES;
        if ((size_t)(end - cursor) < value->text_len) {
            return -1;
        }
        value->text = (const char *)cursor;
        cursor += value->text_len;
    }
    record->values = scan->values;

    return cursor == end ? 0 : -1;
}

static uint32_t get_u32(const unsigned char *bytes)
{
    uint32_t number = 0;

    for (int i = 0; i < LENGTH_BYTES; i++) {
        number |= (uint32_t)bytes[i] << (8 * i);
    }
    return number;
}

static int64_t get_i64(const unsigned char *bytes)
{
    uint64_t bits = 0;

    for (int i = 0; i < INTEGER_BYTES; i++) {
        bits |= (uint64_t)bytes[i] << (8 * i);
    }
    return (int64_t)bits;
}
