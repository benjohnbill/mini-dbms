/* INDEX 대 SCAN과 B+ 트리 노드 크기를 재는 측정 프로그램입니다. make bench가
   -O2, sanitizer 없이 엔진을 직접 링크해 만들고 scripts/bench.py가
   부릅니다.

   bench engine <data 폴더> <record 수>
       scripts/generate-users.py로 만든 users 테이블에서 같은 record를
       WHERE id = k(INDEX), WHERE email = '...'(SCAN), 강제 SCAN한
       WHERE id = k로 찾아 시간을 잽니다. 세 결과가 다르면 실패합니다.
   bench nodes <키 수>
       빌드 때 정한 BPTREE_NODE_KEYS로 ID 1부터 키 수까지를 오름차순으로 넣고 트리
       크기와 삽입, 찾기 시간을 잽니다. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bptree.h"
#include "engine.h"
#include "engine_bench.h"

#define INDEX_RUNS 1000
#define SCAN_RUNS 30
#define FIND_RUNS 1000
#define SQL_MAX 256
#define NANOS_PER_SECOND 1000000000.0
#define NANOS_PER_MILLI 1000000.0
#define PERCENT_99 0.99
#define FIND_SEED 20261116U /* 노드 크기마다 같은 키를 찾도록 고정합니다 */
#define XORSHIFT_LEFT_1 13
#define XORSHIFT_RIGHT 7
#define XORSHIFT_LEFT_2 17
#define ARGUMENT_COUNT 3
#define DECIMAL 10

/* 질의 세트의 한 줄입니다. 레코드 위치를 하나 더하려면 positions_fill에 한
   줄을 더합니다. number가 record 수보다 크면 없는 값입니다. */
struct bench_position {
    const char *label;
    int64_t number;
};

/* 한 질의를 runs번 잰 결과입니다. 시간은 ms입니다. */
struct bench_timing {
    double median;
    double p99;
    double min;
    double max;
};

enum bench_way {
    BENCH_WAY_ID,          /* WHERE id = k, planner가 INDEX를 고릅니다 */
    BENCH_WAY_EMAIL,       /* WHERE email = '...', planner가 SCAN을 고릅니다 */
    BENCH_WAY_FORCED_SCAN, /* WHERE id = k를 강제로 SCAN합니다 */
};

#define POSITION_COUNT 4
#define WAY_COUNT 3

static int records_bench_run(const char *data_dir, int64_t record_count);
static int nodes_bench_run(int64_t key_count);
static bool count_parse(const char *text, int64_t *count);
static double clock_now(void);
static void positions_fill(struct bench_position *positions,
                           int64_t record_count);
static void sql_format(char *sql, enum bench_way way, int64_t number);
static struct engine_result *query_run(struct engine *engine,
                                       enum bench_way way, const char *sql);
static int query_measure(struct engine *engine, enum bench_way way,
                         const char *sql, int runs, struct bench_timing *timing,
                         struct engine_result **last_result);
static void timing_compute(double *samples, int runs,
                           struct bench_timing *timing);
static int compare_doubles(const void *left, const void *right);
static bool results_are_same(const struct engine_result *left,
                             const struct engine_result *right);
static bool values_are_same(const struct value *left,
                            const struct value *right);
static uint64_t xorshift_next(uint64_t *state);

int main(int argc, char **argv)
{
    int64_t count;

    if (argc == ARGUMENT_COUNT + 1 && strcmp(argv[1], "engine") == 0 &&
        count_parse(argv[3], &count)) {
        return records_bench_run(argv[2], count) == 0 ? 0 : 1;
    }
    if (argc == ARGUMENT_COUNT && strcmp(argv[1], "nodes") == 0 &&
        count_parse(argv[2], &count)) {
        return nodes_bench_run(count) == 0 ? 0 : 1;
    }
    fprintf(stderr, "usage: bench engine <data dir> <records>\n"
                    "       bench nodes <keys>\n");

    return 2;
}

/* 엔진 준비 시간(테이블 파일을 읽어 다음 ID와 B+ 트리를 만드는 시간)은
   따로 적고, 질의 시간은 준비가 끝난 뒤부터 잽니다. */
static int records_bench_run(const char *data_dir, int64_t record_count)
{
    static const char *const way_names[WAY_COUNT] = {"id", "email",
                                                     "forced-scan"};
    static const int way_runs[WAY_COUNT] = {INDEX_RUNS, SCAN_RUNS, SCAN_RUNS};
    struct bench_position positions[POSITION_COUNT];
    char error[ENGINE_ERROR_MAX] = "";
    char sql[SQL_MAX];
    double started = clock_now();
    struct engine *engine = engine_create(data_dir, NULL, error, sizeof(error));
    int status = 0;

    if (engine == NULL) {
        fprintf(stderr, "engine_create: %s\n", error);
        return -1;
    }
    printf("engine ready: %.1f ms (records %lld, node keys %d)\n",
           (clock_now() - started) * 1000.0, (long long)record_count,
           BPTREE_NODE_KEYS);
    positions_fill(positions, record_count);
    /* 두 경로를 한 번씩 데웁니다. */
    for (int way = BENCH_WAY_ID; way <= BENCH_WAY_EMAIL; way++) {
        sql_format(sql, (enum bench_way)way, positions[0].number);
        engine_result_destroy(query_run(engine, (enum bench_way)way, sql));
    }
    printf("%-8s %-11s %-5s %5s %8s %10s %10s %10s %10s %s\n", "position",
           "way", "path", "runs", "records", "examined", "median_ms", "p99_ms",
           "min_ms", "max_ms");
    for (int i = 0; i < POSITION_COUNT && status == 0; i++) {
        struct engine_result *results[WAY_COUNT] = {NULL, NULL, NULL};

        for (int way = 0; way < WAY_COUNT && status == 0; way++) {
            struct bench_timing timing;
            struct engine_result *result;

            sql_format(sql, (enum bench_way)way, positions[i].number);
            status = query_measure(engine, (enum bench_way)way, sql,
                                   way_runs[way], &timing, &results[way]);
            if (status != 0) {
                fprintf(stderr, "%s: query failed\n", sql);
                break;
            }
            result = results[way];
            printf("%-8s %-11s %-5s %5d %8zu %10llu %10.4f %10.4f %10.4f "
                   "%10.4f\n",
                   positions[i].label, way_names[way],
                   result->access_path == ENGINE_ACCESS_PATH_INDEX ? "INDEX"
                                                                   : "SCAN",
                   way_runs[way], result->record_count,
                   (unsigned long long)result->records_examined, timing.median,
                   timing.p99, timing.min, timing.max);
        }
        if (status == 0 && (!results_are_same(results[0], results[1]) ||
                            !results_are_same(results[0], results[2]))) {
            fprintf(stderr, "%s: INDEX and SCAN results differ\n",
                    positions[i].label);
            status = -1;
        }
        for (int way = 0; way < WAY_COUNT; way++) {
            engine_result_destroy(results[way]);
        }
    }
    engine_destroy(engine);
    if (status == 0) {
        printf("all pairs: INDEX and SCAN found the same record\n");
    }

    return status;
}

/* 오름차순 삽입은 왼쪽 리프를 반만 채우므로 이 순서가 엔진의 실제
   모양입니다. 찾기는 1부터 key_count까지의 무작위 키 FIND_RUNS개입니다. */
static int nodes_bench_run(int64_t key_count)
{
    struct bptree *tree = bptree_create();
    double samples[FIND_RUNS];
    struct bptree_stats stats;
    struct bench_timing timing;
    uint64_t state = FIND_SEED;
    double started;
    double insert_seconds;

    if (tree == NULL || key_count < 1) {
        bptree_destroy(tree);
        return -1;
    }
    started = clock_now();
    for (int64_t key = 1; key <= key_count; key++) {
        if (bptree_reserve(tree) != 0) {
            bptree_destroy(tree);
            return -1;
        }
        bptree_insert(tree, key, (uint64_t)key);
    }
    insert_seconds = clock_now() - started;
    for (int i = 0; i < FIND_RUNS; i++) {
        int64_t key =
            (int64_t)(xorshift_next(&state) % (uint64_t)key_count) + 1;
        uint64_t value = 0;
        double find_started = clock_now();
        bool found = bptree_find(tree, key, &value);

        samples[i] = (clock_now() - find_started) * NANOS_PER_SECOND;
        if (!found || value != (uint64_t)key) {
            bptree_destroy(tree);
            return -1;
        }
    }
    timing_compute(samples, FIND_RUNS, &timing);
    bptree_get_stats(tree, &stats);
    bptree_destroy(tree);
    printf("node keys %3d: height %zu, nodes %zu, memory %.1f MiB, "
           "insert %.1f ms, find median %.0f ns, p99 %.0f ns\n",
           BPTREE_NODE_KEYS, stats.height, stats.node_count,
           (double)stats.node_bytes / (1024.0 * 1024.0),
           insert_seconds * 1000.0, timing.median, timing.p99);

    return 0;
}

static bool count_parse(const char *text, int64_t *count)
{
    char *end = NULL;
    long long parsed = strtoll(text, &end, DECIMAL);

    if (end == text || *end != '\0' || parsed < 1) {
        return false;
    }
    *count = parsed;
    return true;
}

/* 초 단위입니다. CLOCK_MONOTONIC은 POSIX라 macOS와 Linux에서 같습니다. */
static double clock_now(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / NANOS_PER_SECOND;
}

/* 앞, 중간, 뒤는 테이블 파일 안의 위치이고, SCAN 시간은 위치와 상관없이
   파일 전체를 읽습니다. 없는 값은 INDEX가 record를 읽지 않는 경우입니다. */
static void positions_fill(struct bench_position *positions,
                           int64_t record_count)
{
    positions[0] = (struct bench_position){"front", 1};
    positions[1] = (struct bench_position){"middle", (record_count + 1) / 2};
    positions[2] = (struct bench_position){"back", record_count};
    positions[3] = (struct bench_position){"missing", record_count + 1};
}

/* email 값은 scripts/generate-users.py의 번호 규칙과 같습니다. */
static void sql_format(char *sql, enum bench_way way, int64_t number)
{
    if (way == BENCH_WAY_EMAIL) {
        snprintf(sql, SQL_MAX,
                 "SELECT * FROM users WHERE email = "
                 "'user%07lld@example.com';",
                 (long long)number);
        return;
    }
    snprintf(sql, SQL_MAX, "SELECT * FROM users WHERE id = %lld;",
             (long long)number);
}

static struct engine_result *query_run(struct engine *engine,
                                       enum bench_way way, const char *sql)
{
    struct engine_result *result;

    engine_bench_force_scan = way == BENCH_WAY_FORCED_SCAN;
    result = engine_execute(engine, sql, NULL);
    engine_bench_force_scan = false;
    return result;
}

/* 문장 하나를 넘겨 결과 구조체를 받기까지를 runs번 잽니다. 결과 해제는
   시간에 넣지 않습니다. 마지막 결과는 비교에 쓰도록 *last_result에
   남깁니다. */
static int query_measure(struct engine *engine, enum bench_way way,
                         const char *sql, int runs, struct bench_timing *timing,
                         struct engine_result **last_result)
{
    double *samples = calloc((size_t)runs, sizeof(*samples));

    if (samples == NULL) {
        return -1;
    }
    for (int i = 0; i < runs; i++) {
        double started = clock_now();
        struct engine_result *result = query_run(engine, way, sql);

        samples[i] = (clock_now() - started) * 1000.0;
        if (result == NULL || result->status != ENGINE_OK) {
            engine_result_destroy(result);
            free(samples);
            return -1;
        }
        if (i == runs - 1) {
            *last_result = result;
        } else {
            engine_result_destroy(result);
        }
    }
    timing_compute(samples, runs, timing);
    free(samples);

    return 0;
}

/* 중앙값은 가운데 두 값의 평균이고, p99는 정렬한 표본에서 99%째 자리의
   값입니다(nearest-rank). samples는 정렬됩니다. */
static void timing_compute(double *samples, int runs,
                           struct bench_timing *timing)
{
    int rank = (int)((double)runs * PERCENT_99 + 0.999999);

    qsort(samples, (size_t)runs, sizeof(*samples), compare_doubles);
    timing->median = runs % 2 == 1
                         ? samples[runs / 2]
                         : (samples[runs / 2 - 1] + samples[runs / 2]) / 2;
    timing->p99 = samples[rank - 1];
    timing->min = samples[0];
    timing->max = samples[runs - 1];
}

static int compare_doubles(const void *left, const void *right)
{
    double left_value = *(const double *)left;
    double right_value = *(const double *)right;

    return (left_value > right_value) - (left_value < right_value);
}

static bool results_are_same(const struct engine_result *left,
                             const struct engine_result *right)
{
    size_t value_count = left->record_count * left->field_count;

    if (left->record_count != right->record_count ||
        left->field_count != right->field_count) {
        return false;
    }
    for (size_t i = 0; i < value_count; i++) {
        if (!values_are_same(&left->values[i], &right->values[i])) {
            return false;
        }
    }
    return true;
}

static bool values_are_same(const struct value *left, const struct value *right)
{
    if (left->type != right->type) {
        return false;
    }
    if (left->type == VALUE_INT) {
        return left->integer == right->integer;
    }
    return left->text_len == right->text_len &&
           memcmp(left->text, right->text, left->text_len) == 0;
}

/* 플랫폼마다 rand()가 달라도 같은 키를 찾도록 직접 만든 xorshift64입니다. */
static uint64_t xorshift_next(uint64_t *state)
{
    *state ^= *state << XORSHIFT_LEFT_1;
    *state ^= *state >> XORSHIFT_RIGHT;
    *state ^= *state << XORSHIFT_LEFT_2;
    return *state;
}
