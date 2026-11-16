#include <pthread.h>
#include <stdbool.h>

#include "acutest.h"
#include "rwlock.h"

/* 테스트가 쥔 상태입니다. 스레드의 순서는 시간이 아니라 이 mutex와 조건
   변수로 맞춥니다. 잠금이 스레드를 기다리게 할 때마다 wait_hook이 기다린
   쪽의 수를 올리고 알립니다. */
struct probe {
    struct rwlock lock;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    int readers_in;
    int writers_in;
    int readers_waited;
    int writers_waited;
    bool release;
};

static void probe_init(struct probe *probe);
static void probe_destroy(struct probe *probe);
static void probe_wait_hook(void *context, enum rwlock_mode mode);
static void probe_wait_until(struct probe *probe, const int *count, int target);
static void probe_wait_waited_or_in(struct probe *probe, const int *waited,
                                    const int *entered);
static int probe_get(struct probe *probe, const int *count);
static void probe_release(struct probe *probe);
static void *reader_main(void *arg);
static void *writer_main(void *arg);
static void *held_reader_main(void *arg);
static void *held_writer_main(void *arg);

static void two_readers_enter_together_test(void)
{
    struct probe probe;
    pthread_t thread;

    probe_init(&probe);
    rwlock_read_lock(&probe.lock);
    /* 첫 읽기를 쥔 채로 두 번째 읽기가 들어오면, 둘은 동시에 들어간
       것입니다. 못 들어오면 이 테스트는 멈춥니다. */
    TEST_ASSERT(pthread_create(&thread, NULL, held_reader_main, &probe) == 0);
    probe_wait_until(&probe, &probe.readers_in, 1);
    TEST_CHECK(probe_get(&probe, &probe.readers_waited) == 0);
    probe_release(&probe);
    pthread_join(thread, NULL);
    rwlock_read_unlock(&probe.lock);
    probe_destroy(&probe);
}

static void writer_enters_alone_test(void)
{
    struct probe probe;
    pthread_t reader;
    pthread_t writer;

    probe_init(&probe);
    rwlock_write_lock(&probe.lock);
    TEST_ASSERT(pthread_create(&reader, NULL, reader_main, &probe) == 0);
    TEST_ASSERT(pthread_create(&writer, NULL, writer_main, &probe) == 0);
    probe_wait_waited_or_in(&probe, &probe.readers_waited, &probe.readers_in);
    probe_wait_waited_or_in(&probe, &probe.writers_waited, &probe.writers_in);
    TEST_CHECK(probe_get(&probe, &probe.readers_in) == 0);
    TEST_CHECK(probe_get(&probe, &probe.writers_in) == 0);
    rwlock_write_unlock(&probe.lock);
    pthread_join(reader, NULL);
    pthread_join(writer, NULL);
    TEST_CHECK(probe.readers_in == 1);
    TEST_CHECK(probe.writers_in == 1);
    probe_destroy(&probe);
}

static void waiting_writer_blocks_new_reader_test(void)
{
    struct probe probe;
    pthread_t writer;
    pthread_t reader;

    probe_init(&probe);
    rwlock_read_lock(&probe.lock);
    TEST_ASSERT(pthread_create(&writer, NULL, held_writer_main, &probe) == 0);
    probe_wait_waited_or_in(&probe, &probe.writers_waited, &probe.writers_in);
    if (!TEST_CHECK(probe_get(&probe, &probe.writers_in) == 0)) {
        probe_release(&probe);
    }
    /* 읽기가 하나 들어가 있어도, 쓰기가 기다리면 새 읽기는 기다립니다. */
    TEST_ASSERT(pthread_create(&reader, NULL, reader_main, &probe) == 0);
    probe_wait_waited_or_in(&probe, &probe.readers_waited, &probe.readers_in);
    TEST_CHECK(probe_get(&probe, &probe.readers_in) == 0);
    rwlock_read_unlock(&probe.lock);
    /* 기다리던 쓰기가 먼저 들어가고, 쓰기가 끝날 때까지 읽기는 못
       들어옵니다. */
    probe_wait_until(&probe, &probe.writers_in, 1);
    TEST_CHECK(probe_get(&probe, &probe.readers_in) == 0);
    probe_release(&probe);
    pthread_join(writer, NULL);
    pthread_join(reader, NULL);
    TEST_CHECK(probe.readers_in == 1);
    probe_destroy(&probe);
}

static void probe_init(struct probe *probe)
{
    *probe = (struct probe){0};
    TEST_ASSERT(rwlock_init(&probe->lock) == 0);
    probe->lock.wait_hook = probe_wait_hook;
    probe->lock.wait_context = probe;
    pthread_mutex_init(&probe->mutex, NULL);
    pthread_cond_init(&probe->changed, NULL);
}

static void probe_destroy(struct probe *probe)
{
    rwlock_destroy(&probe->lock);
    pthread_mutex_destroy(&probe->mutex);
    pthread_cond_destroy(&probe->changed);
}

static void probe_add(struct probe *probe, int *count)
{
    pthread_mutex_lock(&probe->mutex);
    (*count)++;
    pthread_cond_broadcast(&probe->changed);
    pthread_mutex_unlock(&probe->mutex);
}

static void probe_wait_hook(void *context, enum rwlock_mode mode)
{
    struct probe *probe = context;

    probe_add(probe, mode == RWLOCK_READ ? &probe->readers_waited
                                         : &probe->writers_waited);
}

static void probe_wait_until(struct probe *probe, const int *count, int target)
{
    pthread_mutex_lock(&probe->mutex);
    while (*count < target) {
        pthread_cond_wait(&probe->changed, &probe->mutex);
    }
    pthread_mutex_unlock(&probe->mutex);
}

/* 스레드가 기다리기 시작했거나 이미 들어갔을 때까지 기다립니다. 잘못
   들어간 경우에도 테스트가 멈추지 않고 실패하게 하려고 둘 다 봅니다. */
static void probe_wait_waited_or_in(struct probe *probe, const int *waited,
                                    const int *entered)
{
    pthread_mutex_lock(&probe->mutex);
    while (*waited == 0 && *entered == 0) {
        pthread_cond_wait(&probe->changed, &probe->mutex);
    }
    pthread_mutex_unlock(&probe->mutex);
}

static int probe_get(struct probe *probe, const int *count)
{
    int value;

    pthread_mutex_lock(&probe->mutex);
    value = *count;
    pthread_mutex_unlock(&probe->mutex);
    return value;
}

static void probe_release(struct probe *probe)
{
    pthread_mutex_lock(&probe->mutex);
    probe->release = true;
    pthread_cond_broadcast(&probe->changed);
    pthread_mutex_unlock(&probe->mutex);
}

static void probe_wait_release(struct probe *probe)
{
    pthread_mutex_lock(&probe->mutex);
    while (!probe->release) {
        pthread_cond_wait(&probe->changed, &probe->mutex);
    }
    pthread_mutex_unlock(&probe->mutex);
}

/* 들어가면 수를 올리고 바로 나옵니다. */
static void *reader_main(void *arg)
{
    struct probe *probe = arg;

    rwlock_read_lock(&probe->lock);
    probe_add(probe, &probe->readers_in);
    rwlock_read_unlock(&probe->lock);
    return NULL;
}

static void *writer_main(void *arg)
{
    struct probe *probe = arg;

    rwlock_write_lock(&probe->lock);
    probe_add(probe, &probe->writers_in);
    rwlock_write_unlock(&probe->lock);
    return NULL;
}

/* 들어가면 수를 올리고, 테스트가 놓아 줄 때까지 잠금을 쥐고 있습니다. */
static void *held_reader_main(void *arg)
{
    struct probe *probe = arg;

    rwlock_read_lock(&probe->lock);
    probe_add(probe, &probe->readers_in);
    probe_wait_release(probe);
    rwlock_read_unlock(&probe->lock);
    return NULL;
}

static void *held_writer_main(void *arg)
{
    struct probe *probe = arg;

    rwlock_write_lock(&probe->lock);
    probe_add(probe, &probe->writers_in);
    probe_wait_release(probe);
    rwlock_write_unlock(&probe->lock);
    return NULL;
}

TEST_LIST = {
    {"[T120] 읽기를 쥔 동안 다른 읽기도 들어간다",
     two_readers_enter_together_test},
    {"[T121] 쓰기를 쥔 동안 읽기와 다른 쓰기는 기다렸다가 쓰기가 끝난 뒤 "
     "들어간다",
     writer_enters_alone_test},
    {"[T122] 쓰기가 기다리는 동안 새 읽기는 읽기가 남아 있어도 기다리고, "
     "쓰기가 먼저 들어간다",
     waiting_writer_blocks_new_reader_test},
    {NULL, NULL},
};
