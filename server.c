// Client socket descriptor and length of client address to be assigned
#include <stdio.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include "thread_fn.c"
#include "global.c"

void sever_init(int port_number)
{
    // 1.  Initialize the semaphore and mutex
    sem_init(&semaphore, 0, SOMAXCONN);
    pthread_mutex_init(&lock, NULL);

    // Intialise variables for proxy server
    int proxy_socketId;                         // socket descriptor of proxy server
    struct sockaddr_in proxy_addr;              // address of proxy server
    memset(&proxy_addr, 0, sizeof(proxy_addr)); // set all bytes of the struct to zero
    size_t addrlen = sizeof(proxy_addr);        // length of proxy address

    // variables for client
    int client_socketId;                            // socket descriptor of the client
    struct sockaddr_in client_addr;                 // address of the client
    socklen_t client_addrlen = sizeof(client_addr); // length of client address

    // 2.  Create a socket
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId < 0)
    {
        perror("Error in creating socket");
        exit(EXIT_FAILURE);
    }

    // 3. Configure the socket
    int reuse = 1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("Error in setting socket options");
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }

    // 4. Bind the socket to an address and port
    proxy_addr.sin_family = AF_INET;          // IPv4 address family
    proxy_addr.sin_addr.s_addr = INADDR_ANY;  // bind to all available interfaces
    proxy_addr.sin_port = htons(port_number); // bind to port 8080

    if (bind(proxy_socketId, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) < 0)
    {
        perror("Error in binding socket");
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }

    // 5. Listen for incoming connections
    if (listen(proxy_socketId, SOMAXCONN) < 0)
    {
        perror("Error in listening for connections");
        close(proxy_socketId);
        exit(EXIT_FAILURE);
    }

    // 6. Accept incoming connections in a loop
    while (1)
    {
        memset(&client_addr, 0, sizeof(client_addr)); // set all bytes of the struct to zero
        client_socketId = accept(proxy_socketId, (struct sockaddr *)&client_addr, &client_addrlen);
        if (client_socketId < 0)
        {
            perror("Error in accepting connection");
            continue; // continue to accept the next connection
        }
        // Store the connected client socket descriptor
        Connected_socketId[index] = client_socketId;
        // pass semaphore and mutex to the thread function
        pthread_create(&tid[index], NULL, thread_fn, (void *)&Connected_socketId[index]); // Create a new thread to handle the client
        index++;
    }
}