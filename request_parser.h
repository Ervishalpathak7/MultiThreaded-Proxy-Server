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
    char *method;
    char *host;
    char *path;
    char *version;
    char *body;
    size_t bodylen;
    struct ParsedHeader *headers;
};


// Create an empty parsing object to be used exactly once for parsing a single request buffer
struct ParsedRequest *ParsedRequest_create();

// Parse a request buffer and populate the ParsedRequest object
struct ParsedRequest *ParsedRequest_parse(struct ParsedRequest *request, const char *buf, int buflen);

#endif // REQUEST_PARSER_H