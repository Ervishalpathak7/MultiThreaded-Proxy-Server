#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

// arg: heap-allocated int* holding the client fd. The handler frees it.
void* request_handler(void* arg);

#endif