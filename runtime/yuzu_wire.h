/* yuzu room protocol v1, bounded network-order serialization.
 * Protocol reference: yuzu-era source commit 48d6226115f849c465202692b6988e1efb7d2410.
 * Eden 0.2.0 socket enum reference: commit ee188168c12188340961314e1dfc70a4f7ad4f7a.
 * This implementation is client-only. No host ABI structs are copied onto the wire. */
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { YW_JOIN=1, YW_JOIN_OK=2, YW_ROOM=3, YW_GAME=4, YW_PROXY=5, YW_LDN=6,
       YW_NAME_COLLISION=8, YW_IP_COLLISION=9, YW_VERSION=10, YW_PASSWORD=11,
       YW_CLOSED=12, YW_FULL=13, YW_KICKED=15, YW_BANNED=16, YW_JOIN_MOD=24,
       YW_PROTOCOL=1, YW_MAX_PACKET=131072, YW_MAX_UDP=65507, YW_MAX_MEMBERS=254 };
typedef struct YWire { uint8_t *data; size_t size, at; int ok; } YWire;
YWire yw_buffer(void *data, size_t size);
void yw_put(YWire *w, uint64_t value, unsigned bytes);
uint64_t yw_get(YWire *w, unsigned bytes);
void yw_bytes(YWire *w, const void *data, size_t len);
const uint8_t *yw_take(YWire *w, size_t len);
void yw_string(YWire *w, const char *text);
int yw_skip_string(YWire *w);
size_t yw_join(void *out, size_t cap, const char *name, const char *password, const char *token);
size_t yw_ldn(void *out, size_t cap, unsigned type, uint32_t src, uint32_t dst, const void *data, size_t len);
/* The room version stays 1, but Eden 0.2.0 shifted the socket enums. */
enum { YW_PROXY_YUZU, YW_PROXY_EDEN };
size_t yw_proxy_profile(void *out, size_t cap, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len, unsigned profile);
size_t yw_proxy(void *out, size_t cap, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len);
int yw_room(const void *data, size_t len, uint32_t *members, unsigned *count);
typedef struct YPacket { unsigned kind, type, broadcast; uint32_t src, dst; uint16_t sport, dport; const uint8_t *data; size_t size; } YPacket;
int yw_packet(const void *data, size_t len, YPacket *out);
