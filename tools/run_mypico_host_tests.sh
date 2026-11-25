#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 在仓库根目录运行；验证 MyPico 新增模块主机单测与既有主循环/排版测试。
# Run from repo root; verifies MyPico host tests plus existing app_loop/layout/epub tests.
set -euo pipefail

CC_BIN="${CC:-cc}"
if [ -x /Library/Developer/CommandLineTools/usr/bin/clang ]; then
    CC_BIN="/Library/Developer/CommandLineTools/usr/bin/clang"
fi
SDK_FLAGS=()
if [ -d /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk ]; then
    SDK_FLAGS=(-isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)
fi

mkdir -p build/mypico-tests

python3 - <<'PY'
import struct, zlib, zipfile
def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
w, h = 16, 24
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        row.extend([(x * 12 + y * 8) & 0xff, 90, 50])
    rows.append(bytes(row))
png = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + png_chunk(b"IDAT", zlib.compress(b"".join(rows))) + png_chunk(b"IEND", b"")
with zipfile.ZipFile("build/mypico-tests/sample_cover.epub", "w") as z:
    z.writestr("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
    z.writestr("OEBPS/content.opf", '''<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>页边和听写</dc:title>
    <dc:creator>埃琳娜·费兰特</dc:creator>
    <meta name="cover" content="cover-img"/>
  </metadata>
  <manifest>
    <item id="cover-img" href="images/cover.png" media-type="image/png" properties="cover-image"/>
    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="ch1"/></spine>
</package>''')
    z.writestr("OEBPS/images/cover.png", png)
    z.writestr("OEBPS/ch1.xhtml", '<html><body><p>正文内容</p></body></html>')
PY

FLAGS=("${SDK_FLAGS[@]}" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -fsanitize=address,undefined -g)

"$CC_BIN" "${FLAGS[@]}" \
  -Imain/book -Imain/sync -Itools/book_epub_stubs -Itools/zip_host_stubs -Itools/book_layout_stubs \
  tools/mypico_host_test.c \
  main/book/book_layout.c main/book/book_epub.c main/book/zip_reader.c main/book/html_text.c \
  main/book/book_image.c main/book/vendor/tjpgd.c main/sync/mypico_mdns.c \
  -lz -o build/mypico-tests/mypico_test
build/mypico-tests/mypico_test build/mypico-tests/sample_cover.epub

"$CC_BIN" "${FLAGS[@]}" \
  -Itools/app_loop_stubs -Imain/app -Imain/ui -Itools/ui_gesture_stubs \
  tools/app_loop_host_test.c main/app/app_loop.c main/ui/ui_gesture.c \
  -o build/mypico-tests/app_loop
build/mypico-tests/app_loop
