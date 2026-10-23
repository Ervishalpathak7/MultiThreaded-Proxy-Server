#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include "parser.c"

#define PORT 9000
#define BUFFER_SIZE 1024

char *http_response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 24\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello from the C Server!";

void *handle_connection(void *arg)
{
    char buffer[BUFFER_SIZE] = {0};
    int client_fd = *(int *)arg;
    free(arg);
    
    ssize_t read_bytes = read(client_fd, buffer, BUFFER_SIZE - 1);
    if (read_bytes == 0)
    {
        perror("[Error] : Error Reading Request");
        close(client_fd);
    }
    
    buffer[read_bytes] = '\n';
    printf("Request : %s \n", buffer);
    http_request_parser(buffer);
    send(client_fd, http_response, strlen(http_response), 0);
    printf("Hello message sent to client.\n");
    close(client_fd);
    return NULL;
}

void startServer()
{
    // Ignore SIGPIPE signal to prevent server crash when writing to a closed socket
    signal(SIGPIPE, SIG_IGN);

    int server_fd, new_socket;
    // Creating Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("[Error] : Error Creating Socket");
        exit(EXIT_FAILURE);
    }

    // Configure Address Structure
    struct sockaddr_in address;
    int address_len = sizeof(address);
    memset(&address, 0, sizeof(address));
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);

    // Binding Socket
    if ((bind(server_fd, (struct sockaddr *)&address, address_len)) < 0)
    {
        perror("[Error] : Error Binding Socker]");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen on Socket
    if ((listen(server_fd, SOMAXCONN)) < 0)
    {
        perror("[Error] : Error while Listening on Socket]");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Socket Listening on port : %d\n", PORT);

    while (1)
    {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&address_len)) < 0)
        {
            perror("[Warn] : Error while Accepting Connection]");
            continue;
        }

        int *worker_fd = malloc(sizeof(int));
        if (worker_fd == NULL)
        {
            perror("[Warn] : Error Alocating memory for new connection]");
            close(new_socket);
            continue;
        }

        *worker_fd = new_socket;
        pthread_t thread_id;
        if ((pthread_create(&thread_id, NULL, handle_connection, worker_fd)) < 0)
        {
            perror("[Warn] : Error while Creating Thread]");
            close(new_socket);
            free(worker_fd);
            continue;
        }

        pthread_detach(thread_id);
    }
    close(server_fd);
}