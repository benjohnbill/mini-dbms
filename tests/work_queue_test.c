#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#include "acutest.h"
#include "work_queue.h"

/* 순서가 어긋나 기다림이 풀리지 않으면 테스트를 멈춰 실패로 끝냅니다. */
#define HANG_LIMIT_SECONDS 10
#define RAN_MAX 8

/* 테스트가 쥔 상태입니다. 작업 스레드의 진행은 시간이 아니라 이 mutex와
   조건 변수로 맞춥니다. 작업은 시작을 알리고 release가 참이 될 때까지
   멈춥니다. */
struct probe {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int started;
    bool release;
    int ran[RAN_MAX];
    int ran_count;
};

static void probe_init(struct probe *probe);
static void probe_destroy(struct probe *probe);
static void probe_wait_started(struct probe *probe, int target);
static void probe_release(struct probe *probe);
static void held_job_run(void *job, void *context);

static void full_queue_rejects_new_job_without_waiting_test(void)
{
    static int jobs[] = {1, 2, 3, 4};
    struct probe probe;
    struct work_queue *queue;

    alarm(HANG_LIMIT_SECONDS);
    probe_init(&probe);
    queue = work_queue_create(1, 2, held_job_run, &probe);
    TEST_ASSERT(queue != NULL);

    /* 작업 1을 스레드 하나가 쥐고 멈춘 동안 대기열 두 칸을 채웁니다. */
    TEST_CHECK(work_queue_submit(queue, &jobs[0]));
    probe_wait_started(&probe, 1);
    TEST_CHECK(work_queue_submit(queue, &jobs[1]));
    TEST_CHECK(work_queue_submit(queue, &jobs[2]));
    TEST_CHECK(!work_queue_submit(queue, &jobs[3]));

    probe_release(&probe);
    work_queue_destroy(queue);
    TEST_ASSERT(probe.ran_count == 3);
    TEST_CHECK(probe.ran[0] == 1);
    TEST_CHECK(probe.ran[1] == 2);
    TEST_CHECK(probe.ran[2] == 3);
    probe_destroy(&probe);
    alarm(0);
}

static void threads_run_jobs_at_the_same_time_test(void)
{
    static int jobs[] = {1, 2, 3};
    struct probe probe;
    struct work_queue *queue;

    alarm(HANG_LIMIT_SECONDS);
    probe_init(&probe);
    queue = work_queue_create(3, 1, held_job_run, &probe);
    TEST_ASSERT(queue != NULL);

    /* 세 작업이 모두 멈춘 채로 시작했다면 세 스레드가 함께 실행한 것입니다.
       아니면 이 테스트는 HANG_LIMIT_SECONDS 뒤에 멈춥니다. */
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(work_queue_submit(queue, &jobs[i]));
        probe_wait_started(&probe, i + 1);
    }
    probe_release(&probe);
    work_queue_destroy(queue);
    TEST_CHECK(probe.ran_count == 3);
    probe_destroy(&probe);
    alarm(0);
}

static void destroy_runs_queued_jobs_first_test(void)
{
    static int jobs[] = {1, 2, 3};
    struct probe probe;
    struct work_queue *queue;

    alarm(HANG_LIMIT_SECONDS);
    probe_init(&probe);
    probe.release = true;
    queue = work_queue_create(1, 3, held_job_run, &probe);
    TEST_ASSERT(queue != NULL);
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(work_queue_submit(queue, &jobs[i]));
    }
    work_queue_destroy(queue);
    TEST_CHECK(probe.ran_count == 3);
    probe_destroy(&probe);
    alarm(0);
}

static int discarded[RAN_MAX];
static int discarded_count;

static void job_discard(void *job)
{
    if (discarded_count < RAN_MAX) {
        discarded[discarded_count++] = *(int *)job;
    }
}

static void discard_drops_only_jobs_not_yet_started_test(void)
{
    static int jobs[] = {1, 2, 3};
    struct probe probe;
    struct work_queue *queue;

    alarm(HANG_LIMIT_SECONDS);
    probe_init(&probe);
    discarded_count = 0;
    queue = work_queue_create(1, 2, held_job_run, &probe);
    TEST_ASSERT(queue != NULL);

    /* 작업 1을 스레드가 쥐고 멈춘 동안 작업 2, 3은 대기열에만 있습니다. */
    TEST_CHECK(work_queue_submit(queue, &jobs[0]));
    probe_wait_started(&probe, 1);
    TEST_CHECK(work_queue_submit(queue, &jobs[1]));
    TEST_CHECK(work_queue_submit(queue, &jobs[2]));
    TEST_CHECK(work_queue_discard(queue, job_discard) == 2);
    TEST_ASSERT(discarded_count == 2);
    TEST_CHECK(discarded[0] == 2);
    TEST_CHECK(discarded[1] == 3);

    probe_release(&probe);
    work_queue_destroy(queue);
    TEST_ASSERT(probe.ran_count == 1);
    TEST_CHECK(probe.ran[0] == 1);
    probe_destroy(&probe);
    alarm(0);
}

static void zero_capacity_queue_rejects_every_job_test(void)
{
    static int job = 1;
    struct probe probe;
    struct work_queue *queue;

    probe_init(&probe);
    probe.release = true;
    queue = work_queue_create(1, 0, held_job_run, &probe);
    TEST_ASSERT(queue != NULL);
    TEST_CHECK(!work_queue_submit(queue, &job));
    work_queue_destroy(queue);
    TEST_CHECK(probe.ran_count == 0);
    probe_destroy(&probe);
}

TEST_LIST = {
    {"[T161] 스레드가 작업 하나를 쥐고 멈춘 동안 대기열 칸이 다 차면 새 "
     "작업은 기다리지 않고 거절되고, 받은 작업은 넣은 순서로 실행된다",
     full_queue_rejects_new_job_without_waiting_test},
    {"[T162] 스레드 세 개는 작업 세 개를 동시에 실행한다",
     threads_run_jobs_at_the_same_time_test},
    {"대기열을 해제하면 남은 작업을 모두 실행한 뒤 스레드가 끝난다",
     destroy_runs_queued_jobs_first_test},
    {"칸이 0인 대기열은 모든 작업을 거절하고 아무것도 실행하지 않는다",
     zero_capacity_queue_rejects_every_job_test},
    {"[T220] 대기열을 비우면 아직 시작하지 않은 작업만 넣은 순서로 버리고, "
     "실행 중인 작업은 끝까지 실행된다",
     discard_drops_only_jobs_not_yet_started_test},
    {NULL, NULL},
};

static void probe_init(struct probe *probe)
{
    *probe = (struct probe){0};
    pthread_mutex_init(&probe->mutex, NULL);
    pthread_cond_init(&probe->changed, NULL);
}

static void probe_destroy(struct probe *probe)
{
    pthread_cond_destroy(&probe->changed);
    pthread_mutex_destroy(&probe->mutex);
}

static void probe_wait_started(struct probe *probe, int target)
{
    pthread_mutex_lock(&probe->mutex);
    while (probe->started < target) {
        pthread_cond_wait(&probe->changed, &probe->mutex);
    }
    pthread_mutex_unlock(&probe->mutex);
}

static void probe_release(struct probe *probe)
{
    pthread_mutex_lock(&probe->mutex);
    probe->release = true;
    pthread_cond_broadcast(&probe->changed);
    pthread_mutex_unlock(&probe->mutex);
}

/* 작업 스레드에서 돕니다. 시작을 알리고 테스트가 풀어 줄 때까지 멈춘 뒤,
   실행한 작업 번호를 남깁니다. */
static void held_job_run(void *job, void *context)
{
    struct probe *probe = context;

    pthread_mutex_lock(&probe->mutex);
    probe->started++;
    pthread_cond_broadcast(&probe->changed);
    while (!probe->release) {
        pthread_cond_wait(&probe->changed, &probe->mutex);
    }
    if (probe->ran_count < RAN_MAX) {
        probe->ran[probe->ran_count++] = *(int *)job;
    }
    pthread_mutex_unlock(&probe->mutex);
}
