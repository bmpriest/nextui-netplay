#!/usr/bin/env python3
"""Verify the production session setting controls the real UDP socket."""
from pathlib import Path
import os
import socket
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class HybridSettingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='netplay-hybrid-setting-')
        cls.folder=Path(cls.temp.name)
        main=cls.folder/'main.c'
        main.write_text('#include "netlink.h"\n#include <stdio.h>\n#include <unistd.h>\nint main(int argc,char **argv){ if(!NetLink_configure(argv[1])||!NetLink_start()) return 1; puts("READY");fflush(stdout);usleep(600000);NetLink_stop();return 0;}\n')
        cls.binary=cls.folder/'test'
        subprocess.run(['cc','-O1','-I'+str(ROOT/'shim'),str(main),str(ROOT/'shim/netlink.c'),str(ROOT/'shim/sha256.c'),'-lpthread','-lz','-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def check_setting(self,value,udp):
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
        session=self.folder/'session'
        session.write_text(f'role=host\nport={port}\n'+('' if value is None else f'hybrid_network={value}\n'))
        env={k:v for k,v in os.environ.items() if not k.startswith('NETLINK_TEST_')}
        child=subprocess.Popen([str(self.binary),str(session)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,env=env)
        try:
            self.assertEqual(child.stdout.readline().strip(),'READY')
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
                if udp:
                    with self.assertRaises(OSError):probe.bind(('0.0.0.0',port))
                else:probe.bind(('0.0.0.0',port))
            _,stderr=child.communicate(timeout=5)
            self.assertEqual(child.returncode,0,stderr)
            if not udp:self.assertIn('hybrid network traffic disabled',stderr)
        finally:
            if child.poll() is None:child.kill();child.wait()

    def test_missing_key_defaults_on(self):self.check_setting(None,True)
    def test_explicit_on(self):self.check_setting(1,True)
    def test_off_leaves_tcp_only(self):self.check_setting(0,False)

if __name__=='__main__':unittest.main()
