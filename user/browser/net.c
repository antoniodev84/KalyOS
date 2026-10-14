#include "browser.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
extern int dns_resolve(const char *, const char *, struct in_addr *);
static uint64_t milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void pause_io(void) {
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
}
int b_url(const char *base, const char *reference, char *out, size_t size) {
    if (!reference || !*reference) {
        reference = base;
    }
    if (!reference) {
        return -1;
    }
    for (const char *p = reference; *p; p++) {
        if ((unsigned char)*p < 32 || *p == ' ' || *p == '\\') {
            return -1;
        }
    }
    char combined[B_URL];
    if (!strncmp(reference, "http://", 7) || !strncmp(reference, "file://", 7) ||
        !strcmp(reference, "about:home")) {
        b_copy(combined, sizeof(combined), reference);
    } else if (strstr(reference, "://") || strchr(reference, ':')) {
        return -1;
    } else if (!strncmp(reference, "//", 2)) {
        snprintf(combined, sizeof(combined), "http:%s", reference);
    } else if (!base || (!strncmp(base, "about:", 6))) {
        snprintf(combined, sizeof(combined), "http://%s", reference);
    } else {
        char prefix[B_URL];
        b_copy(prefix, sizeof(prefix), base);
        char *fragment = strchr(prefix, '#');
        if (fragment) {
            *fragment = 0;
        }
        char *start = strstr(prefix, "://");
        if (!start) {
            return -1;
        }
        char *path = strchr(start + 3, '/');
        if (*reference == '#') {
            snprintf(combined, sizeof(combined), "%s%s", prefix, reference);
        } else if (*reference == '?') {
            char *query = strchr(prefix, '?');
            if (query) {
                *query = 0;
            }
            snprintf(combined, sizeof(combined), "%s%s", prefix, reference);
        } else if (*reference == '/') {
            if (path) {
                *path = 0;
            }
            snprintf(combined, sizeof(combined), "%s%s", prefix, reference);
        } else {
            char *query = strchr(prefix, '?');
            if (query) {
                *query = 0;
            }
            char *last = path ? strrchr(path, '/') : NULL;
            if (last) {
                last[1] = 0;
            } else {
                strcat(prefix, "/");
            }
            snprintf(combined, sizeof(combined), "%s%s", prefix, reference);
        }
    }
    if (strlen(combined) + 1 >= sizeof(combined) || strlen(combined) >= size) {
        return -1;
    }
    if (!strcmp(combined, "about:home")) {
        b_copy(out, size, combined);
        return 0;
    }
    char *origin = strstr(combined, "://");
    if (!origin) {
        return -1;
    }
    char *path = strchr(origin + 3, '/');
    if (!path) {
        if (strlen(combined) + 2 > size) {
            return -1;
        }
        strcat(combined, "/");
        path = combined + strlen(combined) - 1;
    }
    char suffix[B_URL] = "";
    char *query = strpbrk(path, "?#");
    if (query) {
        b_copy(suffix, sizeof(suffix), query);
        *query = 0;
    }
    char normalized[B_URL] = "/", copy[B_URL];
    b_copy(copy, sizeof(copy), path);
    char *save;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) {
            continue;
        }
        if (!strcmp(part, "..")) {
            char *last = strrchr(normalized, '/');
            if (last == normalized) {
                normalized[1] = 0;
            } else {
                *last = 0;
            }
            continue;
        }
        if (normalized[1]) {
            strcat(normalized, "/");
        }
        strcat(normalized, part);
    }
    size_t path_length = strlen(path);
    if (path_length > 1 && path[path_length - 1] == '/' && normalized[1]) {
        strcat(normalized, "/");
    }
    *path = 0;
    if (strlen(combined) + strlen(normalized) + strlen(suffix) >= size) {
        return -1;
    }
    snprintf(out, size, "%s%s%s", combined, normalized, suffix);
    return 0;
}
static int header(const char *raw, const char *name, char *out, size_t size) {
    const char *p = strstr(raw, "\r\n");
    if (!p) {
        return 0;
    }
    p += 2;
    size_t n = strlen(name);
    while (*p && strncmp(p, "\r\n", 2)) {
        const char *end = strstr(p, "\r\n");
        if (!end) {
            return 0;
        }
        if (!strncasecmp(p, name, n) && p[n] == ':') {
            p += n + 1;
            while (p < end && isspace((unsigned char)*p)) {
                p++;
            }
            while (end > p && isspace((unsigned char)end[-1])) {
                end--;
            }
            size_t len = end - p;
            if (len >= size) {
                return -1;
            }
            memcpy(out, p, len);
            out[len] = 0;
            return 1;
        }
        p = end + 2;
    }
    return 0;
}
static int chunk_decode(char *body, size_t input, size_t *output) {
    size_t read = 0, write = 0;
    while (read < input) {
        size_t line = read;
        while (line + 1 < input && (body[line] != '\r' || body[line + 1] != '\n')) {
            line++;
        }
        if (line + 1 >= input || line - read > 128) {
            return -1;
        }
        char number[129];
        memcpy(number, body + read, line - read);
        number[line - read] = 0;
        char *end;
        unsigned long n = strtoul(number, &end, 16);
        if (!isxdigit((unsigned char)number[0]) || end == number || (*end && *end != ';') ||
            n > B_SOURCE) {
            return -1;
        }
        read = line + 2;
        if (!n) {
            while (read + 1 < input) {
                size_t end_line = read;
                while (end_line + 1 < input &&
                       (body[end_line] != '\r' || body[end_line + 1] != '\n')) {
                    end_line++;
                }
                if (end_line + 1 >= input || end_line - read > 4096) {
                    return -1;
                }
                if (end_line == read) {
                    *output = write;
                    body[write] = 0;
                    return 0;
                }
                if (!memchr(body + read, ':', end_line - read)) {
                    return -1;
                }
                read = end_line + 2;
            }
            return -1;
        }
        if (n > input - read || input - read - n < 2 || body[read + n] != '\r' ||
            body[read + n + 1] != '\n') {
            return -1;
        }
        memmove(body + write, body + read, n);
        write += n;
        read += n + 2;
    }
    return -1;
}
static int raw_fetch(const char *url, struct b_response *r, char **result) {
    char current[B_URL];
    b_copy(current, sizeof(current), url);
    for (unsigned redirect = 0; redirect < 6; redirect++) {
        if (!strncmp(current, "file://", 7)) {
            int fd = open(current + 7, O_RDONLY);
            if (fd < 0) {
                goto system_error;
            }
            char *body = malloc(B_SOURCE + 1);
            if (!body) {
                close(fd);
                goto system_error;
            }
            size_t used = 0;
            ssize_t n;
            while (used < B_SOURCE && (n = read(fd, body + used, B_SOURCE - used)) > 0) {
                used += n;
            }
            char extra;
            n = used == B_SOURCE ? read(fd, &extra, 1) : n;
            close(fd);
            if (n < 0 || (used == B_SOURCE && n > 0)) {
                free(body);
                b_copy(r->message, sizeof(r->message), "File exceeds 1 MiB or could not be read");
                return -1;
            }
            body[used] = 0;
            *result = body;
            r->length = used;
            b_copy(r->url, sizeof(r->url), current);
            return 0;
        }
        if (strncmp(current, "http://", 7)) {
            b_copy(r->message, sizeof(r->message),
                   "Only HTTP and local file URLs are supported; HTTPS requires TLS");
            return -1;
        }
        const char *start = current + 7, *path = strchr(start, '/');
        if (!path) {
            path = start + strlen(start);
        }
        size_t host_length = path - start;
        if (!host_length || host_length >= 256) {
            goto invalid;
        }
        char host[256];
        memcpy(host, start, host_length);
        host[host_length] = 0;
        unsigned port = 80;
        char *colon = strchr(host, ':');
        if (colon) {
            *colon++ = 0;
            char *end;
            port = strtoul(colon, &end, 10);
            if (*end || !port || port > 65535) {
                goto invalid;
            }
        }
        if (!*host || strchr(host, '@')) {
            goto invalid;
        }
        struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
        if (dns_resolve(host, "10.0.2.3", &address.sin_addr)) {
            goto system_error;
        }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            goto system_error;
        }
        if (connect(fd, (void *)&address, sizeof(address))) {
            close(fd);
            goto system_error;
        }
        char target[B_URL];
        b_copy(target, sizeof(target), *path ? path : "/");
        char *fragment = strchr(target, '#');
        if (fragment) {
            *fragment = 0;
        }
        char request[1536];
        int count =
            snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: KalyBrowser/1.0\r\nAccept: "
                     "text/html,text/css,application/javascript,text/plain\r\nAccept-Encoding: "
                     "identity\r\nConnection: close\r\n\r\n",
                     target, host, colon ? ":" : "", colon ? colon : "");
        if (count < 0 || (unsigned)count >= sizeof(request)) {
            close(fd);
            goto invalid;
        }
        uint64_t deadline = milliseconds() + 15000;
        for (int offset = 0; offset < count;) {
            ssize_t n = send(fd, request + offset, count - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n > 0) {
                offset += n;
            } else if (n < 0 && errno == EAGAIN && milliseconds() < deadline) {
                pause_io();
            } else {
                close(fd);
                goto system_error;
            }
        }
        char *raw = malloc(B_SOURCE + 16385);
        if (!raw) {
            close(fd);
            goto system_error;
        }
        size_t used = 0, header_size = 0, content_length = 0;
        int has_length = 0, finished = 0;
        while (used < B_SOURCE + 16384) {
            ssize_t n = recv(fd, raw + used, B_SOURCE + 16384 - used, MSG_DONTWAIT);
            if (n > 0) {
                used += n;
                raw[used] = 0;
                if (!header_size) {
                    char *end = strstr(raw, "\r\n\r\n");
                    if (end) {
                        header_size = end - raw + 4;
                        if (header_size > 16384) {
                            break;
                        }
                        char length[64];
                        int found = header(raw, "Content-Length", length, sizeof(length));
                        if (found > 0) {
                            char *last;
                            content_length = strtoul(length, &last, 10);
                            if (*last || content_length > B_SOURCE) {
                                break;
                            }
                            has_length = 1;
                        }
                    } else if (used > 16384) {
                        break;
                    }
                }
                if (header_size && has_length && used - header_size >= content_length) {
                    finished = 1;
                    break;
                }
            } else if (!n) {
                finished = 1;
                break;
            } else if (errno == EAGAIN && milliseconds() < deadline) {
                pause_io();
            } else {
                break;
            }
        }
        close(fd);
        if (!finished || !header_size || (has_length && used - header_size < content_length) ||
            used - header_size > B_SOURCE) {
            free(raw);
            b_copy(r->message, sizeof(r->message),
                   "Incomplete HTTP response, timeout or page exceeds 1 MiB");
            return -1;
        }
        if (strncmp(raw, "HTTP/1.", 7) || raw[8] != ' ') {
            free(raw);
            goto invalid;
        }
        int status = atoi(raw + 9);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char location[B_URL], next[B_URL];
            int found = header(raw, "Location", location, sizeof(location));
            free(raw);
            if (found <= 0 || b_url(current, location, next, sizeof(next)) ||
                strncmp(next, "http://", 7)) {
                goto invalid;
            }
            b_copy(current, sizeof(current), next);
            continue;
        }
        if (status < 200 || status >= 300) {
            free(raw);
            snprintf(r->message, sizeof(r->message), "HTTP error %d", status);
            return -1;
        }
        char encoding[128];
        int found = header(raw, "Content-Encoding", encoding, sizeof(encoding));
        if (found < 0 || (found && strcasecmp(encoding, "identity"))) {
            free(raw);
            b_copy(r->message, sizeof(r->message), "Compressed HTTP content is not supported");
            return -1;
        }
        size_t size = has_length ? content_length : used - header_size;
        found = header(raw, "Transfer-Encoding", encoding, sizeof(encoding));
        if (found < 0 || (found && strcasecmp(encoding, "chunked"))) {
            free(raw);
            goto invalid;
        }
        memmove(raw, raw + header_size, size);
        raw[size] = 0;
        if (found && chunk_decode(raw, size, &size)) {
            free(raw);
            b_copy(r->message, sizeof(r->message), "Invalid chunked HTTP response");
            return -1;
        }
        *result = raw;
        r->length = size;
        b_copy(r->url, sizeof(r->url), current);
        printf("KalyBrowser: HTTP %d %s (%lu bytes)\n", status, current, size);
        return 0;
    }
    b_copy(r->message, sizeof(r->message), "Too many HTTP redirects");
    return -1;
invalid:
    b_copy(r->message, sizeof(r->message), "Invalid URL or HTTP response");
    return -1;
system_error:
    snprintf(r->message, sizeof(r->message), "Network/file error: %s", strerror(errno));
    return -1;
}
static void tag_attr(const char *tag, const char *end, const char *name, char *value, size_t size) {
    value[0] = 0;
    const char *p = tag + 1;
    while (p < end && !isspace((unsigned char)*p)) {
        p++;
    }
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '/')) {
            p++;
        }
        const char *start = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '=') {
            p++;
        }
        size_t n = p - start;
        while (p < end && isspace((unsigned char)*p)) {
            p++;
        }
        if (p >= end || *p != '=') {
            continue;
        }
        p++;
        while (p < end && isspace((unsigned char)*p)) {
            p++;
        }
        int quote = *p == '\'' || *p == '"' ? *p++ : 0;
        const char *content = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) {
            p++;
        }
        if (n == strlen(name) && !strncasecmp(start, name, n)) {
            size_t len = p - content;
            if (len >= size) {
                len = size - 1;
            }
            memcpy(value, content, len);
            value[len] = 0;
            return;
        }
        if (quote && p < end) {
            p++;
        }
    }
}
static const char *tag_end(const char *p) {
    int quote = 0;
    for (; *p; p++) {
        if (quote) {
            if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
    }
    return NULL;
}
int b_fetch(const char *url, struct b_response *r, char **body) {
    memset(r, 0, sizeof(*r));
    if (raw_fetch(url, r, body)) {
        return -1;
    }
    char *expanded = malloc(B_SOURCE + 1);
    if (!expanded) {
        free(*body);
        *body = NULL;
        return -1;
    }
    size_t used = 0;
    const char *p = *body;
    unsigned resources = 0;
    while (*p) {
        if (!strncmp(p, "<!--", 4)) {
            const char *end_comment = strstr(p + 4, "-->");
            size_t length = end_comment ? (size_t)(end_comment + 3 - p) : strlen(p);
            if (length > B_SOURCE - used) {
                goto limit;
            }
            memcpy(expanded + used, p, length);
            used += length;
            p += length;
            continue;
        }
        const char *end = NULL;
        char src[B_URL] = "", rel[64] = "";
        int script = 0;
        if (*p == '<' &&
            ((!strncasecmp(p, "<script", 7) && (isspace((unsigned char)p[7]) || p[7] == '>')) ||
             (!strncasecmp(p, "<link", 5) && (isspace((unsigned char)p[5]) || p[5] == '>')))) {
            end = tag_end(p);
            if (end) {
                script = !strncasecmp(p, "<script", 7);
                tag_attr(p, end, script ? "src" : "href", src, sizeof(src));
                tag_attr(p, end, "rel", rel, sizeof(rel));
                if (!script && strcasecmp(rel, "stylesheet")) {
                    src[0] = 0;
                }
            }
        }
        if (script && end && !*src) {
            const char *close = end + 1;
            while (*close && strncasecmp(close, "</script", 8)) {
                close++;
            }
            const char *stop = *close ? strchr(close, '>') : NULL;
            size_t length = stop ? (size_t)(stop + 1 - p) : strlen(p);
            if (length > B_SOURCE - used) {
                goto limit;
            }
            memcpy(expanded + used, p, length);
            used += length;
            p += length;
            continue;
        }
        if (*src && resources++ < 8) {
            char target[B_URL];
            struct b_response asset;
            char *data = NULL;
            if (b_url(r->url, src, target, sizeof(target)) ||
                (strncmp(r->url, "http://", 7) == 0 && strncmp(target, "http://", 7)) ||
                raw_fetch(target, &asset, &data)) {
                snprintf(r->message, sizeof(r->message),
                         "A stylesheet or script could not be loaded");
                free(data);
                free(expanded);
                free(*body);
                *body = NULL;
                return -1;
            }
            const char *opening = script ? "<script>" : "<style>",
                       *closing = script ? "</script>" : "</style>";
            size_t length = strlen(opening) + asset.length + strlen(closing);
            if (length > B_SOURCE - used) {
                free(data);
                goto limit;
            }
            memcpy(expanded + used, opening, strlen(opening));
            used += strlen(opening);
            memcpy(expanded + used, data, asset.length);
            used += asset.length;
            free(data);
            memcpy(expanded + used, closing, strlen(closing));
            used += strlen(closing);
            p = end + 1;
            if (script) {
                const char *close = p;
                while (*close && strncasecmp(close, "</script", 8)) {
                    close++;
                }
                if (*close) {
                    const char *stop = strchr(close, '>');
                    p = stop ? stop + 1 : close + strlen(close);
                }
            }
        } else {
            if (*src && resources > 8) {
                goto limit;
            }
            if (used == B_SOURCE) {
                goto limit;
            }
            expanded[used++] = *p++;
        }
    }
    expanded[used] = 0;
    free(*body);
    *body = expanded;
    r->length = used;
    r->ok = 1;
    return 0;
limit:
    free(expanded);
    free(*body);
    *body = NULL;
    b_copy(r->message, sizeof(r->message), "Page expansion or external resource limit exceeded");
    return -1;
}
