/* Test-only driver for the real client and guest HLE. No game data, no server. */
#include "host.h"
#include "net.h"
#include "settings.h"
#include "config_folder.h"
#include "online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
const char *rt_data_dir=".";
const RtModule rt_module_main={0};
int rt_code_patches_on(void){return 0;}
int settings_online(void){return 0;}
int settings_line_read(const char *p,char *o,int n){(void)p;(void)o;(void)n;return 0;}
int settings_line_write(const char *p,const char *o){(void)p;(void)o;return 0;}
const char *config_path(enum ConfigFile f){(void)f;return "none.txt";}
u64 rt_now_ns(void){return GetTickCount64()*1000000ull;}
void kernel_signal_event(u32 h){(void)h;}
void rt_backtrace(void){}
u32 rt_attach_system_event(Cpu *c,u64 e,int a){(void)c;(void)e;(void)a;return 0;}
u64 rt_guest_copy(const void *p,size_t n){(void)n;return (u64)p;}
void f_7109199790_orig(Cpu *c){(void)c;}
void f_71000b41f0_orig(Cpu *c){(void)c;}
u64 rt_hook_function(u64 a,u64 b){(void)a;(void)b;return 1;}
u64 rt_native(GuestFn *fn,const char *n){(void)fn;(void)n;return 1;}
void rt_call(Cpu *c,u64 a){(void)c;(void)a;}
u64 rt_symbol(const char *n){(void)n;return 0;}
u64 rt_callsym(Cpu *c,const char *n,int a,...){(void)c;(void)n;(void)a;return 0;}
void input_seq_mark(const char *s){(void)s;}
void rb_guest_writing(void *p,u64 n){(void)p;(void)n;}
int kernel_rb_park_guest(void *p){(void)p;return 0;}
void (*kernel_rb_wake_hook)(void);
void online_rb_save(OnlineRb *o){memset(o,0,sizeof *o);}
void online_rb_load(const OnlineRb *o){(void)o;}
void online_rb_counts(long long *a,long long *b,long long *c){*a=*b=*c=0;}
int online_stream_connect(i32 f,u32 i,u16 p,u32 a,u16 b,int n){(void)f;(void)i;(void)p;(void)a;(void)b;(void)n;return 95;}
i64 online_stream_send(i32 f,const void *p,u64 n,int b){(void)f;(void)p;(void)n;(void)b;return -95;}
i64 online_stream_recv(i32 f,void *p,u64 n,int k,int b){(void)f;(void)p;(void)n;(void)k;(void)b;return -95;}
u16 online_stream_revents(i32 f,u16 e){(void)f;(void)e;return 0;}
int online_stream_error(i32 f){(void)f;return 95;}
void online_stream_close(i32 f){(void)f;}
#define DECL(n) void hle_##n(Cpu *);
DECL(_ZN2nn3ldn10InitializeEv)
DECL(_ZN2nn3ldn15OpenAccessPointEv)
DECL(_ZN2nn3ldn11OpenStationEv)
DECL(_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE)
DECL(_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi)
DECL(_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE)
DECL(_ZN2nn3ldn8GetStateEv)
DECL(_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE)
DECL(_ZN2nn3ldn10DisconnectEv)
DECL(_ZN2nn3ldn14DestroyNetworkEv)
DECL(nnsocketSocket)
DECL(nnsocketBind)
DECL(_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj)
DECL(_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj)
#define CALL(n) hle_##n(&c)
static void hex(const u8 *p,size_t n){for(size_t i=0;i<n;i++)printf("%02x",p[i]);}
int main(void){
 setvbuf(stdout,NULL,_IONBF,0); char line[8192]; int fd=-1; u8 networks[24*0x480]={0}; int found=0;
 while(fgets(line,sizeof line,stdin)){
  Cpu c={0}; char cmd[32]={0}; sscanf(line,"%31s",cmd);
  if(!strcmp(cmd,"quit")) return 0;
  if(!strcmp(cmd,"rollback-default")) printf("rollback-default %d\n",net_rollback_default());
  else if(!strcmp(cmd,"status")){char text[512];net_status(text,sizeof text);printf("status %s\n",text);}
  else if(!strcmp(cmd,"ip")) printf("ip %u\n",net_local_ip());
  else if(!strcmp(cmd,"state")){CALL(_ZN2nn3ldn8GetStateEv);printf("state %llu\n",c.x[0]);}
  else if(!strcmp(cmd,"init")){CALL(_ZN2nn3ldn10InitializeEv);printf("result %lld\n",(i64)c.x[0]);}
  else if(!strcmp(cmd,"host")){
   CALL(_ZN2nn3ldn15OpenAccessPointEv); u8 config[0x20]={0},security[0x44]={0},user[0x30]={0};
   u64 intent=0x01006A800016E000ull;memcpy(config,&intent,8); config[0x12]=8;config[0x14]=1;memcpy(user,"TestHost",8);
   c.x[0]=(u64)config;c.x[1]=(u64)security;c.x[2]=(u64)user;
   CALL(_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE);
   printf("result %lld\n",(i64)c.x[0]);
  } else if(!strcmp(cmd,"scan")){
   u8 filter[0x60]={0};found=0;c.x[0]=(u64)networks;c.x[1]=(u64)&found;c.x[2]=24;c.x[3]=(u64)filter;
   CALL(_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi);printf("scan %d ",found);if(found)hex(networks,0x480);puts("");
  } else if(!strcmp(cmd,"join")){
   CALL(_ZN2nn3ldn11OpenStationEv);u8 user[0x30]={0},sec[0x44]={0};memcpy(user,"TestGuest",9);
   c.x[0]=(u64)networks;c.x[1]=(u64)sec;c.x[2]=(u64)user;c.x[3]=1;
   CALL(_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE);
   printf("result %lld\n",(i64)c.x[0]);
  } else if(!strcmp(cmd,"info")){
   u8 info[0x480]={0};c.x[0]=(u64)info;CALL(_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE);
   printf("info %lld ",(i64)c.x[0]);if(!c.x[0])hex(info,sizeof info);puts("");
  } else if(!strcmp(cmd,"disconnect")){CALL(_ZN2nn3ldn10DisconnectEv);printf("result %lld\n",(i64)c.x[0]);}
  else if(!strcmp(cmd,"destroy")){CALL(_ZN2nn3ldn14DestroyNetworkEv);printf("result %lld\n",(i64)c.x[0]);}
  else if(!strcmp(cmd,"udpbind")){
   unsigned port=0;sscanf(line,"%*s %u",&port);c.x[0]=2;c.x[1]=2;CALL(nnsocketSocket);fd=(int)c.x[0];
   u8 addr[16]={16,2,(u8)(port>>8),(u8)port};c.x[0]=fd;c.x[1]=(u64)addr;c.x[2]=16;CALL(nnsocketBind);printf("result %lld\n",(i64)c.x[0]);
  } else if(!strcmp(cmd,"udpsend")){
   unsigned ip=0,port=0;char data[1024]={0};sscanf(line,"%*s %u %u %1023s",&ip,&port,data);
   u8 addr[16]={16,2,(u8)(port>>8),(u8)port,(u8)(ip>>24),(u8)(ip>>16),(u8)(ip>>8),(u8)ip};
   c.x[0]=fd;c.x[1]=(u64)data;c.x[2]=strlen(data);c.x[4]=(u64)addr;c.x[5]=16;
   CALL(_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj);printf("result %lld\n",(i64)c.x[0]);
  } else if(!strcmp(cmd,"udprecv")){
   u8 data[2048]={0},addr[16]={0};u32 alen=16;c.x[0]=fd;c.x[1]=(u64)data;c.x[2]=sizeof data;c.x[3]=0x80;c.x[4]=(u64)addr;c.x[5]=(u64)&alen;
   CALL(_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj);printf("recv %lld ",(i64)c.x[0]);if((i64)c.x[0]>=0)hex(data,(size_t)c.x[0]);puts("");
  } else puts("error");
 }
 return 0;
}
