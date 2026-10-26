#ifndef REQUEST_PARSER_H
#define REQUEST_PARSER_H

#include <stddef.h>

// ParsedHeader: any header after the request line is a key-value pair with the
// format "key:value\r\n" and is maintained in the ParsedHeader linked list
// within ParsedRequest
struct ParsedHeader
{
    char *key;
    size_t keylen;
    char *value;
    size_t valuelen;
    struct ParsedHeader *next;
};

// ParsedRequest: represents a parsed HTTP request, including the method, host, path, version, and headers.
struct ParsedRequest
{
    char *method;    // GET | POST etc
    char *path;      // request target, e.g. "/index.html"
    char *version;   // e.g. "HTTP/1.1"
    char *host;      // copied from the Host header (NULL if absent)
    struct ParsedHeader *headers;
    size_t head_len; // bytes in the head, including the final \r\n\r\n
};

// Index just past the first "\r\n\r\n", or -1 if the head isn't complete yet.
ssize_t find_header_end(const char* buf, size_t len);


// Create an empty parsing object to be used exactly once for parsing a single request buffer
struct ParsedRequest *ParsedRequest_create(void);

// Returns 0 on success, -1 on malformed/incomplete input.
int ParsedRequest_parse(struct ParsedRequest *req, const char *buf, size_t buflen);

// Destroy the parsing object and free associated memory
void ParsedRequest_destroy(struct ParsedRequest *request);

// Case-insensitive lookup. Returns NULL if absent. Pointer is owned by req.
const char *ParsedRequest_get_header(const struct ParsedRequest *req, const char *key);

void ParsedRequest_destroy(struct ParsedRequest *req);   // NULL-safe


#endif // REQUEST_PARSER_H