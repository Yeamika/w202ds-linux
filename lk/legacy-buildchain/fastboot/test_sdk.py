"""Run the real Android SDK client against the production Fastboot engine.

TCP adds only the AOSP FB01/length framing; the C engine still handles every
command, DATA response, download, partition write and readback comparison.
"""
import ctypes
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

class SdkTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('FASTBOOT'), 'set FASTBOOT to an Android SDK client')
    def test_sdk_download_flash_and_getvar(self):
        include=Path(os.environ['W202_HANDOFF_INCLUDE'])
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory)
            subprocess.run([os.environ.get('CC','cc'),'-shared','-fPIC','-O1','-Wall',
                '-Wextra','-Werror','-I'+str(include),str(Path(__file__).with_name('sdk_harness.c')),
                '-o',str(tmp/'engine.so')],check=True)
            lib=ctypes.CDLL(str(tmp/'engine.so'))
            lib.host_pending.restype=ctypes.c_uint
            lib.host_receive.argtypes=[ctypes.c_void_p,ctypes.c_uint]
            lib.host_transmit.argtypes=[ctypes.c_void_p]
            errors=[]
            listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(4)
            listener.settimeout(20)
            port=listener.getsockname()[1]
            def receive(conn,n):
                result=b''
                while len(result)<n:
                    part=conn.recv(n-len(result))
                    if not part:raise EOFError
                    result+=part
                return result
            def server():
                try:
                    # Each SDK invocation owns a new TCP connection.
                    for _ in range(3):
                        conn,_=listener.accept()
                        with conn:
                            conn.settimeout(20)
                            self.assertEqual(receive(conn,4),b'FB01');conn.sendall(b'FB01')
                            lib.host_reset()
                            while True:
                                pending=lib.host_pending()
                                if pending&0x80000000:
                                    buf=ctypes.create_string_buffer(64)
                                    n=lib.host_transmit(buf)
                                    conn.sendall(struct.pack('>Q',n)+buf.raw[:n]);continue
                                try:n=struct.unpack('>Q',receive(conn,8))[0]
                                except EOFError:break
                                self.assertLessEqual(n,0x1000000)
                                packet=receive(conn,n)
                                while packet:
                                    pending=lib.host_pending();self.assertFalse(pending&0x80000000)
                                    part=packet[:pending];packet=packet[pending:]
                                    self.assertTrue(part)
                                    self.assertEqual(lib.host_receive(part,len(part)),0)
                except BaseException as error:errors.append(error)
            thread=threading.Thread(target=server,daemon=True);thread.start()
            image=tmp/'uboot-b.img'
            image.write_bytes((bytes(range(256))*20480))
            client=[os.environ['FASTBOOT'],'-s',f'tcp:127.0.0.1:{port}']
            query=subprocess.run(client+['getvar','version'],capture_output=True,text=True,timeout=20)
            self.assertEqual(query.returncode,0,query.stdout+query.stderr)
            self.assertIn('0.4',query.stdout+query.stderr)
            flash=subprocess.run(client+['flash','uboot_b',str(image)],capture_output=True,text=True,timeout=30)
            self.assertEqual(flash.returncode,0,flash.stdout+flash.stderr)
            self.assertIn("Writing 'uboot_b'",flash.stdout+flash.stderr)
            self.assertEqual(lib.host_writes(),0x500000//LK_CHUNK)
            self.assertEqual(lib.host_matches(),1)
            wrong=subprocess.run(client+['flash','uboot',str(image)],capture_output=True,text=True,timeout=30)
            self.assertNotEqual(wrong.returncode,0)
            self.assertEqual(lib.host_writes(),0x500000//LK_CHUNK)
            thread.join(timeout=5);listener.close()
            self.assertFalse(thread.is_alive());self.assertFalse(errors,errors)

LK_CHUNK=16384
if __name__=='__main__':unittest.main()
