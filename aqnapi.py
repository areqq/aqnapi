#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""aqnapi — zunifikowany klient napisów (Napisy24 / napiprojekt / OpenSubtitles).

Jednoplikowe narzędzie CLI korzystające **wyłącznie z biblioteki standardowej**
Pythona 3.9+. Obsługuje pobieranie i wysyłanie napisów z trzech serwisów, ze
spójnym interfejsem: polecenia agregujące (działające po wielu serwisach naraz)
oraz pełne podpolecenia per-serwis.

Pobrane napisy są zawsze konwertowane do SRT w kodowaniu UTF-8 z BOM i końcami
wiersza LF.

Dokumentacja protokołów: katalog docs/ obok tego pliku.
"""
from __future__ import annotations

import argparse
import base64
import configparser
import gzip
import hashlib
import html
import io
import json
import logging
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import urllib.error
import zipfile
import zlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

__version__ = "1.0.2"
USER_AGENT_OS = f"aqnapi v{__version__}"

__all__ = [
    # wersja
    "__version__",
    # wyjątki
    "AqError", "AuthError", "NetworkError", "NotFoundError", "ServerError",
    "ConfigError",
    # hasze
    "oshash", "md5_10mb", "file_md5", "bytes_md5", "file_hashes",
    # fps
    "fps_from_file", "trusted_fps",
    # kryptografia
    "n24_obf", "n24_deobf", "np_encode_password",
    "AES", "aes_cbc_encrypt", "sevenzip_key", "write_7z_aes", "SEVENZIP_PASSWORD",
    # silnik napisów
    "Cue", "detect_encoding", "decode_text", "detect_format", "to_srt",
    "emit_srt", "convert_to_srt_bytes", "extract_from_zip",
    "cues_to_srt", "cues_to_vtt", "cues_to_ass", "cues_to_microdvd",
    "emit_subtitle", "parse_any", "sanitize_cues", "strip_sdh_line",
    "normalize_for_napisy24", "check_srt_for_napisy24",
    "compute_sync_transform", "apply_sync",
    # http / config
    "http_get", "http_post_json", "http_post_multipart", "build_multipart",
    "Config",
    # klienci i struktury
    "Napisy24Client", "NapiprojektClient", "OpenSubtitlesClient",
    "SubtitleHit", "UploadResult",
    # skróty wysokopoziomowe
    "download_subtitles", "search_subtitles", "convert_file",
    # cli
    "main", "build_parser",
]

log = logging.getLogger("aqnapi")

CHUNK_10MB = 10 * 1024 * 1024
OSH_CHUNK = 65536
DEFAULT_FPS = 23.976
DEFAULT_TIMEOUT = 30.0

# UTF-8 BOM prefix dodawany do wszystkich zapisywanych napisów.
UTF8_BOM = b"\xef\xbb\xbf"


# ===========================================================================
# Błędy
# ===========================================================================

class AqError(Exception):
    """Bazowy błąd narzędzia."""


class AuthError(AqError):
    """Błąd uwierzytelnienia / brak danych logowania."""


class NetworkError(AqError):
    """Błąd sieci / transportu HTTP."""


class NotFoundError(AqError):
    """Brak napisów / zasobu."""


class ServerError(AqError):
    """Serwer zwrócił błąd lub nieprawidłową odpowiedź."""


class ConfigError(AqError):
    """Błąd konfiguracji."""


# ===========================================================================
# Hasze plików
# ===========================================================================

def oshash(path: str) -> str:
    """Hash OpenSubtitles / MPC (OSH): filesize + suma pierwszych i ostatnich
    64 KiB czytanych jako little-endian uint64. Zwraca 16 znaków hex (małe)."""
    size = os.path.getsize(path)
    if size < 2 * OSH_CHUNK:
        raise AqError(f"Plik za mały na hash OSH (min. {2 * OSH_CHUNK} B): {path}")
    mask = 0xFFFFFFFFFFFFFFFF
    h = size & mask
    with open(path, "rb") as f:
        for _ in range(OSH_CHUNK // 8):
            h = (h + struct.unpack("<Q", f.read(8))[0]) & mask
        f.seek(size - OSH_CHUNK)
        for _ in range(OSH_CHUNK // 8):
            h = (h + struct.unpack("<Q", f.read(8))[0]) & mask
    return "%016x" % h


def md5_10mb(path: str) -> str:
    """Hash napiprojekt: MD5 pierwszych 10 MiB pliku (32 hex, małe)."""
    m = hashlib.md5()
    with open(path, "rb") as f:
        m.update(f.read(CHUNK_10MB))
    return m.hexdigest()


def file_md5(path: str) -> str:
    """MD5 całej zawartości pliku."""
    m = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            m.update(chunk)
    return m.hexdigest()


def bytes_md5(data: bytes) -> str:
    """MD5 bajtów w pamięci."""
    return hashlib.md5(data).hexdigest()


def file_hashes(path: str) -> Dict[str, object]:
    return {
        "osh": oshash(path),
        "md5_10mb": md5_10mb(path),
        "size": os.path.getsize(path),
        "name": os.path.basename(path),
    }


# ===========================================================================
# FPS z pliku filmowego (MKV / AVI / MP4-MOV) — czysty parsing binarny.
# Używane do konwersji MicroDVD -> SRT. Analogicznie do klienta DMnapi (MKV),
# rozszerzone o MP4/MOV (ISO BMFF).
# ===========================================================================

def _mkv_vint(f, first_mask: int = 0xF0) -> Tuple[int, int]:
    """Zdekoduj EBML element-ID i długość. Zwraca (class_id, length).
    class_id zachowuje bit-marker (postać kanoniczna), length ma go usunięty."""
    def read_one(mask_bits: int) -> int:
        suma = 0
        mask = 0x01
        while not (suma & mask):
            b = f.read(1)
            if not b:
                raise AqError("Strumień MKV uszkodzony (EOF)")
            suma = (suma << 8) + b[0]
            if mask == 0x01 and not (suma & mask_bits):
                raise AqError("Strumień MKV uszkodzony (VINT)")
            mask <<= 7
        return suma if mask_bits == 0xF0 else (suma ^ mask)

    class_id = read_one(0xF0)
    length = read_one(0xFF)
    return class_id, length


def _fps_mkv(f) -> Optional[float]:
    """FPS z MKV: znajdź DefaultDuration (ns/klatkę) ścieżki wideo."""
    track = 0
    f.seek(0)
    # Elementy-kontenery, w które "wchodzimy" (nie pomijamy zawartości):
    containers = {0x18538067, 0x1654AE6B, 0xAE}  # Segment, Tracks, TrackEntry
    for _ in range(1_000_000):  # zabezpieczenie przed pętlą
        try:
            class_id, length = _mkv_vint(f)
        except AqError:
            return None
        if class_id == 0x83:  # TrackType
            b = f.read(1)
            track = b[0] if b else 0
        elif class_id == 0x23E383 and track == 1:  # DefaultDuration (video)
            raw = f.read(4)
            if len(raw) < 4:
                return None
            ns = struct.unpack(">I", raw)[0]
            if ns <= 0:
                return None
            return 1_000_000_000 / float(ns)
        elif class_id not in containers and class_id != 0x83:
            f.seek(length, 1)
    return None


def _fps_avi(f) -> Optional[float]:
    """FPS z AVI: dwMicroSecPerFrame na offsecie 32 (little-endian uint32)."""
    f.seek(32)
    raw = f.read(4)
    if len(raw) < 4:
        return None
    micros = struct.unpack("<I", raw)[0]
    if micros <= 0:
        return None
    return 1_000_000.0 / float(micros)


def _iter_boxes(f, start: int, end: int):
    """Iteruj pudełka ISO BMFF w zakresie [start, end). Zwraca
    (typ, start, payload_offset, next_box_offset)."""
    pos = start
    while pos < end:
        f.seek(pos)
        hdr = f.read(8)
        if len(hdr) < 8:
            return
        size, btype = struct.unpack(">I4s", hdr)
        if size == 1:
            raw = f.read(8)
            if len(raw) < 8:
                return
            size = struct.unpack(">Q", raw)[0]
            payload = pos + 16
        elif size == 0:
            size = end - pos
            payload = pos + 8
        else:
            payload = pos + 8
        if size < 8:
            return
        yield btype.decode("latin1"), pos, payload, pos + size
        pos += size


_BMFF_CONTAINERS = {"moov", "trak", "mdia", "minf", "stbl"}


def _bmff_find(f, start: int, end: int, wanted: str):
    """Znajdź pierwsze pudełko `wanted`, wchodząc w kontenery."""
    for btype, s, p, e in _iter_boxes(f, start, end):
        if btype == wanted:
            return p, e
        if btype in _BMFF_CONTAINERS:
            r = _bmff_find(f, p, e, wanted)
            if r:
                return r
    return None


def _fps_mp4(f, flen: int) -> Optional[float]:
    """FPS z MP4/MOV: ścieżka wideo (hdlr=='vide'), mdhd.timescale + stts."""
    for btype, s, p, e in _iter_boxes(f, 0, flen):
        if btype != "moov":
            continue
        for tt, ts, tp, te in _iter_boxes(f, p, e):
            if tt != "trak":
                continue
            mdia = _bmff_find(f, tp, te, "mdia")
            if not mdia:
                continue
            mp, me = mdia
            hd = _bmff_find(f, mp, me, "hdlr")
            if not hd:
                continue
            f.seek(hd[0])       # początek payloadu = version+flags
            f.read(4)           # version+flags
            f.read(4)           # pre_defined
            if f.read(4) != b"vide":
                continue
            mh = _bmff_find(f, mp, me, "mdhd")
            if not mh:
                continue
            f.seek(mh[0])
            ver = f.read(1)[0]
            f.read(3)           # flags
            if ver == 1:
                f.read(16)      # ctime(8)+mtime(8)
                timescale = struct.unpack(">I", f.read(4))[0]
            else:
                f.read(8)       # ctime(4)+mtime(4)
                timescale = struct.unpack(">I", f.read(4))[0]
            st = _bmff_find(f, mp, me, "stts")
            if not st:
                continue
            f.seek(st[0])
            f.read(4)  # version+flags
            n = struct.unpack(">I", f.read(4))[0]
            tot_s = 0
            tot_d = 0
            for _ in range(n):
                raw = f.read(8)
                if len(raw) < 8:
                    break
                c, d = struct.unpack(">II", raw)
                tot_s += c
                tot_d += c * d
            if timescale and tot_d:
                return tot_s * timescale / float(tot_d)
    return None


def fps_from_file(path: str) -> Optional[float]:
    """Odczytaj FPS z pliku filmowego (MKV/AVI/MP4/MOV). None jeśli się nie da."""
    try:
        with open(path, "rb") as f:
            magic = f.read(8)
            f.seek(0)
            if magic[:4] == b"\x1a\x45\xdf\xa3":
                return _fps_mkv(f)
            if magic[:4] == b"RIFF":
                return _fps_avi(f)
            if magic[4:8] == b"ftyp":
                f.seek(0, os.SEEK_END)
                flen = f.tell()
                f.seek(0)
                return _fps_mp4(f, flen)
    except (OSError, struct.error, AqError):
        return None
    return None


def trusted_fps(value: Optional[float]) -> Optional[float]:
    """Zwróć fps tylko gdy mieści się w bramce zaufania 22 < fps < 32."""
    if value is not None and 22 < value < 32:
        return value
    return None


# ===========================================================================
# Kryptografia serwisów
# ===========================================================================

def _n24_mask(i: int) -> int:
    return (0x7F + (i + 1) * (i + 1)) & 0xFF


def n24_obf(data) -> str:
    """Zaciemnianie pól klienta Napisy24: XOR maską, reverse, UPPER hex."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    enc = bytes(b ^ _n24_mask(i) for i, b in enumerate(data))
    return enc[::-1].hex().upper()


def n24_deobf(hexstr: str) -> bytes:
    """Odwrotność n24_obf (diagnostyka/testy)."""
    enc = bytes.fromhex(hexstr)[::-1]
    return bytes(b ^ _n24_mask(i) for i, b in enumerate(enc))


def np_encode_password(password: str) -> str:
    """Kodowanie hasła napiprojekt wg PDF: XOR kluczem 3 -> base64."""
    xored = bytes(b ^ 3 for b in password.encode("utf-8"))
    return base64.b64encode(xored).decode("ascii")


# ===========================================================================
# AES-256 (czysty Python) + KDF 7z + zapis kontenera 7z
# ===========================================================================

_AES_RCON = (
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
    0x1B, 0x36, 0x6C, 0xD8, 0xAB, 0x4D,
)

# Standardowy S-box AES (FIPS-197, tablica 4.1).
_AES_SBOX = (
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
)


def _build_sbox():
    return _AES_SBOX


def _xtime(a: int) -> int:
    a <<= 1
    if a & 0x100:
        a ^= 0x11B
    return a & 0xFF


def _mul(a: int, b: int) -> int:
    res = 0
    for _ in range(8):
        if b & 1:
            res ^= a
        b >>= 1
        a = _xtime(a)
    return res & 0xFF


class AES:
    """Minimalny AES-256 (tylko szyfrowanie bloku) — na potrzeby CBC 7z."""

    def __init__(self, key: bytes):
        if len(key) != 32:
            raise ValueError("AES-256 wymaga klucza 32-bajtowego")
        self.sbox = _build_sbox()
        self.rounds = 14
        self._expand_key(key)

    def _expand_key(self, key: bytes):
        nk = 8
        nr = self.rounds
        words = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
        sbox = self.sbox
        for i in range(nk, 4 * (nr + 1)):
            temp = list(words[i - 1])
            if i % nk == 0:
                temp = temp[1:] + temp[:1]  # RotWord
                temp = [sbox[b] for b in temp]  # SubWord
                temp[0] ^= _AES_RCON[i // nk - 1]
            elif nk > 6 and i % nk == 4:
                temp = [sbox[b] for b in temp]
            words.append([words[i - nk][j] ^ temp[j] for j in range(4)])
        # round keys jako lista 16-bajtowych bloków
        self.round_keys = []
        for r in range(nr + 1):
            rk = []
            for c in range(4):
                rk.extend(words[r * 4 + c])
            self.round_keys.append(rk)

    def encrypt_block(self, block: bytes) -> bytes:
        sbox = self.sbox
        # stan jako 16 bajtów w kolejności kolumnowej (jak klucz)
        state = list(block)
        self._add_round_key(state, self.round_keys[0])
        for r in range(1, self.rounds):
            self._sub_bytes(state, sbox)
            self._shift_rows(state)
            self._mix_columns(state)
            self._add_round_key(state, self.round_keys[r])
        self._sub_bytes(state, sbox)
        self._shift_rows(state)
        self._add_round_key(state, self.round_keys[self.rounds])
        return bytes(state)

    @staticmethod
    def _add_round_key(state, rk):
        for i in range(16):
            state[i] ^= rk[i]

    @staticmethod
    def _sub_bytes(state, sbox):
        for i in range(16):
            state[i] = sbox[state[i]]

    @staticmethod
    def _shift_rows(state):
        # stan kolumnowy: bajt wiersza r, kolumny c jest na indeksie c*4 + r
        new = state[:]
        for r in range(4):
            for c in range(4):
                new[c * 4 + r] = state[((c + r) % 4) * 4 + r]
        state[:] = new

    @staticmethod
    def _mix_columns(state):
        for c in range(4):
            i = c * 4
            a0, a1, a2, a3 = state[i], state[i + 1], state[i + 2], state[i + 3]
            state[i] = _mul(a0, 2) ^ _mul(a1, 3) ^ a2 ^ a3
            state[i + 1] = a0 ^ _mul(a1, 2) ^ _mul(a2, 3) ^ a3
            state[i + 2] = a0 ^ a1 ^ _mul(a2, 2) ^ _mul(a3, 3)
            state[i + 3] = _mul(a0, 3) ^ a1 ^ a2 ^ _mul(a3, 2)


def aes_cbc_encrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    """Szyfrowanie CBC z dopełnieniem zerami do wielokrotności 16 bajtów."""
    aes = AES(key)
    if len(data) % 16:
        data = data + b"\x00" * (16 - len(data) % 16)
    out = bytearray()
    prev = iv
    for off in range(0, len(data), 16):
        block = bytes(a ^ b for a, b in zip(data[off:off + 16], prev))
        enc = aes.encrypt_block(block)
        out += enc
        prev = enc
    return bytes(out)


SEVENZIP_PASSWORD = "iBlm8NTigvru0Jr0"


def sevenzip_key(password: str, salt: bytes = b"", cycles_power: int = 19) -> bytes:
    """Derywacja klucza AES-256 wg 7-Zip (7zAes): jeden kontekst SHA-256
    zasilany 2^cycles_power razy (salt + hasło_UTF16LE + licznik_u64LE)."""
    pw = password.encode("utf-16-le")
    if cycles_power == 0x3F:  # tryb specjalny: bez iteracji
        raw = (salt + pw)[:32]
        return raw + b"\x00" * (32 - len(raw))
    sha = hashlib.sha256()
    counter = 0
    for _ in range(1 << cycles_power):
        sha.update(salt)
        sha.update(pw)
        sha.update(struct.pack("<Q", counter))
        counter += 1
    return sha.digest()


def _7z_number(value: int) -> bytes:
    """Zakoduj liczbę w formacie nagłówka 7z (WriteNumber)."""
    first = 0
    mask = 0x80
    i = 8
    for k in range(8):
        if value < (1 << (7 * (k + 1))):
            first |= (value >> (8 * k)) & 0xFF
            i = k
            break
        first |= mask
        mask >>= 1
    out = bytearray([first & 0xFF])
    v = value
    for _ in range(i):
        out.append(v & 0xFF)
        v >>= 8
    return bytes(out)


# Identyfikatory właściwości nagłówka 7z
_K_HEADER = 0x01
_K_MAIN_STREAMS = 0x04
_K_FILES_INFO = 0x05
_K_PACK_INFO = 0x06
_K_UNPACK_INFO = 0x07
_K_SUBSTREAMS = 0x08
_K_SIZE = 0x09
_K_CRC = 0x0A
_K_FOLDER = 0x0B
_K_CODERS_UNPACK_SIZE = 0x0C
_K_NAME = 0x11
_K_END = 0x00
_AES_CODEC_ID = b"\x06\xf1\x07\x01"  # 7zAES256SHA256


def write_7z_aes(entry_name: str, data: bytes,
                 password: str = SEVENZIP_PASSWORD) -> bytes:
    """Zbuduj archiwum 7z (store + AES-256-CBC) zawierające jeden wpis
    `entry_name` z zawartością `data`. Zwraca bajty archiwum."""
    iv = os.urandom(16)
    key = sevenzip_key(password)
    encrypted = aes_cbc_encrypt(key, iv, data)
    unpacked_size = len(data)
    packed_size = len(encrypted)
    data_crc = zlib.crc32(data) & 0xFFFFFFFF

    props = bytes([0x53, 0x0F]) + iv  # numCyclesPower=19, saltSize=0, ivSize=16

    # --- StreamsInfo ---
    pack_info = bytearray([_K_PACK_INFO])
    pack_info += _7z_number(0)          # PackPos
    pack_info += _7z_number(1)          # NumPackStreams
    pack_info += bytes([_K_SIZE])
    pack_info += _7z_number(packed_size)
    pack_info += bytes([_K_END])

    folder = bytearray()
    folder += _7z_number(1)             # NumCoders
    folder += bytes([0x24])             # flaga: idSize=4 (0x04) + attrs (0x20)
    folder += _AES_CODEC_ID
    folder += _7z_number(len(props))
    folder += props

    unpack_info = bytearray([_K_UNPACK_INFO])
    unpack_info += bytes([_K_FOLDER])
    unpack_info += _7z_number(1)        # NumFolders
    unpack_info += bytes([0x00])        # External = 0
    unpack_info += folder
    unpack_info += bytes([_K_CODERS_UNPACK_SIZE])
    unpack_info += _7z_number(unpacked_size)
    unpack_info += bytes([_K_CRC])
    unpack_info += bytes([0x01])        # AllAreDefined
    unpack_info += struct.pack("<I", data_crc)
    unpack_info += bytes([_K_END])

    streams_info = bytearray([_K_MAIN_STREAMS])
    streams_info += pack_info
    streams_info += unpack_info
    streams_info += bytes([_K_END])

    # --- FilesInfo (jedna nazwa) ---
    name_utf16 = entry_name.encode("utf-16-le") + b"\x00\x00"
    name_prop = bytearray([_K_NAME])
    name_prop += _7z_number(len(name_utf16) + 1)  # +1 na bajt External
    name_prop += bytes([0x00])         # External = 0
    name_prop += name_utf16

    files_info = bytearray([_K_FILES_INFO])
    files_info += _7z_number(1)        # NumFiles
    files_info += name_prop
    files_info += bytes([_K_END])

    header = bytearray([_K_HEADER])
    header += streams_info
    header += files_info
    header += bytes([_K_END])
    header = bytes(header)

    header_crc = zlib.crc32(header) & 0xFFFFFFFF
    next_header_offset = packed_size
    next_header_size = len(header)

    start_header = struct.pack("<QQI", next_header_offset, next_header_size, header_crc)
    start_header_crc = zlib.crc32(start_header) & 0xFFFFFFFF

    signature = b"\x37\x7a\xbc\xaf\x27\x1c" + b"\x00\x04"
    signature += struct.pack("<I", start_header_crc)
    signature += start_header

    return bytes(signature) + encrypted + header


# ===========================================================================
# Silnik napisów: detekcja, parsowanie, konwersja do SRT (UTF-8+BOM, LF)
# ===========================================================================

@dataclass
class Cue:
    start_ms: int
    end_ms: int
    lines: List[str]


def detect_encoding(data: bytes) -> str:
    if data.startswith(b"\xef\xbb\xbf"):
        return "utf-8-sig"
    for enc in ("utf-8", "cp1250", "iso-8859-2"):
        try:
            data.decode(enc)
            return enc
        except UnicodeDecodeError:
            continue
    return "utf-8"  # ostatecznie z errors=replace przy dekodowaniu


def decode_text(data: bytes) -> str:
    enc = detect_encoding(data)
    return data.decode(enc, errors="replace")


_RE_SRT_TIME = re.compile(
    r"(\d+):(\d{2}):(\d{2})[,.](\d{1,3})\s*-->\s*(\d+):(\d{2}):(\d{2})[,.](\d{1,3})")
_RE_MICRODVD = re.compile(r"^\{(\d+)\}\{(\d+)\}(.*)$")
_RE_MPL2 = re.compile(r"^\[(\d+)\]\[(\d+)\](.*)$")
_RE_TMPLAYER = re.compile(r"^(\d{1,2}):(\d{2}):(\d{2})[:=](.*)$")


def detect_format(text: str) -> str:
    head = text.lstrip("﻿").lstrip()
    if head[:6].upper() == "WEBVTT":
        return "vtt"
    low = text.lower()
    if "[script info]" in low or "[v4+ styles]" in low or "[v4 styles]" in low or \
            ("dialogue:" in low and "[events]" in low):
        return "ass"
    if "-->" in text:
        return "srt"
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if _RE_MICRODVD.match(line):
            return "microdvd"
        if _RE_MPL2.match(line):
            return "mpl2"
        if _RE_TMPLAYER.match(line):
            return "tmplayer"
        break
    return "srt"


def _ms_to_srt(ms: int) -> str:
    if ms < 0:
        ms = 0
    h = ms // 3_600_000
    ms -= h * 3_600_000
    m = ms // 60_000
    ms -= m * 60_000
    s = ms // 1000
    ms -= s * 1000
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def parse_srt(text: str) -> List[Cue]:
    cues: List[Cue] = []
    blocks = re.split(r"\r?\n\r?\n", text.strip())
    for block in blocks:
        lines = [l for l in block.splitlines()]
        if not lines:
            continue
        idx = 0
        if lines[0].strip().isdigit():
            idx = 1
        if idx >= len(lines):
            continue
        m = _RE_SRT_TIME.search(lines[idx])
        if not m:
            continue
        g = list(map(int, m.groups()))
        start = ((g[0] * 3600 + g[1] * 60 + g[2]) * 1000) + g[3]
        end = ((g[4] * 3600 + g[5] * 60 + g[6]) * 1000) + g[7]
        cues.append(Cue(start, end, [l for l in lines[idx + 1:] if l.strip() != ""]))
    return cues


def parse_microdvd(text: str, fps: float) -> List[Cue]:
    cues: List[Cue] = []
    for line in text.splitlines():
        m = _RE_MICRODVD.match(line.strip())
        if not m:
            continue
        start_f, end_f, body = int(m.group(1)), int(m.group(2)), m.group(3)
        start = int(start_f * 1000 / fps)
        end = int(end_f * 1000 / fps)
        lines = body.split("|")
        # usuń znaczniki formatowania {y:i} itp.
        lines = [re.sub(r"\{[^}]*\}", "", l) for l in lines]
        cues.append(Cue(start, end, lines))
    return cues


def parse_mpl2(text: str) -> List[Cue]:
    cues: List[Cue] = []
    for line in text.splitlines():
        m = _RE_MPL2.match(line.strip())
        if not m:
            continue
        start = int(m.group(1)) * 100  # dziesiąte sekundy
        end = int(m.group(2)) * 100
        lines = m.group(3).split("|")
        lines = [l[1:] if l.startswith("/") else l for l in lines]  # /kursywa
        cues.append(Cue(start, end, lines))
    return cues


def parse_tmplayer(text: str) -> List[Cue]:
    cues: List[Cue] = []
    raw = []
    for line in text.splitlines():
        m = _RE_TMPLAYER.match(line.strip())
        if not m:
            continue
        h, mm, s = int(m.group(1)), int(m.group(2)), int(m.group(3))
        start = (h * 3600 + mm * 60 + s) * 1000
        raw.append((start, m.group(4).split("|")))
    for i, (start, lines) in enumerate(raw):
        end = raw[i + 1][0] if i + 1 < len(raw) else start + 3000
        cues.append(Cue(start, end, lines))
    return cues


_RE_VTT_TIME = re.compile(
    r"((?:\d+:)?\d{1,2}:\d{2}[.,]\d{1,3})\s*-->\s*"
    r"((?:\d+:)?\d{1,2}:\d{2}[.,]\d{1,3})")


def _vtt_ts_to_ms(ts: str) -> int:
    ts = ts.strip().replace(",", ".")
    parts = ts.split(":")
    if len(parts) == 3:
        h, m, rest = parts
    else:  # MM:SS.mmm (bez godzin)
        h, (m, rest) = "0", parts
    s, _, ms = rest.partition(".")
    return (int(h) * 3600 + int(m) * 60 + int(s)) * 1000 + int((ms + "000")[:3])


def _vtt_clean(line: str) -> str:
    """Usuń tagi VTT (<c>, <v ..>, <00:..>) i zdekoduj encje HTML."""
    return html.unescape(re.sub(r"<[^>]+>", "", line))


def parse_vtt(text: str) -> List[Cue]:
    text = text.replace("\r\n", "\n").replace("\r", "\n").lstrip("﻿")
    cues: List[Cue] = []
    for block in re.split(r"\n[ \t]*\n", text.strip()):
        lines = block.split("\n")
        head = lines[0].strip().upper()
        if head.startswith(("WEBVTT", "NOTE", "STYLE", "REGION")):
            continue
        ts_idx = next((i for i, l in enumerate(lines) if "-->" in l), None)
        if ts_idx is None:
            continue
        m = _RE_VTT_TIME.search(lines[ts_idx])
        if not m:
            continue
        start = _vtt_ts_to_ms(m.group(1))
        end = _vtt_ts_to_ms(m.group(2))
        body = [_vtt_clean(l) for l in lines[ts_idx + 1:]]
        cues.append(Cue(start, end, body))
    return cues


_RE_ASS_TIME = re.compile(r"(\d+):(\d{2}):(\d{2})[.:](\d{1,3})")


def _ass_ts_to_ms(ts: str) -> int:
    m = _RE_ASS_TIME.match(ts.strip())
    if not m:
        return 0
    h, mm, ss, cs = m.groups()
    return (int(h) * 3600 + int(mm) * 60 + int(ss)) * 1000 + int((cs + "00")[:2]) * 10


def _ass_clean(text_field: str) -> List[str]:
    """Wyczyść pole Text z ASS: usuń override {\\...}, zamień \\N/\\n na nowe
    linie, \\h na spację."""
    t = re.sub(r"\{[^}]*\}", "", text_field)
    t = t.replace("\\N", "\n").replace("\\n", "\n").replace("\\h", " ")
    return t.split("\n")


def parse_ass(text: str) -> List[Cue]:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    cues: List[Cue] = []
    in_events = False
    idx_start, idx_end, idx_text = 1, 2, 9  # domyślny układ V4+
    have_format = False
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("["):
            in_events = s.lower() == "[events]"
            continue
        if not in_events or not s:
            continue
        low = s.lower()
        if low.startswith("format:"):
            fields = [f.strip().lower() for f in s.split(":", 1)[1].split(",")]
            idx_start = fields.index("start") if "start" in fields else 1
            idx_end = fields.index("end") if "end" in fields else 2
            idx_text = fields.index("text") if "text" in fields else len(fields) - 1
            have_format = True
            continue
        if low.startswith("dialogue:"):
            payload = s.split(":", 1)[1]
            parts = payload.split(",", idx_text)  # Text pozostaje w całości
            if len(parts) <= idx_text:
                continue
            start = _ass_ts_to_ms(parts[idx_start])
            end = _ass_ts_to_ms(parts[idx_end])
            cues.append(Cue(start, end, _ass_clean(parts[idx_text])))
    return cues


def cues_to_srt(cues: List[Cue]) -> str:
    out = []
    for i, cue in enumerate(cues, 1):
        text_lines = [l for l in cue.lines if l is not None]
        if not text_lines:
            text_lines = [""]
        out.append(str(i))
        out.append(f"{_ms_to_srt(cue.start_ms)} --> {_ms_to_srt(cue.end_ms)}")
        out.extend(text_lines)
        out.append("")
    return "\n".join(out).strip() + "\n"


def _ms_to_vtt(ms: int) -> str:
    return _ms_to_srt(ms).replace(",", ".")


def _ms_to_ass(ms: int) -> str:
    if ms < 0:
        ms = 0
    h = ms // 3_600_000
    ms -= h * 3_600_000
    m = ms // 60_000
    ms -= m * 60_000
    s = ms // 1000
    cs = (ms - s * 1000) // 10
    return f"{h:d}:{m:02d}:{s:02d}.{cs:02d}"


def cues_to_vtt(cues: List[Cue]) -> str:
    out = ["WEBVTT", ""]
    for cue in cues:
        out.append(f"{_ms_to_vtt(cue.start_ms)} --> {_ms_to_vtt(cue.end_ms)}")
        out.extend(cue.lines or [""])
        out.append("")
    return "\n".join(out).strip() + "\n"


def cues_to_microdvd(cues: List[Cue], fps: float = DEFAULT_FPS) -> str:
    out = []
    for cue in cues:
        sf = int(round(cue.start_ms * fps / 1000))
        ef = int(round(cue.end_ms * fps / 1000))
        out.append("{%d}{%d}%s" % (sf, ef, "|".join(cue.lines or [""])))
    return "\n".join(out) + "\n"


_ASS_HEADER = (
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "Collisions: Normal\n"
    "PlayResX: 1920\n"
    "PlayResY: 1080\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
    "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
    "MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
    "0,0,0,0,100,100,0,0,1,2,1,2,10,10,20,1\n\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
    "Effect, Text\n"
)


def cues_to_ass(cues: List[Cue]) -> str:
    out = [_ASS_HEADER]
    for cue in cues:
        text = "\\N".join(cue.lines or [""])
        out.append(f"Dialogue: 0,{_ms_to_ass(cue.start_ms)},{_ms_to_ass(cue.end_ms)},"
                   f"Default,,0,0,0,,{text}")
    return "\n".join(out) + "\n"


#: Emittery per format wyjściowy (do konwersji i eksportu).
def emit_subtitle(cues: List[Cue], fmt: str = "srt", fps: float = DEFAULT_FPS) -> bytes:
    """Zbuduj bajty pliku napisów w docelowym formacie. SRT jest w UTF-8 z BOM
    i końcami LF; pozostałe formaty w UTF-8 (LF) bez BOM."""
    fmt = fmt.lower()
    if fmt == "srt":
        return emit_srt(cues_to_srt(cues))
    if fmt == "vtt":
        text = cues_to_vtt(cues)
    elif fmt == "ass":
        text = cues_to_ass(cues)
    elif fmt in ("microdvd", "sub", "txt"):
        text = cues_to_microdvd(cues, fps)
    else:
        raise AqError(f"Nieobsługiwany format wyjściowy: {fmt}")
    return text.replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


#: Rozpoznanie formatu wyjściowego po rozszerzeniu pliku.
_EXT_FORMAT = {".srt": "srt", ".vtt": "vtt", ".ass": "ass", ".ssa": "ass",
               ".sub": "microdvd", ".txt": "microdvd"}


# --- czyszczenie SDH / dla niesłyszących (opcjonalne) ---

_RE_SDH_BRACKET = re.compile(r"\[[^\]]*\]|\([^)]*\)")
_RE_SDH_SPEAKER = re.compile(
    r"^\s*-?\s*[A-ZĄĆĘŁŃÓŚŹŻ][A-ZĄĆĘŁŃÓŚŹŻ0-9 .'\-]{1,20}:\s*")
_RE_SDH_MUSIC = re.compile(r"[♪♫#]")


def strip_sdh_line(line: str) -> str:
    """Usuń oznaczenia SDH: [odgłosy], (opisy), etykiety MÓWCA:, nuty ♪."""
    line = _RE_SDH_BRACKET.sub("", line)
    line = _RE_SDH_SPEAKER.sub("", line)
    line = _RE_SDH_MUSIC.sub("", line)
    return line.strip()


def parse_any(text: str, fmt: Optional[str] = None,
              fps: float = DEFAULT_FPS) -> List[Cue]:
    """Sparsuj dowolny obsługiwany format do listy Cue."""
    if fmt is None:
        fmt = detect_format(text)
    if fmt == "microdvd":
        return parse_microdvd(text, fps)
    if fmt == "mpl2":
        return parse_mpl2(text)
    if fmt == "tmplayer":
        return parse_tmplayer(text)
    if fmt == "vtt":
        return parse_vtt(text)
    if fmt == "ass":
        return parse_ass(text)
    return parse_srt(text)


def to_srt(text: str, fmt: Optional[str] = None, fps: float = DEFAULT_FPS) -> str:
    """Skonwertuj dowolny obsługiwany format do tekstu SRT (bez sanityzacji)."""
    return cues_to_srt(parse_any(text, fmt, fps))


# --- sanityzacja: bezpieczne korekty przed zapisem ---

_RE_HTML_TAG = re.compile(r"</?[a-zA-Z][^>]*>")   # <i> </i> <font ...> <b> ...
_RE_BRACE_TAG = re.compile(r"\{[^}]*\}")           # {\an8} {y:i} (ASS/MicroDVD)


def strip_format_tags(line: str) -> str:
    """Usuń tagi formatujące (HTML oraz nawiasy klamrowe ASS/MicroDVD)."""
    return _RE_BRACE_TAG.sub("", _RE_HTML_TAG.sub("", line))


@dataclass
class SanitizeReport:
    tags_stripped: int = 0       # bloki, w których usunięto tagi
    sdh_stripped: int = 0        # bloki, w których usunięto SDH/HI
    long_clamped: int = 0        # skrócone (ekstremalnie długie)
    overlaps_fixed: int = 0      # naprawione nakładki
    nonpositive_fixed: int = 0   # naprawiony koniec <= start
    short_extended: int = 0      # wydłużone (min-display)
    empty_removed: int = 0       # usunięte puste bloki
    total_cues: int = 0

    def any_changes(self) -> bool:
        return any((self.tags_stripped, self.sdh_stripped, self.long_clamped,
                    self.overlaps_fixed, self.nonpositive_fixed,
                    self.short_extended, self.empty_removed))

    def summary(self) -> str:
        parts = []
        if self.tags_stripped:
            parts.append(f"usunięto tagi w {self.tags_stripped}")
        if self.sdh_stripped:
            parts.append(f"usunięto SDH w {self.sdh_stripped}")
        if self.long_clamped:
            parts.append(f"skrócono {self.long_clamped} zbyt długich")
        if self.overlaps_fixed:
            parts.append(f"naprawiono {self.overlaps_fixed} nakładek")
        if self.nonpositive_fixed:
            parts.append(f"naprawiono {self.nonpositive_fixed} złych czasów")
        if self.short_extended:
            parts.append(f"wydłużono {self.short_extended} zbyt krótkich")
        if self.empty_removed:
            parts.append(f"usunięto {self.empty_removed} pustych")
        return ", ".join(parts)


def sanitize_cues(cues: List[Cue], *, keep_tags: bool = False,
                  max_display_ms: int = 10000, min_display_ms: int = 0,
                  strip_sdh: bool = False) -> Tuple[List[Cue], SanitizeReport]:
    """Zastosuj bezpieczne korekty do listy Cue. Zwraca (nowe_cue, raport).

    Nie zmienia treści napisów poza usunięciem tagów formatujących, opcjonalnie
    SDH/HI, i pustych linii/bloków. Naprawia wyłącznie jawnie uszkodzone czasy."""
    rep = SanitizeReport()
    out: List[Cue] = []
    for cue in cues:
        lines = cue.lines
        if not keep_tags:
            stripped = [strip_format_tags(l) for l in lines]
            if stripped != lines:
                rep.tags_stripped += 1
            lines = stripped
        if strip_sdh:
            sdh = [strip_sdh_line(l) for l in lines]
            if sdh != lines:
                rep.sdh_stripped += 1
            lines = sdh
        lines = [l.strip() for l in lines]
        lines = [l for l in lines if l != ""]
        if not lines:
            rep.empty_removed += 1
            continue
        start, end = cue.start_ms, cue.end_ms
        if end <= start:
            end = start + (min_display_ms or 1000)
            rep.nonpositive_fixed += 1
        out.append(Cue(start, end, lines))

    for i, cue in enumerate(out):
        nxt = out[i + 1].start_ms if i + 1 < len(out) else None
        if nxt is not None and cue.end_ms > nxt:
            cue.end_ms = max(cue.start_ms + 1, nxt)
            rep.overlaps_fixed += 1
        if cue.end_ms - cue.start_ms > max_display_ms:
            cue.end_ms = cue.start_ms + max_display_ms
            rep.long_clamped += 1
        if min_display_ms and (cue.end_ms - cue.start_ms) < min_display_ms:
            target = cue.start_ms + min_display_ms
            if nxt is not None:
                target = min(target, nxt)
            if target > cue.end_ms:
                cue.end_ms = target
                rep.short_extended += 1

    rep.total_cues = len(out)
    return out, rep


def emit_srt(text: str) -> bytes:
    """Zwróć bajty SRT w UTF-8 z BOM i końcami wiersza LF."""
    # normalizuj końce wiersza do LF
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    body = text.encode("utf-8")
    if body.startswith(UTF8_BOM):
        return body
    return UTF8_BOM + body


def convert_to_srt_bytes(raw: bytes, fps: float = DEFAULT_FPS, *,
                         sanitize: bool = True, keep_tags: bool = False,
                         max_display_ms: int = 10000, min_display_ms: int = 0,
                         strip_sdh: bool = False,
                         report: Optional[SanitizeReport] = None) -> bytes:
    """Pełny pipeline: bajty (ew. ZIP) -> SRT UTF-8+BOM/LF.

    Domyślnie stosuje bezpieczne korekty (:func:`sanitize_cues`) i odrzuca
    materiał uszkodzony: jeśli z niepustego wejścia nie da się odczytać żadnej
    linii napisów, rzuca :class:`AqError` (żeby nie zapisać pustego/zepsutego
    pliku)."""
    if raw[:2] == b"PK":
        raw = extract_from_zip(raw)
    text = decode_text(raw)
    cues = parse_any(text, fps=fps)
    if not cues and text.strip():
        raise AqError("Napisy wyglądają na uszkodzone lub w nierozpoznanym "
                      "formacie (0 rozpoznanych linii) — nie zapisuję.")
    if sanitize:
        cues, rep = sanitize_cues(cues, keep_tags=keep_tags,
                                  max_display_ms=max_display_ms,
                                  min_display_ms=min_display_ms,
                                  strip_sdh=strip_sdh)
        if report is not None:
            for f in rep.__dataclass_fields__:
                setattr(report, f, getattr(rep, f))
    return emit_srt(cues_to_srt(cues))


def extract_from_zip(data: bytes) -> bytes:
    """Wyciągnij największy plik napisowy (.srt/.txt/.sub) z archiwum ZIP."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        candidates = [
            n for n in z.namelist()
            if n.lower().endswith((".srt", ".txt", ".sub", ".ass", ".ssa"))
            and not n.lower().endswith(".url")
        ]
        if not candidates:
            candidates = [n for n in z.namelist() if not n.lower().endswith(".url")]
        if not candidates:
            raise NotFoundError("Archiwum ZIP nie zawiera pliku napisów")
        best = max(candidates, key=lambda n: z.getinfo(n).file_size)
        return z.read(best)


# --- normalizacja pod upload do Napisy24 (wymaga CRLF, <=2 linie/blok) ---

def normalize_for_napisy24(text: str, fix_timing: bool = False) -> bytes:
    """Zapewnij SRT z końcami CRLF (wymóg walidatora Napisy24)."""
    srt = to_srt(text)
    if fix_timing:
        srt = _clamp_overlaps(srt)
    srt = srt.replace("\r\n", "\n").replace("\r", "\n")
    return srt.replace("\n", "\r\n").encode("utf-8")


def check_srt_for_napisy24(text: str) -> List[str]:
    """Zwróć listę problemów, przez które Napisy24 odrzuci plik."""
    problems = []
    cues = parse_srt(to_srt(text))
    for i, cue in enumerate(cues, 1):
        real = [l for l in cue.lines if l.strip() != ""]
        if len(real) > 2:
            problems.append(f"Blok {i}: {len(real)} linii tekstu (max 2)")
    for i in range(len(cues) - 1):
        if cues[i].end_ms > cues[i + 1].start_ms:
            problems.append(f"Bloki {i + 1}/{i + 2}: nachodzące czasy")
    return problems


def _clamp_overlaps(srt_text: str) -> str:
    cues = parse_srt(srt_text)
    for i in range(len(cues) - 1):
        if cues[i].end_ms > cues[i + 1].start_ms:
            cues[i].end_ms = max(cues[i].start_ms, cues[i + 1].start_ms - 1)
    return cues_to_srt(cues)


# --- synchronizacja: dopasowanie liniowe czasu wg par kotwic ---

def compute_sync_transform(pairs: List[Tuple[int, int]]) -> Tuple[float, float]:
    """Wylicz (scale, offset) tak, że ref_ms ≈ scale * target_ms + offset.

    `pairs` to lista (target_ms, ref_ms). 0 par → tożsamość; 1 para → samo
    przesunięcie; 2+ → regresja liniowa (skala + offset, obsługuje dryf/FPS)."""
    if not pairs:
        return 1.0, 0.0
    if len(pairs) == 1:
        t, r = pairs[0]
        return 1.0, float(r - t)
    n = len(pairs)
    sx = sum(t for t, _ in pairs)
    sy = sum(r for _, r in pairs)
    sxx = sum(t * t for t, _ in pairs)
    sxy = sum(t * r for t, r in pairs)
    denom = n * sxx - sx * sx
    if denom == 0:  # wszystkie kotwice o tym samym czasie źródłowym
        return 1.0, (sy - sx) / n
    scale = (n * sxy - sx * sy) / denom
    offset = (sy - scale * sx) / n
    return scale, offset


def apply_sync(cues: List[Cue], scale: float, offset: float) -> List[Cue]:
    """Przelicz czasy wszystkich Cue wg transformacji liniowej."""
    out = []
    for c in cues:
        ns = int(round(c.start_ms * scale + offset))
        ne = int(round(c.end_ms * scale + offset))
        out.append(Cue(max(0, ns), max(0, ne), list(c.lines)))
    return out


# ===========================================================================
# Warstwa HTTP
# ===========================================================================

@dataclass
class HttpResponse:
    status: int
    body: bytes
    headers: Dict[str, str]

    def text(self, encoding: str = "utf-8") -> str:
        return self.body.decode(encoding, errors="replace")

    def json(self):
        return json.loads(self.body.decode("utf-8"))


def _decode_body(resp) -> bytes:
    body = resp.read()
    if resp.headers.get("Content-Encoding", "").lower() == "gzip":
        body = gzip.decompress(body)
    return body


def http_request(method: str, url: str, *, headers: Optional[dict] = None,
                 data: Optional[bytes] = None,
                 timeout: float = DEFAULT_TIMEOUT) -> HttpResponse:
    headers = dict(headers or {})
    headers.setdefault("User-Agent", "Mozilla/5.0 (aqnapi)")
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    _log_request(method, url, headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return HttpResponse(resp.status, _decode_body(resp),
                                {k: v for k, v in resp.headers.items()})
    except urllib.error.HTTPError as e:
        body = e.read() if hasattr(e, "read") else b""
        try:
            if e.headers and e.headers.get("Content-Encoding", "").lower() == "gzip":
                body = gzip.decompress(body)
        except Exception:  # noqa: BLE001
            pass
        return HttpResponse(e.code, body,
                            {k: v for k, v in (e.headers or {}).items()})
    except urllib.error.URLError as e:
        raise NetworkError(f"Błąd połączenia: {e.reason}") from e
    except socket.timeout as e:
        raise NetworkError("Przekroczono limit czasu połączenia") from e


def http_get(url: str, headers: Optional[dict] = None,
             timeout: float = DEFAULT_TIMEOUT) -> HttpResponse:
    return http_request("GET", url, headers=headers, timeout=timeout)


def http_post_json(url: str, payload: dict, headers: Optional[dict] = None,
                   timeout: float = DEFAULT_TIMEOUT) -> HttpResponse:
    h = dict(headers or {})
    h["Content-Type"] = "application/json"
    h.setdefault("Accept", "application/json")
    return http_request("POST", url, headers=h,
                        data=json.dumps(payload).encode("utf-8"), timeout=timeout)


def build_multipart(fields: List[Tuple], boundary: Optional[str] = None) -> Tuple[bytes, str]:
    """Zbuduj ciało multipart/form-data.

    Każde pole to krotka:
      ("name", "wartość")                              — pole tekstowe
      ("name", ("filename", bajty, "content/type"))    — plik
    Zwraca (body_bytes, content_type)."""
    if boundary is None:
        boundary = "----aqnapi" + os.urandom(8).hex()
    body = bytearray()
    for name, value in fields:
        body += ("--" + boundary + "\r\n").encode()
        if isinstance(value, tuple):
            filename, filebytes, ctype = value
            body += (
                'Content-Disposition: form-data; name="%s"; filename="%s"\r\n'
                % (name, filename)
            ).encode()
            body += ("Content-Type: %s\r\n\r\n" % ctype).encode()
            body += filebytes if isinstance(filebytes, bytes) else str(filebytes).encode()
            body += b"\r\n"
        else:
            body += ('Content-Disposition: form-data; name="%s"\r\n\r\n' % name).encode()
            body += value if isinstance(value, bytes) else str(value).encode("utf-8")
            body += b"\r\n"
    body += ("--" + boundary + "--\r\n").encode()
    return bytes(body), "multipart/form-data; boundary=" + boundary


def http_post_multipart(url: str, fields: List[Tuple],
                        headers: Optional[dict] = None,
                        timeout: float = DEFAULT_TIMEOUT) -> HttpResponse:
    body, ctype = build_multipart(fields)
    h = dict(headers or {})
    h["Content-Type"] = ctype
    return http_request("POST", url, headers=h, data=body, timeout=timeout)


def raw_http10_post(host: str, path: str, content_type: str, body: bytes,
                    user_agent: str = "Mozilla/4.0",
                    timeout: float = DEFAULT_TIMEOUT) -> bytes:
    """Surowy POST HTTP/1.0 przez socket — dla Napisy24 (klient wysyła HTTP/1.0
    i oczekuje odpowiedzi binarnej po nagłówku)."""
    head = (
        "POST %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n"
        % (path, host, user_agent, content_type, len(body))
    ).encode()
    sock = socket.create_connection((host, 80), timeout=timeout)
    try:
        sock.sendall(head + body)
        resp = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            resp += chunk
    finally:
        sock.close()
    sep = resp.find(b"\r\n\r\n")
    return bytes(resp[sep + 4:]) if sep >= 0 else bytes(resp)


def _log_request(method: str, url: str, headers: dict):
    if not log.isEnabledFor(logging.DEBUG):
        return
    safe = dict(headers)
    for k in list(safe):
        if k.lower() in ("authorization", "api-key"):
            safe[k] = "***"
    log.debug("%s %s headers=%s", method, url, safe)


# ===========================================================================
# Konfiguracja i dane logowania
# ===========================================================================

CONFIG_PATH = os.path.expanduser("~/.config/aqnapi/config.ini")
CACHE_DIR = os.path.expanduser("~/.cache/aqnapi")


class Config:
    """Dane logowania i ustawienia. Priorytet: flagi > env > plik ini."""

    def __init__(self, path: Optional[str] = None):
        self.path = path or CONFIG_PATH
        self._cp = configparser.ConfigParser()
        if os.path.isfile(self.path):
            self._cp.read(self.path, encoding="utf-8")

    def _get(self, section: str, key: str, env: str,
             override: Optional[str]) -> Optional[str]:
        if override:
            return override
        if os.environ.get(env):
            return os.environ[env]
        if self._cp.has_option(section, key):
            return self._cp.get(section, key)
        return None

    # Napisy24
    def n24_login(self, o=None):
        return self._get("napisy24", "login", "NAPI24_LOGIN", o)

    def n24_pass(self, o=None):
        return self._get("napisy24", "pass", "NAPI24_PASS", o)

    # napiprojekt
    def np_user(self, o=None):
        return self._get("napiprojekt", "user", "NAPI_USER", o)

    def np_pass(self, o=None):
        return self._get("napiprojekt", "pass", "NAPI_PASS", o)

    # OpenSubtitles
    def os_api_key(self, o=None):
        return self._get("opensubtitles", "api_key", "OS_API_KEY", o)

    def os_username(self, o=None):
        return self._get("opensubtitles", "username", "OS_USERNAME", o)

    def os_password(self, o=None):
        return self._get("opensubtitles", "password", "OS_PASSWORD", o)


def _cache_token_path() -> str:
    return os.path.join(CACHE_DIR, "os_token.json")


def load_cached_os_token() -> Optional[dict]:
    p = _cache_token_path()
    if not os.path.isfile(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            data = json.load(f)
        if _jwt_expiry(data.get("token", "")) > _now() + 60:
            return data
    except Exception:  # noqa: BLE001
        return None
    return None


def save_cached_os_token(data: dict):
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(_cache_token_path(), "w", encoding="utf-8") as f:
        json.dump(data, f)


def _now() -> int:
    import time
    return int(time.time())


def _jwt_expiry(token: str) -> int:
    """Odczytaj `exp` (Unix) z JWT. 0 gdy się nie da."""
    try:
        payload = token.split(".")[1]
        payload += "=" * (-len(payload) % 4)
        data = json.loads(base64.urlsafe_b64decode(payload))
        return int(data.get("exp", 0))
    except Exception:  # noqa: BLE001
        return 0


# ===========================================================================
# Wspólne struktury wyników
# ===========================================================================

@dataclass
class SubtitleHit:
    """Pozycja z wyszukiwania (niezależna od serwisu)."""
    service: str
    sub_id: str
    title: str = ""
    year: str = ""
    language: str = ""
    release: str = ""
    downloads: int = 0
    file_id: str = ""      # OpenSubtitles: id pliku do pobrania
    fps: Optional[float] = None
    extra: dict = field(default_factory=dict)


@dataclass
class UploadResult:
    service: str
    ok: bool
    message: str = ""
    raw: str = ""


# ===========================================================================
# Klient: Napisy24.pl
# ===========================================================================

class Napisy24Client:
    HOST = "napisy24.pl"
    CLIENT_VER = "v1.99.1"
    AGENT_UA = "dmnapi"
    AGENT_AP = "4lumen28"
    WEB_UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")

    def __init__(self, timeout: float = DEFAULT_TIMEOUT):
        self.timeout = timeout

    # ---- pobieranie/wyszukiwanie (protokół klienta + webapi) ----

    def _multipart_run(self, endpoint: str, fields: Dict[str, object]) -> bytes:
        boundary = "--------071926211419984"
        body = bytearray()
        for name, value in fields.items():
            payload = value if isinstance(value, bytes) else str(value).encode("utf-8")
            body += ("--" + boundary + "\r\n").encode()
            body += ('Content-Disposition: form-data; name="%s"\r\n' % name).encode()
            body += b"Content-Type: text/plain\r\n"
            body += b"Content-Transfer-Encoding: 8bit\r\n\r\n"
            body += payload
            body += b"\r\n"
        body += ("--" + boundary + "--").encode()
        ct = "multipart/form-data; boundary=%s" % boundary
        return raw_http10_post(self.HOST, "/run/" + endpoint, ct, bytes(body),
                               user_agent="Mozilla/4.0", timeout=self.timeout)

    def login(self, login: str, password: str) -> Tuple[bool, str]:
        body = self._multipart_run("CheckLogin.php", {
            "postAction": "Logowanie",
            "postVer": self.CLIENT_VER,
            "login": n24_obf(login),
            "pass": n24_obf(password),
        })
        text = body.decode("utf-8", errors="replace").strip()
        ok = text.startswith("login=ok")
        return ok, text

    @staticmethod
    def _parse_checksub(body: bytes) -> dict:
        sep = body.find(b"||")
        header = body[:sep] if sep >= 0 else body
        htext = header.decode("utf-8", errors="replace")
        result = {"raw_header": htext, "zip": b""}
        m = re.match(r"OK-(\d+)", htext)
        result["count"] = int(m.group(1)) if m else 0
        for part in htext.split("|"):
            if ":" in part:
                k, _, v = part.partition(":")
                result[k.strip()] = v.strip()
        if sep >= 0 and result["count"] > 0:
            result["zip"] = body[sep + 2:]
        return result

    def checksub_agent(self, path: str, lang: str = "PL") -> dict:
        """CheckSubAgent.php — bez zaciemniania, konto agenta, bez logowania."""
        fields = {
            "postAction": "CheckSub",
            "ua": self.AGENT_UA,
            "ap": self.AGENT_AP,
            "fh": oshash(path).upper(),
            "md": md5_10mb(path),
            "fs": str(os.path.getsize(path)),
            "fn": os.path.basename(path),
            "nl": lang,
        }
        body = urllib.parse.urlencode(fields).encode()
        raw = raw_http10_post(self.HOST, "/run/CheckSubAgent.php",
                              "application/x-www-form-urlencoded", body,
                              timeout=self.timeout)
        return self._parse_checksub(raw)

    def checksub(self, path: str, lang: str = "pl", prefer_best: bool = True) -> dict:
        """CheckSub2.php — protokół klienta z zaciemnianiem."""
        fields = {
            "postAction": "CheckSub",
            "postVer": self.CLIENT_VER,
            "fh": n24_obf(oshash(path).upper()),
            "md": n24_obf(md5_10mb(path)),
            "fs": n24_obf(str(os.path.getsize(path))),
            "fn": n24_obf(os.path.basename(path)),
            "nl": n24_obf(lang),
            "n24pref": "1" if prefer_best else "0",
            "licz": "1",
        }
        body = self._multipart_run("CheckSub2.php", fields)
        return self._parse_checksub(body)

    def search(self, imdb: str = "", title: str = "") -> List[SubtitleHit]:
        """webapi.php — wyszukiwanie po IMDB lub tytule (XML, bez obf)."""
        if imdb:
            q = "imdb=" + urllib.parse.quote(_norm_imdb(imdb))
        elif title:
            q = "title=" + urllib.parse.quote(title)
        else:
            raise AqError("Podaj imdb lub title")
        url = f"http://{self.HOST}/libs/webapi.php?{q}"
        resp = http_get(url, headers={"Referer": f"http://{self.HOST}"},
                        timeout=self.timeout)
        return self._parse_webapi(resp.text())

    @staticmethod
    def _parse_webapi(xml_text: str) -> List[SubtitleHit]:
        import xml.etree.ElementTree as ET
        hits: List[SubtitleHit] = []
        # webapi zwraca luźne <subtitle>...; usuń deklarację XML i opakuj
        body = re.sub(r"<\?xml[^>]*\?>", "", xml_text).strip()
        wrapped = "<root>" + body + "</root>"
        try:
            root = ET.fromstring(wrapped)
        except ET.ParseError:
            return hits
        for sub in root.iter("subtitle"):
            def t(tag):
                el = sub.find(tag)
                return el.text.strip() if el is not None and el.text else ""
            fps_val = None
            try:
                fps_val = float(t("fps")) if t("fps") else None
            except ValueError:
                fps_val = None
            hits.append(SubtitleHit(
                service="napisy24",
                sub_id=t("id"),
                title=t("title") or t("altTitle"),
                year=t("year"),
                language=t("language"),
                release=t("release"),
                fps=fps_val,
                extra={"altTitle": t("altTitle"), "imdb": t("imdb")},
            ))
        return hits

    def download_by_id(self, napis_id: str) -> bytes:
        """download.php?napisId=N — zwraca ZIP z napisami."""
        url = f"http://{self.HOST}/run/pages/download.php?napisId={urllib.parse.quote(str(napis_id))}"
        resp = http_get(url, headers={"Referer": f"http://{self.HOST}"},
                        timeout=self.timeout)
        if resp.body[:2] != b"PK":
            raise NotFoundError(f"Serwer nie zwrócił ZIP dla napisId={napis_id}")
        return resp.body

    def imdb_info(self, imdb: str) -> dict:
        imdb = _norm_imdb(imdb)
        body = self._multipart_run("CheckIMDB.php", {"imdbId": imdb})
        text = body.decode("utf-8", errors="replace").strip()
        return {"raw": text, "valid": not text.upper().startswith("INVALID")}

    # ---- upload/delete przez formularz WWW (/dodaj-napisy) ----

    def _web_opener(self):
        import http.cookiejar
        cj = http.cookiejar.CookieJar()
        return urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))

    def web_login(self, opener, login: str, password: str) -> bool:
        base = f"https://{self.HOST}"
        r = opener.open(urllib.request.Request(
            base + "/", headers={"User-Agent": self.WEB_UA}), timeout=self.timeout)
        home = r.read().decode("utf-8", errors="replace")
        m = re.search(r'name="([0-9a-f]{32})"\s+value="1"', home)
        token = m.group(1) if m else None
        fields = {
            "username": login, "passwd": password, "remember": "yes",
            "option": "com_comprofiler", "view": "login", "op2": "login",
            "return": "", "loginfrom": "loginmodule",
        }
        if token:
            fields[token] = "1"
        data = urllib.parse.urlencode(fields).encode()
        r2 = opener.open(urllib.request.Request(
            base + "/cb-login", data=data,
            headers={"User-Agent": self.WEB_UA,
                     "Content-Type": "application/x-www-form-urlencoded"}),
            timeout=self.timeout)
        page = r2.read().decode("utf-8", errors="replace")
        # nawiąż sesję RSForm
        opener.open(urllib.request.Request(
            base + "/dodaj-napisy", headers={"User-Agent": self.WEB_UA}),
            timeout=self.timeout).read()
        return "Wyloguj" in page

    def web_upload(self, opener, filename: str, filebytes: bytes,
                   meta: dict) -> UploadResult:
        base = f"https://{self.HOST}"
        is_series = bool(meta.get("season"))
        fields: List[Tuple] = [
            ("form[form_typ]", "Serial" if is_series else "Film"),
            ("form[form_dodajIMDB]", meta.get("imdb", "")),
            ("form[form_dodaj_tytul]", meta.get("title", "")),
            ("form[form_dodaj_polskiTytul]", meta.get("title_pl", "")),
            ("form[form_dodaj_rok]", str(meta.get("year", ""))),
            ("form[form_dodaj_wydanie]", meta.get("release", "")),
            ("form[form_dodaj_hash]", ""),
            ("form[form_dodaj_tlumaczenie]", meta.get("translator", "")),
            ("form[form_dodaj_dopasowanie]", meta.get("sync", "")),
            ("form[form_dodaj_korekta]", meta.get("proof", "")),
            ("form[form_dodaj_rozdzielczosc]", meta.get("resolution", "")),
            ("form[form_dodaj_fps][]", meta.get("fps", "23.976")),
            ("form[form_dodaj_jezyk][]", meta.get("lang_label", "Polski")),
            ("form[form_dodajIloscPlyt]", "1"),
            ("form[form_czas_cd1]", meta.get("duration", "")),
            ("form[form_wielkosc_cd1]", str(meta.get("size", ""))),
            ("form[form_dodajNapis1_plik]", (filename, filebytes, "application/octet-stream")),
            ("form[form_dodajNapis2_plik]", ("", b"", "application/octet-stream")),
            ("form[form_dodajNapis3_plik]", ("", b"", "application/octet-stream")),
            ("form[form_dodajNapis4_plik]", ("", b"", "application/octet-stream")),
            ("form[dodajTlumaczenie]", "Dodaj"),
            ("form[formId]", "7"),
            ("form[remId]", ""),
            ("form[form_dodajTlumaczenieId]", "0"),
        ]
        if is_series:
            fields += [
                ("form[realtxt]", meta.get("title", "")),
                ("form[serial][]", meta.get("series_imdb", meta.get("imdb", ""))),
                ("form[form_dodaj_nrSezonu]", str(meta.get("season", ""))),
                ("form[form_dodaj_nrOdcinka]", str(meta.get("episode", ""))),
                ("form[form_dodaj_tytulOdcinka]", meta.get("episode_title", "")),
                ("form[form_dodaj_cover]", "Serialu"),
            ]
        body, ctype = build_multipart(fields)
        req = urllib.request.Request(
            base + "/dodaj-napisy", data=body,
            headers={"User-Agent": self.WEB_UA, "Content-Type": ctype,
                     "Referer": base + "/dodaj-napisy"})
        r = opener.open(req, timeout=self.timeout)
        page = r.read().decode("utf-8", errors="replace")
        ok = "Napisy Dodane/Zmienione" in page or "dziękujemy" in page
        return UploadResult("napisy24", ok,
                            "OK" if ok else "Serwer nie potwierdził dodania", page[:500])

    def web_delete(self, opener, napis_id: str,
                   reason: str = "usuniecie napisu") -> UploadResult:
        base = f"https://{self.HOST}"
        opener.open(urllib.request.Request(
            base + f"/dodaj-napisy?usun={napis_id}",
            headers={"User-Agent": self.WEB_UA}), timeout=self.timeout).read()
        fields = {
            "form[form_usunPowod]": reason,
            "form[btnSend]": "Usuń napisy",
            "form[usunId]": str(napis_id),
            "form[formId]": "8",
        }
        data = urllib.parse.urlencode(fields).encode()
        r = opener.open(urllib.request.Request(
            base + f"/dodaj-napisy?usun={napis_id}", data=data,
            headers={"User-Agent": self.WEB_UA,
                     "Content-Type": "application/x-www-form-urlencoded"}),
            timeout=self.timeout)
        page = r.read().decode("utf-8", errors="replace")
        ok = "Napisy usunięte" in page
        return UploadResult("napisy24", ok,
                            "Usunięto" if ok else "Nie potwierdzono usunięcia", page[:300])


def _norm_imdb(imdb_id: str) -> str:
    """Znormalizuj do postaci ttNNNNNNN (9 znaków)."""
    m = re.search(r"(\d+)", imdb_id)
    if not m:
        return imdb_id
    num = m.group(1)
    return "tt" + num.zfill(7)


# ===========================================================================
# Klient: napiprojekt.pl
# ===========================================================================

class NapiprojektClient:
    API_URL = "http://www.napiprojekt.pl/api/api-napiprojekt3.php"
    MOVIE_SEARCH_URL = "http://napiprojekt.pl/api/api-movie-search.php"
    MOVIE_ASSOCIATE_URL = "http://napiprojekt.pl/api/api-movie-associate2.php"
    USER_ACCOUNT_URL = "http://napiprojekt.pl/api/api_user_account.php"
    FILE_INFO_URL = "http://napiprojekt.pl/api/api.php"
    CLIENT = "aqnapi"
    CLIENT_VER = __version__

    def __init__(self, timeout: float = DEFAULT_TIMEOUT):
        self.timeout = timeout

    def _post(self, fields: List[Tuple]) -> str:
        base = [("client", self.CLIENT), ("client_ver", self.CLIENT_VER)]
        resp = http_post_multipart(self.API_URL, base + fields, timeout=self.timeout)
        return resp.text()

    def download(self, movie_hash: str, lang: str = "PL") -> bytes:
        """mode=1 — pobieranie po hashu (MD5-10MiB). Zwraca surowe napisy."""
        fields = [
            ("mode", "1"),
            ("downloaded_subtitles_id", movie_hash),
            ("downloaded_subtitles_lang", lang.upper()),
            ("downloaded_subtitles_txt", "1"),
        ]
        xml = self._post(fields)
        import xml.etree.ElementTree as ET
        try:
            root = ET.fromstring(xml.strip())
        except ET.ParseError as e:
            raise ServerError(f"Zła odpowiedź XML: {e}") from e
        sub = root.find("subtitles")
        content = sub.find("content") if sub is not None else None
        if content is None or not (content.text or "").strip():
            raise NotFoundError("napiprojekt nie ma napisów dla tego pliku")
        return base64.b64decode(content.text.strip())

    def file_info_fps(self, movie_hash: str) -> Optional[float]:
        url = self.FILE_INFO_URL + "?" + urllib.parse.urlencode(
            {"mode": "file_info", "client": "dreambox", "id": movie_hash})
        try:
            resp = http_get(url, timeout=self.timeout)
            m = re.search(r"<fps>([\d.]+)</fps>", resp.text())
            return float(m.group(1)) if m else None
        except (AqError, ValueError):
            return None

    def upload(self, movie_hash: str, subtitle_bytes: bytes, lang: str = "PL",
               author: str = "", corrected: bool = False, comment: str = "",
               only_testing: bool = False) -> UploadResult:
        """mode=512 (nowe) / 1024 (poprawki). Archiwum 7z-AES budowane lokalnie."""
        archive = write_7z_aes(f"{movie_hash}.txt", subtitle_bytes)
        subs_md5 = bytes_md5(subtitle_bytes)
        fields: List[Tuple] = [
            ("mode", "1024" if corrected else "512"),
            ("SubtitlesHash", subs_md5),
            ("SubtitlesAutor", author),
            ("SubtitlesLang", lang.upper()),
        ]
        if comment:
            fields.append(("SubtitlesComment", comment))
        if only_testing:
            fields.append(("OnlyTesting", "1"))
        fields.append(("subtitles", (f"{movie_hash}.zip", archive, "subtitles/zip")))
        base = [("client", self.CLIENT), ("client_ver", self.CLIENT_VER)]
        resp = http_post_multipart(self.API_URL, base + fields, timeout=self.timeout)
        return self._parse_upload(resp.text())

    @staticmethod
    def _parse_upload(xml: str) -> UploadResult:
        import xml.etree.ElementTree as ET
        try:
            root = ET.fromstring(xml.strip())
        except ET.ParseError as e:
            raise ServerError(f"Zła odpowiedź XML: {e}\n{xml[:300]}") from e
        node = root.find("upload_subtitles") or root
        status = (node.findtext("status") or "").strip().lower()
        error = (node.findtext("error") or "").strip()
        warning = (node.findtext("warning") or "").strip()
        ok = status in ("uploaded", "success", "ok")
        return UploadResult("napiprojekt", ok,
                            warning or error or status or "brak statusu", xml[:500])

    def search_movies(self, title: str) -> List[dict]:
        url = self.MOVIE_SEARCH_URL + "?" + urllib.parse.urlencode(
            {"mode": "get", "client": "allplayer", "search": title})
        resp = http_get(url, timeout=self.timeout)
        return self._parse_search(resp.text())

    @staticmethod
    def _parse_search(xml: str) -> List[dict]:
        import xml.etree.ElementTree as ET
        out = []
        try:
            root = ET.fromstring(xml.strip())
        except ET.ParseError:
            return out
        for mv in root.findall("movie"):
            desc = mv.find("description")
            if desc is None:
                continue
            titles = desc.find("titles")
            links = desc.find("direct_links")

            def lt(parent, tag):
                if parent is None:
                    return ""
                el = parent.find(tag)
                return el.text.strip() if el is not None and el.text else ""

            imdb = lt(links, "imdb_com")
            out.append({
                "movie_id": lt(desc, "id"),
                "title_pl": lt(titles, "polish"),
                "title_orig": lt(titles, "original"),
                "year": lt(desc, "year"),
                "imdb": imdb,
                "imdb_id": (re.search(r"tt\d+", imdb).group(0) if re.search(r"tt\d+", imdb) else ""),
                "filmweb": lt(links, "filmweb_pl"),
            })
        return out

    def find_movie(self, title: str, imdb: str = "") -> Optional[dict]:
        movies = self.search_movies(title)
        if imdb:
            want = _norm_imdb(imdb)
            movies = [m for m in movies if m["imdb_id"] == want] or movies
        return movies[0] if movies else None

    def associate(self, user: str, password: str, movie_hash: str,
                  movie_id: str) -> UploadResult:
        """api-movie-associate2.php (GET, hasło jawne)."""
        if not user or not password:
            raise AuthError("Powiązanie wymaga loginu i hasła")
        url = self.MOVIE_ASSOCIATE_URL + "?" + urllib.parse.urlencode({
            "nick": user, "pass": password,
            "id_pliku": movie_hash, "id_filmu": str(movie_id),
        })
        resp = http_get(url, timeout=self.timeout)
        import xml.etree.ElementTree as ET
        try:
            root = ET.fromstring(resp.text().strip())
        except ET.ParseError as e:
            raise ServerError(f"Zła odpowiedź XML: {e}") from e
        status = (root.findtext("status") or "").strip().lower()
        if status not in ("success", "ok"):
            node = root.find("movie") or root
            status = (node.findtext("status") or status).strip().lower()
        ok = status in ("success", "ok")
        return UploadResult("napiprojekt", ok, status or "brak statusu", resp.text()[:300])

    def account(self, user: str, password: str) -> dict:
        """api_user_account.php (GET, hasło jawne)."""
        url = self.USER_ACCOUNT_URL + "?" + urllib.parse.urlencode(
            {"user": user, "pass": password})
        resp = http_get(url, timeout=self.timeout)
        import xml.etree.ElementTree as ET
        try:
            root = ET.fromstring(resp.text().strip())
        except ET.ParseError as e:
            raise AuthError("Błędne dane logowania lub zła odpowiedź") from e
        info = root.find("konto")
        if info is None and root.tag != "user_info":
            raise AuthError("Błędne dane logowania (brak danych konta)")
        out = {}
        for parent in root:
            for child in parent:
                out[f"{parent.tag}.{child.tag}"] = (child.text or "").strip()
        return out


# ===========================================================================
# Klient: OpenSubtitles.com (REST API v1)
# ===========================================================================

class OpenSubtitlesClient:
    BASE = "api.opensubtitles.com"

    def __init__(self, api_key: str, timeout: float = DEFAULT_TIMEOUT,
                 base: Optional[str] = None):
        if not api_key:
            raise AuthError("OpenSubtitles wymaga klucza API (Api-Key)")
        self.api_key = api_key
        self.timeout = timeout
        self.base = base or self.BASE
        self.token: Optional[str] = None

    def _headers(self, auth: bool = False) -> dict:
        h = {
            "Api-Key": self.api_key,
            "User-Agent": USER_AGENT_OS,
            "Accept": "application/json",
        }
        if auth:
            if not self.token:
                raise AuthError("Ta operacja wymaga zalogowania (POST /login)")
            h["Authorization"] = "Bearer " + self.token
        return h

    def _url(self, path: str) -> str:
        return f"https://{self.base}/api/v1{path}"

    def login(self, username: str, password: str) -> dict:
        cached = load_cached_os_token()
        if cached and cached.get("api_key") == self.api_key and \
                cached.get("username") == username:
            self.token = cached["token"]
            self.base = cached.get("base", self.base)
            return cached
        resp = http_post_json(self._url("/login"),
                              {"username": username, "password": password},
                              headers=self._headers(), timeout=self.timeout)
        if resp.status == 401:
            raise AuthError("OpenSubtitles: błędny login lub hasło")
        if resp.status != 200:
            raise ServerError(f"OpenSubtitles /login: HTTP {resp.status}: {resp.text()[:200]}")
        data = resp.json()
        self.token = data.get("token")
        if data.get("base_url"):
            self.base = data["base_url"]
        save_cached_os_token({
            "token": self.token, "base": self.base,
            "api_key": self.api_key, "username": username,
            "allowed_downloads": data.get("user", {}).get("allowed_downloads"),
        })
        return data

    def logout(self) -> bool:
        resp = http_request("DELETE", self._url("/logout"),
                            headers=self._headers(auth=True), timeout=self.timeout)
        try:
            os.remove(_cache_token_path())
        except OSError:
            pass
        return resp.status == 200

    def search(self, *, query: str = "", imdb_id: str = "", tmdb_id: str = "",
               moviehash: str = "", languages: str = "", type_: str = "",
               season: str = "", episode: str = "", year: str = "") -> List[SubtitleHit]:
        params: Dict[str, str] = {}
        if query:
            params["query"] = query
        if imdb_id:
            params["imdb_id"] = re.sub(r"\D", "", imdb_id).lstrip("0") or "0"
        if tmdb_id:
            params["tmdb_id"] = str(tmdb_id)
        if moviehash:
            params["moviehash"] = moviehash.lower()
        if languages:
            params["languages"] = ",".join(sorted(l.strip().lower()
                                                   for l in languages.split(",") if l.strip()))
        if type_:
            params["type"] = type_
        if season:
            params["season_number"] = str(season)
        if episode:
            params["episode_number"] = str(episode)
        if year:
            params["year"] = str(year)
        query_str = urllib.parse.urlencode(sorted(params.items()),
                                           quote_via=urllib.parse.quote_plus)
        url = self._url("/subtitles") + ("?" + query_str if query_str else "")
        resp = http_get(url, headers=self._headers(), timeout=self.timeout)
        if resp.status == 429:
            raise ServerError("OpenSubtitles: przekroczono limit zapytań (429)")
        if resp.status != 200:
            raise ServerError(f"OpenSubtitles /subtitles: HTTP {resp.status}")
        return self._parse_search(resp.json())

    @staticmethod
    def _parse_search(data: dict) -> List[SubtitleHit]:
        hits: List[SubtitleHit] = []
        for item in data.get("data", []):
            a = item.get("attributes", {})
            files = a.get("files", [])
            fd = a.get("feature_details", {}) or {}
            file_id = str(files[0]["file_id"]) if files else ""
            hits.append(SubtitleHit(
                service="opensubtitles",
                sub_id=str(a.get("subtitle_id", item.get("id", ""))),
                title=fd.get("movie_name") or fd.get("title", ""),
                year=str(fd.get("year", "")),
                language=a.get("language", ""),
                release=a.get("release", ""),
                downloads=int(a.get("download_count", 0) or 0),
                file_id=file_id,
                fps=a.get("fps"),
                extra={"file_name": files[0]["file_name"] if files else "",
                       "hearing_impaired": a.get("hearing_impaired"),
                       "from_trusted": a.get("from_trusted")},
            ))
        return hits

    def download(self, file_id: str, *, sub_format: str = "",
                 in_fps: Optional[float] = None,
                 out_fps: Optional[float] = None) -> Tuple[bytes, dict]:
        """POST /download -> link, potem pobranie linku. Zwraca (bajty, meta)."""
        payload: Dict[str, object] = {"file_id": int(file_id)}
        if sub_format:
            payload["sub_format"] = sub_format
        if in_fps and out_fps:
            payload["in_fps"] = in_fps
            payload["out_fps"] = out_fps
        resp = http_post_json(self._url("/download"), payload,
                              headers=self._headers(auth=True), timeout=self.timeout)
        if resp.status == 406 or resp.status == 403:
            raise ServerError(f"OpenSubtitles /download: {resp.text()[:200]}")
        if resp.status != 200:
            raise ServerError(f"OpenSubtitles /download: HTTP {resp.status}: {resp.text()[:200]}")
        data = resp.json()
        link = data.get("link")
        if not link:
            raise NotFoundError("OpenSubtitles nie zwrócił linku do pobrania")
        file_resp = http_get(link, timeout=self.timeout)
        return file_resp.body, data

    def formats(self) -> List[str]:
        resp = http_get(self._url("/infos/formats"), headers=self._headers(),
                        timeout=self.timeout)
        return resp.json().get("data", {}).get("output_formats", [])

    def languages(self) -> List[dict]:
        resp = http_get(self._url("/infos/languages"), headers=self._headers(),
                        timeout=self.timeout)
        return resp.json().get("data", [])

    def guessit(self, filename: str) -> dict:
        url = self._url("/utilities/guessit") + "?" + urllib.parse.urlencode(
            {"filename": filename})
        resp = http_get(url, headers=self._headers(), timeout=self.timeout)
        return resp.json()


# ===========================================================================
# Publiczne API wysokiego poziomu (użycie jako moduł)
# ===========================================================================

def download_subtitles(movie_path: str, lang: str = "pl",
                        services: Tuple[str, ...] = ("np", "n24", "os"),
                        *, api_key: Optional[str] = None,
                        os_username: Optional[str] = None,
                        os_password: Optional[str] = None,
                        fps: Optional[float] = None,
                        timeout: float = DEFAULT_TIMEOUT) -> bytes:
    """Pobierz napisy dla pliku filmowego, próbując kolejnych serwisów.

    Zwraca gotowe bajty SRT (UTF-8 z BOM, końce LF). Rzuca :class:`NotFoundError`,
    gdy żaden serwis nie ma napisów.

    >>> data = download_subtitles("film.mkv", "pl")            # doctest: +SKIP
    >>> open("film.srt", "wb").write(data)                     # doctest: +SKIP
    """
    resolved_fps = _resolve_fps(movie_path, None, fps)
    errors = []
    services = tuple(_parse_services(",".join(services), default="np,n24,os"))

    if "np" in services:
        try:
            raw = NapiprojektClient(timeout).download(md5_10mb(movie_path), lang.upper())
            return convert_to_srt_bytes(raw, fps=resolved_fps)
        except AqError as e:
            errors.append(f"napiprojekt: {e}")
    if "n24" in services:
        try:
            res = Napisy24Client(timeout).checksub_agent(movie_path, lang.upper())
            if res.get("count", 0) > 0 and res.get("zip"):
                return convert_to_srt_bytes(res["zip"], fps=resolved_fps)
            errors.append("napisy24: brak trafień")
        except AqError as e:
            errors.append(f"napisy24: {e}")
    if "os" in services and api_key:
        try:
            client = OpenSubtitlesClient(api_key, timeout)
            if os_username and os_password:
                client.login(os_username, os_password)
            hits = client.search(moviehash=oshash(movie_path), languages=lang)
            if hits:
                best = max(hits, key=lambda h: h.downloads)
                raw, _ = client.download(best.file_id)
                return convert_to_srt_bytes(raw, fps=_resolve_fps(movie_path, best.fps, fps))
            errors.append("opensubtitles: brak trafień")
        except AqError as e:
            errors.append(f"opensubtitles: {e}")

    raise NotFoundError("Nie znaleziono napisów. " + "; ".join(errors))


def search_subtitles(*, imdb: str = "", title: str = "", query: str = "",
                     lang: str = "", services: Tuple[str, ...] = ("np", "n24", "os"),
                     api_key: Optional[str] = None,
                     timeout: float = DEFAULT_TIMEOUT) -> List[SubtitleHit]:
    """Wyszukaj napisy w wielu serwisach. Zwraca listę :class:`SubtitleHit`."""
    services = tuple(_parse_services(",".join(services), default="np,n24,os"))
    hits: List[SubtitleHit] = []
    if "n24" in services and (imdb or title):
        try:
            hits += Napisy24Client(timeout).search(imdb=imdb, title=title)
        except AqError:
            pass
    if "np" in services and (title or query):
        try:
            for m in NapiprojektClient(timeout).search_movies(title or query):
                hits.append(SubtitleHit("napiprojekt", m["movie_id"],
                                        title=m["title_orig"] or m["title_pl"],
                                        year=m["year"], extra=m))
        except AqError:
            pass
    if "os" in services and api_key:
        try:
            hits += OpenSubtitlesClient(api_key, timeout).search(
                query=query or title, imdb_id=imdb, languages=lang)
        except AqError:
            pass
    return hits


def convert_file(input_path: str, output_path: Optional[str] = None, *,
                 movie_path: Optional[str] = None,
                 fps: Optional[float] = None,
                 sanitize: bool = True, keep_tags: bool = False,
                 max_display_ms: int = 10000, min_display_ms: int = 0) -> str:
    """Skonwertuj plik napisów do SRT (UTF-8+BOM/LF). Zwraca ścieżkę wyjścia.

    Domyślnie stosuje bezpieczne korekty (usuwanie tagów, docinanie zbyt długich
    i nakładających się czasów) — zob. :func:`sanitize_cues`."""
    with open(input_path, "rb") as f:
        raw = f.read()
    resolved_fps = _resolve_fps(movie_path, None, fps)
    out = _default_out(input_path, output_path)
    data = convert_to_srt_bytes(raw, fps=resolved_fps, sanitize=sanitize,
                                keep_tags=keep_tags, max_display_ms=max_display_ms,
                                min_display_ms=min_display_ms)
    with open(out, "wb") as f:
        f.write(data)
    return out


# ===========================================================================
# Warstwa CLI
# ===========================================================================

def _resolve_fps(movie_path: Optional[str], hit_fps: Optional[float],
                 flag_fps: Optional[float]) -> float:
    """Łańcuch źródeł FPS: plik filmowy -> metadane serwisu -> flaga -> domyślny."""
    if movie_path and os.path.isfile(movie_path):
        f = trusted_fps(fps_from_file(movie_path))
        if f:
            return f
    if trusted_fps(hit_fps):
        return hit_fps
    if flag_fps:
        return flag_fps
    return DEFAULT_FPS


def _default_out(movie_path: Optional[str], explicit: Optional[str]) -> str:
    if explicit:
        return explicit
    if movie_path:
        stem = os.path.splitext(movie_path)[0]
        return stem + ".srt"
    return "napisy.srt"


def _save_subtitles(raw: bytes, out_path: str, fps: float,
                    sanitize_kw: Optional[dict] = None) -> None:
    rep = SanitizeReport()
    data = convert_to_srt_bytes(raw, fps=fps, report=rep, **(sanitize_kw or {}))
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"Zapisano: {out_path} ({len(data)} B, {rep.total_cues} linii, "
          f"SRT UTF-8+BOM/LF)")
    if rep.any_changes():
        print("  Korekty: " + rep.summary())


def _sanitize_kw(args) -> dict:
    """Zbuduj argumenty sanityzacji z flag CLI."""
    if getattr(args, "no_sanitize", False):
        return {"sanitize": False}
    return {
        "sanitize": True,
        "keep_tags": bool(getattr(args, "keep_tags", False)),
        "max_display_ms": int(float(getattr(args, "max_display", None) or 10.0) * 1000),
        "min_display_ms": int(float(getattr(args, "min_display", None) or 0) * 1000),
        "strip_sdh": bool(getattr(args, "strip_sdh", False)),
    }


def add_sanitize_flags(sp) -> None:
    sp.add_argument("--keep-tags", action="store_true", dest="keep_tags",
                    help="nie usuwaj tagów formatujących (<i>, {..})")
    sp.add_argument("--strip-sdh", action="store_true", dest="strip_sdh",
                    help="usuń oznaczenia SDH/HI: [odgłosy], (opisy), MÓWCA:, ♪")
    sp.add_argument("--no-sanitize", action="store_true", dest="no_sanitize",
                    help="wyłącz wszystkie korekty przed zapisem")
    sp.add_argument("--max-display", type=float, default=10.0,
                    help="próg 'ekstremalnie długo' w sekundach (domyślnie 10)")
    sp.add_argument("--min-display", type=float, default=0.0,
                    help="minimalny czas wyświetlania w sekundach (domyślnie 0=wyłączone)")


def _skip_existing(out_path: str, force: bool) -> bool:
    """True + komunikat, gdy plik już istnieje i nie wymuszono nadpisania."""
    if not force and os.path.exists(out_path):
        print(f"Pominięto (napisy już istnieją): {out_path}  — użyj --force, aby nadpisać")
        return True
    return False


def _as_list(value) -> List[str]:
    return value if isinstance(value, list) else [value]


def _reject_output_with_many(files: List[str], output: Optional[str]) -> bool:
    """True (+komunikat), gdy podano -o razem z wieloma plikami wejściowymi."""
    if output and len(files) > 1:
        print("Błąd: --output/-o nie łączy się z wieloma plikami "
              "(nazwa wyjścia jest wtedy tworzona z nazwy każdego pliku).",
              file=sys.stderr)
        return True
    return False


# ---- polecenia agregujące ----

GITHUB_REPO = "areqq/aqnapi"


def _version_tuple(s: str) -> tuple:
    return tuple(int(x) for x in re.findall(r"\d+", s or "0"))


def cmd_update(args, cfg):
    """Samo-aktualizacja: pobierz najnowsze wydanie z GitHuba i podmień plik,
    jeśli dostępna nowsza wersja."""
    url = f"https://api.github.com/repos/{GITHUB_REPO}/releases/latest"
    resp = http_get(url, headers={"User-Agent": USER_AGENT_OS,
                                  "Accept": "application/vnd.github+json"},
                    timeout=args.timeout)
    if resp.status != 200:
        raise ServerError(f"GitHub API zwróciło HTTP {resp.status}")
    data = resp.json()
    latest = (data.get("tag_name") or "").lstrip("v")
    if not latest:
        raise ServerError("Nie udało się odczytać wersji z wydania GitHub")
    if _version_tuple(latest) <= _version_tuple(__version__):
        print(f"Masz najnowszą wersję (v{__version__}).")
        return 0
    print(f"Dostępna nowsza wersja: v{latest} (masz v{__version__})")
    if args.check:
        return 0
    # ścieżka do samego siebie + wybór artefaktu
    self_path = os.path.realpath(sys.argv[0])
    if self_path.startswith("/zip/") or not os.path.isfile(self_path):
        self_path = os.path.realpath(sys.executable)   # binarka APE
    asset_name = "aqnapi.com" if self_path.lower().endswith((".com", ".exe")) else "aqnapi.py"
    asset = next((a for a in data.get("assets", []) if a.get("name") == asset_name), None)
    if not asset:
        raise NotFoundError(f"Wydanie nie zawiera artefaktu '{asset_name}'")
    print(f"Pobieram {asset_name} …")
    dl = http_get(asset["browser_download_url"],
                  headers={"User-Agent": USER_AGENT_OS}, timeout=args.timeout)
    if dl.status != 200 or not dl.body:
        raise ServerError(f"Pobieranie artefaktu nie powiodło się (HTTP {dl.status})")
    tmp = self_path + ".new"
    with open(tmp, "wb") as f:
        f.write(dl.body)
    try:
        os.chmod(tmp, os.stat(self_path).st_mode)
    except OSError:
        os.chmod(tmp, 0o755)
    os.replace(tmp, self_path)
    print(f"Zaktualizowano do v{latest}: {self_path}")
    return 0


def cmd_hash(args, cfg):
    h = file_hashes(args.file)
    print(f"OSH (fh)        : {h['osh']}")
    print(f"MD5-10MiB (md)  : {h['md5_10mb']}")
    print(f"rozmiar         : {h['size']} B")
    print(f"nazwa           : {h['name']}")
    return 0


def cmd_fps(args, cfg):
    f = fps_from_file(args.file)
    if f is None:
        print("Nie udało się odczytać FPS z pliku (obsługa: MKV/AVI/MP4/MOV)")
        return 1
    trusted = "" if trusted_fps(f) else "  (poza bramką 22<fps<32 — traktowane jako niepewne)"
    print(f"FPS: {f:.3f}{trusted}")
    return 0


def _out_format(out_path: str, explicit: Optional[str]) -> str:
    """Wyznacz format wyjściowy z flagi --format lub rozszerzenia pliku."""
    if explicit:
        return explicit.lower()
    return _EXT_FORMAT.get(os.path.splitext(out_path)[1].lower(), "srt")


def cmd_convert(args, cfg):
    fps = _resolve_fps(args.movie, None, args.fps)
    out = _default_out(args.input, args.output)
    fmt = _out_format(out, getattr(args, "format", None))
    if fmt == "srt":
        with open(args.input, "rb") as f:
            _save_subtitles(f.read(), out, fps, _sanitize_kw(args))
        return 0
    # eksport do innego formatu: parsuj -> (sanityzuj) -> emituj
    cues = _load_cues(args.input, fps)
    kw = _sanitize_kw(args)
    if kw.get("sanitize", True):
        cues, rep = sanitize_cues(
            cues, keep_tags=kw["keep_tags"], max_display_ms=kw["max_display_ms"],
            min_display_ms=kw["min_display_ms"], strip_sdh=kw["strip_sdh"])
    data = emit_subtitle(cues, fmt, fps)
    with open(out, "wb") as f:
        f.write(data)
    print(f"Zapisano: {out} ({len(cues)} linii, format {fmt})")
    return 0


def cmd_fpsconv(args, cfg):
    from_fps = args.from_fps
    to_fps = args.to_fps
    if to_fps is None and args.movie:
        to_fps = trusted_fps(fps_from_file(args.movie))
    if not from_fps or not to_fps:
        raise AqError("Podaj --from ORAZ --to (albo --to przez --movie).")
    cues = _load_cues(args.input, from_fps)
    scale = from_fps / to_fps
    conv = apply_sync(cues, scale, 0.0)
    out = args.output or (os.path.splitext(args.input)[0] + f".{to_fps:g}fps.srt")
    fmt = _out_format(out, getattr(args, "format", None))
    with open(out, "wb") as f:
        f.write(emit_subtitle(conv, fmt, to_fps))
    print(f"Przeliczono FPS {from_fps:g} -> {to_fps:g} (scale={scale:.5f}): "
          f"{out} ({len(conv)} linii)")
    return 0


_EXT_FOR = {"srt": "srt", "vtt": "vtt", "ass": "ass", "microdvd": "sub"}


def cmd_merge(args, cfg):
    """Połącz kilka plików napisów w jeden (np. CD1+CD2)."""
    files = args.files
    if len(files) < 2:
        raise AqError("Podaj co najmniej 2 pliki do połączenia.")
    fps = args.fps or DEFAULT_FPS
    offsets = [int(round(o * 1000)) for o in (args.offset or [])]
    merged = _load_cues(files[0], fps)
    running_end = max((c.end_ms for c in merged), default=0)
    for i, path in enumerate(files[1:]):
        cues = _load_cues(path, fps)
        shift = offsets[i] if i < len(offsets) else running_end
        shifted = apply_sync(cues, 1.0, shift)
        merged += shifted
        running_end = max([running_end] + [c.end_ms for c in shifted])
    out = args.output or (os.path.splitext(files[0])[0] + ".merged.srt")
    fmt = _out_format(out, getattr(args, "format", None))
    with open(out, "wb") as f:
        f.write(emit_subtitle(merged, fmt, fps))
    print(f"Połączono {len(files)} plików → {out} ({len(merged)} linii, format {fmt})")
    return 0


def cmd_split(args, cfg):
    """Podziel plik napisów na części w podanych punktach czasowych."""
    import bisect
    fps = args.fps or DEFAULT_FPS
    cues = _load_cues(args.input, fps)
    points = []
    for a in args.at:
        v = _parse_user_time(a)
        if v is None:
            raise AqError(f"Zły format --at '{a}' (użyj hh:mm:ss,mmm lub sekund)")
        points.append(v)
    points.sort()
    parts: List[List[Cue]] = [[] for _ in range(len(points) + 1)]
    for cue in cues:
        parts[bisect.bisect_right(points, cue.start_ms)].append(cue)
    fmt = getattr(args, "format", None) or "srt"
    ext = _EXT_FOR.get(fmt, "srt")
    base = args.output or os.path.splitext(args.input)[0]
    written = []
    for i, part in enumerate(parts):
        if not part:
            continue
        origin = points[i - 1] if i > 0 else 0
        seg = apply_sync(part, 1.0, -origin) if (args.rebase and i > 0) else part
        p_out = f"{base}.part{i + 1}.{ext}"
        with open(p_out, "wb") as f:
            f.write(emit_subtitle(seg, fmt, fps))
        written.append(p_out)
    if not written:
        print("Brak linii do zapisania.")
        return 1
    print("Podzielono na: " + ", ".join(written)
          + ("  (czasy części wyzerowane)" if args.rebase else ""))
    return 0


def cmd_config(args, cfg):
    """Zarządzaj plikiem konfiguracyjnym (dane logowania, klucz API)."""
    path = getattr(args, "config", None) or CONFIG_PATH
    cp = configparser.ConfigParser()
    if os.path.isfile(path):
        cp.read(path, encoding="utf-8")

    if args.config_cmd == "path":
        print(path)
        return 0
    if args.config_cmd == "show":
        if not cp.sections():
            print(f"(pusty lub brak pliku: {path})")
            return 0
        for sect in cp.sections():
            print(f"[{sect}]")
            for k, v in cp.items(sect):
                shown = ("*" * len(v)) if (k in ("pass", "password") and v) else v
                print(f"  {k} = {shown}")
        return 0

    # init — interaktywnie
    import getpass

    def ask(section, key, prompt, secret=False):
        cur = cp.get(section, key, fallback="")
        if secret:
            hint = " [Enter=bez zmian]" if cur else ""
            val = getpass.getpass(f"{prompt}{hint}: ")
            return val if val else cur
        hint = f" [{cur}]" if cur else ""
        val = input(f"{prompt}{hint}: ").strip()
        return val if val else cur

    print("Konfiguracja aqnapi — Enter zostawia obecną wartość.\n")
    for section in ("napisy24", "napiprojekt", "opensubtitles"):
        if not cp.has_section(section):
            cp.add_section(section)
    cp["napisy24"]["login"] = ask("napisy24", "login", "Napisy24 login/e-mail")
    cp["napisy24"]["pass"] = ask("napisy24", "pass", "Napisy24 hasło", secret=True)
    cp["napiprojekt"]["user"] = ask("napiprojekt", "user", "napiprojekt login")
    cp["napiprojekt"]["pass"] = ask("napiprojekt", "pass", "napiprojekt hasło", secret=True)
    cp["opensubtitles"]["api_key"] = ask("opensubtitles", "api_key", "OpenSubtitles API key")
    cp["opensubtitles"]["username"] = ask("opensubtitles", "username", "OpenSubtitles login")
    cp["opensubtitles"]["password"] = ask("opensubtitles", "password", "OpenSubtitles hasło", secret=True)

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        cp.write(f)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    print(f"\nZapisano: {path} (uprawnienia 600)")
    return 0


def _load_cues(path: str, fps: float = DEFAULT_FPS) -> List[Cue]:
    """Wczytaj plik napisów (ew. ZIP) i sparsuj do listy Cue."""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] == b"PK":
        raw = extract_from_zip(raw)
    cues = parse_any(decode_text(raw), fps=fps)
    if not cues:
        raise AqError(f"Nie rozpoznano napisów w pliku: {path}")
    return cues


def cmd_sync(args, cfg):
    """Synchronizuj napisy (target) wg wzorca (reference) — interaktywnie lub
    przez --offset / --anchor."""
    fps = args.fps or DEFAULT_FPS
    ref_cues = _load_cues(args.reference, fps)
    tgt_cues = _load_cues(args.target, fps)
    out = args.output or (os.path.splitext(args.target)[0] + ".synced.srt")

    work = tgt_cues
    if args.offset is not None:
        scale, offset = 1.0, args.offset * 1000.0
    elif args.anchor:
        pairs = []
        for a in args.anchor:
            try:
                r_i, t_i = (int(x) for x in a.replace(":", ",").split(","))
            except ValueError:
                raise AqError(f"Zły format --anchor '{a}' (użyj R,T, np. 1,1)")
            if not (1 <= r_i <= len(ref_cues)) or not (1 <= t_i <= len(tgt_cues)):
                raise AqError(f"--anchor {a}: numer linii poza zakresem")
            pairs.append((tgt_cues[t_i - 1].start_ms, ref_cues[r_i - 1].start_ms))
        scale, offset = compute_sync_transform(pairs)
    else:
        result = _sync_tui(ref_cues, tgt_cues)
        if result is None:
            print("Anulowano — nic nie zapisano.")
            return 1
        pairs, work = result
        if not pairs and work == tgt_cues:
            print("Nie zaznaczono par ani nie zmieniono czasów — nic nie zapisano.")
            return 1
        scale, offset = compute_sync_transform(pairs)

    synced = apply_sync(work, scale, offset)
    data = emit_srt(cues_to_srt(synced))
    with open(out, "wb") as f:
        f.write(data)
    print(f"Zsynchronizowano: {out} ({len(synced)} linii)")
    print(f"  transformacja: nowy = {scale:.5f} * stary + ({offset / 1000:+.3f} s)")
    return 0


def _sync_tui(ref_cues: List[Cue], tgt_cues: List[Cue]):
    """Interaktywny wybór par kotwic i edycja czasów w dwóch kolumnach (curses).

    Zwraca (pairs, work_cues) gdzie pairs = lista (target_ms, ref_ms), a
    work_cues = (ew. ręcznie zmieniona) kopia napisów do synchronizacji;
    albo None przy anulowaniu."""
    try:
        import curses
    except ImportError:
        raise AqError("Tryb interaktywny wymaga modułu curses. "
                      "Użyj --offset SEK lub --anchor R,T.")
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        raise AqError("Brak terminala interaktywnego. Użyj --offset SEK lub --anchor R,T.")
    box = {"result": None}

    def run(stdscr):
        box["result"] = _sync_loop(stdscr, ref_cues, tgt_cues)

    curses.wrapper(run)
    return box["result"]


def _parse_user_time(s: str) -> Optional[int]:
    """Zamień tekst użytkownika na ms. Akceptuje 'HH:MM:SS,mmm', 'MM:SS,mmm'
    oraz same sekundy ('12.5'). None gdy nie da się sparsować."""
    s = s.strip().replace(",", ".")
    if re.fullmatch(r"-?\d+(\.\d+)?", s):
        return int(round(float(s) * 1000))
    m = re.fullmatch(r"(?:(\d+):)?(\d{1,2}):(\d{2})(?:\.(\d{1,3}))?", s)
    if m:
        h = int(m.group(1) or 0)
        mm = int(m.group(2))
        ss = int(m.group(3))
        ms = int((m.group(4) or "0").ljust(3, "0")[:3])
        return (h * 3600 + mm * 60 + ss) * 1000 + ms
    return None


def _sync_prompt(stdscr, label: str) -> str:
    import curses
    h, w = stdscr.getmaxyx()
    curses.echo()
    curses.curs_set(1)
    try:
        stdscr.move(h - 1, 0)
        stdscr.clrtoeol()
        stdscr.addstr(h - 1, 0, label[:w - 1])
        stdscr.refresh()
        raw = stdscr.getstr(h - 1, min(len(label), w - 2), 24)
        s = raw.decode("utf-8", "replace") if raw else ""
    except curses.error:
        s = ""
    finally:
        curses.noecho()
        curses.curs_set(0)
    return s


def _shift_cue(cue: Cue, delta_ms: int) -> None:
    cue.start_ms = max(0, cue.start_ms + delta_ms)
    cue.end_ms = max(cue.start_ms, cue.end_ms + delta_ms)


def _sync_loop(stdscr, ref_cues: List[Cue], tgt_cues: List[Cue]):
    import curses
    curses.curs_set(0)
    # robocza kopia napisów do synchronizacji (edycja ręczna jej dotyczy)
    work = [Cue(c.start_ms, c.end_ms, list(c.lines)) for c in tgt_cues]
    cols = [ref_cues, work]
    titles = ["WZÓR (referencja)", "DO SYNCHRONIZACJI"]
    cursor = [0, 0]
    top = [0, 0]
    sel = [None, None]        # zaznaczona linia w każdej kolumnie
    pairs: List[Tuple[int, int]] = []   # (ref_idx, tgt_idx)
    active = 0                # aktywna kolumna (0=wzór, 1=cel)
    msg = ""

    def build_pairs():
        return [(work[r].start_ms, ref_cues[l].start_ms) for (l, r) in pairs]

    while True:
        h, w = stdscr.getmaxyx()
        colw = max(10, (w - 3) // 2)
        body_h = max(1, h - 6)    # zostaw miejsce na panel podglądu + status + help
        stdscr.erase()
        for c in (0, 1):
            x = 0 if c == 0 else colw + 3
            attr = curses.A_BOLD | (curses.A_REVERSE if active == c else 0)
            try:
                stdscr.addstr(0, x, titles[c][:colw], attr)
            except curses.error:
                pass
        for c in (0, 1):
            if cursor[c] < top[c]:
                top[c] = cursor[c]
            if cursor[c] >= top[c] + body_h:
                top[c] = cursor[c] - body_h + 1
        paired = [{p[0] for p in pairs}, {p[1] for p in pairs}]
        pair_no = [{}, {}]
        for n, (li, ri) in enumerate(pairs, 1):
            pair_no[0][li] = n
            pair_no[1][ri] = n
        for c in (0, 1):
            x = 0 if c == 0 else colw + 3
            for row in range(body_h):
                i = top[c] + row
                if i >= len(cols[c]):
                    break
                cue = cols[c][i]
                if i in pair_no[c]:
                    mark = str(pair_no[c][i] % 10)
                elif sel[c] == i:
                    mark = ">"
                else:
                    mark = " "
                text = " ".join(cue.lines)
                line = f"{mark}{i + 1:>4} {_ms_to_srt(cue.start_ms)[:-4]} {text}"
                a = 0
                if active == c and cursor[c] == i:
                    a |= curses.A_REVERSE
                elif i in paired[c]:
                    a |= curses.A_BOLD
                try:
                    stdscr.addstr(1 + row, x, line[:colw], a)
                except curses.error:
                    pass

        # --- dolny panel: pełna treść aktualnych linii z obu kolumn ---
        def line_full(c):
            idx = sel[c] if sel[c] is not None else cursor[c]
            if not (0 <= idx < len(cols[c])):
                return "-"
            cue = cols[c][idx]
            return (f"[{idx + 1}] {_ms_to_srt(cue.start_ms)}→{_ms_to_srt(cue.end_ms)}  "
                    f"{' | '.join(cue.lines)}")
        try:
            stdscr.addstr(h - 4, 0, ("WZÓR: " + line_full(0))[:w - 1], curses.A_DIM)
            stdscr.addstr(h - 3, 0, ("CEL : " + line_full(1))[:w - 1], curses.A_DIM)
        except curses.error:
            pass

        scale, offset = compute_sync_transform(build_pairs())
        status = (f" par: {len(pairs)}   scale={scale:.4f}   "
                  f"offset={offset / 1000:+.3f}s   {msg}")
        help1 = ("TAB kol | ↑↓ ruch | ENTER zaznacz→łączy | u cofnij | "
                 "cel: ,/. ±0,1s  </> ±1s  e wpisz czas | a zapis | q wyjście")
        try:
            stdscr.addstr(h - 2, 0, status[:w - 1], curses.A_REVERSE)
            stdscr.addstr(h - 1, 0, help1[:w - 1])
        except curses.error:
            pass
        stdscr.refresh()
        msg = ""

        k = stdscr.getch()
        if k in (ord('q'), 27):
            return None
        elif k == ord('\t'):
            active ^= 1
        elif k in (curses.KEY_DOWN, ord('j')):
            cursor[active] = min(len(cols[active]) - 1, cursor[active] + 1)
        elif k in (curses.KEY_UP, ord('k')):
            cursor[active] = max(0, cursor[active] - 1)
        elif k == curses.KEY_NPAGE:
            cursor[active] = min(len(cols[active]) - 1, cursor[active] + body_h)
        elif k == curses.KEY_PPAGE:
            cursor[active] = max(0, cursor[active] - body_h)
        elif k == curses.KEY_HOME:
            cursor[active] = 0
        elif k == curses.KEY_END:
            cursor[active] = len(cols[active]) - 1
        elif k in (curses.KEY_ENTER, 10, 13, ord(' ')):
            sel[active] = cursor[active]
            if sel[0] is not None and sel[1] is not None:
                pairs.append((sel[0], sel[1]))
                sel = [None, None]
        elif k == ord('u'):
            if pairs:
                pairs.pop()
        # --- edycja czasów: tylko kolumna CEL ---
        elif k in (ord(','), ord('.'), ord('<'), ord('>'), ord('e')):
            if active != 1:
                msg = "edycja czasu tylko w kolumnie CEL (TAB)"
            else:
                cue = work[cursor[1]]
                if k == ord(','):
                    _shift_cue(cue, -100)
                elif k == ord('.'):
                    _shift_cue(cue, 100)
                elif k == ord('<'):
                    _shift_cue(cue, -1000)
                elif k == ord('>'):
                    _shift_cue(cue, 1000)
                else:  # 'e' — wpisz dokładny czas startu
                    s = _sync_prompt(stdscr, "Nowy czas startu (hh:mm:ss,mmm | sek): ")
                    val = _parse_user_time(s)
                    if val is None:
                        msg = "nie rozpoznano czasu"
                    else:
                        _shift_cue(cue, val - cue.start_ms)
        elif k == ord('a'):
            return build_pairs(), work


def cmd_get(args, cfg):
    """Pobierz po haszu pliku (jednego lub wielu), próbując wybranych serwisów."""
    files = _as_list(args.file)
    if _reject_output_with_many(files, args.output):
        return 2
    services = _parse_services(args.service, default="np,n24,os")
    rc = 0
    for movie in files:
        out = _default_out(movie, args.output)
        if _skip_existing(out, args.force):
            continue
        if len(files) > 1:
            print(f"== {movie} ==")
        r = _get_one(movie, out, services, args, cfg)
        rc = rc or r
    return rc


def _get_one(movie, out, services, args, cfg) -> int:
    fps = _resolve_fps(movie, None, args.fps)
    lang = args.lang or "pl"

    errors = []
    # napiprojekt (po MD5-10MiB)
    if "np" in services:
        try:
            raw = NapiprojektClient(args.timeout).download(md5_10mb(movie), lang)
            _save_subtitles(raw, out, fps, _sanitize_kw(args))
            print("Źródło: napiprojekt")
            return 0
        except AqError as e:
            errors.append(f"napiprojekt: {e}")
    # napisy24 (po OSH, konto agenta)
    if "n24" in services:
        try:
            res = Napisy24Client(args.timeout).checksub_agent(movie, lang.upper())
            if res.get("count", 0) > 0 and res.get("zip"):
                _save_subtitles(res["zip"], out, fps, _sanitize_kw(args))
                print("Źródło: napisy24")
                return 0
            errors.append("napisy24: brak trafień")
        except AqError as e:
            errors.append(f"napisy24: {e}")
    # OpenSubtitles (po moviehash)
    if "os" in services:
        try:
            api_key = cfg.os_api_key(args.os_api_key)
            client = OpenSubtitlesClient(api_key, args.timeout)
            client.login(cfg.os_username(args.os_user), cfg.os_password(args.os_pass))
            hits = client.search(moviehash=oshash(movie), languages=lang)
            if hits:
                best = max(hits, key=lambda h: h.downloads)
                raw, meta = client.download(best.file_id)
                _save_subtitles(raw, out, _resolve_fps(movie, best.fps, args.fps),
                                _sanitize_kw(args))
                print(f"Źródło: opensubtitles (pozostały limit: {meta.get('remaining')})")
                return 0
            errors.append("opensubtitles: brak trafień")
        except AqError as e:
            errors.append(f"opensubtitles: {e}")

    print("Nie znaleziono napisów. Szczegóły:")
    for e in errors:
        print("  - " + e)
    return 1


def cmd_search(args, cfg):
    services = _parse_services(args.service, default="np,n24,os")
    lang = args.lang or ""
    all_hits: List[SubtitleHit] = []

    if "n24" in services and (args.imdb or args.title):
        try:
            all_hits += Napisy24Client(args.timeout).search(
                imdb=args.imdb or "", title=args.title or "")
        except AqError as e:
            log.warning("napisy24 search: %s", e)
    if "np" in services and (args.title or args.imdb):
        try:
            for m in NapiprojektClient(args.timeout).search_movies(args.title or args.query or ""):
                all_hits.append(SubtitleHit("napiprojekt", m["movie_id"],
                                            title=m["title_orig"] or m["title_pl"],
                                            year=m["year"],
                                            extra={"imdb": m["imdb_id"], "movie_id": m["movie_id"]}))
        except AqError as e:
            log.warning("napiprojekt search: %s", e)
    if "os" in services:
        try:
            api_key = cfg.os_api_key(args.os_api_key)
            client = OpenSubtitlesClient(api_key, args.timeout)
            all_hits += client.search(query=args.query or args.title or "",
                                      imdb_id=args.imdb or "", languages=lang,
                                      season=args.season or "", episode=args.episode or "")
        except AqError as e:
            log.warning("opensubtitles search: %s", e)

    all_hits = _filter_by_lang(all_hits, lang)
    if not all_hits:
        print("Brak wyników.")
        return 1

    if not (getattr(args, "pick", False) or getattr(args, "auto", False)):
        _print_hits(all_hits)
        return 0

    # tryb pobierania: wybór (interaktywny lub auto) + auto-nazwa
    downloadable = [h for h in all_hits
                    if h.service in ("opensubtitles", "napisy24") and (h.file_id or h.sub_id)]
    if not downloadable:
        print("Brak bezpośrednio pobieralnych wyników "
              "(napiprojekt: użyj 'aqnapi napiprojekt download PLIK').")
        return 1
    if args.movie:
        downloadable.sort(key=lambda h: _score_release(h, args.movie), reverse=True)
    else:
        downloadable.sort(key=lambda h: h.downloads, reverse=True)

    if getattr(args, "auto", False):
        chosen = downloadable[0]
        print(f"Auto-wybór: {chosen.service} [{chosen.release or chosen.title}]")
    else:
        idx = _pick_hit(downloadable)
        if idx is None:
            print("Anulowano.")
            return 1
        chosen = downloadable[idx]

    raw = _download_hit(chosen, args, cfg)
    if args.movie or args.output:
        out = _default_out(args.movie, args.output)
    else:
        out = _safe_filename(chosen.release or chosen.title or "napisy") + ".srt"
    if _skip_existing(out, getattr(args, "force", False)):
        return 0
    _save_subtitles(raw, out, _resolve_fps(args.movie, chosen.fps, args.fps),
                    _sanitize_kw(args))
    print(f"Pobrano z: {chosen.service} (ID {chosen.file_id or chosen.sub_id})")
    return 0


def _release_tokens(s: str) -> set:
    return set(re.findall(r"[a-z0-9]+", (s or "").lower()))


def _score_release(hit: SubtitleHit, movie_path: str) -> float:
    """Dopasowanie wyniku do nazwy pliku filmowego (im więcej wspólnych tokenów
    release, tym lepiej); remis rozstrzyga liczba pobrań."""
    stem = os.path.splitext(os.path.basename(movie_path))[0]
    ft = _release_tokens(stem)
    ht = _release_tokens((hit.release or "") + " " + (hit.title or ""))
    inter = len(ft & ht) if (ft and ht) else 0
    return inter + hit.downloads / 1e9


def _safe_filename(name: str) -> str:
    name = re.sub(r"[^\w .\-]+", "_", name, flags=re.UNICODE).strip("._ ")
    return (name or "napisy")[:120]


def _download_hit(hit: SubtitleHit, args, cfg) -> bytes:
    if hit.service == "opensubtitles":
        client = _os_client(args, cfg)
        client.login(cfg.os_username(args.os_user), cfg.os_password(args.os_pass))
        raw, _ = client.download(hit.file_id)
        return raw
    if hit.service == "napisy24":
        return Napisy24Client(args.timeout).download_by_id(hit.sub_id)
    raise AqError("Ten wynik nie jest bezpośrednio pobieralny.")


def _pick_hit(hits: List[SubtitleHit]) -> Optional[int]:
    """Interaktywny wybór jednego wyniku z listy (curses). None = anulowano."""
    try:
        import curses
    except ImportError:
        raise AqError("Wybór interaktywny wymaga modułu curses. Użyj --auto.")
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        raise AqError("Brak terminala interaktywnego. Użyj --auto (auto-wybór).")
    box = {"idx": None}

    def run(stdscr):
        box["idx"] = _pick_loop(stdscr, hits)

    curses.wrapper(run)
    return box["idx"]


def _pick_loop(stdscr, hits: List[SubtitleHit]) -> Optional[int]:
    import curses
    curses.curs_set(0)
    cursor = 0
    top = 0
    while True:
        h, w = stdscr.getmaxyx()
        body_h = max(1, h - 4)
        stdscr.erase()
        try:
            stdscr.addstr(0, 0, "Wybierz napisy do pobrania"[:w - 1], curses.A_BOLD)
        except curses.error:
            pass
        if cursor < top:
            top = cursor
        if cursor >= top + body_h:
            top = cursor - body_h + 1
        for row in range(body_h):
            i = top + row
            if i >= len(hits):
                break
            hit = hits[i]
            title = hit.title + (f" ({hit.year})" if hit.year else "")
            line = (f"{hit.service[:4]:<4} {hit.language:<3} "
                    f"pob:{hit.downloads:>6}  {title}  [{hit.release}]")
            attr = curses.A_REVERSE if i == cursor else 0
            try:
                stdscr.addstr(1 + row, 0, line[:w - 1], attr)
            except curses.error:
                pass
        cur = hits[cursor]
        detail = (f"{cur.service} ID={cur.file_id or cur.sub_id} "
                  f"lang={cur.language} pobrania={cur.downloads} "
                  f"release={cur.release}")
        help1 = "↑↓/jk ruch | PgUp/PgDn | ENTER pobierz | q anuluj"
        try:
            stdscr.addstr(h - 2, 0, detail[:w - 1], curses.A_DIM)
            stdscr.addstr(h - 1, 0, help1[:w - 1], curses.A_REVERSE)
        except curses.error:
            pass
        stdscr.refresh()
        k = stdscr.getch()
        if k in (ord('q'), 27):
            return None
        elif k in (curses.KEY_DOWN, ord('j')):
            cursor = min(len(hits) - 1, cursor + 1)
        elif k in (curses.KEY_UP, ord('k')):
            cursor = max(0, cursor - 1)
        elif k == curses.KEY_NPAGE:
            cursor = min(len(hits) - 1, cursor + body_h)
        elif k == curses.KEY_PPAGE:
            cursor = max(0, cursor - body_h)
        elif k == curses.KEY_HOME:
            cursor = 0
        elif k == curses.KEY_END:
            cursor = len(hits) - 1
        elif k in (curses.KEY_ENTER, 10, 13):
            return cursor


def _filter_by_lang(hits: List[SubtitleHit], lang: str) -> List[SubtitleHit]:
    """Zawęź wyniki do podanych języków. Pozycje bez znanego języka (np. katalog
    napiprojekt) są zachowywane."""
    if not lang:
        return hits
    wanted = {x.strip().lower() for x in lang.split(",") if x.strip()}
    return [h for h in hits if not h.language or h.language.lower() in wanted]


def _print_hits(hits: List[SubtitleHit]):
    print(f"{'SERWIS':<13} {'ID':<12} {'JĘZYK':<6} {'POB.':>6}  TYTUŁ / RELEASE")
    print("-" * 78)
    for h in hits:
        title = h.title
        if h.year:
            title += f" ({h.year})"
        if h.release:
            title += f"  [{h.release[:32]}]"
        dl_id = h.file_id or h.sub_id
        print(f"{h.service:<13} {dl_id:<12} {h.language:<6} {h.downloads:>6}  {title[:60]}")
    print("\nPobierz: aqnapi <serwis> download/getid/download-id <ID>")


def cmd_upload(args, cfg):
    services = _parse_services(args.service, default="np")
    results = []
    with open(args.srt, "rb") as f:
        srt_raw = f.read()
    srt_text = decode_text(srt_raw)

    if "np" in services:
        try:
            r = _np_upload(args, cfg, srt_text)
            results.append(r)
        except AqError as e:
            results.append(UploadResult("napiprojekt", False, str(e)))
    if "n24" in services:
        try:
            r = _n24_upload(args, cfg, srt_text)
            results.append(r)
        except AqError as e:
            results.append(UploadResult("napisy24", False, str(e)))
    if "os" in services:
        results.append(UploadResult(
            "opensubtitles", False,
            "Upload do OpenSubtitles nie jest dostępny w REST API (patrz docs/opensubtitles.md)"))

    rc = 0
    for r in results:
        status = "OK" if r.ok else "BŁĄD"
        print(f"[{status}] {r.service}: {r.message}")
        if not r.ok:
            rc = 1
    return rc


def _np_upload(args, cfg, srt_text: str) -> UploadResult:
    if not args.movie:
        raise AqError("napiprojekt upload wymaga --movie (hash pliku filmowego)")
    movie_hash = md5_10mb(args.movie)
    subtitle_bytes = srt_text.encode("utf-8")
    client = NapiprojektClient(args.timeout)
    return client.upload(movie_hash, subtitle_bytes,
                         lang=(args.lang or "PL").upper(),
                         author=args.translator or "",
                         corrected=args.corrected, comment=args.comment or "",
                         only_testing=args.dry_run)


def _n24_upload(args, cfg, srt_text: str) -> UploadResult:
    problems = check_srt_for_napisy24(srt_text)
    if problems:
        raise AqError("Plik nie przejdzie walidacji Napisy24:\n  " + "\n  ".join(problems))
    if args.dry_run:
        return UploadResult("napisy24", True, "dry-run: plik poprawny (nie wysłano)")
    login = cfg.n24_login(args.n24_user)
    password = cfg.n24_pass(args.n24_pass)
    if not login or not password:
        raise AuthError("napisy24 upload wymaga loginu i hasła")
    filebytes = normalize_for_napisy24(srt_text, fix_timing=args.fix_timing)
    meta = {
        "imdb": _norm_imdb(args.imdb) if args.imdb else "",
        "title": args.title or "", "year": args.year or "",
        "release": args.release or "", "translator": args.translator or "",
        "resolution": args.resolution or "", "duration": args.duration or "",
        "size": args.size or (os.path.getsize(args.movie) if args.movie else ""),
        "fps": str(args.fps or "23.976"),
        "season": args.season or "", "episode": args.episode or "",
        "episode_title": args.episode_title or "",
    }
    client = Napisy24Client(args.timeout)
    opener = client._web_opener()
    if not client.web_login(opener, login, password):
        raise AuthError("napisy24: logowanie nieudane")
    filename = os.path.basename(args.srt)
    return client.web_upload(opener, filename, filebytes, meta)


def _parse_services(value: Optional[str], default: str) -> List[str]:
    raw = (value or default).lower()
    alias = {"napisy24": "n24", "n24": "n24", "napiprojekt": "np", "np": "np",
             "opensubtitles": "os", "os": "os"}
    out = []
    for tok in re.split(r"[,\s]+", raw):
        if tok in alias and alias[tok] not in out:
            out.append(alias[tok])
    return out or ["np", "n24", "os"]


# ---- podpolecenia per-serwis: napisy24 ----

def cmd_n24(args, cfg):
    client = Napisy24Client(args.timeout)
    if args.n24_cmd == "hash":
        return cmd_hash(args, cfg)
    if args.n24_cmd == "login":
        ok, msg = client.login(cfg.n24_login(args.n24_user), cfg.n24_pass(args.n24_pass))
        print(msg)
        return 0 if ok else 1
    if args.n24_cmd == "download":
        files = _as_list(args.file)
        if _reject_output_with_many(files, args.output):
            return 2
        rc = 0
        for movie in files:
            out = _default_out(movie, args.output)
            if _skip_existing(out, args.force):
                continue
            res = client.checksub_agent(movie, (args.lang or "pl").upper())
            if res.get("count", 0) > 0 and res.get("zip"):
                _save_subtitles(res["zip"], out, _resolve_fps(movie, None, args.fps),
                                _sanitize_kw(args))
            else:
                print(f"Brak napisów dla: {movie}")
                rc = 1
        return rc
    if args.n24_cmd == "search":
        hits = client.search(imdb=args.imdb or "", title=args.title or "")
        if not hits:
            print("Brak wyników.")
            return 1
        _print_hits(hits)
        return 0
    if args.n24_cmd == "getid":
        out = _default_out(args.movie, args.output)
        if _skip_existing(out, args.force):
            return 0
        raw = client.download_by_id(args.id)
        _save_subtitles(raw, out, _resolve_fps(args.movie, None, args.fps),
                        _sanitize_kw(args))
        return 0
    if args.n24_cmd == "imdb":
        print(client.imdb_info(args.imdb)["raw"])
        return 0
    if args.n24_cmd == "upload":
        with open(args.srt, "rb") as f:
            r = _n24_upload(args, cfg, decode_text(f.read()))
        print(f"[{'OK' if r.ok else 'BŁĄD'}] {r.message}")
        return 0 if r.ok else 1
    if args.n24_cmd == "delete":
        login, password = cfg.n24_login(args.n24_user), cfg.n24_pass(args.n24_pass)
        opener = client._web_opener()
        if not client.web_login(opener, login, password):
            raise AuthError("napisy24: logowanie nieudane")
        r = client.web_delete(opener, args.id)
        print(f"[{'OK' if r.ok else 'BŁĄD'}] {r.message}")
        return 0 if r.ok else 1
    return 2


# ---- podpolecenia per-serwis: napiprojekt ----

def cmd_np(args, cfg):
    client = NapiprojektClient(args.timeout)
    if args.np_cmd == "download":
        files = _as_list(args.file)
        if _reject_output_with_many(files, args.output):
            return 2
        rc = 0
        for movie in files:
            out = _default_out(movie, args.output)
            if _skip_existing(out, args.force):
                continue
            h = md5_10mb(movie)
            try:
                raw = client.download(h, (args.lang or "PL").upper())
            except NotFoundError:
                print(f"Brak napisów dla: {movie}")
                rc = 1
                continue
            fps = _resolve_fps(movie, client.file_info_fps(h), args.fps)
            _save_subtitles(raw, out, fps, _sanitize_kw(args))
        return rc
    if args.np_cmd == "search":
        movies = client.search_movies(args.title or args.query)
        if not movies:
            print("Brak wyników.")
            return 1
        for m in movies:
            print(f"MovieId: {m['movie_id']}")
            print(f"  {m['title_orig']} ({m['year']})  PL: {m['title_pl']}")
            print(f"  IMDB: {m['imdb_id']}   {m['imdb']}")
        return 0
    if args.np_cmd == "associate":
        r = client.associate(cfg.np_user(args.np_user), cfg.np_pass(args.np_pass),
                             md5_10mb(args.movie), args.movie_id)
        print(f"[{'OK' if r.ok else 'BŁĄD'}] {r.message}")
        return 0 if r.ok else 1
    if args.np_cmd == "account":
        info = client.account(cfg.np_user(args.np_user), cfg.np_pass(args.np_pass))
        for k, v in info.items():
            print(f"{k}: {v}")
        return 0
    if args.np_cmd == "fileinfo":
        fps = client.file_info_fps(md5_10mb(args.file))
        print(f"FPS (serwer): {fps}" if fps else "Brak danych FPS")
        return 0
    if args.np_cmd == "upload":
        with open(args.srt, "rb") as f:
            r = _np_upload(args, cfg, decode_text(f.read()))
        print(f"[{'OK' if r.ok else 'BŁĄD'}] {r.message}")
        return 0 if r.ok else 1
    return 2


# ---- podpolecenia per-serwis: opensubtitles ----

def _os_client(args, cfg) -> OpenSubtitlesClient:
    return OpenSubtitlesClient(cfg.os_api_key(args.os_api_key), args.timeout)


def cmd_os(args, cfg):
    if args.os_cmd == "login":
        client = _os_client(args, cfg)
        data = client.login(cfg.os_username(args.os_user), cfg.os_password(args.os_pass))
        u = data.get("user", {})
        print(f"Zalogowano. base_url={client.base} "
              f"limit pobrań={u.get('allowed_downloads')} VIP={u.get('vip')}")
        return 0
    if args.os_cmd == "logout":
        client = _os_client(args, cfg)
        cached = load_cached_os_token()
        if cached:
            client.token = cached["token"]
            client.base = cached.get("base", client.base)
        print("Wylogowano" if client.logout() else "Nie udało się wylogować")
        return 0
    if args.os_cmd == "search":
        client = _os_client(args, cfg)
        hits = client.search(query=args.query or args.title or "",
                             imdb_id=args.imdb or "", moviehash=args.moviehash or "",
                             languages=args.lang or "", season=args.season or "",
                             episode=args.episode or "")
        if not hits:
            print("Brak wyników.")
            return 1
        _print_hits(hits)
        return 0
    if args.os_cmd == "download":
        out = _default_out(args.movie, args.output)
        if _skip_existing(out, args.force):
            return 0
        client = _os_client(args, cfg)
        client.login(cfg.os_username(args.os_user), cfg.os_password(args.os_pass))
        raw, meta = client.download(args.file_id)
        fps = _resolve_fps(args.movie, None, args.fps)
        _save_subtitles(raw, out, fps, _sanitize_kw(args))
        print(f"Pozostały limit pobrań: {meta.get('remaining')} "
              f"(reset: {meta.get('reset_time')})")
        return 0
    if args.os_cmd == "formats":
        print(", ".join(_os_client(args, cfg).formats()))
        return 0
    if args.os_cmd == "languages":
        for l in _os_client(args, cfg).languages():
            print(f"{l.get('language_code')}: {l.get('language_name')}")
        return 0
    if args.os_cmd == "guessit":
        print(json.dumps(_os_client(args, cfg).guessit(args.filename),
                         indent=2, ensure_ascii=False))
        return 0
    return 2


# ===========================================================================
# Parser argumentów
# ===========================================================================

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="aqnapi",
        description="Zunifikowany klient napisów: Napisy24 / napiprojekt / OpenSubtitles.")
    p.add_argument("--version", action="version", version=f"aqnapi {__version__}")
    p.add_argument("-v", "--verbose", action="store_true", help="log żądań (bez haseł)")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="limit czasu (s)")
    p.add_argument("--config", help="ścieżka do pliku konfiguracyjnego")
    sub = p.add_subparsers(dest="command", required=True)

    def add_creds(sp):
        sp.add_argument("--n24-user"); sp.add_argument("--n24-pass")
        sp.add_argument("--np-user"); sp.add_argument("--np-pass")
        sp.add_argument("--os-api-key"); sp.add_argument("--os-user"); sp.add_argument("--os-pass")

    # ---- agregujące ----
    g = sub.add_parser("get", help="pobierz napisy dla pliku/plików (po haszu, wiele serwisów)")
    g.add_argument("file", nargs="+", help="jeden lub więcej plików filmowych")
    g.add_argument("-l", "--lang", default="pl")
    g.add_argument("-o", "--output", help="tylko dla jednego pliku")
    g.add_argument("--service")
    g.add_argument("--fps", type=float)
    g.add_argument("--force", action="store_true", help="nadpisz istniejące napisy")
    add_sanitize_flags(g)
    add_creds(g)
    g.set_defaults(func=cmd_get)

    s = sub.add_parser("search", help="szukaj napisów (wiele serwisów)")
    s.add_argument("--imdb"); s.add_argument("--title"); s.add_argument("--query")
    s.add_argument("-l", "--lang", default="pl"); s.add_argument("--service")
    s.add_argument("--season"); s.add_argument("--episode")
    s.add_argument("--pick", action="store_true",
                   help="wybierz wynik interaktywnie i pobierz")
    s.add_argument("--auto", action="store_true",
                   help="pobierz najlepszy wynik bez pytania (ranking wg --movie/pobrań)")
    s.add_argument("--movie", help="plik filmowy: auto-nazwa i ranking release pod plik")
    s.add_argument("-o", "--output"); s.add_argument("--fps", type=float)
    s.add_argument("--force", action="store_true")
    add_sanitize_flags(s); add_creds(s)
    s.set_defaults(func=cmd_search)

    u = sub.add_parser("upload", help="wyślij napisy (napiprojekt/napisy24)")
    u.add_argument("--srt", required=True); u.add_argument("--movie")
    u.add_argument("--service", default="np"); u.add_argument("-l", "--lang", default="PL")
    u.add_argument("--imdb"); u.add_argument("--title"); u.add_argument("--title-pl", dest="title_pl")
    u.add_argument("--year"); u.add_argument("--release"); u.add_argument("--translator")
    u.add_argument("--sync"); u.add_argument("--proof"); u.add_argument("--resolution")
    u.add_argument("--duration"); u.add_argument("--size", type=int); u.add_argument("--fps", type=float)
    u.add_argument("--season"); u.add_argument("--episode"); u.add_argument("--episode-title", dest="episode_title")
    u.add_argument("--corrected", action="store_true"); u.add_argument("--comment")
    u.add_argument("--fix-timing", action="store_true", dest="fix_timing")
    u.add_argument("--dry-run", action="store_true", dest="dry_run"); add_creds(u)
    u.set_defaults(func=cmd_upload)

    h = sub.add_parser("hash", help="pokaż hasze pliku (OSH + MD5-10MiB)")
    h.add_argument("file"); h.set_defaults(func=cmd_hash)

    up = sub.add_parser("update", help="zaktualizuj program do najnowszego wydania z GitHub")
    up.add_argument("--check", action="store_true", help="tylko sprawdź, nie pobieraj")
    up.set_defaults(func=cmd_update)

    fp = sub.add_parser("fps", help="odczytaj FPS z pliku filmowego")
    fp.add_argument("file"); fp.set_defaults(func=cmd_fps)

    cv = sub.add_parser("convert", help="konwertuj napisy do SRT UTF-8+BOM/LF")
    cv.add_argument("input"); cv.add_argument("-o", "--output")
    cv.add_argument("--movie", help="plik filmowy do odczytu FPS (MicroDVD)")
    cv.add_argument("--fps", type=float)
    cv.add_argument("--format", choices=["srt", "vtt", "ass", "microdvd"],
                    help="format wyjściowy (domyślnie z rozszerzenia -o, inaczej srt)")
    add_sanitize_flags(cv)
    cv.set_defaults(func=cmd_convert)

    fc = sub.add_parser("fpsconv", help="przelicz czasy napisów przy zmianie FPS")
    fc.add_argument("input")
    fc.add_argument("--from", dest="from_fps", type=float, help="FPS źródłowy napisów")
    fc.add_argument("--to", dest="to_fps", type=float,
                    help="FPS docelowy (albo z --movie)")
    fc.add_argument("--movie", help="odczytaj docelowy FPS z pliku filmowego")
    fc.add_argument("-o", "--output")
    fc.add_argument("--format", choices=["srt", "vtt", "ass", "microdvd"])
    fc.set_defaults(func=cmd_fpsconv)

    mg = sub.add_parser("merge", help="połącz pliki napisów (np. CD1+CD2)")
    mg.add_argument("files", nargs="+", help="pliki w kolejności łączenia")
    mg.add_argument("-o", "--output")
    mg.add_argument("--offset", type=float, action="append", metavar="SEK",
                    help="przesunięcie kolejnego pliku w sek (powtarzalne); "
                         "domyślnie auto = koniec poprzedniego")
    mg.add_argument("--fps", type=float)
    mg.add_argument("--format", choices=["srt", "vtt", "ass", "microdvd"])
    mg.set_defaults(func=cmd_merge)

    sp2 = sub.add_parser("split", help="podziel plik napisów po czasie")
    sp2.add_argument("input")
    sp2.add_argument("--at", action="append", required=True, metavar="CZAS",
                     help="punkt podziału (hh:mm:ss,mmm lub sek); powtarzalny")
    sp2.add_argument("-o", "--output", help="baza nazwy (domyślnie <input>)")
    sp2.add_argument("--no-rebase", action="store_false", dest="rebase",
                     help="nie zeruj czasów części po podziale")
    sp2.add_argument("--fps", type=float)
    sp2.add_argument("--format", choices=["srt", "vtt", "ass", "microdvd"])
    sp2.set_defaults(func=cmd_split, rebase=True)

    cfgp = sub.add_parser("config", help="konfiguracja: dane logowania i klucz API")
    cfgs = cfgp.add_subparsers(dest="config_cmd", required=True)
    cfgs.add_parser("init", help="interaktywne ustawienie poświadczeń")
    cfgs.add_parser("show", help="pokaż konfigurację (hasła zamaskowane)")
    cfgs.add_parser("path", help="wypisz ścieżkę pliku konfiguracyjnego")
    cfgp.set_defaults(func=cmd_config)

    sy = sub.add_parser("sync", help="synchronizuj napisy wg wzorca (interaktywnie w 2 kolumnach)")
    sy.add_argument("reference", help="plik-wzór (poprawne czasy)")
    sy.add_argument("target", help="plik do synchronizacji")
    sy.add_argument("-o", "--output", help="wyjście (domyślnie <target>.synced.srt)")
    sy.add_argument("--offset", type=float,
                    help="proste przesunięcie w sekundach (bez UI); np. -2.5")
    sy.add_argument("--anchor", action="append", metavar="R,T",
                    help="para kotwic: nr linii wzorca,nr linii celu (1-based); "
                         "powtarzalne, bez UI")
    sy.add_argument("--fps", type=float, help="FPS dla MicroDVD przy wczytaniu")
    sy.set_defaults(func=cmd_sync)

    # ---- per-serwis: napisy24 ----
    n = sub.add_parser("napisy24", aliases=["n24"], help="operacje Napisy24.pl")
    ns = n.add_subparsers(dest="n24_cmd", required=True)
    for name in ("hash",):
        x = ns.add_parser(name); x.add_argument("file")
    xl = ns.add_parser("login"); add_creds(xl)
    xd = ns.add_parser("download"); xd.add_argument("file", nargs="+")
    xd.add_argument("-l", "--lang", default="pl")
    xd.add_argument("-o", "--output"); xd.add_argument("--fps", type=float)
    xd.add_argument("--force", action="store_true"); add_sanitize_flags(xd)
    xs = ns.add_parser("search"); xs.add_argument("--imdb"); xs.add_argument("--title")
    xg = ns.add_parser("getid"); xg.add_argument("id"); xg.add_argument("-o", "--output")
    xg.add_argument("--movie"); xg.add_argument("--fps", type=float)
    xg.add_argument("--force", action="store_true"); add_sanitize_flags(xg)
    xi = ns.add_parser("imdb"); xi.add_argument("imdb")
    xu = ns.add_parser("upload")
    xu.add_argument("--srt", required=True); xu.add_argument("--movie")
    xu.add_argument("--imdb"); xu.add_argument("--title"); xu.add_argument("--title-pl", dest="title_pl")
    xu.add_argument("--year"); xu.add_argument("--release"); xu.add_argument("--translator")
    xu.add_argument("--sync"); xu.add_argument("--proof"); xu.add_argument("--resolution")
    xu.add_argument("--duration"); xu.add_argument("--size", type=int); xu.add_argument("--fps", type=float)
    xu.add_argument("--season"); xu.add_argument("--episode"); xu.add_argument("--episode-title", dest="episode_title")
    xu.add_argument("-l", "--lang", default="PL"); xu.add_argument("--corrected", action="store_true")
    xu.add_argument("--comment"); xu.add_argument("--fix-timing", action="store_true", dest="fix_timing")
    xu.add_argument("--dry-run", action="store_true", dest="dry_run"); add_creds(xu)
    xrm = ns.add_parser("delete", aliases=["rm"]); xrm.add_argument("id"); add_creds(xrm)
    n.set_defaults(func=cmd_n24)

    # ---- per-serwis: napiprojekt ----
    npp = sub.add_parser("napiprojekt", aliases=["np"], help="operacje napiprojekt.pl")
    nps = npp.add_subparsers(dest="np_cmd", required=True)
    pd = nps.add_parser("download"); pd.add_argument("file", nargs="+")
    pd.add_argument("-l", "--lang", default="PL")
    pd.add_argument("-o", "--output"); pd.add_argument("--fps", type=float)
    pd.add_argument("--force", action="store_true"); add_sanitize_flags(pd)
    psr = nps.add_parser("search"); psr.add_argument("title", nargs="?", default=""); psr.add_argument("--query", default="")
    pas = nps.add_parser("associate"); pas.add_argument("movie"); pas.add_argument("movie_id"); add_creds(pas)
    pac = nps.add_parser("account"); add_creds(pac)
    pfi = nps.add_parser("fileinfo"); pfi.add_argument("file")
    pu = nps.add_parser("upload")
    pu.add_argument("--srt", required=True); pu.add_argument("--movie", required=True)
    pu.add_argument("-l", "--lang", default="PL"); pu.add_argument("--translator")
    pu.add_argument("--corrected", action="store_true"); pu.add_argument("--comment")
    pu.add_argument("--dry-run", action="store_true", dest="dry_run"); add_creds(pu)
    npp.set_defaults(func=cmd_np)

    # ---- per-serwis: opensubtitles ----
    o = sub.add_parser("opensubtitles", aliases=["os"], help="operacje OpenSubtitles.com")
    oss = o.add_subparsers(dest="os_cmd", required=True)
    ol = oss.add_parser("login"); add_creds(ol)
    olg = oss.add_parser("logout"); add_creds(olg)
    osr = oss.add_parser("search")
    osr.add_argument("--query"); osr.add_argument("--title"); osr.add_argument("--imdb")
    osr.add_argument("--moviehash"); osr.add_argument("-l", "--lang", default="pl")
    osr.add_argument("--season"); osr.add_argument("--episode"); add_creds(osr)
    od = oss.add_parser("download"); od.add_argument("file_id"); od.add_argument("-o", "--output")
    od.add_argument("--movie"); od.add_argument("--fps", type=float)
    od.add_argument("--force", action="store_true"); add_sanitize_flags(od); add_creds(od)
    ofm = oss.add_parser("formats"); add_creds(ofm)
    olng = oss.add_parser("languages"); add_creds(olng)
    ogs = oss.add_parser("guessit"); ogs.add_argument("filename"); add_creds(ogs)
    o.set_defaults(func=cmd_os)

    return p


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if getattr(args, "verbose", False) else logging.WARNING,
        format="%(levelname)s: %(message)s")
    cfg = Config(getattr(args, "config", None))
    # uzupełnij brakujące atrybuty creds (dla poleceń bez add_creds)
    for attr in ("n24_user", "n24_pass", "np_user", "np_pass",
                 "os_api_key", "os_user", "os_pass", "fps", "movie", "output"):
        if not hasattr(args, attr):
            setattr(args, attr, None)
    if not hasattr(args, "force"):
        args.force = False
    try:
        return args.func(args, cfg)
    except AuthError as e:
        print(f"Błąd uwierzytelnienia: {e}", file=sys.stderr)
        return 2
    except NotFoundError as e:
        print(f"Nie znaleziono: {e}", file=sys.stderr)
        return 1
    except (NetworkError, ServerError, ConfigError, AqError) as e:
        print(f"Błąd: {e}", file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print(f"Brak pliku: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nPrzerwano.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
