#include "limiter.h"
#include <pthread.h>

pthread_mutex_t limiter_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t limiter_cond = PTHREAD_COND_INITIALIZER;
static int available = 0;

void limiter_init(int max_clients)
{
    pthread_mutex_lock(&limiter_mutex);
    available = max_clients;
    pthread_mutex_unlock(&limiter_mutex);
}

void limiter_acquire()
{
    pthread_mutex_lock(&limiter_mutex);
    while (available == 0)
        pthread_cond_wait(&limiter_cond, &limiter_mutex);
    available--;
    pthread_mutex_unlock(&limiter_mutex);
}

void limiter_release()
{
    pthread_mutex_lock(&limiter_mutex);
    available++;
    pthread_cond_signal(&limiter_cond);
    pthread_mutex_unlock(&limiter_mutex);
}