#include "rwlock.h"

static void rwlock_wait(struct rwlock *lock, pthread_cond_t *cond,
                        enum rwlock_mode mode);

int rwlock_init(struct rwlock *lock)
{
    *lock = (struct rwlock){0};
    if (pthread_mutex_init(&lock->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&lock->readers_may_enter, NULL) != 0) {
        pthread_mutex_destroy(&lock->mutex);
        return -1;
    }
    if (pthread_cond_init(&lock->writer_may_enter, NULL) != 0) {
        pthread_cond_destroy(&lock->readers_may_enter);
        pthread_mutex_destroy(&lock->mutex);
        return -1;
    }
    return 0;
}

void rwlock_destroy(struct rwlock *lock)
{
    pthread_cond_destroy(&lock->writer_may_enter);
    pthread_cond_destroy(&lock->readers_may_enter);
    pthread_mutex_destroy(&lock->mutex);
}

/* 쓰는 중일 때뿐 아니라 기다리는 쓰기가 있을 때도 기다립니다. 그래야
   읽기가 끊이지 않아도 쓰기가 굶지 않습니다. */
void rwlock_read_lock(struct rwlock *lock)
{
    pthread_mutex_lock(&lock->mutex);
    while (lock->writing || lock->waiting_writers > 0) {
        rwlock_wait(lock, &lock->readers_may_enter, RWLOCK_READ);
    }
    lock->active_readers++;
    pthread_mutex_unlock(&lock->mutex);
}

/* 마지막 읽기가 나갈 때만 쓰기를 깨웁니다. */
void rwlock_read_unlock(struct rwlock *lock)
{
    pthread_mutex_lock(&lock->mutex);
    lock->active_readers--;
    if (lock->active_readers == 0 && lock->waiting_writers > 0) {
        pthread_cond_signal(&lock->writer_may_enter);
    }
    pthread_mutex_unlock(&lock->mutex);
}

void rwlock_write_lock(struct rwlock *lock)
{
    pthread_mutex_lock(&lock->mutex);
    lock->waiting_writers++;
    while (lock->writing || lock->active_readers > 0) {
        rwlock_wait(lock, &lock->writer_may_enter, RWLOCK_WRITE);
    }
    lock->waiting_writers--;
    lock->writing = true;
    pthread_mutex_unlock(&lock->mutex);
}

/* 기다리는 쓰기가 있으면 그 쓰기를 먼저 깨우고, 없을 때만 읽기를 모두
   깨웁니다. */
void rwlock_write_unlock(struct rwlock *lock)
{
    pthread_mutex_lock(&lock->mutex);
    lock->writing = false;
    if (lock->waiting_writers > 0) {
        pthread_cond_signal(&lock->writer_may_enter);
    } else {
        pthread_cond_broadcast(&lock->readers_may_enter);
    }
    pthread_mutex_unlock(&lock->mutex);
}

static void rwlock_wait(struct rwlock *lock, pthread_cond_t *cond,
                        enum rwlock_mode mode)
{
    if (lock->wait_hook != NULL) {
        lock->wait_hook(lock->wait_context, mode);
    }
    pthread_cond_wait(cond, &lock->mutex);
}
