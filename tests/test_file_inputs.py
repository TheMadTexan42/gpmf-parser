"""Integration tests for raw/MP4 decoding and GPS-recorded GPX time.

Uses only the Python standard library. Personal samples are optional and are
never copied into the repository. Run directly or through CTest.
"""
import argparse
import collections
import datetime as dt
import pathlib
import re
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


def klv(key, kind, size, data, repeat=None):
    repeat = len(data) // size if repeat is None else repeat
    return key.encode() + struct.pack(">BBH", ord(kind) if kind else 0, size, repeat) + data + bytes(-len(data) % 4)


def nest(key, data):
    return klv(key, None, 1, data)


def device(data, number=1):
    return nest("DEVC", klv("DVID", "L", 4, struct.pack(">I", number)) + data)


def gps9(rows, stmp=987654321):
    scales = (10_000_000, 10_000_000, 1000, 1000, 100, 1, 1000, 100, 1)
    return device(nest("STRM",
        klv("STMP", "J", 8, struct.pack(">Q", stmp)) +
        klv("TYPE", "c", 1, b"lllllllSS") +
        klv("SCAL", "l", 4, struct.pack(">9i", *scales)) +
        klv("GPS9", "?", 32, b"".join(struct.pack(">7i2H", *row) for row in rows))))


def gps5(gpsu=None, fix=3, count=3):
    fields = klv("STMP", "J", 8, struct.pack(">Q", 999_999_999))
    if fix is not None:
        fields += klv("GPSF", "L", 4, struct.pack(">I", fix))
    if gpsu is not None:
        fields += klv("GPSU", "U", 16, gpsu)
    fields += klv("SCAL", "l", 4, struct.pack(">5i", 10_000_000, 10_000_000, 1000, 1000, 1000))
    fields += klv("GPS5", "l", 20, struct.pack(">5i", 400000000, -800000000, 123000, 0, 0) * count)
    return device(nest("STRM", fields))


def atom(key, value):
    return struct.pack(">I", len(value) + 8) + key.encode() + value


def words(*values):
    return struct.pack(">" + "I" * len(values), *values)


def mp4(payloads, duration=1000, edit=0):
    """Small metadata-only MP4 fixture with actual sample tables and optional edit.

    Deliberately contains no video: GPX export needs only the metadata track.
    """
    ftyp = atom("ftyp", b"isom" + words(0) + b"isom")
    offsets, offset = [], len(ftyp) + 8
    for payload in payloads:
        offsets.append(offset)
        offset += len(payload)
    movie_header = words(0, 0, 0, 1000, duration * len(payloads)) + bytes(80)
    media_header = words(0, 0, 0, 1000, duration * len(payloads)) + bytes(4)
    handler = words(0, 0) + b"meta" + bytes(12) + b"Metadata\0"
    description = words(0, 1) + atom("gpmd", bytes(6) + struct.pack(">H", 1))
    tables = atom("stsd", description)
    tables += atom("stsz", words(0, 0, len(payloads), *(len(p) for p in payloads)))
    tables += atom("stsc", words(0, 1, 1, 1, 1))
    tables += atom("stco", words(0, len(offsets), *offsets))
    tables += atom("stts", words(0, 1, len(payloads), duration))
    media = atom("mdhd", media_header) + atom("hdlr", handler) + atom("minf", atom("stbl", tables))
    # A leading empty edit introduces a different presentation offset.
    edits = atom("edts", atom("elst", words(0, 2, edit, 0xFFFFFFFF, 65536,
                                            duration * len(payloads), 0, 65536))) if edit else b""
    movie = atom("mvhd", movie_header) + atom("trak", atom("mdia", media) + edits)
    return ftyp + atom("mdat", b"".join(payloads)) + atom("moov", movie) + atom("free", bytes(8))


def points(path):
    ns = {"g": "http://www.topografix.com/GPX/1/1"}
    return [(p.attrib["lat"], p.attrib["lon"], p.findtext("g:ele", namespaces=ns),
             p.findtext("g:time", namespaces=ns))
            for p in ET.parse(path).findall(".//g:trkpt", ns)]


def records(data):
    pos = 0
    while pos < len(data):
        key = data[pos:pos+4].decode()
        kind, size, repeat = struct.unpack_from(">BBH", data, pos+4)
        end = pos + 8 + size * repeat
        yield key, kind, size, repeat, data[pos+8:end]
        pos = (end + 3) & ~3
    assert pos == len(data)


class FileInputs(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gpmf-input-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def run_tool(self, exe, *args, ok=True):
        result = subprocess.run([str(exe), *(str(a) for a in args)], capture_output=True,
                                text=True, errors="replace", timeout=60)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout[-2000:])
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def export(self, data, name="input", raw=True):
        source = self.write(name + (".gpmf" if raw else ".mp4"), data)
        output = self.root / (name + ".gpx")
        result = self.run_tool(ARGS.gpx, source, output, *( ["--raw"] if raw else []))
        return points(output), result

    def test_gps9_raw_mp4_and_changed_container_timing(self):
        rows = [(400000000, -800000000, 123456, 0, 0, 9772, 48058123, 65535, 3),
                (400000001, -800000001, 123457, 0, 0, 9772, 48058234, 200, 2),
                (400000002, -800000002, 123458, 0, 0, 9772, 48058345, 100, 0),
                (400000003, -800000003, 123459, 0, 0, 9772, 48058456, 100, 259)]
        raw = gps9(rows)
        expected = [("40.000000000", "-80.000000000", "123.456", "2026-10-03T13:20:58.123000Z"),
                    ("40.000000100", "-80.000000100", "123.457", "2026-10-03T13:20:58.234000Z")]
        for name, data, mode in [("raw", raw, True), ("mp4", mp4([raw]), False),
                                  ("retimed", mp4([raw], duration=17000, edit=90000), False)]:
            actual, result = self.export(data, name, mode)
            self.assertEqual(actual, expected)
            self.assertIn("skipped 2 below requested fix level", result.stderr)
            if not mode:
                diagnostic = self.run_tool(ARGS.demo, self.root / (name + ".mp4"), "-s", "-i", "-c").stdout
                expected_time = "90.000 to 107.000" if name == "retimed" else "0.000 to 1.000"
                self.assertIn(expected_time, diagnostic)

    def test_gps5_anchor_missing_and_invalid_time(self):
        for anchor, expected_first in [(b"261003132058.123", "2026-10-03T13:20:58.123000Z"),
                                        (None, None), (b"261399256199.123", None)]:
            # Recorded GPSU is 16 bytes; .123 is four characters after 12 digits.
            actual, result = self.export(gps5(anchor), "gps5")
            self.assertEqual(len(actual), 3)
            self.assertEqual(actual[0][3], expected_first)
            self.assertEqual([p[3] for p in actual[1:]], [None, None])
            untimed = 2 if expected_first else 3
            self.assertIn(f"({untimed} untimed)", result.stderr)
            from_mp4, _ = self.export(mp4([gps5(anchor)], edit=42000), "gps5-mp4", False)
            self.assertEqual(actual, from_mp4)

    def test_fix_requirements_for_both_formats_and_gps_types(self):
        rows = [(400000000 + i, -800000000, 123000, 0, 0, 9772, 48058000 + i*100, 100, fix)
                for i, fix in enumerate([0, 1, 2, 3, 259])]
        sources = [(gps9(rows), {"3d": [3], "2d": [2, 3], "all": [0, 1, 2, 3, 4]}),
                   (b"".join(gps5(fix=fix, count=1) for fix in [0, 2, 3, None]),
                    {"3d": [2], "2d": [1, 2], "all": [0, 1, 2, 3]})]
        for data, expected in sources:
            for raw in [True, False]:
                path = self.write("fix-input.gpmf" if raw else "fix-input.mp4", data if raw else mp4([data]))
                output = self.root / "fix-output.gpx"
                all_points = None
                for level in ["all", "2d", "3d"]:
                    with self.subTest(raw=raw, level=level, gps9=data[:100].find(b"GPS9") >= 0):
                        result = self.run_tool(ARGS.gpx, path, output, *( ["--raw"] if raw else []), "--fix", level)
                        actual = points(output)
                        if level == "all": all_points = actual
                        self.assertEqual(actual, [all_points[i] for i in expected[level]])
                        self.assertIn(f"skipped {len(all_points)-len(actual)} below requested fix level", result.stderr)
                self.run_tool(ARGS.gpx, path, output, *( ["--raw"] if raw else []), "--fix=3d")
                self.assertEqual(points(output), [all_points[i] for i in expected["3d"]])
                self.run_tool(ARGS.gpx, path, output, *( ["--raw"] if raw else []))
                self.assertEqual(points(output), [all_points[i] for i in expected["2d"]])
        for options in [["--fix"], ["--fix", "bad"], ["--fix="], ["--fix", "--raw"]]:
            self.run_tool(ARGS.gpx, *options, ok=False)

    def test_utc_rollover_leap_day_duplicates_and_regressions(self):
        day = (dt.date(2024, 2, 28) - dt.date(2000, 1, 1)).days
        times = [(day, 86399999), (day+1, 0), (day+1, 0), (day, 86399000),
                 (day+2, 0), (day+2, 86400000), (-1, 0)]
        rows = [(400000000, -800000000, 123000, 0, 0, d, s, 100, 3) for d, s in times]
        actual, result = self.export(gps9(rows))
        self.assertEqual([p[3] for p in actual], ["2024-02-28T23:59:59.999000Z",
            "2024-02-29T00:00:00.000000Z", "2024-02-29T00:00:00.000000Z",
            "2024-02-28T23:59:59.000000Z", "2024-03-01T00:00:00.000000Z", None, None])
        self.assertIn("1 duplicates, 1 regressions", result.stderr)
        self.assertIn("(2 untimed)", result.stderr)

    def test_validation_and_failure_exit_codes(self):
        raw = gps9([(400000000, -800000000, 123000, 0, 0, 9772, 48058000, 100, 3)])
        deep = klv("DVID", "L", 4, words(1))
        for _ in range(17): deep = nest("STRM", deep)
        bad_nested = device(b"ACCLs\x06\xff\xff" + bytes(8))
        malformed = [b"", b"DEVC", raw[:-1], raw + b"x", raw + bytes(8),
                     b"JUNK" + raw[4:], b"DEVC\0\1\xff\xff" + bytes(4),
                     device(deep), bad_nested, device(klv("ACCL", "s", 3, bytes(3)))]
        for i, data in enumerate(malformed):
            with self.subTest(i=i):
                path = self.write("bad.gpmf", data)
                self.run_tool(ARGS.demo, path, "--raw", "-s", ok=False)
                output = self.write("bad.gpx", b"existing output")
                self.run_tool(ARGS.gpx, path, output, "--raw", ok=False)
                self.assertEqual(output.read_bytes(), b"existing output")
        self.run_tool(ARGS.demo, self.root / "absent.mp4", ok=False)
        self.run_tool(ARGS.gpx, self.root / "absent.mp4", ok=False)

    def test_raw_indices_filter_and_fuzz_option_rejection(self):
        accel = device(nest("STRM", klv("SCAL", "s", 2, struct.pack(">h", 10)) +
                            klv("ACCL", "s", 6, struct.pack(">3h", 10, 20, 30))))
        path = self.write("two.gpmf", accel * 2)
        first = self.run_tool(ARGS.demo, path, "--raw", "-i").stdout
        all_records = self.run_tool(ARGS.demo, path, "--raw", "-i", "-a").stdout
        self.assertEqual(first.count("RAW RECORD "), 1)
        self.assertEqual(all_records.count("RAW RECORD "), 2)
        self.assertIn("Validated 2 raw records", first)
        self.assertIn("ACCL 1.000", first)
        filtered = self.run_tool(ARGS.demo, path, "--raw", "-fGYRO").stdout
        self.assertNotIn("ACCL 1.000", filtered)
        for option in ["-F1", "-M1", "-G1", "-fBAD", "--unknown"]:
            self.run_tool(ARGS.demo, path, "--raw", option, ok=False)
        # A invalid second record is checked even when only the first is displayed.
        self.run_tool(ARGS.demo, self.write("later-bad.gpmf", accel + b"bad"), "--raw", ok=False)

    def test_no_valid_gps_and_output_cannot_overwrite_input(self):
        raw = gps9([(400000000, -800000000, 123000, 0, 0, 9772, 48058000, 100, 0)])
        path = self.write("no-fix.gpmf", raw)
        self.run_tool(ARGS.gpx, path, self.root / "no-fix.gpx", "--raw", ok=False)
        self.run_tool(ARGS.gpx, path, path, "--raw", ok=False)
        self.assertEqual(path.read_bytes(), raw)
        bad_position = gps9([(1000000000, -800000000, 123000, 0, 0, 9772, 48058000, 100, 3)])
        self.run_tool(ARGS.gpx, self.write("bad-position.gpmf", bad_position), "--raw", ok=False)
        # An explicit invalid GPSF is different from an absent fix-quality field.
        invalid_fix = self.write("invalid-gpsf.gpmf", gps5(fix=0xFFFFFFFF))
        self.run_tool(ARGS.gpx, invalid_fix, "--raw", ok=False)

    def test_repository_raw_samples_and_corresponding_mp4_values(self):
        for raw in ARGS.samples.glob("*.raw"):
            with self.subTest(raw=raw.name):
                self.run_tool(ARGS.demo, raw, "--raw", "-g", "-a", "-s")
                raw_values = self.run_tool(ARGS.demo, raw, "--raw").stdout
                synthetic = self.write("wrapped.mp4", mp4([raw.read_bytes()]))
                mp4_values = self.run_tool(ARGS.demo, synthetic, "-c").stdout
                self.assertEqual(re.findall(r"^  ACCL .*$", raw_values, re.M),
                                 re.findall(r"^  ACCL .*$", mp4_values, re.M))
                original = raw.with_suffix(".mp4")
                if original.exists():
                    self.assertIn(raw.read_bytes(), original.read_bytes())
                    self.run_tool(ARGS.demo, original, "-g", "-s")
                    # Some .raw files are a later payload, rather than the first.
                    original_values = self.run_tool(ARGS.demo, original, "-a", "-c").stdout
                    raw_accel = re.findall(r"^  ACCL [-+\d].*$", raw_values, re.M)
                    original_accel = re.findall(r"^  ACCL [-+\d].*$", original_values, re.M)
                    self.assertTrue(raw_accel)
                    self.assertIn("\n".join(raw_accel), "\n".join(original_accel))

    def test_reference_gpmf(self):
        if not ARGS.reference_gpmf:
            self.skipTest("personal MAX2 reference not supplied")
        data = ARGS.reference_gpmf.read_bytes()
        top = list(records(data))
        self.assertEqual(len(top), 448)
        counts = collections.Counter()
        expected = []

        def inspect(chunk):
            for key, kind, size, count, value in records(chunk):
                if key in ("ACCL", "GYRO", "GPS9"): counts[key] += count
                if kind == 0: inspect(value)
                if key == "GPS9":
                    for pos in range(0, len(value), size):
                        row = struct.unpack_from(">7i2H", value, pos)
                        if row[8] not in (2, 3): continue
                        timestamp = dt.datetime(2000, 1, 1) + dt.timedelta(days=row[5], milliseconds=row[6])
                        expected.append((f"{row[0]/1e7:.9f}", f"{row[1]/1e7:.9f}",
                                         f"{row[2]/1000:.3f}", timestamp.strftime("%Y-%m-%dT%H:%M:%S.%fZ")))

        inspect(data)
        self.assertEqual(counts, {"ACCL": 88342, "GYRO": 353365, "GPS9": 4481})
        index = self.run_tool(ARGS.demo, ARGS.reference_gpmf, "--raw", "-a", "-i", "-s").stdout
        self.assertIn("Validated 448 raw records (6693736 bytes)", index)
        for key in ["ACCL", "GYRO", "GPS9"]:
            scaled = self.run_tool(ARGS.demo, ARGS.reference_gpmf, "--raw", "-a", "-f" + key).stdout
            self.assertEqual(len(re.findall(r"^  " + key + " ", scaled, re.M)), counts[key])
        # Exercise decompression for the camera's DISP stream and structure printing.
        self.run_tool(ARGS.demo, ARGS.reference_gpmf, "--raw", "-a", "-fDISP")
        self.run_tool(ARGS.demo, ARGS.reference_gpmf, "--raw", "-g", "-s")
        actual, result = self.export(data, "reference-raw")
        wrapped, _ = self.export(mp4([data], duration=17000, edit=42000), "reference-mp4", False)
        self.assertEqual(len(actual), 3559)
        self.assertEqual(actual, expected)
        self.assertEqual(wrapped, expected)
        self.assertIn("(0 untimed)", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", required=True, type=pathlib.Path)
    parser.add_argument("--gpx", required=True, type=pathlib.Path)
    parser.add_argument("--samples", required=True, type=pathlib.Path)
    parser.add_argument("--reference-gpmf", type=pathlib.Path)
    ARGS = parser.parse_args()
    ARGS.demo = ARGS.demo.resolve()
    ARGS.gpx = ARGS.gpx.resolve()
    unittest.main(argv=[__file__], verbosity=2)
