#include <stdio.h>
#include <string.h>

struct Request
{
    char method[10];
    char path[256];
    const char *body;
};

struct Request http_request_parser(char *request)
{
    struct Request req = {0};
    int parsed = sscanf(request, "%9s %255s %*s", req.method, req.path);
    if (parsed < 2)
    {
        printf("Failed to parse HTTP request line.\n");
        return req;
    }

    // Step 2: Find the double CRLF (\r\n\r\n) which marks the start of the body
    const char *body_start = strstr(request, "\r\n\r\n");
    if (body_start != NULL)
    {
        req.body = body_start + 4;
    }

    // Print out the extracted components
    printf("Method: %s\n", req.method);
    printf("Path:   %s\n", req.path);
    printf("Body:   %s\n", req.body ? req.body : "(No Body)");

    return req;
}
