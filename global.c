
#include <stdio.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/socket.h>



// max allowed size of request/response
#define MAX_BYTES 4096 

// lock for synchronizing access to cache
pthread_mutex_t lock;

// semaphore for controlling access to the cache
sem_t semaphore;

// connected client socket descriptor
int Connected_socketId[SOMAXCONN];
pthread_t tid[SOMAXCONN];
int index = 0;