"""yuzu room CLIENT integration through real ENet, Zstd and runtime guest HLE.
The loopback fixture is a test double, not a shipped room service. Wire frames are
constructed independently with struct, matching the pinned reference protocol.
No game data or external/public server is used. Windows first.
"""
from pathlib import Path
import ctypes as C
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools'))
CLIENT_IP=0xC0A83202
REFERENCE_IP=0xC0A83203

def text(value):
    data=value.encode(); return struct.pack('>I',len(data))+data

def node(ip,name,index):
    value=bytearray(0x40)
    struct.pack_into('<I',value,0,ip)
    value[4:10]=b'\x02\x00'+ip.to_bytes(4,'big')
    value[10:12]=bytes((index,1))
    data=name.encode(); value[12:12+len(data)]=data
    struct.pack_into('<H',value,0x2e,1)
    return value

def network():
    value=bytearray(0x480)
    struct.pack_into('<Q',value,0,0x01006A800016E000)
    value[0x10:0x20]=bytes(range(16))
    value[0x20:0x26]=b'\x02\x00'+REFERENCE_IP.to_bytes(4,'big')
    value[0x26]=4; value[0x27:0x2b]=b'test'
    struct.pack_into('<H',value,0x48,6)
    value[0x4a:0x4c]=b'\x03\x02'
    value[0x66:0x68]=b'\x08\x01'
    for i in range(8): value[0x68+i*64+10]=i
    value[0x68:0xa8]=node(REFERENCE_IP,'Reference',0)
    return value

def ldn(kind,src,dst,payload=b''):
    return struct.pack('>BBIIBI',6,kind,src,dst,dst==0xffffffff,len(payload))+payload

def proxy(src,sport,dst,dport,payload,profile='eden'):
    # Reference enums: Eden v0.2.0 INET=1/UDP=3; historical yuzu INET=0/UDP=2.
    family,udp=(1,3) if profile=='eden' else (0,2)
    return struct.pack('>BBIHBIHBBI',5,family,src,sport,family,dst,dport,udp,dst==0xffffffff,len(payload))+payload

class Address(C.Structure):
    _fields_=[('host',C.c_uint32),('port',C.c_uint16)]
class Packet(C.Structure):
    _fields_=[('references',C.c_size_t),('flags',C.c_uint32),('data',C.c_void_p),('size',C.c_size_t),('free',C.c_void_p),('user',C.c_void_p)]
class Event(C.Structure):
    _fields_=[('type',C.c_int),('peer',C.c_void_p),('channel',C.c_uint8),('value',C.c_uint32),('packet',C.POINTER(Packet))]

class RoomFixture:
    def __init__(self,refusal=0):
        self.errors=[]; self.received=[]; self.lock=threading.Lock(); self.commands=queue.Queue()
        self.stop=threading.Event(); self.refusal=refusal; self.admitted=threading.Event(); self.peer=None
        self.net=network(); self.sessions=0; self.reference_present=True; self.auto_join=True
        self.delay_connect=0;self.pending=[]
        self.proxy_profile='eden';self.echo_game_udp=False
        self.enet=C.CDLL(str(ROOT/'build/yuzu/enet.dll'))
        signatures={
            'enet_initialize':([],C.c_int),
            'enet_address_set_host':([C.POINTER(Address),C.c_char_p],C.c_int),
            'enet_host_create':([C.POINTER(Address),C.c_size_t,C.c_size_t,C.c_uint32,C.c_uint32],C.c_void_p),
            'enet_host_destroy':([C.c_void_p],None),
            'enet_host_service':([C.c_void_p,C.POINTER(Event),C.c_uint32],C.c_int),
            'enet_host_flush':([C.c_void_p],None),
            'enet_packet_create':([C.c_void_p,C.c_size_t,C.c_uint32],C.POINTER(Packet)),
            'enet_packet_destroy':([C.POINTER(Packet)],None),
            'enet_peer_send':([C.c_void_p,C.c_uint8,C.POINTER(Packet)],C.c_int),
            'enet_peer_disconnect':([C.c_void_p,C.c_uint32],None),
        }
        for name,(args,result) in signatures.items():
            fn=getattr(self.enet,name); fn.argtypes=args; fn.restype=result
        self.zstd=C.CDLL(str(ROOT/'build/yuzu/libzstd.dll'))
        for name in ('ZSTD_compress','ZSTD_decompress'):
            fn=getattr(self.zstd,name); fn.restype=C.c_size_t
            fn.argtypes=[C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]+([C.c_int] if name=='ZSTD_compress' else [])
        self.zstd.ZSTD_isError.argtypes=[C.c_size_t];self.zstd.ZSTD_isError.restype=C.c_uint
        assert self.enet.enet_initialize()==0
        address=Address(); assert self.enet.enet_address_set_host(C.byref(address),b'127.0.0.1')==0
        # Select a loopback-only ephemeral port; nothing is exposed on a public interface.
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
            s.bind(('127.0.0.1',0)); address.port=s.getsockname()[1]
        self.port=address.port
        self.host=self.enet.enet_host_create(C.byref(address),4,1,0,0)
        assert self.host
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def compress(self,data):
        out=C.create_string_buffer(len(data)+1024)
        n=self.zstd.ZSTD_compress(out,len(out),data,len(data),1)
        assert not self.zstd.ZSTD_isError(n)
        return out.raw[:n]
    def decompress(self,data):
        out=C.create_string_buffer(65507)
        n=self.zstd.ZSTD_decompress(out,len(out),data,len(data))
        assert not self.zstd.ZSTD_isError(n)
        return out.raw[:n]
    def send_now(self,data):
        packet=self.enet.enet_packet_create(data,len(data),1)
        if self.enet.enet_peer_send(self.peer,0,packet)<0:
            self.enet.enet_packet_destroy(packet)
            raise AssertionError('fixture send failed')
    def send(self,data): self.commands.put(('send',data))
    def members(self):
        entries=[('OpenSmash',CLIENT_IP)]
        if self.reference_present: entries.append(('Reference',REFERENCE_IP))
        data=b'\x03'+text('Client test')+text('')+struct.pack('>IH',8,self.port)+text('Super Smash Bros. Ultimate')+text('')+struct.pack('>I',len(entries))
        for name,ip in entries:
            data+=text(name)+struct.pack('>I',ip)+text('Super Smash Bros. Ultimate')+struct.pack('>Q',0x01006A800016E000)+text('13.0.5')+text('')+text('')+text('')
        return data
    def handle(self,data):
        with self.lock: self.received.append(data)
        if data[0]==1:
            expected=b'\x01'+text('OpenSmash')+struct.pack('>II',0xffffffff,1)+text('test-password')+text('')
            assert data==expected, ('join bytes differ',data)
            self.sessions+=1
            if self.refusal: self.send_now(bytes((self.refusal,)));return
            self.send_now(self.members()); self.send_now(b'\x02'+struct.pack('>I',CLIENT_IP));self.admitted.set()
        elif data[0]==6:
            _,kind,src,dst,bcast,length=struct.unpack('>BBIIBI',data[:15]);payload=data[15:]
            assert src==CLIENT_IP and len(payload)==length
            if kind==0 and self.auto_join:
                self.send_now(ldn(1,REFERENCE_IP,CLIENT_IP,self.net))
            elif kind==2 and self.auto_join:
                assert len(payload)==64 and struct.unpack_from('<I',payload)[0]==CLIENT_IP
                self.net[0xa8:0xe8]=payload;self.net[0xb2]=1;self.net[0x67]=2
                response=ldn(3,REFERENCE_IP,CLIENT_IP,self.net)
                if self.delay_connect:self.pending.append((time.monotonic()+self.delay_connect,response))
                else:self.send_now(response)
        elif data[0]==5 and self.echo_game_udp:
            # Eden's ProxySocket ignores protocol != Protocol::UDP. Its exact
            # v0.2.0 socket enums are 1/3, independent of our C codec constants.
            family,udp=(1,3) if self.proxy_profile=='eden' else (0,2)
            _,sf,src,sport,df,dst,dport,protocol,bcast,length=struct.unpack('>BBIHBIHBBI',data[:21])
            if (sf,df,protocol)!=(family,family,udp):return
            assert src==CLIENT_IP and len(data[21:])==length
            payload=self.decompress(data[21:])
            self.send_now(proxy(REFERENCE_IP,dport,CLIENT_IP,sport,self.compress(b'accepted:'+payload),self.proxy_profile))
    def run(self):
        try:
            while not self.stop.is_set():
                now=time.monotonic()
                ready=[p for p in self.pending if p[0]<=now]
                self.pending=[p for p in self.pending if p[0]>now]
                for _,data in ready:self.send_now(data)
                while True:
                    try: action,data=self.commands.get_nowait()
                    except queue.Empty:break
                    if action=='send': self.send_now(data)
                    elif action=='members': self.send_now(self.members())
                    elif action=='disconnect': self.enet.enet_peer_disconnect(self.peer,0)
                event=Event(); result=self.enet.enet_host_service(self.host,C.byref(event),5)
                assert result>=0
                if result>0:
                    if event.type==1:self.peer=event.peer
                    elif event.type==3:
                        try:self.handle(C.string_at(event.packet.contents.data,event.packet.contents.size))
                        finally:self.enet.enet_packet_destroy(event.packet)
                self.enet.enet_host_flush(self.host)
        except BaseException as error:self.errors.append(error)
    def packets(self,kind=None,subtype=None):
        with self.lock:
            return [p for p in self.received if (kind is None or p[0]==kind) and (subtype is None or len(p)>1 and p[1]==subtype)]
    def close(self):
        self.stop.set();self.thread.join(5)
        if self.thread.is_alive():raise RuntimeError('fixture did not stop')
        self.enet.enet_host_destroy(self.host)
        if self.errors:raise self.errors[0]

class Client:
    def __init__(self,exe,room,**extra):
        env={**os.environ,'SSBU_NET_BACKEND':'yuzu','SSBU_YUZU_SERVER':f'127.0.0.1:{room.port}',
             'SSBU_NET_NAME':'OpenSmash','SSBU_YUZU_PASSWORD':'test-password','SSBU_YUZU_TOKEN':'',
             'SSBU_RB':'0','SSBU_YUZU_LIBDIR':str(ROOT/'build/yuzu'),'SSBU_YUZU_PROXY_PROFILE':'',**extra}
        self.process=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        self.output=queue.Queue();self.errors=[]
        self.reader=threading.Thread(target=lambda:[self.output.put(line.rstrip()) for line in self.process.stdout],daemon=True);self.reader.start()
        self.err_reader=threading.Thread(target=lambda:[self.errors.append(line) for line in self.process.stderr],daemon=True);self.err_reader.start()
    def command(self,command,timeout=12):
        self.process.stdin.write(command+'\n');self.process.stdin.flush()
        try:return self.output.get(timeout=timeout)
        except queue.Empty:raise AssertionError('client timed out: '+command+' '+''.join(self.errors))
    def wait_ip(self):
        end=time.monotonic()+6
        while time.monotonic()<end:
            value=self.command('ip')
            if value==f'ip {CLIENT_IP}':return
            time.sleep(.02)
        raise AssertionError('client did not join: '+self.command('status')+' '+''.join(self.errors))
    def close(self):
        if self.process.poll() is None:self.command_write_quit()
        self.process.wait(timeout=15);self.reader.join(2);self.err_reader.join(2)
        for stream in (self.process.stdin,self.process.stdout,self.process.stderr):stream.close()
    def command_write_quit(self):
        self.process.stdin.write('quit\n');self.process.stdin.flush()

def until(condition,timeout=4):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if condition():return
        time.sleep(.02)
    raise AssertionError('condition timed out')

@unittest.skipUnless(os.name=='nt','Windows client integration')
class YuzuClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ROOT/'build/yuzu/enet.dll').exists() or not (ROOT/'build/yuzu/libzstd.dll').exists():
            raise unittest.SkipTest('run python tools/yuzu_client_deps.py first')
        import cc
        clang=cc.CLANG
        if not Path(clang).is_file():clang=str(ROOT/'deps/llvm/bin/clang-cl.exe')
        if not Path(clang).is_file():raise unittest.SkipTest('clang-cl required')
        cls.temp=tempfile.TemporaryDirectory(prefix='yuzu-client-test-');cls.exe=Path(cls.temp.name)/'client.exe'
        sources=['tools/dev/yuzuclienttest.c','runtime/net_relay.c','runtime/net_eos.c','runtime/hle_net.c','runtime/net_yuzu.c','runtime/net_yuzu_wire.c']
        command=[clang,'/nologo','/O1','/Gy',*cc.CFLAGS,*[str(ROOT/p) for p in sources],'/Fe'+str(cls.exe),'/Fo'+cls.temp.name+os.sep,
                 '/link',*cc.LINK_OPTIONS,'/OPT:REF','ws2_32.lib','iphlpapi.lib','advapi32.lib','user32.lib','ole32.lib','oleaut32.lib']
        result=subprocess.run(command,capture_output=True,text=True,timeout=120)
        if result.returncode:
            cls.temp.cleanup()
            raise AssertionError(result.stdout+result.stderr)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def setUp(self):
        self.room=RoomFixture();self.client=Client(self.exe,self.room)
        self.addCleanup(self.room.close);self.addCleanup(self.client.close)
        self.client.wait_ip();self.assertEqual(self.client.command('init'),'result 0')
    def test_native_default_rollback_is_disabled_for_compatibility_backend(self):
        room=RoomFixture();self.addCleanup(room.close)
        client=Client(self.exe,room,SSBU_NET_BACKEND='YUZU',SSBU_NET_HOST='1',SSBU_NET_RELAY='127.0.0.1:9')
        self.addCleanup(client.close)
        self.assertEqual(client.command('rollback-default'),'rollback-default 0')
        client.wait_ip()
        self.assertIn('yuzu room:',client.command('status'))

    def test_room_handshake_and_game_information(self):
        self.assertIn('yuzu room:',self.client.command('status'))
        until(lambda:self.room.packets(4))
        expected=b'\x04'+text('Super Smash Bros. Ultimate')+struct.pack('>Q',0x01006A800016E000)+text('13.0.5')
        self.assertEqual(self.room.packets(4)[0],expected)
    def test_scan_join_and_no_native_heartbeat_timeout(self):
        scan=self.client.command('scan');self.assertTrue(scan.startswith('scan 1 '),scan)
        self.assertEqual(bytes.fromhex(scan.split()[2]),network())
        self.assertEqual(self.client.command('join'),'result 0')
        self.assertEqual(self.client.command('state'),'state 5')
        time.sleep(6.3)
        self.assertEqual(self.client.command('state'),'state 5')
        self.assertEqual(self.client.command('disconnect'),'result 0')
        until(lambda:self.room.packets(6,4))
        payload=self.room.packets(6,4)[0][15:]
        self.assertEqual(len(payload),64);self.assertEqual(struct.unpack_from('<I',payload)[0],CLIENT_IP)
    def test_slow_join_does_not_duplicate_legacy_connect(self):
        self.room.delay_connect=1.4
        self.client.command('scan')
        self.assertEqual(self.client.command('join'),'result 0')
        self.assertEqual(len(self.room.packets(6,2)),1)
        self.assertEqual(self.client.command('state'),'state 5')

    def test_host_scan_other_client_join_disconnect_destroy(self):
        self.room.auto_join=False
        self.assertEqual(self.client.command('host'),'result 0')
        # Reference SendBroadcast does not set remote_ip; a zero destination is valid.
        self.room.send(struct.pack('>BBIIBI',6,0,REFERENCE_IP,0,1,0))
        until(lambda:self.room.packets(6,1))
        host_info=self.room.packets(6,1)[0][15:]
        self.assertEqual(struct.unpack_from('<I',host_info,0x68)[0],CLIENT_IP)
        self.room.send(ldn(2,REFERENCE_IP,CLIENT_IP,node(REFERENCE_IP,'Reference',0)))
        until(lambda:self.room.packets(6,3))
        synced=self.room.packets(6,3)[-1][15:]
        self.assertEqual(synced[0x67],2);self.assertEqual(synced[0xb2:0xb4],b'\x01\x01')
        self.assertEqual(synced[0xac:0xb2],b'\x02\x00'+REFERENCE_IP.to_bytes(4,'big'))
        # A retransmitted Connect must not add the member twice.
        self.room.send(ldn(2,REFERENCE_IP,CLIENT_IP,node(REFERENCE_IP,'Reference',0)))
        until(lambda:len(self.room.packets(6,3))>=2)
        self.assertEqual(self.room.packets(6,3)[-1][15+0x67],2)
        self.room.send(ldn(4,REFERENCE_IP,CLIENT_IP,node(REFERENCE_IP,'Reference',1)))
        until(lambda:self.client.command('info').split()[2][0x67*2:0x67*2+2]=='01')
        self.assertEqual(self.client.command('destroy'),'result 0')
        self.assertEqual(self.client.command('state'),'state 2')
    def test_virtual_udp_zstd_and_address_routing(self):
        self.assertEqual(self.client.command('udpbind 5000'),'result 0')
        self.assertEqual(self.client.command(f'udpsend {REFERENCE_IP} 6000 hello-yuzu'),'result 10')
        until(lambda:self.room.packets(5))
        packet=self.room.packets(5)[0]
        self.assertEqual(packet[:21],struct.pack('>BBIHB I HBBI'.replace(' ',''),5,1,CLIENT_IP,5000,1,REFERENCE_IP,6000,3,0,len(packet)-21))
        self.assertEqual(self.room.decompress(packet[21:]),b'hello-yuzu')
        self.room.send(proxy(REFERENCE_IP,6000,CLIENT_IP,5000,self.room.compress(b'hello-opensmash')))
        until(lambda:self.client.command('udprecv')=='recv 15 '+b'hello-opensmash'.hex())
        self.assertEqual(self.client.command('udpsend 168625153 6000 wrong-subnet'),'result -1')
    def test_eden_join_then_game_udp_handshake(self):
        self.room.echo_game_udp=True
        self.client.command('scan');self.assertEqual(self.client.command('join'),'result 0')
        self.assertEqual(self.client.command('udpbind 12345'),'result 0')
        # Reproduction: LDN joined, then the game broadcasts from port 12345.
        self.assertEqual(self.client.command('udpsend 3232301055 12345 join-game'),'result 9')
        t=b'accepted:join-game'
        until(lambda:self.client.command('udprecv')==f'recv {len(t)} '+t.hex())
        frame=self.room.packets(5)[0]
        self.assertEqual((frame[1],frame[8],frame[15],frame[16]),(1,1,3,1))

    def test_legacy_yuzu_proxy_profile_can_be_selected(self):
        room=RoomFixture();self.addCleanup(room.close)
        room.proxy_profile='yuzu';room.echo_game_udp=True
        client=Client(self.exe,room,SSBU_YUZU_PROXY_PROFILE='yuzu');self.addCleanup(client.close)
        client.wait_ip();self.assertEqual(client.command('init'),'result 0')
        client.command('scan');self.assertEqual(client.command('join'),'result 0')
        self.assertEqual(client.command('udpbind 12345'),'result 0')
        self.assertEqual(client.command(f'udpsend {REFERENCE_IP} 12345 legacy'),'result 6')
        until(lambda:client.command('udprecv')=='recv 15 '+b'accepted:legacy'.hex())
        frame=room.packets(5)[0]
        self.assertEqual((frame[1],frame[8],frame[15]),(0,0,2))
        # Receiving either supported tuple is safe; each packet has one UDP payload.
        room.send(proxy(REFERENCE_IP,12345,CLIENT_IP,12345,room.compress(b'eden-reply')))
        until(lambda:client.command('udprecv')=='recv 10 '+b'eden-reply'.hex())

    def test_bad_compressed_or_oversized_data_is_dropped(self):
        self.assertEqual(self.client.command('udpbind 5000'),'result 0')
        for payload in (b'not-zstd',self.room.compress(b'x'*65508)):
            self.room.send(proxy(REFERENCE_IP,6000,CLIENT_IP,5000,payload))
        self.room.send(proxy(REFERENCE_IP,6000,CLIENT_IP,5000,self.room.compress(b'good')))
        until(lambda:self.client.command('udprecv')=='recv 4 '+b'good'.hex())
        self.assertEqual(self.client.command('udprecv'),'recv -1')
    def test_removed_room_member_clears_game_session(self):
        self.client.command('scan');self.assertEqual(self.client.command('join'),'result 0')
        self.room.reference_present=False;self.room.commands.put(('members',None))
        until(lambda:self.client.command('state')=='state 4')
    def test_reconnect_invalidates_old_game_session(self):
        self.client.command('scan');self.assertEqual(self.client.command('join'),'result 0')
        self.room.commands.put(('disconnect',None))
        until(lambda:self.room.sessions>=2,8)
        self.assertEqual(self.client.command('state'),'state 4')
    def test_reconnect_discards_queued_game_datagrams(self):
        self.room.auto_join=False
        self.assertEqual(self.client.command('host'),'result 0')
        self.assertEqual(self.client.command('udpbind 5000'),'result 0')
        self.room.send(proxy(REFERENCE_IP,6000,CLIENT_IP,5000,self.room.compress(b'old-session')))
        self.room.send(ldn(0,REFERENCE_IP,0xffffffff))
        until(lambda:self.room.packets(6,1))
        self.room.commands.put(('disconnect',None))
        until(lambda:self.room.sessions>=2,8)
        self.client.wait_ip()
        self.assertEqual(self.client.command('udprecv'),'recv -1', 'reconnect retained a datagram from the old session')
        self.room.send(proxy(REFERENCE_IP,6000,CLIENT_IP,5000,self.room.compress(b'new-session')))
        until(lambda:self.client.command('udprecv')=='recv 11 '+b'new-session'.hex())
        self.assertEqual(self.client.command('udprecv'),'recv -1')

    def test_unsupported_room_information_is_explained(self):
        self.room.send(b'\x03'+b'\xff'*4)
        until(lambda:'unsupported or malformed room information' in self.client.command('status'))
        self.assertEqual(self.client.command('ip'),'ip 0')

    def test_malformed_ldn_and_unknown_sender_do_not_join(self):
        self.room.auto_join=False;self.client.command('host')
        self.room.send(ldn(2,REFERENCE_IP,CLIENT_IP,b'short'))
        self.room.send(ldn(2,0xC0A83299,CLIENT_IP,node(0xC0A83299,'Unknown',0)))
        # Valid Scan after the bad frames is an ordering barrier on the reliable channel.
        self.room.send(ldn(0,REFERENCE_IP,0xffffffff));until(lambda:self.room.packets(6,1))
        self.assertEqual(self.room.packets(6,1)[-1][15+0x67],1)

@unittest.skipUnless(os.name=='nt','Windows client integration')
class RefusalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        YuzuClientTests.setUpClass();cls.exe=YuzuClientTests.exe
    @classmethod
    def tearDownClass(cls):YuzuClientTests.tearDownClass()
    def test_wrong_password_is_terminal_without_fallback(self):
        room=RoomFixture(refusal=11);self.addCleanup(room.close)
        client=Client(self.exe,room);self.addCleanup(client.close)
        until(lambda:'wrong room password' in client.command('status'))
        self.assertEqual(client.command('ip'),'ip 0')
        time.sleep(.2);self.assertEqual(room.sessions,1)
    def test_version_mismatch_is_explained(self):
        room=RoomFixture(refusal=10);self.addCleanup(room.close)
        client=Client(self.exe,room);self.addCleanup(client.close)
        until(lambda:'protocol version differs' in client.command('status'))
        self.assertEqual(client.command('ip'),'ip 0')
    def test_rollback_is_rejected_before_connecting(self):
        room=RoomFixture();self.addCleanup(room.close)
        client=Client(self.exe,room,SSBU_RB='1');self.addCleanup(client.close)
        self.assertIn('SSBU_RB=0',client.command('status'));self.assertEqual(room.sessions,0)
    def test_invalid_proxy_profile_is_rejected_before_connecting(self):
        room=RoomFixture();self.addCleanup(room.close)
        client=Client(self.exe,room,SSBU_YUZU_PROXY_PROFILE='unknown');self.addCleanup(client.close)
        self.assertIn('SSBU_YUZU_PROXY_PROFILE must be eden or yuzu',client.command('status'))
        self.assertEqual(room.sessions,0)

    def test_invalid_nickname_is_rejected_before_connecting(self):
        room=RoomFixture();self.addCleanup(room.close)
        client=Client(self.exe,room,SSBU_NET_NAME='xx');self.addCleanup(client.close)
        self.assertIn('nickname must be 4-20',client.command('status'))
        self.assertEqual(room.sessions,0)

    def test_missing_client_libraries_do_not_fallback(self):
        room=RoomFixture();self.addCleanup(room.close)
        with tempfile.TemporaryDirectory() as directory:
            client=Client(self.exe,room,SSBU_YUZU_LIBDIR=directory)
            try:self.assertIn('missing client libraries',client.command('status'))
            finally:client.close()
        self.assertEqual(room.sessions,0)

if __name__=='__main__':unittest.main()
