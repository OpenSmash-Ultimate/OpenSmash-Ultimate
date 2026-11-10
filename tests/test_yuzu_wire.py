"""Golden wire frames and malformed-packet checks for the client codec, no game."""
from pathlib import Path
import ctypes as C
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parent.parent
class Parsed(C.Structure):
    _fields_=[('kind',C.c_uint),('type',C.c_uint),('broadcast',C.c_uint),('src',C.c_uint32),('dst',C.c_uint32),('sport',C.c_uint16),('dport',C.c_uint16),('data',C.c_void_p),('size',C.c_size_t)]
def text(s):
    b=s.encode();return struct.pack('>I',len(b))+b
class WireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='yuzu-wire-test-');folder=Path(cls.temp.name)
        if os.name=='nt':
            sys.path.insert(0,str(ROOT/'tools'));import cc
            clang=cc.CLANG
            if not Path(clang).is_file():clang=str(ROOT/'deps/llvm/bin/clang-cl.exe')
            if not Path(clang).is_file():raise unittest.SkipTest('clang-cl required')
            output=folder/'wire.dll'
            cmd=[clang,'/nologo','/W4','/WX','/O1','/LD',*cc.LINK_FLAGS,str(ROOT/'runtime/net_yuzu_wire.c'),'/Fe'+str(output),'/Fo'+str(folder)+os.sep,
                 '/link',*cc.LINK_OPTIONS,*['/EXPORT:'+n for n in ('yw_join','yw_ldn','yw_proxy','yw_proxy_profile','yw_packet','yw_room')]]
        else:
            clang=shutil.which('clang')
            if not clang:raise unittest.SkipTest('clang required')
            output=folder/'wire.so';cmd=[clang,'-std=c11','-O1','-Wall','-Wextra','-Werror','-shared','-fPIC',str(ROOT/'runtime/net_yuzu_wire.c'),'-o',str(output)]
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)
        cls.lib=C.CDLL(str(output))
        cls.lib.yw_join.argtypes=[C.c_void_p,C.c_size_t,C.c_char_p,C.c_char_p,C.c_char_p];cls.lib.yw_join.restype=C.c_size_t
        cls.lib.yw_ldn.argtypes=[C.c_void_p,C.c_size_t,C.c_uint,C.c_uint32,C.c_uint32,C.c_void_p,C.c_size_t];cls.lib.yw_ldn.restype=C.c_size_t
        cls.lib.yw_proxy.argtypes=[C.c_void_p,C.c_size_t,C.c_uint32,C.c_uint16,C.c_uint32,C.c_uint16,C.c_void_p,C.c_size_t];cls.lib.yw_proxy.restype=C.c_size_t
        cls.lib.yw_proxy_profile.argtypes=[*cls.lib.yw_proxy.argtypes,C.c_uint];cls.lib.yw_proxy_profile.restype=C.c_size_t
        cls.lib.yw_packet.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Parsed)];cls.lib.yw_packet.restype=C.c_int
        cls.lib.yw_room.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(C.c_uint32),C.POINTER(C.c_uint)];cls.lib.yw_room.restype=C.c_int
    @classmethod
    def tearDownClass(cls):
        if os.name=='nt':
            import _ctypes
            _ctypes.FreeLibrary(cls.lib._handle)
        cls.temp.cleanup()
    def parsed(self,data):
        p=Parsed();return self.lib.yw_packet(data,len(data),C.byref(p)),p
    def test_join_matches_reference_bytes(self):
        out=C.create_string_buffer(512)
        n=self.lib.yw_join(out,len(out),b'Guest',b'secret',b'token')
        self.assertEqual(out.raw[:n],b'\x01'+text('Guest')+struct.pack('>II',0xffffffff,1)+text('secret')+text('token'))
        self.assertEqual(self.lib.yw_join(out,3,b'Guest',b'',b''),0)
    def test_ldn_matches_reference_bytes_and_payload(self):
        data=bytes(range(64));out=C.create_string_buffer(128)
        n=self.lib.yw_ldn(out,len(out),2,0xc0a80102,0xc0a80103,data,len(data))
        expected=struct.pack('>BBIIBI',6,2,0xc0a80102,0xc0a80103,0,64)+data
        self.assertEqual(out.raw[:n],expected)
        ok,p=self.parsed(expected);self.assertEqual(ok,1);self.assertEqual(p.type,2);self.assertEqual(C.string_at(p.data,p.size),data)
    def test_proxy_uses_protocol_enums_and_network_order_ports(self):
        out=C.create_string_buffer(128);data=b'compressed-frame'
        n=self.lib.yw_proxy(out,len(out),0xc0a80102,65511,0xffffffff,5000,data,len(data))
        expected=struct.pack('>BBIHBIHBBI',5,0,0xc0a80102,65511,0,0xffffffff,5000,2,1,len(data))+data
        self.assertEqual(out.raw[:n],expected)
        ok,p=self.parsed(expected);self.assertEqual(ok,1);self.assertEqual((p.sport,p.dport,p.broadcast),(65511,5000,1))
    def test_eden_020_proxy_uses_shifted_socket_enums(self):
        # Eden v0.2.0: src/common/socket_types.h has Domain::INET=1, Protocol::UDP=3.
        # room_member.cpp serializes these enum values directly; room version stays 1.
        # Keep literal reference bytes independent of the codec's profile constants.
        data=b'compressed-frame';out=C.create_string_buffer(128)
        n=self.lib.yw_proxy_profile(out,len(out),0xc0a8081f,12345,0xc0a8f167,12345,data,len(data),1)
        expected=bytes.fromhex('05 01 c0a8081f 3039 01 c0a8f167 3039 03 00 00000010')+data
        self.assertEqual(out.raw[:n],expected)
        ok,p=self.parsed(expected);self.assertEqual(ok,1)
        self.assertEqual((p.src,p.dst,p.sport,p.dport),(0xc0a8081f,0xc0a8f167,12345,12345))
        self.assertEqual(C.string_at(p.data,p.size),data)
        self.assertEqual(self.lib.yw_proxy_profile(out,len(out),1,1,2,2,data,len(data),99),0)

    def test_proxy_rejects_tcp_and_mixed_profile_fields(self):
        for family,udp in ((0,2),(1,3)):
            frame=bytearray(struct.pack('>BBIHBIHBBI',5,family,0xc0a8081f,12345,family,0xc0a8f167,12345,udp,0,1)+b'x')
            self.assertEqual(self.parsed(bytes(frame))[0],1)
            for offset,value in ((1,1-family),(8,1-family),(15,udp-1),(15,3 if udp==2 else 2)):
                bad=frame.copy();bad[offset]=value
                with self.subTest(family=family,offset=offset,value=value):self.assertEqual(self.parsed(bytes(bad))[0],0)
            for size in range(len(frame)):
                with self.subTest(family=family,truncated=size):self.assertEqual(self.parsed(bytes(frame[:size]))[0],0)
            self.assertEqual(self.parsed(bytes(frame)+b'extra')[0],0)

    def test_reference_broadcast_can_have_zero_destination(self):
        frame=struct.pack('>BBIIBI',6,0,0xc0a80103,0,1,0)
        self.assertEqual(self.parsed(frame)[0],1)
        frame=struct.pack('>BBIIBI',6,0,0xc0a80103,0,0,0)
        self.assertEqual(self.parsed(frame)[0],0)

    def test_every_truncated_ldn_frame_is_rejected(self):
        frame=struct.pack('>BBIIBI',6,1,0xc0a80103,0xc0a80102,0,0x480)+bytes(0x480)
        for size in range(len(frame)):
            with self.subTest(size=size):self.assertEqual(self.parsed(frame[:size])[0],0)
        self.assertEqual(self.parsed(frame)[0],1)
    def test_invalid_types_lengths_domains_flags_and_trailing_bytes(self):
        frame=bytearray(struct.pack('>BBIIBI',6,2,0xc0a80103,0xc0a80102,0,64)+bytes(64))
        bad=[]
        for offset,value in ((1,7),(10,2)):
            f=frame.copy();f[offset]=value;bad.append(bytes(f))
        f=frame.copy();f[11:15]=b'\xff'*4;bad.append(bytes(f))
        bad += [bytes(frame)+b'extra',bytes([99])+bytes(frame[1:])]
        proxy=bytearray(struct.pack('>BBIHBIHBBI',5,0,0xc0a80103,6000,0,0xc0a80102,5000,2,0,1)+b'x')
        for offset in (1,8,15):
            f=proxy.copy();f[offset]=17;bad.append(bytes(f))
        for f in bad:self.assertEqual(self.parsed(f)[0],0,f[:21])
    def test_room_parser_rejects_duplicate_members_and_huge_lengths(self):
        def room(ips):
            frame=b'\x03'+text('Room')+text('')+struct.pack('>IH',8,24872)+text('Game')+text('')+struct.pack('>I',len(ips))
            for ip in ips:frame+=text('Guest')+struct.pack('>I',ip)+text('Game')+struct.pack('>Q',1)+text('v')+text('')+text('')+text('')
            return frame
        ips=(C.c_uint32*254)();count=C.c_uint()
        frame=room([0xc0a80102,0xc0a80103]);self.assertEqual(self.lib.yw_room(frame,len(frame),ips,C.byref(count)),1);self.assertEqual(count.value,2)
        for frame in (room([0xc0a80102]*2),b'\x03'+b'\xff'*4,room([0]),room([0xc0a80102])+b'extra'):
            self.assertEqual(self.lib.yw_room(frame,len(frame),ips,C.byref(count)),0)
if __name__=='__main__':unittest.main()
