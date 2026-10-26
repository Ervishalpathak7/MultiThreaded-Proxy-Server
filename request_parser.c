
#include "request_parser.h"

struct ParsedRequest *ParsedRequest_create()
{
    struct ParsedRequest *request = (struct ParsedRequest *)malloc(sizeof(struct ParsedRequest));
    request->method = NULL;
    request->host = NULL;
    request->path = NULL;
    request->version = NULL;
    request->body = NULL;
    request->headers = NULL;
    request->bodylen = 0;
    return request;
}

struct ParsedHeader *ParsedHeader_create(const char *key, const char *value)
{
    struct ParsedHeader *header = (struct ParsedHeader *)malloc(sizeof(struct ParsedHeader));
    header->key = strdup(key);
    header->keylen = strlen(key) + 1;
    header->value = strdup(value);
    header->valuelen = strlen(value) + 1;
    header->next = NULL;
    return header;
}

struct ParsedHeader *ParsedHeader_parse(const char *header_line)
{

    char *line_copy = strdup(header_line);
    char *key = strtok(line_copy, ":");
    char *value = strtok(NULL, "");
    if (key && value)
    {
        while (*key == ' ')
            key++;
        while (*value == ' ')
            value++;

        struct ParsedHeader *header = ParsedHeader_create(key, value);
        free(line_copy);
        return header;
    }
    free(line_copy);
    return NULL;
}

struct ParsedRequest *ParsedRequest_parse(struct ParsedRequest *request, const char *buf, int buflen)
{
    // Parse the request line
    char *request_line_end = strstr(buf, "\r\n");
    if (!request_line_end)
        return NULL;

    char *request_line = strndup(buf, request_line_end - buf);
    if (!request_line)
        return NULL;

    char *method = strtok(request_line, " ");
    char *host = strtok(NULL, " ");
    char *path = strtok(NULL, " ");
    char *version = strtok(NULL, "\r\n");

    if (method && host && path && version)
    {
        request->method = strdup(method);
        request->host = strdup(host);
        request->path = strdup(path);
        request->version = strdup(version);
    }
    else
    {
        free(request_line);
        return NULL; 
    }
    
    free(request_line);


    // Parse headers
    const char *header_start = request_line_end + 2;             // Skip the request line and CRLF
    const char *header_end = strstr(header_start, "\r\n\r\n"); // Find the end of headers

    if (header_end)
    {
        const char *current = header_start;
        while (current < header_end)
        {
            const char *line_end = strstr(current, "\r\n");
            if (!line_end)
                break;

            char *line = strndup(current, line_end - current);
            if (line)
            {
                struct ParsedHeader *header = ParsedHeader_parse(line);
                if (header)
                {
                    if (request->headers == NULL)
                    {
                        request->headers = header;
                    }
                    else
                    {
                        struct ParsedHeader *current_header = request->headers;
                        while (current_header->next != NULL)
                        {
                            current_header = current_header->next;
                        }
                        current_header->next = header;
                    }
                }
                free(line);
            }
            current = line_end + 2; // Move to the next header line
        }
    }
    // Parse body if present
    const char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start)
    {
        body_start += 4; // Skip the CRLF CRLF
        request->body = strdup(body_start);
        request->bodylen = strlen(request->body) + 1;
    }
    return request;
}