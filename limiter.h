#ifndef LIMITER_H
#define LIMITER_H

// Initialize the limiter with the maximum number of clients allowed
void limiter_init(int max_clients);

// Acquire a slot in the limiter, blocking if necessary
void limiter_acquire(void);

// Release a slot in the limiter
void limiter_release(void);

#endif // LIMITER_H