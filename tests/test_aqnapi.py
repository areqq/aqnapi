#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Testy jednostkowe aqnapi (stdlib unittest, offline).

Uruchom:  python3 -m unittest discover -s tests -v
Testy sieciowe: brak (wszystko offline / syntetyczne dane)."""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import aqnapi  # noqa: E402


def make_file(data: bytes) -> str:
    fd, path = tempfile.mkstemp()
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    return path


class TestHashes(unittest.TestCase):
    def test_oshash_known(self):
        # plik 128 KiB: pierwsze 64KiB same 0x01, ostatnie 64KiB same 0x02.
        head = b"\x01" * aqnapi.OSH_CHUNK
        tail = b"\x02" * aqnapi.OSH_CHUNK
        path = make_file(head + tail)
        try:
            size = 2 * aqnapi.OSH_CHUNK
            mask = 0xFFFFFFFFFFFFFFFF
            # 0x0101010101010101 * 8192 + 0x0202... * 8192 + size
            word1 = struct.unpack("<Q", b"\x01" * 8)[0]
            word2 = struct.unpack("<Q", b"\x02" * 8)[0]
            expected = (size + word1 * 8192 + word2 * 8192) & mask
            self.assertEqual(aqnapi.oshash(path), "%016x" % expected)
        finally:
            os.remove(path)

    def test_oshash_too_small(self):
        path = make_file(b"\x00" * 100)
        try:
            with self.assertRaises(aqnapi.AqError):
                aqnapi.oshash(path)
        finally:
            os.remove(path)

    def test_md5_10mb(self):
        import hashlib
        data = b"abc" * 1000
        path = make_file(data)
        try:
            self.assertEqual(aqnapi.md5_10mb(path), hashlib.md5(data).hexdigest())
        finally:
            os.remove(path)


class TestFps(unittest.TestCase):
    def _ebml_num(self, value, width):
        return value.to_bytes(width, "big")

    def test_mkv_default_duration(self):
        # Zbuduj minimalny MKV: Segment > Tracks > TrackEntry >
        #   TrackType(0x83)=1, DefaultDuration(0x23E383)=40000000 ns -> 25 fps.
        default_dur = struct.pack(">I", 40_000_000)
        track_type = bytes([0x83, 0x81, 0x01])  # id, size=1, value=1
        dd = bytes([0x23, 0xE3, 0x83, 0x84]) + default_dur  # id(3B) size=4 val
        track_entry_body = track_type + dd
        track_entry = bytes([0xAE, 0x80 | len(track_entry_body)]) + track_entry_body
        tracks_body = track_entry
        tracks = bytes([0x16, 0x54, 0xAE, 0x6B]) + bytes([0x80 | len(tracks_body)]) + tracks_body
        seg_body = tracks
        segment = bytes([0x18, 0x53, 0x80, 0x67]) + bytes([0x80 | len(seg_body)]) + seg_body
        ebml_header = b"\x1a\x45\xdf\xa3\x80"  # pusty EBML header (size 0)
        data = ebml_header + segment
        path = make_file(data)
        try:
            fps = aqnapi.fps_from_file(path)
            self.assertIsNotNone(fps)
            self.assertAlmostEqual(fps, 25.0, places=2)
        finally:
            os.remove(path)

    def test_avi_fps(self):
        # RIFF....AVI z dwMicroSecPerFrame na offsecie 32 = 40000 -> 25 fps
        data = bytearray(b"RIFF" + b"\x00" * 60)
        struct.pack_into("<I", data, 32, 40_000)
        path = make_file(bytes(data))
        try:
            self.assertAlmostEqual(aqnapi.fps_from_file(path), 25.0, places=2)
        finally:
            os.remove(path)

    def test_mp4_fps(self):
        # Zbuduj minimalny MP4: ftyp + moov>trak>mdia>{hdlr,mdhd,minf>stbl>stts}
        def box(btype, payload):
            return struct.pack(">I", len(payload) + 8) + btype + payload

        ftyp = box(b"ftyp", b"isom" + b"\x00" * 4 + b"isom")
        # hdlr: version+flags(4) predef(4) handler 'vide' + reszta
        hdlr_payload = b"\x00\x00\x00\x00" + b"\x00\x00\x00\x00" + b"vide" + b"\x00" * 12 + b"\x00"
        hdlr = box(b"hdlr", hdlr_payload)
        # mdhd v0: version+flags(4) ctime(4) mtime(4) timescale(4)=24000 duration(4) lang(2) pre(2)
        mdhd_payload = (b"\x00\x00\x00\x00" + b"\x00" * 4 + b"\x00" * 4 +
                        struct.pack(">I", 24000) + struct.pack(">I", 240000) +
                        b"\x00\x00" + b"\x00\x00")
        mdhd = box(b"mdhd", mdhd_payload)
        # stts: version+flags(4) entry_count(4)=1 [count=10, delta=1001] -> 24000/1001=23.976
        stts_payload = (b"\x00\x00\x00\x00" + struct.pack(">I", 1) +
                        struct.pack(">II", 10, 1001))
        stts = box(b"stts", stts_payload)
        stbl = box(b"stbl", stts)
        minf = box(b"minf", stbl)
        mdia = box(b"mdia", hdlr + mdhd + minf)
        trak = box(b"trak", mdia)
        moov = box(b"moov", trak)
        path = make_file(ftyp + moov)
        try:
            fps = aqnapi.fps_from_file(path)
            self.assertIsNotNone(fps)
            self.assertAlmostEqual(fps, 23.976, places=2)
        finally:
            os.remove(path)

    def test_trusted_fps_gate(self):
        self.assertEqual(aqnapi.trusted_fps(25.0), 25.0)
        self.assertIsNone(aqnapi.trusted_fps(60.0))
        self.assertIsNone(aqnapi.trusted_fps(None))


class TestCrypto(unittest.TestCase):
    def test_n24_obf_roundtrip(self):
        for s in ["pl", "tt1234567", "login@example.com", "hasło123"]:
            self.assertEqual(aqnapi.n24_deobf(aqnapi.n24_obf(s)).decode("utf-8"), s)

    def test_np_password(self):
        self.assertEqual(aqnapi.np_encode_password(""), "")
        # znany: 'a' (0x61) ^ 3 = 0x62 = 'b' -> base64('b') = 'Yg=='
        self.assertEqual(aqnapi.np_encode_password("a"), "Yg==")

    def test_aes256_fips_vector(self):
        # FIPS-197 Appendix C.3 (AES-256)
        key = bytes.fromhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
        pt = bytes.fromhex("00112233445566778899aabbccddeeff")
        ct = bytes.fromhex("8ea2b7ca516745bfeafc49904b496089")
        self.assertEqual(aqnapi.AES(key).encrypt_block(pt), ct)


class Test7z(unittest.TestCase):
    def test_number_roundtrip(self):
        def decode(data):
            first = data[0]
            mask = 0x80
            value = 0
            i = 0
            for i in range(8):
                if not (first & mask):
                    value |= (first & (mask - 1)) << (8 * i)
                    break
                b = data[1 + i]
                value |= b << (8 * i)
                mask >>= 1
            return value
        for v in [0, 1, 127, 128, 255, 256, 16384, 1_000_000, 2**32]:
            self.assertEqual(decode(aqnapi._7z_number(v)), v, f"dla {v}")

    @unittest.skipUnless(shutil.which("7z") or shutil.which("7za"),
                         "brak binarki 7z do weryfikacji")
    def test_write_7z_aes_extractable(self):
        content = ("Napisy testowe\nz polskimi znakami: ąćęłń\n" * 50).encode("utf-8")
        archive = aqnapi.write_7z_aes("abc123.txt", content)
        tmpdir = tempfile.mkdtemp()
        try:
            arc_path = os.path.join(tmpdir, "test.7z")
            with open(arc_path, "wb") as f:
                f.write(archive)
            exe = shutil.which("7z") or shutil.which("7za")
            proc = subprocess.run(
                [exe, "x", f"-p{aqnapi.SEVENZIP_PASSWORD}", "-y",
                 f"-o{tmpdir}", arc_path],
                capture_output=True)
            self.assertEqual(proc.returncode, 0,
                             f"7z x nie powiódł się: {proc.stderr.decode(errors='replace')}")
            with open(os.path.join(tmpdir, "abc123.txt"), "rb") as f:
                self.assertEqual(f.read(), content)
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


class TestSubtitleEngine(unittest.TestCase):
    def test_microdvd_to_srt(self):
        text = "{0}{25}Pierwsza linia|Druga linia\n{50}{75}Trzecia"
        srt = aqnapi.to_srt(text, "microdvd", fps=25.0)
        self.assertIn("00:00:00,000 --> 00:00:01,000", srt)
        self.assertIn("Pierwsza linia", srt)
        self.assertIn("Druga linia", srt)
        self.assertIn("00:00:02,000 --> 00:00:03,000", srt)

    def test_mpl2_to_srt(self):
        text = "[0][10]Hej|/kursywa"
        srt = aqnapi.to_srt(text, "mpl2")
        self.assertIn("00:00:00,000 --> 00:00:01,000", srt)
        self.assertIn("kursywa", srt)

    def test_tmplayer_to_srt(self):
        text = "00:00:01:Pierwsza\n00:00:05:Druga"
        srt = aqnapi.to_srt(text, "tmplayer")
        self.assertIn("00:00:01,000 -->", srt)

    def test_srt_passthrough(self):
        text = "1\n00:00:01,000 --> 00:00:02,000\nTekst\n"
        srt = aqnapi.to_srt(text)
        self.assertIn("00:00:01,000 --> 00:00:02,000", srt)
        self.assertIn("Tekst", srt)

    def test_detect_format(self):
        self.assertEqual(aqnapi.detect_format("{0}{25}x"), "microdvd")
        self.assertEqual(aqnapi.detect_format("[0][10]x"), "mpl2")
        self.assertEqual(aqnapi.detect_format("00:00:01:x"), "tmplayer")
        self.assertEqual(aqnapi.detect_format("1\n00:00:01,000 --> 00:00:02,000\nx"), "srt")
        self.assertEqual(aqnapi.detect_format("WEBVTT\n\n00:01.000 --> 00:02.000\nx"), "vtt")
        self.assertEqual(aqnapi.detect_format("[Script Info]\n[Events]\nDialogue: 0,..."), "ass")

    def test_vtt_to_srt(self):
        vtt = ("WEBVTT\n\n"
               "1\n"
               "00:00:01.000 --> 00:00:02.500 align:start position:10%\n"
               "<v Roger>Cze&amp;ść <i>tam</i>\n\n"
               "00:03.000 --> 00:04.000\n"          # MM:SS.mmm bez godzin
               "Druga linia\n")
        srt = aqnapi.to_srt(vtt)
        self.assertIn("00:00:01,000 --> 00:00:02,500", srt)
        self.assertIn("00:00:03,000 --> 00:00:04,000", srt)
        self.assertIn("Cze&ść tam", srt)   # encja zdekodowana, tagi usunięte
        self.assertNotIn("align:start", srt)
        self.assertNotIn("<v", srt)

    def test_ass_to_srt(self):
        ass = (
            "[Script Info]\nTitle: x\n\n"
            "[V4+ Styles]\nFormat: Name, Fontname\nStyle: Default,Arial\n\n"
            "[Events]\n"
            "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
            "Dialogue: 0,0:00:01.00,0:00:03.50,Default,,0,0,0,,"
            "Witaj {\\i1}świecie{\\i0}\\Ndruga, z przecinkiem\n"
        )
        srt = aqnapi.to_srt(ass)
        self.assertIn("00:00:01,000 --> 00:00:03,500", srt)
        self.assertIn("Witaj świecie", srt)          # override usunięte
        self.assertIn("druga, z przecinkiem", srt)          # przecinek w treści zachowany
        self.assertNotIn("{", srt)

    def test_ass_time(self):
        self.assertEqual(aqnapi._ass_ts_to_ms("0:00:01.50"), 1500)
        self.assertEqual(aqnapi._vtt_ts_to_ms("00:02.250"), 2250)
        self.assertEqual(aqnapi._vtt_ts_to_ms("01:00:00.000"), 3600000)

    def test_emit_bom_lf(self):
        out = aqnapi.emit_srt("1\r\n00:00:01,000 --> 00:00:02,000\r\nX\r\n")
        self.assertTrue(out.startswith(aqnapi.UTF8_BOM))
        self.assertNotIn(b"\r", out)

    def test_encoding_detection(self):
        self.assertEqual(aqnapi.detect_encoding("żółć".encode("utf-8")), "utf-8")
        self.assertEqual(aqnapi.detect_encoding(b"\xef\xbb\xbfx"), "utf-8-sig")

    def test_convert_zip(self):
        buf = io.BytesIO()
        import zipfile
        with zipfile.ZipFile(buf, "w") as z:
            z.writestr("napisy.srt", "1\n00:00:01,000 --> 00:00:02,000\nCześć\n")
            z.writestr("Napisy24.pl.url", "x")
        out = aqnapi.convert_to_srt_bytes(buf.getvalue())
        self.assertTrue(out.startswith(aqnapi.UTF8_BOM))
        self.assertIn("Cześć".encode("utf-8"), out)

    def test_napisy24_normalize_crlf(self):
        out = aqnapi.normalize_for_napisy24("1\n00:00:01,000 --> 00:00:02,000\nX\n")
        self.assertIn(b"\r\n", out)

    def test_napisy24_check_too_many_lines(self):
        text = "1\n00:00:01,000 --> 00:00:02,000\nA\nB\nC\n"
        problems = aqnapi.check_srt_for_napisy24(text)
        self.assertTrue(any("max 2" in p for p in problems))


class TestSanitize(unittest.TestCase):
    def test_strip_format_tags(self):
        self.assertEqual(aqnapi.strip_format_tags("<i>Cześć</i> <b>świecie</b>"),
                         "Cześć świecie")
        self.assertEqual(aqnapi.strip_format_tags('<font color="#fff">X</font>'), "X")
        self.assertEqual(aqnapi.strip_format_tags("{\\an8}Góra"), "Góra")
        # nie ruszaj zwykłego tekstu z '<' matematycznym
        self.assertEqual(aqnapi.strip_format_tags("3 < 5 oraz a>b"), "3 < 5 oraz a>b")

    def test_sanitize_strips_and_clamps(self):
        cues = [
            aqnapi.Cue(0, 300000, ["<i>Bardzo długo</i>"]),   # 300 s -> clamp
            aqnapi.Cue(310000, 311000, ["{\\an8}Tekst"]),
        ]
        out, rep = aqnapi.sanitize_cues(cues, max_display_ms=10000)
        self.assertEqual(out[0].end_ms, 10000)            # skrócone do 10 s
        self.assertEqual(rep.long_clamped, 1)
        self.assertEqual(out[0].lines, ["Bardzo długo"])  # tag usunięty
        self.assertGreaterEqual(rep.tags_stripped, 1)

    def test_sanitize_overlap_fix(self):
        cues = [aqnapi.Cue(0, 5000, ["A"]), aqnapi.Cue(3000, 6000, ["B"])]
        out, rep = aqnapi.sanitize_cues(cues)
        self.assertLessEqual(out[0].end_ms, out[1].start_ms)
        self.assertEqual(rep.overlaps_fixed, 1)

    def test_sanitize_nonpositive_and_empty(self):
        cues = [
            aqnapi.Cue(1000, 500, ["Odwrócony"]),   # koniec < start
            aqnapi.Cue(2000, 3000, ["<i></i>"]),     # pusty po czyszczeniu
        ]
        out, rep = aqnapi.sanitize_cues(cues)
        self.assertEqual(rep.nonpositive_fixed, 1)
        self.assertEqual(rep.empty_removed, 1)
        self.assertTrue(out[0].end_ms > out[0].start_ms)

    def test_convert_rejects_broken(self):
        with self.assertRaises(aqnapi.AqError):
            aqnapi.convert_to_srt_bytes("to nie są żadne napisy".encode("utf-8"))

    def test_convert_applies_sanitize(self):
        srt = ("1\n00:00:00,000 --> 00:05:00,000\n<i>Długo</i>\n").encode("utf-8")
        out = aqnapi.convert_to_srt_bytes(srt, max_display_ms=7000)
        text = out.decode("utf-8-sig")
        self.assertIn("00:00:07,000", text)   # skrócone
        self.assertNotIn("<i>", text)          # tag usunięty


class TestSync(unittest.TestCase):
    def test_compute_transform(self):
        self.assertEqual(aqnapi.compute_sync_transform([]), (1.0, 0.0))
        # 1 para -> samo przesunięcie
        self.assertEqual(aqnapi.compute_sync_transform([(1000, 3000)]), (1.0, 2000.0))
        # 2 pary -> skala + offset: new = 2*old
        scale, off = aqnapi.compute_sync_transform([(1000, 2000), (5000, 10000)])
        self.assertAlmostEqual(scale, 2.0)
        self.assertAlmostEqual(off, 0.0)

    def test_apply_sync(self):
        cues = [aqnapi.Cue(1000, 2000, ["a"]), aqnapi.Cue(5000, 6000, ["b"])]
        out = aqnapi.apply_sync(cues, 2.0, 0.0)
        self.assertEqual((out[0].start_ms, out[0].end_ms), (2000, 4000))
        self.assertEqual((out[1].start_ms, out[1].end_ms), (10000, 12000))

    def test_parse_user_time(self):
        self.assertEqual(aqnapi._parse_user_time("12.5"), 12500)
        self.assertEqual(aqnapi._parse_user_time("00:00:02,250"), 2250)
        self.assertEqual(aqnapi._parse_user_time("01:00:00.000"), 3600000)
        self.assertEqual(aqnapi._parse_user_time("2:05"), 125000)  # MM:SS
        self.assertIsNone(aqnapi._parse_user_time("nonsens"))

    def test_shift_cue(self):
        c = aqnapi.Cue(1000, 2000, ["x"])
        aqnapi._shift_cue(c, 500)
        self.assertEqual((c.start_ms, c.end_ms), (1500, 2500))
        aqnapi._shift_cue(c, -5000)          # nie zejdzie poniżej 0
        self.assertEqual(c.start_ms, 0)

    def _write_srt(self, cues):
        text = "\n".join(
            f"{i}\n{aqnapi._ms_to_srt(s)} --> {aqnapi._ms_to_srt(e)}\n{txt}\n"
            for i, (s, e, txt) in enumerate(cues, 1)) + "\n"
        return make_file(text.encode("utf-8"))

    def test_sync_offset(self):
        ref = self._write_srt([(2000, 3000, "R1")])
        tgt = self._write_srt([(500, 1500, "T1"), (4000, 5000, "T2")])
        out = tgt + ".synced.srt"
        try:
            rc = aqnapi.main(["sync", ref, tgt, "-o", out, "--offset", "1.5"])
            self.assertEqual(rc, 0)
            data = open(out, "rb").read().decode("utf-8-sig")
            self.assertIn("00:00:02,000 -->", data)   # 500 + 1500ms
            self.assertIn("00:00:05,500 -->", data)   # 4000 + 1500ms
        finally:
            for p in (ref, tgt, out):
                if os.path.exists(p):
                    os.remove(p)

    def test_sync_anchor_scale(self):
        ref = self._write_srt([(2000, 3000, "R1"), (10000, 11000, "R2")])
        tgt = self._write_srt([(1000, 2000, "T1"), (5000, 6000, "T2")])
        out = tgt + ".synced.srt"
        try:
            rc = aqnapi.main(["sync", ref, tgt, "-o", out,
                              "--anchor", "1,1", "--anchor", "2,2"])
            self.assertEqual(rc, 0)
            data = open(out, "rb").read().decode("utf-8-sig")
            self.assertIn("00:00:02,000 -->", data)    # 1000 * 2 -> zgodne z ref1
            self.assertIn("00:00:10,000 -->", data)    # 5000 * 2 -> zgodne z ref2 (skala)
        finally:
            for p in (ref, tgt, out):
                if os.path.exists(p):
                    os.remove(p)


class TestExportAndSdhAndFps(unittest.TestCase):
    def _cues(self):
        return [aqnapi.Cue(1000, 2000, ["Linia 1", "Linia 2"]),
                aqnapi.Cue(3000, 4500, ["Druga"])]

    def test_export_vtt(self):
        out = aqnapi.emit_subtitle(self._cues(), "vtt").decode("utf-8")
        self.assertTrue(out.startswith("WEBVTT"))
        self.assertIn("00:00:01.000 --> 00:00:02.000", out)
        self.assertNotIn("﻿", out)   # bez BOM

    def test_export_ass(self):
        out = aqnapi.emit_subtitle(self._cues(), "ass").decode("utf-8")
        self.assertIn("[Events]", out)
        self.assertIn("Dialogue: 0,0:00:01.00,0:00:02.00", out)
        self.assertIn("Linia 1\\NLinia 2", out)   # \N między liniami

    def test_export_microdvd_roundtrips_fps(self):
        out = aqnapi.emit_subtitle(self._cues(), "microdvd", fps=25.0).decode("utf-8")
        self.assertIn("{25}{50}Linia 1|Linia 2", out)   # 1000ms*25/1000=25

    def test_srt_export_has_bom(self):
        self.assertTrue(aqnapi.emit_subtitle(self._cues(), "srt").startswith(aqnapi.UTF8_BOM))

    def test_strip_sdh(self):
        self.assertEqual(aqnapi.strip_sdh_line("[muzyka] Cześć"), "Cześć")
        self.assertEqual(aqnapi.strip_sdh_line("(wzdycha) No dobra"), "No dobra")
        self.assertEqual(aqnapi.strip_sdh_line("JAN: Witaj"), "Witaj")
        self.assertEqual(aqnapi.strip_sdh_line("♪ La la la ♪"), "La la la")

    def test_sanitize_strip_sdh_flag(self):
        cues = [aqnapi.Cue(0, 2000, ["[śmiech]", "MAREK: Halo"])]
        out, rep = aqnapi.sanitize_cues(cues, strip_sdh=True)
        self.assertEqual(out[0].lines, ["Halo"])
        self.assertEqual(rep.sdh_stripped, 1)

    def test_fps_scale(self):
        # 25 -> 23.976 : czasy się wydłużają (scale = 25/23.976 > 1)
        cues = [aqnapi.Cue(1000, 2000, ["x"])]
        scale = 25.0 / 23.976
        conv = aqnapi.apply_sync(cues, scale, 0.0)
        self.assertAlmostEqual(conv[0].start_ms, round(1000 * scale), delta=1)

    def test_score_release(self):
        h1 = aqnapi.SubtitleHit("os", "1", release="1080p.BluRay.x264-GROUP", downloads=5)
        h2 = aqnapi.SubtitleHit("os", "2", release="720p.WEB-DL", downloads=999)
        movie = "Film.2021.1080p.BluRay.x264-GROUP.mkv"
        self.assertGreater(aqnapi._score_release(h1, movie),
                           aqnapi._score_release(h2, movie))

    def test_safe_filename(self):
        self.assertEqual(aqnapi._safe_filename("a/b:c*?.srt"), "a_b_c_.srt")


class TestMergeSplitConfig(unittest.TestCase):
    def _srt(self, cues):
        text = "\n".join(
            f"{i}\n{aqnapi._ms_to_srt(s)} --> {aqnapi._ms_to_srt(e)}\n{t}\n"
            for i, (s, e, t) in enumerate(cues, 1)) + "\n"
        return make_file(text.encode("utf-8"))

    def test_merge_auto_offset(self):
        a = self._srt([(1000, 2000, "A1"), (3000, 4000, "A2")])   # koniec 4000
        b = self._srt([(500, 1500, "B1")])                         # od 0
        out = a + ".merged.srt"
        try:
            rc = aqnapi.main(["merge", a, b, "-o", out])
            self.assertEqual(rc, 0)
            data = open(out, "rb").read().decode("utf-8-sig")
            self.assertIn("A1", data)
            # B1 przesunięte o 4000ms -> start 4500ms
            self.assertIn("00:00:04,500 --> 00:00:05,500", data)
        finally:
            for p in (a, b, out):
                if os.path.exists(p):
                    os.remove(p)

    def test_merge_explicit_offset(self):
        a = self._srt([(0, 1000, "A")])
        b = self._srt([(0, 1000, "B")])
        out = a + ".merged.srt"
        try:
            aqnapi.main(["merge", a, b, "-o", out, "--offset", "60"])
            data = open(out, "rb").read().decode("utf-8-sig")
            self.assertIn("00:01:00,000 --> 00:01:01,000", data)  # B +60s
        finally:
            for p in (a, b, out):
                if os.path.exists(p):
                    os.remove(p)

    def test_split_rebase(self):
        src = self._srt([(1000, 2000, "P1"), (65000, 66000, "P2")])
        base = src + ".out"
        try:
            rc = aqnapi.main(["split", src, "--at", "00:01:00,000", "-o", base])
            self.assertEqual(rc, 0)
            p1 = base + ".part1.srt"
            p2 = base + ".part2.srt"
            self.assertTrue(os.path.exists(p1) and os.path.exists(p2))
            d2 = open(p2, "rb").read().decode("utf-8-sig")
            # P2 startował 65000ms; po podziale w 60000 i rebase -> 5000ms
            self.assertIn("00:00:05,000 --> 00:00:06,000", d2)
        finally:
            for p in (src, base + ".part1.srt", base + ".part2.srt"):
                if os.path.exists(p):
                    os.remove(p)

    def test_config_show_masks_password(self):
        fd, path = tempfile.mkstemp(suffix=".ini")
        with os.fdopen(fd, "w") as f:
            f.write("[napisy24]\nlogin = jan\npass = tajne123\n")
        import io
        import contextlib
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                aqnapi.main(["--config", path, "config", "show"])
            out = buf.getvalue()
            self.assertIn("login = jan", out)
            self.assertNotIn("tajne123", out)      # hasło zamaskowane
            self.assertIn("*" * len("tajne123"), out)
        finally:
            os.remove(path)


class TestMultipart(unittest.TestCase):
    def test_build_multipart(self):
        body, ct = aqnapi.build_multipart(
            [("field", "value"), ("file", ("a.srt", b"DATA", "text/plain"))])
        self.assertIn("multipart/form-data; boundary=", ct)
        self.assertIn(b'name="field"', body)
        self.assertIn(b'filename="a.srt"', body)
        self.assertIn(b"DATA", body)


class TestConfig(unittest.TestCase):
    def test_precedence_env_over_file(self):
        fd, path = tempfile.mkstemp(suffix=".ini")
        with os.fdopen(fd, "w") as f:
            f.write("[napisy24]\nlogin = fromfile\n")
        try:
            cfg = aqnapi.Config(path)
            self.assertEqual(cfg.n24_login(), "fromfile")
            os.environ["NAPI24_LOGIN"] = "fromenv"
            try:
                self.assertEqual(cfg.n24_login(), "fromenv")
                self.assertEqual(cfg.n24_login("flag"), "flag")  # flaga wygrywa
            finally:
                del os.environ["NAPI24_LOGIN"]
        finally:
            os.remove(path)


class TestJwt(unittest.TestCase):
    def test_jwt_expiry(self):
        import base64 as b64
        import json
        header = b64.urlsafe_b64encode(b'{"alg":"HS256"}').rstrip(b"=").decode()
        payload = b64.urlsafe_b64encode(json.dumps({"exp": 1893456000}).encode()).rstrip(b"=").decode()
        token = f"{header}.{payload}.sig"
        self.assertEqual(aqnapi._jwt_expiry(token), 1893456000)


class TestCli(unittest.TestCase):
    def test_hash_command(self):
        path = make_file(b"\x01" * aqnapi.OSH_CHUNK + b"\x02" * aqnapi.OSH_CHUNK)
        try:
            self.assertEqual(aqnapi.main(["hash", path]), 0)
        finally:
            os.remove(path)

    def test_convert_command(self):
        src = make_file("{0}{25}Test|Linia2".encode("utf-8"))
        out = src + ".srt"
        try:
            rc = aqnapi.main(["convert", src, "-o", out, "--fps", "25"])
            self.assertEqual(rc, 0)
            with open(out, "rb") as f:
                data = f.read()
            self.assertTrue(data.startswith(aqnapi.UTF8_BOM))
            self.assertIn(b"Test", data)
        finally:
            os.remove(src)
            if os.path.exists(out):
                os.remove(out)

    def test_parser_builds(self):
        # nie może rzucać (dublowane subparsery itp.)
        aqnapi.build_parser()

    def test_get_accepts_many_files_and_force(self):
        p = aqnapi.build_parser()
        args = p.parse_args(["get", "a.mkv", "b.mkv", "c.mkv", "--force"])
        self.assertEqual(args.file, ["a.mkv", "b.mkv", "c.mkv"])
        self.assertTrue(args.force)

    def test_skip_existing(self):
        fd, path = tempfile.mkstemp(suffix=".srt")
        os.close(fd)
        try:
            self.assertTrue(aqnapi._skip_existing(path, force=False))   # istnieje -> pomiń
            self.assertFalse(aqnapi._skip_existing(path, force=True))   # force -> nie pomijaj
            self.assertFalse(aqnapi._skip_existing(path + ".nope", False))
        finally:
            os.remove(path)

    def test_reject_output_with_many(self):
        self.assertTrue(aqnapi._reject_output_with_many(["a", "b"], "out.srt"))
        self.assertFalse(aqnapi._reject_output_with_many(["a"], "out.srt"))
        self.assertFalse(aqnapi._reject_output_with_many(["a", "b"], None))

    def test_filter_by_lang(self):
        hits = [
            aqnapi.SubtitleHit("napisy24", "1", language="pl"),
            aqnapi.SubtitleHit("opensubtitles", "2", language="en"),
            aqnapi.SubtitleHit("napiprojekt", "3", language=""),  # katalog — brak języka
        ]
        pl = aqnapi._filter_by_lang(hits, "pl")
        self.assertEqual({h.sub_id for h in pl}, {"1", "3"})  # pl + bez języka
        both = aqnapi._filter_by_lang(hits, "pl,en")
        self.assertEqual({h.sub_id for h in both}, {"1", "2", "3"})
        self.assertEqual(len(aqnapi._filter_by_lang(hits, "")), 3)  # brak filtra

    def test_search_default_lang_is_pl(self):
        args = aqnapi.build_parser().parse_args(["search", "--title", "X"])
        self.assertEqual(args.lang, "pl")
        args_os = aqnapi.build_parser().parse_args(["opensubtitles", "search"])
        self.assertEqual(args_os.lang, "pl")

    def test_fps_command(self):
        data = bytearray(b"RIFF" + b"\x00" * 60)
        struct.pack_into("<I", data, 32, 40_000)
        path = make_file(bytes(data))
        try:
            self.assertEqual(aqnapi.main(["fps", path]), 0)
        finally:
            os.remove(path)


if __name__ == "__main__":
    unittest.main()
