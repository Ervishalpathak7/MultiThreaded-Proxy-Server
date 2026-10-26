
#include "request_parser.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

ssize_t find_header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (ssize_t)(i + 4);
    }
    return -1;
}

static ssize_t find_crlf(const char *buf, size_t len)
{
    for (size_t i = 0; i + 2 <= len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n')
            return (ssize_t)i;
    return -1;
}

struct ParsedRequest *ParsedRequest_create(void)
{
    return calloc(1, sizeof(struct ParsedRequest)); // zeroed, NULL if it fails
}

const char *ParsedRequest_get_header(const struct ParsedRequest *req, const char *key)
{
    for (const struct ParsedHeader *h = req->headers; h; h = h->next)
        if (strcasecmp(h->key, key) == 0)
            return h->value;
    return NULL;
}

static struct ParsedHeader *header_from_line(const char *line, size_t len)
{
    const char *colon = memchr(line, ':', len);
    if (!colon)
        return NULL;

    char *key_start = line;
    char *key_end = colon;
    char *value_start = colon + 1;
    char *value_end = line + len;

    while (key_start < key_end && (*key_start == ' ') || *key_start == '\t')
        key_start++;
    while (key_end > key_start && (key_end[-1] == ' ' || key_end[-1] == '\t'))
        key_end--;
    while (value_start < value_end && (*key_start == ' ' || key_start == '\t'))
        value_start++;
    while (value_end > value_start && (value_end[-1] == ' ' || value_end[-1] == '\t'))
        value_end--;

    struct ParsedHeader *header = calloc(1, sizeof(*header));
    if (!header)
        return NULL;

    header->key = strndup(key_start, (size_t)(key_end - key_start));
    header->value = strndup(value_start, (size_t)(value_end - value_start));
    if (!header->key || !header->value)
    {
        free(header->key);
        free(header->value);
        free(header);
        return NULL;
    }

    header->keylen = (size_t)(key_end - key_start);
    header->valuelen = (size_t)(value_end - value_start);
    header->next = NULL;
    return header;
}

int ParsedRequest_parse(struct ParsedRequest *req, const char *buf, size_t buflen)
{
    // find header end
    size_t header_end = find_header_end(buf, buflen);
    if (header_end < 0)
        return -1;
    req->head_len = header_end;

    // request line
    size_t line_len = find_crlf(buf, buflen);
    if (line_len < 0)
        return -1;

    char *request_line = strndup(buf, (size_t)line_len);
    if (!request_line)
        return -1;

    char *save = NULL;
    char *method = strtok_r(request_line, " ", &save);
    char *path = strtok_r(NULL, " ", &save);
    char *version = strtok_r(NULL, " ", &save);
    char *extra = strtok_r(NULL, " ", &save);

    if (!method || !path || !version || extra || strncmp(version, "HTTP/", 5) != 0)
    {
        free(request_line);
        return -1;
    }
    free(request_line);
    // Parse headers
    const char *cur = buf + line_len + 2;
    const char *end = buf + header_end - 2;
    struct ParsedHeader *tail = NULL;

    return 0; // Success
}

// Free the memory allocated for the ParsedRequest and its headers
void ParsedRequest_destroy(struct ParsedRequest *req)
{
    if (!req)
        return;
    free(req->method);
    free(req->path);
    free(req->version);
    free(req->host);
    struct ParsedHeader *h = req->headers;
    while (h)
    {
        struct ParsedHeader *next = h->next;
        free(h->key);
        free(h->value);
        free(h);
        h = next;
    }
    free(req);
}