#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# M4 item 6: writes the hand-written seed inputs into tools/fuzz/seeds/ (the vttest captures
# there were made with a pty driver, see docs/FUZZING.md). The harness reads its parameters
# (size, ops) from the end of the input and strings from the front, so a plain terminal byte
# stream is already a usable seed.
import os
D = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'seeds')
E = b'\x1b'
S = {
 'wide-edge': '漢字テスト'.encode() * 20 + b'\x1b[1;79H\xe4\xb8\xad\xe6\x96\x87' + b'\x1b[1;1H\x1b[1@X\x1b[2;2HY\x1b[K',
 'combining': 'e\u0301a\u0308\u0323o\u0302\u0303\u0304\u0305\u0306\u0307\u0308\u0309\u030a\u030b\u030c\u030d'.encode() * 30,
 'emoji': '😀👩‍👩‍👧‍👦❤️🇫🇮🇯🇵 ✌🏽 #️⃣'.encode() * 20,
 'rtl': 'שלום עולם مرحبا بالعالم abc'.encode() * 10,
 'bad-utf8': b'\xc3\x28\xa0\xa1\xe2\x28\xa1\xe2\x82\x28\xf0\x28\x8c\xbc\xf0\x90\x28\xbc\xf8\xa1\xa1\xa1\xa1\xfc\xa1\xa1\xa1\xa1\xa1\xed\xa0\x80\xef\xbf\xbf\xf4\x90\x80\x80\xc0\x80' * 10,
 'osc-title': b'\x1b]0;' + b'T' * 5000 + b'\x07\x1b]2;\xe6\xbc\xa2\xe5\xad\x97\x1b\\\x1b]2;\xe6\xbc' + b'\x07',
 'osc-unterminated': b'\x1b]2;' + 'é'.encode() * 3000,
 'dcs': b'\x1bP$qm\x1b\\\x1bP+q544e\x1b\\\x1bPq#0;2;0;0;0#1;2;100;100;0~~~\x1b\\\x1bP' + b'x' * 3000,
 'csi-params': b'\x1b[' + b';'.join([b'99999999999'] * 40) + b'm\x1b[?1049h\x1b[?1049l\x1b[?47h\x1b[?47l\x1b[?1048h\x1b[?1048l\x1b[9999;9999H*\x1b[0;0r\x1b[5;2r\x1b[99S\x1b[99T',
 'mouse-modes': b'\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h\x1b[?1005h\x1b[?1015h\x1b[?9h\x1b[?1004h',
 'scroll-region': b'\x1b[2;5r' + b'line\r\n' * 50 + b'\x1b[r\x1b[3L\x1b[3M\x1b[5P\x1b[5X\x1b[5@',
 'charsets': b'\x1b(0lqqk\x1b(B\x1b)0\x0eabc\x0f\x1b*0\x1bn\x1b+A\x1bo#\x1b7\x1b8\x1bc',
 'decaln-dec': b'\x1b#8\x1b#3DH\x1b#4DH\x1b#6DW\x1b[?3h\x1b[?3l\x1b[?5h\x1b[?6h\x1b[?7l' + b'W' * 300 + b'\x1b[?7h',
 'sgr-rgb': b'\x1b[38;2;1;2;3m\x1b[48:2::4:5:6m\x1b[38;5;300m\x1b[4:3m\x1b[58;2;1;2;3m\x1b[1;2;3;4;5;7;8;9;53mX\x1b[0m',
 'tabs': b'\x1bH\x1b[3g\x1b[0g\t\t\x1b[5I\x1b[5Z\x1b[W\x1b[?5W' + b'\t' * 50,
 'resize-wrap': b'x' * 2000 + '漢'.encode() * 500,
}
os.makedirs(D, exist_ok=True)
for k, v in S.items():
    if isinstance(v, str): v = v.encode()
    open(os.path.join(D, 'hand-' + k), 'wb').write(v)
