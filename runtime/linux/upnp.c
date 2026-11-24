/* UPnP port mapping on Linux, for the netplay relay (net_relay.c): what Windows' UPnP NAT API
 * does there, by hand. Find the router (SSDP: a multicast M-SEARCH, answered with the address of
 * its description), read which control URL its WAN connection service has, and ask it with
 * SOAP to forward a UDP port to this machine (and for its outside address); the mapping is
 * removed again at exit. */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef unsigned short u16;

#ifdef __APPLE__
/* macOS refuses the local network (EHOSTUNREACH, or EPERM) until the user lets the game reach it,
 * as for net_relay.c's packets: the first refusal is told, with where. */
static void refused(const char *where, int err) {
    static int told;
    if ((err == EHOSTUNREACH || err == EPERM) && !__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "[net] UPnP: macOS keeps the game off your local network (%s: %s): allow OpenSmash in System Settings > "
                        "Privacy & Security > Local Network\n", where, strerror(err));
}
#define REFUSED(where) refused(where, errno)
#else
#define REFUSED(where) ((void)0)
#endif

static char control_host[128], control_path[512], service[128];
static u16 control_port, mapped_port;

static void timeouts(int s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* connect(), for at most ms: a router that does not answer otherwise holds it for TCP's own
 * timeout (macOS does not apply SO_SNDTIMEO to it: 75 s), and the mapping's removal at exit with
 * it. 0 connected; else -1, errno saying why. */
static int connect_within(int s, const struct sockaddr *to, socklen_t len, int ms) {
    int flags = fcntl(s, F_GETFL);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK)) return connect(s, to, len);
    int r = connect(s, to, len);
    if (r && errno == EINPROGRESS) {
        struct pollfd p = {s, POLLOUT, 0};
        while ((r = poll(&p, 1, ms)) < 0 && errno == EINTR) {}
        int err = 0;
        socklen_t n = sizeof err;
        if (!r) err = ETIMEDOUT;
        else if (r > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &n)) err = errno;
        r = err || r < 0 ? -1 : 0;
        if (err) errno = err;
    }
    int saved = errno;
    fcntl(s, F_SETFL, flags);   /* (blocking again for send and recv, which keep to SO_SNDTIMEO and SO_RCVTIMEO) */
    errno = saved;
    return r;
}

/* "http://host[:port]/path" */
static int parse_url(const char *url, char *host, size_t hn, u16 *port, char *path, size_t pn) {
    if (strncasecmp(url, "http://", 7)) return 0;
    const char *h = url + 7, *slash = strchr(h, '/'), *end = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', (size_t)(end - h));
    size_t len = (size_t)((colon ? colon : end) - h);
    if (!len || len >= hn) return 0;
    memcpy(host, h, len);
    host[len] = 0;
    *port = colon ? (u16)atoi(colon + 1) : 80;
    snprintf(path, pn, "%s", slash ? slash : "/");
    return 1;
}

/* One HTTP request (connection closed after); the whole response into out. Returns the status. */
static int http(const char *host, u16 port, const char *request, char *out, size_t cap) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ports[8];
    snprintf(ports, sizeof ports, "%u", port);
    if (getaddrinfo(host, ports, &hints, &res) || !res) return 0;
    int s = socket(AF_INET, SOCK_STREAM, 0), status = 0;
    if (s >= 0) {
        timeouts(s, 3000);
        if (!connect_within(s, res->ai_addr, res->ai_addrlen, 3000) && send(s, request, strlen(request), MSG_NOSIGNAL) == (ssize_t)strlen(request)) {
            size_t n = 0;
            for (ssize_t r; n + 1 < cap && (r = recv(s, out + n, cap - 1 - n, 0)) > 0;) n += (size_t)r;
            out[n] = 0;
            sscanf(out, "HTTP/%*s %d", &status);
        } else REFUSED(host);
        close(s);
    }
    freeaddrinfo(res);
    return status;
}

/* The text between <tag> and </tag> after `from`, into out. */
static const char *element(const char *from, const char *tag, char *out, size_t n) {
    char open[64], close_tag[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close_tag, sizeof close_tag, "</%s>", tag);
    const char *a = strcasestr(from, open);
    if (!a) return NULL;
    a += strlen(open);
    const char *b = strcasestr(a, close_tag);
    if (!b) return NULL;
    size_t len = (size_t)(b - a) < n - 1 ? (size_t)(b - a) : n - 1;
    memcpy(out, a, len);
    out[len] = 0;
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\r' || out[len - 1] == '\n')) out[--len] = 0;
    return b;
}

static int soap(const char *action, const char *args, char *reply, size_t cap) {
    char body[2048], request[4096];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
             action, service, args, action);
    snprintf(request, sizeof request,
             "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             control_path, control_host, control_port, service, action, strlen(body), body);
    return http(control_host, control_port, request, reply, cap);
}

/* Find the router's WAN connection service: 1 found. */
static int discover(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    timeouts(s, 2500);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    static const char *const targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                          "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                          "urn:schemas-upnp-org:service:WANIPConnection:1"};
    for (unsigned i = 0; i < 3; i++) {
        char m[256];
        snprintf(m, sizeof m, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nST: %s\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n", targets[i]);
        if (sendto(s, m, strlen(m), 0, (struct sockaddr *)&to, sizeof to) < 0) REFUSED("239.255.255.250");
    }
    char location[512] = "";
    for (int tries = 0; tries < 8 && !location[0]; tries++) {
        char buf[2048];
        ssize_t n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        const char *l = strcasestr(buf, "\nLOCATION:");
        if (!l) continue;
        l += 10;
        while (*l == ' ') l++;
        size_t len = strcspn(l, "\r\n");
        if (len < sizeof location) { memcpy(location, l, len); location[len] = 0; }
    }
    close(s);
    if (!location[0]) return 0;
    char host[128], path[512], request[1024];
    u16 port;
    if (!parse_url(location, host, sizeof host, &port, path, sizeof path)) return 0;
    snprintf(request, sizeof request, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, host, port);
    static char desc[65536];
    if (http(host, port, request, desc, sizeof desc) != 200) return 0;
    static const char *const services[] = {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                           "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (unsigned i = 0; i < 3; i++) {
        const char *at = strstr(desc, services[i]);
        char url[512];
        if (!at || !element(at, "controlURL", url, sizeof url)) continue;
        snprintf(service, sizeof service, "%s", services[i]);
        snprintf(control_host, sizeof control_host, "%s", host);
        control_port = port;
        if (!strncasecmp(url, "http://", 7)) return parse_url(url, control_host, sizeof control_host, &control_port, control_path, sizeof control_path);
        char base[512];   /* relative to URLBase, else to where the description came from */
        if (element(desc, "URLBase", base, sizeof base) && parse_url(base, control_host, sizeof control_host, &control_port, path, sizeof path)) {}
        snprintf(control_path, sizeof control_path, "%s%s", url[0] == '/' ? "" : "/", url);
        return 1;
    }
    return 0;
}

static void delete_mapping(u16 port) {
    char args[256], reply[4096];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>", port);
    soap("DeletePortMapping", args, reply, sizeof reply);
}

/* 1 mapped (public_ip filled when the router says), 2 no UPnP router, 3 the router refused. */
int rt_upnp_add(u16 port, const char *lan_ip, const char *what, char *public_ip, size_t n) {
    if (!discover()) return 2;
    delete_mapping(port);   /* (a stale one from an earlier run) */
    char args[1024], reply[8192];
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>",
             port, port, lan_ip, what);
    if (soap("AddPortMapping", args, reply, sizeof reply) != 200) return 3;
    __atomic_store_n(&mapped_port, port, __ATOMIC_RELEASE);
    if (soap("GetExternalIPAddress", "", reply, sizeof reply) == 200) element(reply, "NewExternalIPAddress", public_ip, n);
    return 1;
}

void rt_upnp_remove(void) {   /* (once: the window's quit and svcExitProcess may both come here) */
    u16 port = __atomic_exchange_n(&mapped_port, 0, __ATOMIC_ACQ_REL);
    if (port) delete_mapping(port);
}
