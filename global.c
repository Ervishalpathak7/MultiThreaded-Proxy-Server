
#include <stdio.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/socket.h>

// lock for synchronizing access to cache
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// semaphore for controlling access to the cache
sem_t semaphore;

// connected client socket descriptor
int Connected_socketId[SOMAXCONN];
pthread_t tid[SOMAXCONN];
int index = 0;