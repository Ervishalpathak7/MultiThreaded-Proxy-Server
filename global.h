#ifndef GLOBAL_H
#define GLOBAL_H

// max allowed size of request/response
#define MAX_BYTES 4096

// max number of client requests served at a time
#define MAX_CLIENTS 400

// slowloris protection timeout in seconds
#define RECV_TIMEOUT 5

#endif