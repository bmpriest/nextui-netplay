#!/usr/bin/env python3
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("netplay-harness.py")
SPEC = importlib.util.spec_from_file_location("netplay_harness", SCRIPT)
HARNESS = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(HARNESS)


class HarnessTests(unittest.TestCase):
    def test_shared_state_layout(self):
        self.assertEqual(HARNESS.NETPLAY_STATE,
                         "/mnt/SDCARD/.userdata/shared/Netplay")
        self.assertEqual(HARNESS.HARNESS_STATE,
                         "/mnt/SDCARD/.userdata/shared/Netplay/netplay-harness")
        probe = HARNESS.probe_script("tg5040")
        self.assertIn(".userdata/shared/Netplay/session", probe)
        self.assertNotIn("Netplay.pak/state/session", probe)

    def test_parse_beacon(self):
        fields = HARNESS.parse_beacon(b"devbridge host=brick ip=192.168.0.180 port=22 sftp=yes\n")
        self.assertEqual(fields["ip"], "192.168.0.180")
        self.assertEqual(fields["sftp"], "yes")
        self.assertIsNone(HARNESS.parse_beacon(b"not-devbridge ip=1.2.3.4"))

    def test_guest_address_from_broker_or_arp(self):
        self.assertEqual(
            HARNESS.guest_address_from_status(
                "guest_count=1\nguest_0_id=aa:bb\nguest_0_ip=10.0.0.23\n"
            ),
            "10.0.0.23",
        )
        self.assertEqual(
            HARNESS.guest_address_from_status("10.0.0.37 0x1 aa:bb:cc\n"),
            "10.0.0.37",
        )
        self.assertIsNone(HARNESS.guest_address_from_status("guest_0_ip=192.168.0.144\n"))

    def test_correlated_session(self):
        sid = "0123456789abcdef0123456789abcdef"
        host = HARNESS.session_text("host", sid, "192.168.0.180", "auto", [])
        client = HARNESS.session_text("client", sid, "192.168.0.180", "link", ["example=1"])
        self.assertIn(f"session_id={sid}\n", host)
        self.assertNotIn("peer=", host)
        self.assertIn("peer=192.168.0.180\n", client)
        self.assertIn("mode=link\n", client)

    def test_artifact_destinations(self):
        destination, mode = HARNESS.artifact_destination("mgba-core", "tg5040")
        self.assertEqual(destination, "/mnt/SDCARD/Emus/tg5040/MGBA.pak/mgba_libretro.so")
        self.assertEqual(mode, "0644")
        destination, mode = HARNESS.artifact_destination("mgba-dual", "tg5040")
        self.assertEqual(destination, "/mnt/SDCARD/Emus/tg5040/MGBA.pak/mgba_dual_libretro.so")
        self.assertEqual(mode, "0644")
        destination, mode = HARNESS.artifact_destination("mgba-launcher", "tg5040")
        self.assertEqual(destination, "/mnt/SDCARD/Emus/tg5040/MGBA.pak/launch.sh")
        self.assertEqual(mode, "0755")
        destination, mode = HARNESS.artifact_destination("mgba-shim", "tg5040")
        self.assertEqual(destination, "/mnt/SDCARD/Emus/tg5040/MGBA.pak/netplay_shim.tg5040.so")
        self.assertEqual(mode, "0644")
        destination, mode = HARNESS.artifact_destination("netplay-app", "tg5040")
        self.assertEqual(destination, "/mnt/SDCARD/Tools/tg5040/Netplay.pak/bin/tg5040/netplay.elf")
        self.assertEqual(mode, "0755")
        destination, mode = HARNESS.artifact_destination("drastic", "tg5040")
        self.assertTrue(destination.endswith("/NDS.pak/drastic/drastic"))
        self.assertEqual(mode, "0755")

    def test_launch_plan_quotes_rom(self):
        command = HARNESS.launch_command("drastic", "tg5040", "/mnt/SDCARD/Roms/Nintendo DS (NDS)/game.nds")
        self.assertIn("NDS.pak/launch.sh", command)
        self.assertIn("'/mnt/SDCARD/Roms/Nintendo DS (NDS)/game.nds'", command)

    def test_sha256(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "artifact"
            path.write_bytes(b"abc")
            self.assertEqual(HARNESS.sha256(path), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

    def test_sha256_output_tolerates_login_banner(self):
        digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        output = f"Welcome to NextUI\n{digest}  /mnt/SDCARD/example\n"
        self.assertEqual(HARNESS.sha256_from_output(output), digest)
        self.assertIsNone(HARNESS.sha256_from_output("sha256sum: not found\n"))

    def test_guest_route_prefers_verified_jump(self):
        class FakeRemote:
            def run(self, address, command, check=False, jump=None):
                if "broker.status" in command:
                    return SimpleNamespace(returncode=0, stdout="guest_0_ip=10.0.0.20\n")
                if address == "10.0.0.20" and jump == "192.168.0.180":
                    return SimpleNamespace(returncode=0, stdout="")
                return SimpleNamespace(returncode=1, stdout="")

        args = SimpleNamespace(
            host="192.168.0.180", client="192.168.0.144", platform="tg5040",
            guest_ip=None, no_auto_jump=False,
        )
        self.assertEqual(
            HARNESS.resolve_client(FakeRemote(), args),
            ("10.0.0.20", "192.168.0.180"),
        )

    def test_guest_route_falls_back_to_lan(self):
        class FakeRemote:
            def run(self, address, command, check=False, jump=None):
                if "broker.status" in command:
                    return SimpleNamespace(returncode=0, stdout="guest_count=0\n")
                return SimpleNamespace(returncode=int(address != "192.168.0.144"), stdout="")

        args = SimpleNamespace(
            host="192.168.0.180", client="192.168.0.144", platform="tg5040",
            guest_ip=None, no_auto_jump=False,
        )
        self.assertEqual(HARNESS.resolve_client(FakeRemote(), args),
                         ("192.168.0.144", None))

    def test_mixed_platform_pair(self):
        args = HARNESS.build_parser().parse_args([
            "doctor", "--host", "192.168.0.41", "--client", "192.168.0.143",
            "--host-platform", "h700",
        ])
        self.assertEqual(HARNESS.platform_for(args, "host"), "h700")
        self.assertEqual(HARNESS.platform_for(args, "client"), "tg5040")
        destination, _ = HARNESS.artifact_destination(
            "netplay-app", HARNESS.platform_for(args, "host"))
        self.assertEqual(destination,
                         "/mnt/SDCARD/Tools/h700/Netplay.pak/bin/h700/netplay.elf")


if __name__ == "__main__":
    unittest.main()
