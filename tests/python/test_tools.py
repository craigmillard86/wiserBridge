"""
Unit tests for the PC-side tools. Run:  python -m unittest discover -s tests/python -v
Standard library only.
"""
import copy
import io
import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import fingerprint_from_log  # noqa: E402
import sniff_to_pcap as sp  # noqa: E402
import wb_fingerprint as wf  # noqa: E402
import wb_health_report as hr  # noqa: E402
import z2m_extract  # noqa: E402

# A synthetic interview record (fictional values, NOT a real device).
INTERVIEW_LINE = (
    '12:00:00 FINGERPRINT-JSON {"short_addr":"0x1234","ieee_addr_do_not_copy":"00:11:22:33:44:55:66:77",'
    '"node_descriptor":{"logical_type":"ZR","manufacturer_code":"0x1111","mac_capability_flags":"0x8e",'
    '"server_mask":"0x2c00","stack_compliance_revision":22,"max_buffer_size":80,'
    '"max_incoming_transfer_size":160,"max_outgoing_transfer_size":160,"node_desc_flags":"0x4001"},'
    '"endpoints":[{"endpoint":1,"profile_id":"0x0104","device_id":"0x0008","device_version":0,'
    '"input_clusters":["0x0000","0x0003","0xfc01"],"output_clusters":["0x0019"]}],'
    '"basic_attributes":{"0x0004":"TestCo","0x0005":"TR-1","0x0007":1},"note":"ok"}'
)


class FingerprintTests(unittest.TestCase):
    def test_template_file_is_valid_and_not_usable(self):
        fp = wf.load(os.path.join(ROOT, "fingerprints", "wa104.template.json"))
        self.assertEqual(wf.validate(fp), [])
        self.assertEqual(fp["status"], "template")
        with self.assertRaises(ValueError):
            wf.to_cli_commands(fp)

    def test_interview_roundtrip(self):
        recs = fingerprint_from_log.extract_records(INTERVIEW_LINE)
        self.assertEqual(len(recs), 1)
        rec = recs[0]
        rec.pop("ieee_addr_do_not_copy")
        fp = wf.from_interview(rec, reference="unit-test")
        self.assertEqual(fp["status"], "captured")
        self.assertEqual(fp["basic"]["manufacturer_name"], "TestCo")
        self.assertEqual(fp["proprietary_clusters"], ["0xfc01"])
        self.assertEqual(wf.validate(fp), [])
        self.assertNotIn("00:11:22:33:44:55:66:77", json.dumps(fp))
        cmds = wf.to_cli_commands(fp)
        self.assertIn("profile set manufacturer TestCo", cmds)
        self.assertIn("profile set in_clusters 0xfc01", cmds)
        self.assertIn("profile set out_clusters 0x0019", cmds)
        self.assertIn("profile set identify on", cmds)
        self.assertIn("profile set manufacturer_code 0x1111", cmds)

    def test_incomplete_interview_stays_template(self):
        rec = json.loads(INTERVIEW_LINE.split("FINGERPRINT-JSON ", 1)[1])
        rec["basic_attributes"] = "timeout"
        fp = wf.from_interview(rec)
        self.assertEqual(fp["status"], "template")

    def test_forbidden_fields_rejected(self):
        fp = wf.empty_fingerprint()
        fp["node"]["ieee_addr"] = "00:11:22:33:44:55:66:77"
        self.assertTrue(any("forbidden" in e for e in wf.validate(fp)))
        fp = wf.empty_fingerprint()
        fp["network_key"] = "x"
        self.assertTrue(any("forbidden" in e for e in wf.validate(fp)))

    def test_captured_requires_evidence(self):
        rec = json.loads(INTERVIEW_LINE.split("FINGERPRINT-JSON ", 1)[1])
        fp = wf.from_interview(rec)
        fp2 = copy.deepcopy(fp)
        fp2["evidence"] = []
        self.assertTrue(any("evidence" in e for e in wf.validate(fp2)))

    def test_bad_cluster_format(self):
        fp = wf.empty_fingerprint()
        fp["endpoints"] = [{"endpoint": 1, "input_clusters": ["6"], "output_clusters": []}]
        self.assertTrue(wf.validate(fp))


class Z2MTests(unittest.TestCase):
    def test_convert(self):
        rec = {"id": 2, "type": "Router", "ieeeAddr": "0x0011223344556677", "manufId": 4369,
               "manufName": "TestCo", "modelId": "TR-1", "powerSource": 1, "interviewCompleted": True,
               "endpoints": {"1": {"epId": 1, "profId": 260, "devId": 8, "inClusterList": [0, 3],
                                   "outClusterList": [25]}}}
        with tempfile.TemporaryDirectory() as d:
            db = os.path.join(d, "database.db")
            with open(db, "w") as f:
                f.write(json.dumps({"id": 1, "type": "Coordinator", "ieeeAddr": "0x00"}) + "\n")
                f.write(json.dumps(rec) + "\n")
            recs = z2m_extract.read_db(db)
        fp = z2m_extract.convert(recs[1], "t", "unit")
        self.assertEqual(fp["status"], "captured")
        self.assertEqual(fp["node"]["manufacturer_code"], "0x1111")
        self.assertEqual(fp["endpoints"][0]["profile_id"], "0x0104")
        self.assertEqual(fp["endpoints"][0]["output_clusters"], ["0x0019"])
        self.assertNotIn("0x0011223344556677", json.dumps(fp))
        self.assertEqual(wf.validate(fp), [])


class PcapTests(unittest.TestCase):
    def test_parse_line(self):
        rec = sp.parse_line("F 15 -60 200 123456 4188013412ffff0000")
        self.assertEqual(rec[0], 15)
        self.assertEqual(rec[1], -60)
        self.assertEqual(rec[4], bytes.fromhex("4188013412ffff0000"))
        self.assertIsNone(sp.parse_line("#WBSNIFF v1 ch=15"))
        self.assertIsNone(sp.parse_line("F 15 -60 200 1 zz"))
        self.assertIsNone(sp.parse_line("F 15 -60 200 1"))

    def test_nofcs_pcap(self):
        buf = io.BytesIO()
        w = sp.PcapWriter(buf, sp.LINKTYPE_NOFCS)
        n = sp.convert_lines(["F 15 -60 200 1000 0102", "junk", "F 15 -61 199 3000 030405"], w, t0=100.0)
        self.assertEqual(n, 2)
        data = buf.getvalue()
        magic, vmaj, vmin, _, _, snap, lt = struct.unpack("<IHHiIII", data[:24])
        self.assertEqual((magic, vmaj, vmin, lt), (0xA1B2C3D4, 2, 4, 230))
        sec, usec, incl, orig = struct.unpack("<IIII", data[24:40])
        self.assertEqual((sec, usec, incl, orig), (100, 0, 2, 2))
        self.assertEqual(data[40:42], b"\x01\x02")
        sec2, usec2, incl2, _ = struct.unpack("<IIII", data[42:58])
        self.assertEqual((sec2, usec2, incl2), (100, 2000, 3))

    def test_tap_header(self):
        h = sp.tap_header(15, -60, 200)
        ver, res, length = struct.unpack("<BBH", h[:4])
        self.assertEqual((ver, res), (0, 0))
        self.assertEqual(length, len(h))
        self.assertEqual(length % 4, 0)
        # FCS TLV
        self.assertEqual(struct.unpack("<HHB", h[4:9]), (0, 1, 0))
        # RSS TLV
        t, l, rss = struct.unpack("<HHf", h[12:20])
        self.assertEqual((t, l, rss), (1, 4, -60.0))
        # channel TLV
        t, l, ch, page = struct.unpack("<HHHB", h[20:27])
        self.assertEqual((t, l, ch, page), (3, 3, 15, 0))
        # LQI TLV
        t, l, lqi = struct.unpack("<HHB", h[28:33])
        self.assertEqual((t, l, lqi), (10, 1, 200))


class HealthReportTests(unittest.TestCase):
    def test_parse(self):
        lines = [
            "2026-11-05T10:00:00.000  Reset reason    : power-on\n",
            "2026-11-05T10:05:00.000 HEALTH up=300s state=JOINED short=0x1234 pan=0x7cc1 ch=11 nbr=3 rtr=2 child=1 "
            "sleepy=1 routes=2 coord_nh=0x5678 heap=300000 minheap=290000 rejoins=1 leaves=0 nlme=0 noLinks=0 "
            "verdict=JOINED_LINKS_PRESENT\n",
            "2026-11-06T10:05:00.000 HEALTH up=86700s state=JOINED short=0x1234 pan=0x7cc1 ch=11 nbr=3 rtr=0 child=2 "
            "sleepy=2 routes=2 coord_nh=0x5678 heap=300000 minheap=289000 rejoins=1 leaves=0 nlme=3 noLinks=1 "
            "verdict=JOINED_LINKS_PRESENT\n",
        ]
        r = hr.parse(lines)
        self.assertEqual(r.samples, 2)
        self.assertEqual(r.joined_samples, 2)
        self.assertEqual(r.isolated_samples, 1)
        self.assertEqual(r.max_children, 2)
        self.assertEqual(r.min_heap, 289000)
        self.assertEqual(len(r.boots), 1)
        self.assertEqual(r.last_kv["nlme"], "3")


if __name__ == "__main__":
    unittest.main()
