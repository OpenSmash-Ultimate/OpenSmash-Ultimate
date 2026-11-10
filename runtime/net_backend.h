/* Optional client transport. Native-only test programs need no extra dependencies. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef void (*NetLdnSink)(uint32_t from, unsigned type, const void *data, uint32_t len);
enum { YLDN_SCAN, YLDN_SCAN_RESP, YLDN_CONNECT, YLDN_SYNC, YLDN_DISCONNECT, YLDN_DESTROY, YLDN_RESET = 255 };
typedef struct NetBackend {
    int (*selected)(void);
    void (*start)(void);
    uint32_t (*ip)(void);
    const char *(*name)(void);
    int (*unreachable)(void);
    void (*send)(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, uint32_t len);
    void (*ldn_send)(uint32_t dst, unsigned type, const void *data, uint32_t len);
    int (*has_peer)(uint32_t ip);
    void (*status)(char *out, size_t cap);
} NetBackend;
extern const NetBackend *net_backend_candidate, *net_backend;
extern NetLdnSink net_backend_ldn_sink;
