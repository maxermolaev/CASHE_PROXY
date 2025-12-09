#include "thread_pool.h"

#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

static long id_counter = 0;

static void *executor_routine(void *arg);

struct task_t {
    long id;
    void (*routine)(void *arg);
    void *arg;
};
typedef struct task_t task_t;

struct thread_pool_t {
    task_t *tasks;
    int capacity;
    int size;
    int front;
    int rear;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty_cond;
    pthread_cond_t not_full_cond;

    pthread_t *executors;
    int num_executors;

    atomic_int shutdown;
};

thread_pool_t * thread_pool_create(int executor_count, int task_queue_capacity) {
    errno = 0;
    thread_pool_t *pool = malloc(sizeof(thread_pool_t));
    if (pool == NULL) {
        if (errno == ENOMEM) log("Thread pool creation error: %s", strerror(errno));
        else log("Thread pool creation error: failed to reallocate memory");
        return NULL;
    }

    errno = 0;
    pool->tasks = calloc(sizeof(task_t), task_queue_capacity);
    if (pool->tasks == NULL) {
        if (errno == ENOMEM) log("Thread pool creation error: %s", strerror(errno));
        else log("Thread pool creation error: failed to reallocate memory");

        free(pool);
        return NULL;
    }

    pool->capacity = task_queue_capacity;
    pool->size = 0;
    pool->front = 0;
    pool->rear = 0;
    pool->shutdown = 0;
    pool->num_executors = executor_count;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->not_empty_cond, NULL);
    pthread_cond_init(&pool->not_full_cond, NULL);

    errno = 0;
    pool->executors = calloc(sizeof(pthread_t), executor_count);
    if (pool->executors == NULL) {
        if (errno == ENOMEM) log("Thread pool creation error: %s", strerror(errno));
        else log("Thread pool creation error: failed to reallocate memory");

        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->not_empty_cond);
        pthread_cond_destroy(&pool->not_full_cond);
        free(pool);
        return NULL;
    }

    char thread_name[16];
    for (int i = 0; i < executor_count; i++) {
        pthread_create(&pool->executors[i], NULL, executor_routine, pool);

        snprintf(thread_name, 16, "thread-pool-%d", i);
        pthread_setname_np(thread_name);
    }

    return pool;
}

void thread_pool_execute(thread_pool_t *pool, routine_t routine, void *arg) {
    if (pool->shutdown) {
        log("Thread pool execution error: thread pool was shutdown");
        return;
    }

    pthread_mutex_lock(&pool->mutex);

    while (pool->size == pool->capacity && !pool->shutdown) pthread_cond_wait(&pool->not_full_cond, &pool->mutex);

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return;
    }

    pool->tasks[pool->rear].id = id_counter++;
    pool->tasks[pool->rear].routine = routine;
    pool->tasks[pool->rear].arg = arg;
    pool->rear = (pool->rear + 1) % pool->capacity;
    pool->size++;

    pthread_cond_signal(&pool->not_empty_cond);

    pthread_mutex_unlock(&pool->mutex);
}

void thread_pool_shutdown(thread_pool_t *pool) {
    if (!pool) return;

    pool->shutdown = 1;

    pthread_cond_broadcast(&pool->not_empty_cond);
    pthread_cond_broadcast(&pool->not_full_cond);

    for (int i = 0; i < pool->num_executors; i++) {
        int waited = 0;
        const int max_wait_sec = 5;
        while (waited < max_wait_sec) {
            if (pthread_kill(pool->executors[i], 0) != 0) {
                break;
            }
            sleep(1);
            waited++;
        }
        pthread_detach(pool->executors[i]);
    }

    free(pool->tasks);
    free(pool->executors);

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty_cond);
    pthread_cond_destroy(&pool->not_full_cond);

    free(pool);
}


static void *executor_routine(void *arg) {
    sigset_t mask;
    sigfillset(&mask);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        log("Failed to block signals in worker");
        pthread_exit(NULL);
    }
    thread_pool_t *pool = (thread_pool_t *) arg;
    while (1) {
        pthread_mutex_lock(&pool->mutex);

        while (pool->size == 0 && !pool->shutdown) pthread_cond_wait(&pool->not_empty_cond, &pool->mutex);

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            pthread_exit(NULL);
        }

        task_t task = pool->tasks[pool->front];
        pool->front = (pool->front + 1) % pool->capacity;
        pool->size--;

        pthread_cond_signal(&pool->not_full_cond);

        pthread_mutex_unlock(&pool->mutex);

        log("Start executing task %d", task.id);
        task.routine(task.arg);
        log("Finish executing task %d", task.id);
    }
}