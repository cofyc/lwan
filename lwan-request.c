/*
 * lwan - simple web server
 * Copyright (c) 2012 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "lwan.h"
#include "int-to-str.h"

static const char* const _http_versions[] = {
    [HTTP_1_0] = "1.0",
    [HTTP_1_1] = "1.1"
};
static const char* const _http_connection_type[] = {
    "Close",
    "Keep-Alive"
};

static ALWAYS_INLINE char *
_identify_http_method(lwan_request_t *request, char *buffer)
{
    STRING_SWITCH(buffer) {
    case HTTP_STR_GET:
        request->method = HTTP_GET;
        return buffer + 4;
    case HTTP_STR_HEAD:
        request->method = HTTP_HEAD;
        return buffer + 5;
    }
    return NULL;
}

static ALWAYS_INLINE char *
_identify_http_path(lwan_request_t *request, char *buffer, size_t limit)
{
    /* FIXME
     * - query string
     * - fragment
     */
    char *end_of_line = memchr(buffer, '\r', limit);
    if (!end_of_line)
        return NULL;
    *end_of_line = '\0';

    char *space = end_of_line - sizeof("HTTP/X.X");
    if (UNLIKELY(*(space + 1) != 'H')) /* assume HTTP/X.Y */
        return NULL;
    *space = '\0';

    if (LIKELY(*(space + 6) == '1'))
        request->http_version = *(space + 8) == '0' ? HTTP_1_0 : HTTP_1_1;
    else
        return NULL;

    request->url = buffer;
    request->url_len = space - buffer;

    if (UNLIKELY(*request->url != '/'))
        return NULL;

    return end_of_line + 1;
}

#define MATCH_HEADER(hdr) \
  do { \
        char *end; \
        p += sizeof(hdr) - 1; \
        if (UNLIKELY(*p++ != ':'))	/* not the header we're looking for */ \
          goto did_not_match; \
        if (UNLIKELY(*p++ != ' '))	/* not the header we're looking for */ \
          goto did_not_match; \
        if (LIKELY(end = strchr(p, '\r'))) {      /* couldn't find line end */ \
          *end = '\0'; \
          value = p; \
          p = end + 1; \
          if (UNLIKELY(*p != '\n')) \
            goto did_not_match; \
        } else \
          goto did_not_match; \
  } while (0)

#define CASE_HEADER(hdr_const,hdr_name) case hdr_const: MATCH_HEADER(hdr_name);

ALWAYS_INLINE static char *
_parse_headers(lwan_request_t *request, char *buffer, char *buffer_end)
{
    char *p;

    for (p = buffer; p && *p; buffer = ++p) {
        char *value;

        if ((p + sizeof(int32_t)) >= buffer_end)
            break;

        STRING_SWITCH(p) {
        CASE_HEADER(HTTP_HDR_CONNECTION, "Connection")
            request->header.connection = (*value | 0x20);
            break;
        CASE_HEADER(HTTP_HDR_HOST, "Host")
            /* Virtual hosts are not supported yet; ignore */
            break;
        CASE_HEADER(HTTP_HDR_IF_MODIFIED_SINCE, "If-Modified-Since")
            /* Ignore */
            break;
        CASE_HEADER(HTTP_HDR_RANGE, "Range")
            /* Ignore */
            break;
        CASE_HEADER(HTTP_HDR_REFERER, "Referer")
            /* Ignore */
            break;
        CASE_HEADER(HTTP_HDR_COOKIE, "Cookie")
            /* Ignore */
            break;
        }
did_not_match:
        p = strchr(p, '\n');
    }

    return buffer;
}

#undef CASE_HEADER
#undef MATCH_HEADER

ALWAYS_INLINE static char *
_ignore_leading_whitespace(char *buffer)
{
    while (*buffer && memchr(" \t\r\n", *buffer, 4))
        buffer++;
    return buffer;
}

ALWAYS_INLINE static void
_compute_flags(lwan_request_t *request)
{
    if (request->http_version == HTTP_1_1)
        request->flags.is_keep_alive = (request->header.connection != 'c');
    else
        request->flags.is_keep_alive = (request->header.connection == 'k');
}

bool
lwan_process_request(lwan_t *l, lwan_request_t *request)
{
    lwan_url_map_t *url_map;
    char buffer[6 * 1024], *p_buffer;
    size_t bytes_read;

    switch (bytes_read = read(request->fd, buffer, sizeof(buffer))) {
    case 0:
        return false;
    case -1:
        perror("read");
        return false;
    case sizeof(buffer):
        return lwan_default_response(l, request, HTTP_TOO_LARGE);
    }

    buffer[bytes_read] = '\0';

    p_buffer = _ignore_leading_whitespace(buffer);
    if (!*p_buffer)
        return lwan_default_response(l, request, HTTP_BAD_REQUEST);

    p_buffer = _identify_http_method(request, p_buffer);
    if (UNLIKELY(!p_buffer))
        return lwan_default_response(l, request, HTTP_NOT_ALLOWED);

    p_buffer = _identify_http_path(request, p_buffer, bytes_read);
    if (UNLIKELY(!p_buffer))
        return lwan_default_response(l, request, HTTP_BAD_REQUEST);

    p_buffer = _parse_headers(request, p_buffer, buffer + bytes_read);
    if (UNLIKELY(!p_buffer))
        return lwan_default_response(l, request, HTTP_BAD_REQUEST);

    _compute_flags(request);

    if ((url_map = lwan_trie_lookup_prefix(l->url_map_trie, request->url))) {
        request->url += url_map->prefix_len;
        return lwan_response(l, request, url_map->callback(request, url_map->data));
    }

    return lwan_default_response(l, request, HTTP_NOT_FOUND);
}

ALWAYS_INLINE void
lwan_request_set_response(lwan_request_t *request, lwan_response_t *response)
{
    request->response = response;
}

#define APPEND_STRING_LEN(const_str_,len_) \
    memcpy(p_headers, (const_str_), (len_)); \
    p_headers += (len_)
#define APPEND_STRING(str_) \
    len = strlen(str_); \
    memcpy(p_headers, (str_), len); \
    p_headers += len
#define APPEND_INT8(value_) \
    APPEND_CHAR(decimal_digits[((value_) / 100) % 10]); \
    APPEND_CHAR(decimal_digits[((value_) / 10) % 10]); \
    APPEND_CHAR(decimal_digits[(value_) % 10])
#define APPEND_INT(value_) \
    len = int_to_string((value_), buffer); \
    APPEND_STRING_LEN(buffer, len)
#define APPEND_CHAR(value_) \
    *p_headers++ = (value_)
#define APPEND_CONSTANT(const_str_) \
    APPEND_STRING_LEN((const_str_), sizeof(const_str_) - 1)

ALWAYS_INLINE size_t
lwan_prepare_response_header(lwan_request_t *request, lwan_http_status_t status, char headers[])
{
    char *p_headers;
    char buffer[32];
    int32_t len;

    p_headers = headers;

    APPEND_CONSTANT("HTTP/");
    APPEND_STRING_LEN(_http_versions[request->http_version], 3);
    APPEND_CHAR(' ');
    APPEND_INT8(status);
    APPEND_CHAR(' ');
    APPEND_STRING(lwan_http_status_as_string(status));
    APPEND_CONSTANT("\r\nContent-Length: ");
    APPEND_INT(request->response->content_length);
    APPEND_CONSTANT("\r\nContent-Type: ");
    APPEND_STRING(request->response->mime_type);
    APPEND_CONSTANT("\r\nConnection: ");
    APPEND_STRING_LEN(_http_connection_type[request->flags.is_keep_alive],
        (request->flags.is_keep_alive ? sizeof("Keep-Alive") : sizeof("Close")) - 1);
    if (request->response->headers) {
        lwan_http_header_t *header;

        for (header = request->response->headers; header->name; header++) {
            APPEND_CHAR('\r');
            APPEND_CHAR('\n');
            APPEND_STRING(header->name);
            APPEND_CHAR(':');
            APPEND_CHAR(' ');
            APPEND_STRING(header->value);
        }
    }
    APPEND_CONSTANT("\r\nServer: lwan\r\n\r\n\0");

    return p_headers - headers - 1;
}

#undef APPEND_STRING_LEN
#undef APPEND_STRING
#undef APPEND_CONSTANT
#undef APPEND_CHAR
#undef APPEND_INT

void
lwan_request_set_corked(lwan_request_t *request, bool setting)
{
    if (UNLIKELY(setsockopt(request->fd, IPPROTO_TCP, TCP_CORK,
                        (int[]){ setting }, sizeof(int)) < 0))
        perror("setsockopt");
}