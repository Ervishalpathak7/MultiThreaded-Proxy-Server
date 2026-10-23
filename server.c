#include <stdlib.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 9000

void handle_connection(void *arg)
{
    int *client_fd = (int *)arg;
    free(arg);

    // Handle the client connection here
    
}

void startServer()
{
    // Ignore SIGPIPE signal to prevent server crash when writing to a closed socket
    signal(SIGPIPE, SIG_IGN);

    int server_fd;

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
    address.sin_port = PORT;

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

    while (1)
    {
        int new_socket;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        if ((new_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len)) < 0)
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
        if ((pthread_create(&thread_id, NULL, handle_connection, &worker_fd)) < 0)
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