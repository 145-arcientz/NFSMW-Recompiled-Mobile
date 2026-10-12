"""Check that each Android decoder uses its own internal FFmpeg DSP.

Usage: python tools/tests/android_ffmpeg_bindings_test.py path/to/app.apk
No game data or external ELF tools required.
"""
import struct
import sys
import zipfile


def dynamic_symbols(data):
    assert data[:6] == b"\x7fELF\x02\x01", "Expected little-endian ELF64"
    offset = struct.unpack_from("<Q", data, 40)[0]
    stride, count = struct.unpack_from("<HH", data, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * stride)
                for i in range(count)]
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for pos in range(section[4], section[4] + section[5], section[9]):
            name, info, visibility, index, _, _ = struct.unpack_from("<IBBHQQ", data, pos)
            text = names[name:names.find(b"\0", name)].decode()
            yield text, info >> 4, visibility & 3, index


def check(apk):
    problems = []
    with zipfile.ZipFile(apk) as archive:
        for library in ("libmain.so", "librexruntime.so"):
            symbols = dynamic_symbols(archive.read("lib/arm64-v8a/" + library))
            for name, binding, visibility, index in symbols:
                if not name.startswith("ff_") or binding not in (1, 2):
                    continue
                if index == 0:
                    problems.append(f"{library} imports internal DSP {name}")
                elif visibility == 0:
                    problems.append(f"{library} exports internal DSP {name}")
    if problems:
        raise SystemExit("FFmpeg DSP binding regression:\n" + "\n".join(problems))
    print("Android FFmpeg DSP binding test passed")


if __name__ == "__main__":
    check(sys.argv[1])
