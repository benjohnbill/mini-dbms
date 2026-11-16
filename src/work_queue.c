#include "work_queue.h"

#include <pthread.h>
#include <stdlib.h>

/* jobs는 칸 capacity개의 원형 배열입니다. 맨 앞 작업은 jobs[head]이고
   job_count개가 이어집니다. 모든 칸은 mutex를 쥐고 읽고 씁니다. */
struct work_queue {
    pthread_mutex_t mutex;
    pthread_cond_t job_ready;
    void **jobs;
    size_t capacity;
    size_t head;
    size_t job_count;
    bool stopping;
    void (*handler)(void *job, void *context);
    void *context;
    pthread_t *threads;
    size_t thread_count;
};

static void *worker_main(void *arg);
static void *queue_take(struct work_queue *queue);
static void queue_stop_threads(struct work_queue *queue);

struct work_queue *work_queue_create(size_t thread_count, size_t capacity,
                                     void (*handler)(void *job, void *context),
                                     void *context)
{
    struct work_queue *queue = calloc(1, sizeof(*queue));

    if (queue == NULL) {
        return NULL;
    }
    /* 칸이 0인 대기열은 모든 작업을 거절합니다. calloc(0)은 NULL일 수
       있지만 그 배열은 쓰이지 않습니다. */
    queue->jobs = calloc(capacity, sizeof(*queue->jobs));
    queue->threads = calloc(thread_count, sizeof(*queue->threads));
    if ((queue->jobs == NULL && capacity > 0) || queue->threads == NULL ||
        pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->jobs);
        free(queue->threads);
        free(queue);
        return NULL;
    }
    pthread_cond_init(&queue->job_ready, NULL);
    queue->capacity = capacity;
    queue->handler = handler;
    queue->context = context;
    /* 스레드를 일부만 만들었으면 만든 스레드만 끝내고 실패합니다. */
    while (queue->thread_count < thread_count) {
        if (pthread_create(&queue->threads[queue->thread_count], NULL,
                           worker_main, queue) != 0) {
            work_queue_destroy(queue);
            return NULL;
        }
        queue->thread_count++;
    }

    return queue;
}

bool work_queue_submit(struct work_queue *queue, void *job)
{
    bool accepted = false;

    pthread_mutex_lock(&queue->mutex);
    if (queue->job_count < queue->capacity) {
        queue->jobs[(queue->head + queue->job_count) % queue->capacity] = job;
        queue->job_count++;
        accepted = true;
        pthread_cond_signal(&queue->job_ready);
    }
    pthread_mutex_unlock(&queue->mutex);

    return accepted;
}

size_t work_queue_discard(struct work_queue *queue, void (*discard)(void *job))
{
    size_t discarded_count;

    pthread_mutex_lock(&queue->mutex);
    discarded_count = queue->job_count;
    for (size_t i = 0; i < discarded_count; i++) {
        discard(queue->jobs[(queue->head + i) % queue->capacity]);
    }
    queue->head = 0;
    queue->job_count = 0;
    pthread_mutex_unlock(&queue->mutex);

    return discarded_count;
}

void work_queue_destroy(struct work_queue *queue)
{
    if (queue == NULL) {
        return;
    }
    queue_stop_threads(queue);
    pthread_cond_destroy(&queue->job_ready);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->jobs);
    free(queue->threads);
    free(queue);
}

/* 작업 스레드 하나입니다. 작업을 하나씩 꺼내 mutex 밖에서 실행합니다.
   멈추라는 신호를 받아도 대기열이 빌 때까지는 계속 꺼냅니다. */
static void *worker_main(void *arg)
{
    struct work_queue *queue = arg;
    void *job;

    while ((job = queue_take(queue)) != NULL) {
        queue->handler(job, queue->context);
    }

    return NULL;
}

/* 작업이 생길 때까지 기다려 맨 앞 작업을 꺼냅니다. 멈추는 중이고 대기열이
   비었으면 NULL입니다. */
static void *queue_take(struct work_queue *queue)
{
    void *job = NULL;

    pthread_mutex_lock(&queue->mutex);
    while (queue->job_count == 0 && !queue->stopping) {
        pthread_cond_wait(&queue->job_ready, &queue->mutex);
    }
    if (queue->job_count > 0) {
        job = queue->jobs[queue->head];
        queue->head = (queue->head + 1) % queue->capacity;
        queue->job_count--;
    }
    pthread_mutex_unlock(&queue->mutex);

    return job;
}

static void queue_stop_threads(struct work_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->stopping = true;
    pthread_cond_broadcast(&queue->job_ready);
    pthread_mutex_unlock(&queue->mutex);
    for (size_t i = 0; i < queue->thread_count; i++) {
        pthread_join(queue->threads[i], NULL);
    }
}
