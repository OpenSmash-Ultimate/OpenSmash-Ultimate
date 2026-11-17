/* Optional Windows yuzu-compatible ROOM CLIENT. No server is started here.
 * Dependencies are loaded from build/yuzu beside ssbu.exe, never from DLL search PATH.
 * Build them using python tools/yuzu_client_deps.py. See docs/YUZU_CLIENT.md. */
#include "net.h"
#include "yuzu_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <enet/enet.h>
#include <windows.h>

typedef struct Outgoing { struct Outgoing *next; size_t len; unsigned generation; uint8_t data[]; } Outgoing;
static struct {
    SRWLOCK lock;
    volatile LONG ip, stop, generation;
    HANDLE thread;
    unsigned nmember, proxy_profile; uint32_t members[YW_MAX_MEMBERS];
    Outgoing *head,*tail; size_t queued;
    char address[256], name[33], password[129], token[4097], detail[256]; uint16_t port;
    uint64_t since; volatile LONG terminal; int initialized;
    HMODULE enet,zstd;
    int (*init)(void); void (*deinit)(void); enet_uint32 (*version)(void);
    ENetHost *(*host_create)(const ENetAddress *,size_t,size_t,enet_uint32,enet_uint32);
    void (*host_destroy)(ENetHost *);
    ENetPeer *(*connect)(ENetHost *,const ENetAddress *,size_t,enet_uint32);
    int (*service)(ENetHost *,ENetEvent *,enet_uint32);
    int (*set_host)(ENetAddress *,const char *);
    ENetPacket *(*packet_create)(const void *,size_t,enet_uint32);
    void (*packet_destroy)(ENetPacket *);
    int (*peer_send)(ENetPeer *,enet_uint8,ENetPacket *);
    void (*flush)(ENetHost *);
    void (*disconnect)(ENetPeer *,enet_uint32);
    void (*reset)(ENetPeer *);
    size_t (*compress)(void *,size_t,const void *,size_t,int);
    size_t (*decompress)(void *,size_t,const void *,size_t);
    unsigned (*is_error)(size_t);
} Y;

static void detail(const char *s) {
    AcquireSRWLockExclusive(&Y.lock);
    int changed=strcmp(Y.detail,s)!=0;
    snprintf(Y.detail,sizeof Y.detail,"%s",s);
    ReleaseSRWLockExclusive(&Y.lock);
    /* Lifecycle only, no credentials or packet contents; consumed by the launcher UI. */
    if(changed) fprintf(stderr,"[yuzu] state: %s\n",s);
}
static int selected(void) { const char *s=getenv("SSBU_NET_BACKEND"); return s && !_stricmp(s,"yuzu"); }
static uint32_t local_ip(void) { return (uint32_t)InterlockedCompareExchange(&Y.ip,0,0); }
static const char *player_name(void) { return Y.name[0]?Y.name:"OpenSmash"; }
static int unreachable(void) { return !local_ip() && (Y.terminal || GetTickCount64()-Y.since>5000); }
static int has_peer(uint32_t ip) {
    if (!local_ip()) return 0;
    int found=0; AcquireSRWLockShared(&Y.lock);
    for(unsigned i=0;i<Y.nmember;i++) if(Y.members[i]==ip) found=1;
    ReleaseSRWLockShared(&Y.lock); return found;
}
static void discard_queue(void) { /* lock held */
    while(Y.head) { Outgoing *p=Y.head; Y.head=p->next; free(p); }
    Y.tail=NULL; Y.queued=0;
}
static void session_reset(void) {
    InterlockedExchange(&Y.ip,0); InterlockedIncrement(&Y.generation);
    AcquireSRWLockExclusive(&Y.lock); Y.nmember=0; discard_queue(); ReleaseSRWLockExclusive(&Y.lock);
    /* The game may exit while holding its own LDN lock. No guest callbacks at shutdown. */
    if(!Y.stop && net_backend_ldn_sink) net_backend_ldn_sink(0,YLDN_RESET,NULL,0);
}
static void enqueue(const void *data,size_t len,unsigned generation) {
    if(!len || len>YW_MAX_PACKET || !local_ip()) return;
    Outgoing *p=malloc(sizeof *p+len); if(!p) return;
    p->len=len; p->next=NULL; p->generation=generation; memcpy(p->data,data,len);
    AcquireSRWLockExclusive(&Y.lock);
    if(!Y.ip || Y.stop || generation!=(unsigned)Y.generation || Y.queued+len>2*1024*1024) { ReleaseSRWLockExclusive(&Y.lock); free(p); return; }
    if(Y.tail) Y.tail->next=p; else Y.head=p; Y.tail=p; Y.queued+=len;
    ReleaseSRWLockExclusive(&Y.lock);
}
static int send_now(ENetPeer *peer,const void *data,size_t len) {
    if(!len) return 0;
    ENetPacket *p=Y.packet_create(data,len,ENET_PACKET_FLAG_RELIABLE);
    if(!p) return 0;
    if(Y.peer_send(peer,0,p)<0) { Y.packet_destroy(p); return 0; }
    return 1;
}
/* Only metadata is logged, with separate discovery/control/UDP budgets so scans
 * cannot hide a later Connect. No credentials or compressed/game payloads. */
static void trace_packet(int outgoing,const char *result,const void *bytes,size_t len) {
    if(!net_log() || !len) return;
    const uint8_t *b=bytes;
    if(b[0]!=YW_LDN && b[0]!=YW_PROXY) return;
    YPacket p; int valid=yw_packet(bytes,len,&p);
    unsigned group=b[0]==YW_PROXY?0:valid && p.type<=YLDN_SCAN_RESP?1:2;
    static volatile LONG counts[2][3];
    LONG count=InterlockedIncrement(&counts[!!outgoing][group]);
    LONG limit=group==1?16:128;
    if(count>limit) return;
    if(valid) {
        static const char *types[]={"Scan","ScanResp","Connect","SyncNetwork","Disconnect","DestroyNetwork"};
        fprintf(stderr,"[yuzu] %s %s %s src=%u.%u.%u.%u:%u dst=%u.%u.%u.%u:%u broadcast=%u bytes=%zu result=%s\n",
                outgoing?"tx":"rx",p.kind==YW_LDN?"ldn":"udp",p.kind==YW_LDN?types[p.type]:b[1]==1?"ProxyPacket/eden":"ProxyPacket/yuzu",
                p.src>>24,p.src>>16&255,p.src>>8&255,p.src&255,p.sport,
                p.dst>>24,p.dst>>16&255,p.dst>>8&255,p.dst&255,p.dport,p.broadcast,p.size,result);
    } else fprintf(stderr,"[yuzu] %s malformed %s frame bytes=%zu result=%s\n",outgoing?"tx":"rx",b[0]==YW_LDN?"ldn":"udp",len,result);
    if(count==limit) fprintf(stderr,"[yuzu] %s %s trace limit reached (%ld packets)\n",outgoing?"tx":"rx",group==0?"UDP":group==1?"discovery":"LDN control",limit);
}
static void drain(ENetPeer *peer) {
    for(unsigned i=0;i<64;i++) {
        AcquireSRWLockExclusive(&Y.lock); Outgoing *p=Y.head;
        if(p) { Y.head=p->next; if(!Y.head) Y.tail=NULL; Y.queued-=p->len; }
        ReleaseSRWLockExclusive(&Y.lock); if(!p) break;
        if(p->generation==(unsigned)Y.generation && local_ip()) {
            int sent=send_now(peer,p->data,p->len);
            trace_packet(1,sent?"submitted-to-enet":"send-failed",p->data,p->len);
        } else trace_packet(1,"stale-session",p->data,p->len);
        free(p);
    }
}
static const char *refusal(unsigned id) {
    switch(id) {
    case YW_NAME_COLLISION:return "nickname is invalid or already in use";
    case YW_IP_COLLISION:return "virtual IP is in use";
    case YW_VERSION:return "room protocol version differs (client expects version 1)";
    case YW_PASSWORD:return "wrong room password";
    case YW_CLOSED:return "room closed";
    case YW_FULL:return "room is full";
    case YW_KICKED:return "removed by room host";
    case YW_BANNED:return "banned from room";
    default:return NULL;
    }
}
static int receive(const ENetPacket *packet,int *welcomed) {
    if(!packet->dataLength || packet->dataLength>YW_MAX_PACKET) return 1;
    const uint8_t *b=packet->data; size_t len=packet->dataLength; const char *why=refusal(b[0]);
    if(why) { detail(why); Y.terminal=1; fprintf(stderr,"[yuzu] %s\n",why); return 0; }
    if(b[0]==YW_ROOM) {
        uint32_t ips[YW_MAX_MEMBERS]; unsigned n=0;
        if(!yw_room(b,len,ips,&n)) {
            detail("unsupported or malformed room information format"); Y.terminal=1; return 0;
        }
        AcquireSRWLockExclusive(&Y.lock); memcpy(Y.members,ips,n*sizeof *ips); Y.nmember=n; ReleaseSRWLockExclusive(&Y.lock);
        return 1;
    }
    if((b[0]==YW_JOIN_OK || b[0]==YW_JOIN_MOD) && !*welcomed && len==5) {
        YWire w=yw_buffer((void *)(b+1),4); uint32_t ip=(uint32_t)yw_get(&w,4);
        if(!ip || ip==0xffffffffu || (ip>>24)>=224 || ip>>24==127) return 0;
        InterlockedExchange(&Y.ip,(LONG)ip); *welcomed=1; detail("connected");
        fprintf(stderr,"[yuzu] joined %s:%u as %s, virtual IP %u.%u.%u.%u (room protocol 1)\n",Y.address,Y.port,Y.name,ip>>24,ip>>16&255,ip>>8&255,ip&255);
        fprintf(stderr,"[yuzu] game UDP wire profile=%s (IPv4=%u UDP=%u)\n",
                Y.proxy_profile==YW_PROXY_EDEN?"eden":"yuzu",Y.proxy_profile==YW_PROXY_EDEN?1:0,Y.proxy_profile==YW_PROXY_EDEN?3:2);
        uint8_t game[256]; YWire g=yw_buffer(game,sizeof game); yw_put(&g,YW_GAME,1);
        yw_string(&g,"Super Smash Bros. Ultimate"); yw_put(&g,0x01006A800016E000ull,8); yw_string(&g,"13.0.5");
        enqueue(game,g.ok?g.at:0,(unsigned)Y.generation); return 1;
    }
    if(!*welcomed) return 1;
    YPacket p;
    if(!yw_packet(b,len,&p)) { trace_packet(0,"invalid-wire",b,len); return 1; }
    if(!has_peer(p.src)) { trace_packet(0,"unknown-room-member",b,len); return 1; }
    if(p.dst!=local_ip() && !p.broadcast) { trace_packet(0,"other-destination",b,len); return 1; }
    if(p.kind==YW_LDN) {
        trace_packet(0,net_backend_ldn_sink?"to-ldn":"no-ldn-handler",b,len);
        if(net_backend_ldn_sink) net_backend_ldn_sink(p.src,p.type,p.data,(uint32_t)p.size);
    } else {
        uint8_t data[YW_MAX_UDP];
        size_t n=Y.decompress(data,sizeof data,p.data,p.size);
        if(!Y.is_error(n) && n<=sizeof data) {
            trace_packet(0,"decompressed-to-sockets",b,len);
            vsock_deliver(p.src,p.sport,p.broadcast?net_broadcast_ip():p.dst,p.dport,data,(uint32_t)n);
        } else trace_packet(0,"invalid-zstd-or-oversize",b,len);
    }
    return 1;
}
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    ENetHost *host=Y.host_create(NULL,1,1,0,0);
    if(!host) { detail("could not create client network endpoint"); Y.terminal=1; return 0; }
    host->maximumPacketSize=YW_MAX_PACKET; host->maximumWaitingData=2*1024*1024;
    uint8_t join[4608]; size_t join_len=yw_join(join,sizeof join,Y.name,Y.password,Y.token);
    while(!Y.stop && !Y.terminal) {
        ENetAddress address={0}; address.port=Y.port;
        detail("connecting");
        if(Y.set_host(&address,Y.address)) { detail("could not resolve server address"); goto retry; }
        ENetPeer *peer=Y.connect(host,&address,1,0);
        if(!peer) { detail("could not start room connection"); goto retry; }
        uint64_t deadline=GetTickCount64()+8000; int welcomed=0,alive=1;
        while(!Y.stop && alive) {
            ENetEvent event; int result=Y.service(host,&event,10);
            if(result<0) { detail("network service failed"); break; }
            if(result>0) switch(event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                detail("waiting for room admission"); alive=send_now(peer,join,join_len); break;
            case ENET_EVENT_TYPE_RECEIVE:
                alive=receive(event.packet,&welcomed); Y.packet_destroy(event.packet); break;
            case ENET_EVENT_TYPE_DISCONNECT: detail("connection lost; retrying"); alive=0; break;
            default: break;
            }
            if(welcomed) drain(peer);
            else if(GetTickCount64()>deadline) { detail("room handshake timed out; retrying"); break; }
            Y.flush(host);
        }
        if(Y.stop && welcomed) { Y.disconnect(peer,0); Y.flush(host); }
        Y.reset(peer); session_reset();
retry:
        for(unsigned i=0;i<20 && !Y.stop && !Y.terminal;i++) Sleep(100);
    }
    session_reset(); Y.host_destroy(host); return 0;
}
static int load_api(void) {
    char base[32768],path[32768]; const char *custom=getenv("SSBU_YUZU_LIBDIR");
    if(custom && *custom) {
        DWORD n=GetFullPathNameA(custom,sizeof base,base,NULL); if(!n || n>=sizeof base) return 0;
    } else {
        DWORD n=GetModuleFileNameA(NULL,base,sizeof base); if(!n || n>=sizeof base) return 0;
        char *slash=strrchr(base,'\\'); if(!slash) return 0; *slash=0;
        if(strlen(base)+6>=sizeof base) return 0; strcat(base,"\\yuzu");
    }
    snprintf(path,sizeof path,"%s\\enet.dll",base); Y.enet=LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    snprintf(path,sizeof path,"%s\\libzstd.dll",base); Y.zstd=LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!Y.enet || !Y.zstd) { detail("missing client libraries: run python tools/yuzu_client_deps.py"); return 0; }
#define API(field,lib,symbol) do { *(FARPROC *)&Y.field=GetProcAddress(Y.lib,symbol); if(!Y.field) { detail("incompatible client library: " symbol); return 0; } } while(0)
    API(version,enet,"enet_linked_version");
    if(Y.version()!=ENET_VERSION) { detail("ENet DLL version differs: rebuild tools/yuzu_client_deps.py"); return 0; }
    API(init,enet,"enet_initialize"); API(deinit,enet,"enet_deinitialize"); API(host_create,enet,"enet_host_create");
    API(host_destroy,enet,"enet_host_destroy"); API(connect,enet,"enet_host_connect"); API(service,enet,"enet_host_service");
    API(set_host,enet,"enet_address_set_host"); API(packet_create,enet,"enet_packet_create"); API(packet_destroy,enet,"enet_packet_destroy");
    API(peer_send,enet,"enet_peer_send"); API(flush,enet,"enet_host_flush"); API(disconnect,enet,"enet_peer_disconnect"); API(reset,enet,"enet_peer_reset");
    API(compress,zstd,"ZSTD_compress"); API(decompress,zstd,"ZSTD_decompress"); API(is_error,zstd,"ZSTD_isError");
#undef API
    return 1;
}
static void stop(void) {
    InterlockedExchange(&Y.stop,1);
    if(Y.thread) { WaitForSingleObject(Y.thread,INFINITE); CloseHandle(Y.thread); Y.thread=NULL; }
    AcquireSRWLockExclusive(&Y.lock); discard_queue(); ReleaseSRWLockExclusive(&Y.lock);
    if(Y.initialized && Y.deinit) { Y.deinit(); Y.initialized=0; }
    if(Y.enet) { FreeLibrary(Y.enet); Y.enet=NULL; } if(Y.zstd) { FreeLibrary(Y.zstd); Y.zstd=NULL; }
}
static void start(void) {
    Y.since=GetTickCount64();
    const char *server=getenv("SSBU_YUZU_SERVER"),*name=getenv("SSBU_NET_NAME"),*password=getenv("SSBU_YUZU_PASSWORD"),*token=getenv("SSBU_YUZU_TOKEN");
    const char *profile=getenv("SSBU_YUZU_PROXY_PROFILE");
    if(!profile || !*profile || !strcmp(profile,"eden")) Y.proxy_profile=YW_PROXY_EDEN;
    else if(!strcmp(profile,"yuzu")) Y.proxy_profile=YW_PROXY_YUZU;
    else { detail("SSBU_YUZU_PROXY_PROFILE must be eden or yuzu"); goto fail; }
    const char *nickname=name&&*name?name:"OpenSmash";
    if(strlen(nickname)<4 || strlen(nickname)>20 || strspn(nickname," ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")!=strlen(nickname)) {
        detail("nickname must be 4-20 ASCII letters, digits, spaces, dots, underscores or hyphens"); goto fail;
    }
    snprintf(Y.name,sizeof Y.name,"%s",nickname); Y.port=24872;
    if(!server || !*server || strlen(server)>=sizeof Y.address) { detail("set SSBU_YUZU_SERVER=host:port"); goto fail; }
    snprintf(Y.address,sizeof Y.address,"%s",server);
    char *colon=strrchr(Y.address,':');
    if(colon) {
        char *end; long port=strtol(colon+1,&end,10);
        if(*end || end==colon+1 || port<1 || port>65535) { detail("invalid server port"); goto fail; }
        *colon=0; Y.port=(uint16_t)port;
    }
    if(!Y.address[0] || strspn(Y.address,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")!=strlen(Y.address)) { detail("expected IPv4 address or hostname, with optional :port"); goto fail; }
    if((password && strlen(password)>=sizeof Y.password) || (token && strlen(token)>=sizeof Y.token)) { detail("room password or token is too long"); goto fail; }
    snprintf(Y.password,sizeof Y.password,"%s",password?password:""); snprintf(Y.token,sizeof Y.token,"%s",token?token:"");
    const char *rb=getenv("SSBU_RB");
    if(rb && *rb && strcmp(rb,"0")) { detail("yuzu rooms require SSBU_RB=0 (mixed rollback is unsupported)"); goto fail; }
    if(!load_api() || Y.init()) { if(!Y.detail[0]) detail("could not initialize ENet"); goto fail; }
    Y.initialized=1; atexit(stop); Y.thread=CreateThread(NULL,0,worker,NULL,0,NULL);
    if(!Y.thread) { detail("could not start client thread"); goto fail; } return;
fail:
    Y.terminal=1; fprintf(stderr,"[yuzu] %s\n",Y.detail);
    if (!Y.thread) stop();
}
static void send_data(uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,const void *data,uint32_t len) {
    unsigned generation=(unsigned)Y.generation;
    if(!local_ip() || src!=local_ip() || len>YW_MAX_UDP) return;
    uint8_t compressed[YW_MAX_PACKET],wire[YW_MAX_PACKET];
    size_t n=Y.compress(compressed,sizeof compressed,data,len,1);
    if(Y.is_error(n)) return;
    enqueue(wire,yw_proxy_profile(wire,sizeof wire,src,sport,dst,dport,compressed,n,Y.proxy_profile),generation);
}
static void send_ldn(uint32_t dst,unsigned type,const void *data,uint32_t len) {
    unsigned generation=(unsigned)Y.generation;
    uint8_t wire[0x480+32]; enqueue(wire,yw_ldn(wire,sizeof wire,type,local_ip(),dst,data,len),generation);
}
static void status(char *out,size_t cap) {
    AcquireSRWLockShared(&Y.lock); uint32_t ip=local_ip();
    if(ip) snprintf(out,cap,"yuzu room: %s:%u, %u members - %s (%u.%u.%u.%u)",Y.address,Y.port,Y.nmember,Y.name,ip>>24,ip>>16&255,ip>>8&255,ip&255);
    else snprintf(out,cap,"yuzu room: %s",Y.detail[0]?Y.detail:"starting");
    ReleaseSRWLockShared(&Y.lock);
}
static const NetBackend backend={selected,start,local_ip,player_name,unreachable,send_data,send_ldn,has_peer,status};
__attribute__((constructor)) static void install(void) { net_backend_candidate=&backend; }
#else
/* Other platforms keep native networking and explicitly report this optional mode. */
static int selected(void) { const char *v=getenv("SSBU_NET_BACKEND"); return v && !strcmp(v,"yuzu"); }
static void start(void) { fprintf(stderr,"[yuzu] this client build currently requires Windows\n"); }
static uint32_t local_ip(void) { return 0; }
static const char *player_name(void) { return "OpenSmash"; }
static int unavailable(void) { return 1; }
static void send_data(uint32_t a,uint16_t b,uint32_t c,uint16_t d,const void *e,uint32_t f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
static void send_ldn(uint32_t a,unsigned b,const void *c,uint32_t d) { (void)a;(void)b;(void)c;(void)d; }
static int has_peer(uint32_t a) { (void)a;return 0; }
static void status(char *s,size_t n) { snprintf(s,n,"yuzu room: this client build requires Windows"); }
static const NetBackend backend={selected,start,local_ip,player_name,unavailable,send_data,send_ldn,has_peer,status};
__attribute__((constructor)) static void install(void) { net_backend_candidate=&backend; }
#endif
