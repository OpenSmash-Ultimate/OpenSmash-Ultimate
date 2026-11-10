/* See yuzu_wire.h. Protocol-compatible codecs with bounded allocation-free reads. */
#include "yuzu_wire.h"
#include <string.h>
YWire yw_buffer(void *data, size_t size) { YWire w = {(uint8_t *)data, size, 0, 1}; return w; }
void yw_put(YWire *w, uint64_t value, unsigned n) {
    if (n>8 || !w->ok || n>w->size-w->at) { w->ok=0; return; }
    for (unsigned i=n;i;i--) w->data[w->at++]=(uint8_t)(value>>((i-1)*8));
}
const uint8_t *yw_take(YWire *w, size_t n) {
    if (!w->ok || n>w->size-w->at) { w->ok=0; return NULL; }
    const uint8_t *p=w->data+w->at; w->at+=n; return p;
}
uint64_t yw_get(YWire *w, unsigned n) {
    if (n>8) { w->ok=0; return 0; }
    const uint8_t *p=yw_take(w,n); uint64_t v=0;
    if (p) for (unsigned i=0;i<n;i++) v=(v<<8)|p[i];
    return v;
}
void yw_bytes(YWire *w, const void *data, size_t n) {
    if (!w->ok || n>w->size-w->at || (n && !data)) { w->ok=0; return; }
    if (n) memcpy(w->data+w->at,data,n); w->at+=n;
}
void yw_string(YWire *w, const char *s) {
    size_t n=s?strlen(s):0;
    if (n>4096) { w->ok=0; return; }
    yw_put(w,n,4); yw_bytes(w,s,n);
}
int yw_skip_string(YWire *w) {
    uint64_t n=yw_get(w,4);
    if (n>4096) { w->ok=0; return 0; }
    return yw_take(w,(size_t)n)!=NULL;
}
size_t yw_join(void *out,size_t cap,const char *name,const char *password,const char *token) {
    YWire w=yw_buffer(out,cap); yw_put(&w,YW_JOIN,1); yw_string(&w,name);
    yw_put(&w,0xffffffffu,4); yw_put(&w,YW_PROTOCOL,4); yw_string(&w,password); yw_string(&w,token);
    return w.ok?w.at:0;
}
size_t yw_ldn(void *out,size_t cap,unsigned type,uint32_t src,uint32_t dst,const void *data,size_t len) {
    static const unsigned sizes[]={0,0x480,0x40,0x480,0x40,0};
    if (type>5 || len!=sizes[type]) return 0;
    YWire w=yw_buffer(out,cap); yw_put(&w,YW_LDN,1); yw_put(&w,type,1);
    yw_put(&w,src,4); yw_put(&w,dst,4); yw_put(&w,dst==0xffffffffu,1);
    yw_put(&w,len,4); yw_bytes(&w,data,len); return w.ok?w.at:0;
}
size_t yw_proxy(void *out,size_t cap,uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,const void *data,size_t len) {
    return yw_proxy_profile(out,cap,src,sport,dst,dport,data,len,YW_PROXY_YUZU);
}
size_t yw_proxy_profile(void *out,size_t cap,uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,const void *data,size_t len,unsigned profile) {
    if (len>YW_MAX_PACKET-32 || (profile!=YW_PROXY_YUZU && profile!=YW_PROXY_EDEN)) return 0;
    /* Eden added an Unspecified entry to both enums without bumping room protocol 1. */
    unsigned family=profile==YW_PROXY_EDEN?1:0, udp=profile==YW_PROXY_EDEN?3:2;
    YWire w=yw_buffer(out,cap); yw_put(&w,YW_PROXY,1); yw_put(&w,family,1); yw_put(&w,src,4); yw_put(&w,sport,2);
    yw_put(&w,family,1); yw_put(&w,dst,4); yw_put(&w,dport,2); yw_put(&w,udp,1);
    yw_put(&w,dst==0xffffffffu || (dst&255)==255,1); yw_put(&w,len,4); yw_bytes(&w,data,len);
    return w.ok?w.at:0;
}
int yw_room(const void *data,size_t len,uint32_t *ips,unsigned *count) {
    if (len>YW_MAX_PACKET) return 0;
    YWire w=yw_buffer((void *)data,len); if (yw_get(&w,1)!=YW_ROOM) return 0;
    yw_skip_string(&w); yw_skip_string(&w); yw_get(&w,4); yw_get(&w,2);
    yw_skip_string(&w); yw_skip_string(&w);
    unsigned n=(unsigned)yw_get(&w,4); if (!w.ok || n>YW_MAX_MEMBERS) return 0;
    uint32_t found[YW_MAX_MEMBERS];
    for (unsigned i=0;i<n && w.ok;i++) {
        yw_skip_string(&w); found[i]=(uint32_t)yw_get(&w,4); yw_skip_string(&w); yw_get(&w,8);
        for (unsigned j=0;j<4;j++) yw_skip_string(&w); /* game version, username, display name, avatar */
        if (!found[i] || found[i]==0xffffffffu) return 0;
        for (unsigned j=0;j<i;j++) if (found[j]==found[i]) return 0;
    }
    if (!w.ok || w.at!=len) return 0;
    if (n) memcpy(ips,found,n*sizeof *ips); *count=n; return 1;
}
int yw_packet(const void *data,size_t len,YPacket *p) {
    if (!p || !data || len>YW_MAX_PACKET) return 0;
    memset(p,0,sizeof *p); YWire w=yw_buffer((void *)data,len); p->kind=(unsigned)yw_get(&w,1);
    if (p->kind==YW_LDN) {
        p->type=(unsigned)yw_get(&w,1); p->src=(uint32_t)yw_get(&w,4); p->dst=(uint32_t)yw_get(&w,4);
        p->broadcast=(unsigned)yw_get(&w,1); p->size=(size_t)yw_get(&w,4);
        static const unsigned sizes[]={0,0x480,0x40,0x480,0x40,0};
        if (p->type>5 || p->size!=sizes[p->type]) return 0;
    } else if (p->kind==YW_PROXY) {
        unsigned source_family=(unsigned)yw_get(&w,1);
        p->src=(uint32_t)yw_get(&w,4); p->sport=(uint16_t)yw_get(&w,2);
        unsigned destination_family=(unsigned)yw_get(&w,1);
        p->dst=(uint32_t)yw_get(&w,4); p->dport=(uint16_t)yw_get(&w,2);
        unsigned protocol=(unsigned)yw_get(&w,1);
        if(source_family!=destination_family ||
           !((source_family==0 && protocol==2) || (source_family==1 && protocol==3))) return 0;
        p->broadcast=(unsigned)yw_get(&w,1); p->size=(size_t)yw_get(&w,4);
    } else return 0;
    if (p->broadcast>1 || !p->src || p->src==0xffffffffu || (!p->broadcast && !p->dst)) return 0;
    p->data=yw_take(&w,p->size); return w.ok && w.at==len;
}
