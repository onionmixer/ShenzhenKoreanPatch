import argparse
import csv
import hashlib
import struct
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "VERSION").read_text(encoding="ascii").strip()
BUILD = ROOT / "build"
RELEASE = ROOT / "release"
LOCALIZATION = ROOT / "localization"
PDF = Path("Content/SHENZHEN IO Manual (Korean).pdf")
PDF_HASH = "b2dda4ada609f1bc766d2983e3199c9c65167acfcf1c33d6803b4614c29db121"
STOCK_EXE = "a0dfe8e1e91b6633c3ba00210762ec0d7e6786ab9c2c7912d9c9afdd654d98f9"
EXE = "ShenzhenKoreanPatcher.exe"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_delta(old, new):
    block = 32
    index = {old[i:i + block]: i for i in range(0, len(old) - block + 1, block)}
    delta = bytearray(b"SKP1" + struct.pack("<I", len(new)))
    cursor = literal_start = 0

    def literal(end):
        if end > literal_start:
            delta.extend(struct.pack("<II", 0xFFFFFFFF, end - literal_start))
            delta.extend(new[literal_start:end])

    while cursor + block <= len(new):
        offset = index.get(new[cursor:cursor + block])
        if offset is None:
            cursor += 1
            continue
        literal(cursor)
        length = block
        while offset + length < len(old) and cursor + length < len(new) and old[offset + length] == new[cursor + length]:
            length += 1
        delta.extend(struct.pack("<II", offset, length))
        cursor += length
        literal_start = cursor
    literal(len(new))
    return bytes(delta)


def stage(path, data):
    output = BUILD / "payload" / path
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    return str(output.relative_to(ROOT)).replace("\\", "\\\\")


def generate(game, manual):
    original = (game / "Shenzhen.exe").read_bytes()
    if sha(original) != STOCK_EXE:
        raise ValueError("Unsupported original Shenzhen.exe")
    pdf = manual.read_bytes()
    if not pdf.startswith(b"%PDF-") or sha(pdf) != PDF_HASH:
        raise ValueError("The Korean manual differs from the documented source file")
    patched = (BUILD / "Shenzhen.patched.exe").read_bytes()
    delta = make_delta(original, patched)
    rc = [f'2000 RCDATA "{stage("Shenzhen.delta", delta)}"']
    rows = []
    paths = [Path("Content/strings.ko.csv")]
    for pattern in ("Content/messages.ko/*.txt", "Content/descriptions.ko/*.txt",
                    "Content/textures/editor/playback_icon_*_ko.png", "Content/textures/parts/lcd_instructions_pattern_ko.png"):
        paths.extend(sorted(p.relative_to(LOCALIZATION) for p in LOCALIZATION.glob(pattern)))
    assets = [(Path("KoreanLocaleRuntime.dll"), (BUILD / "KoreanLocaleRuntime.dll").read_bytes()), (PDF, pdf)]
    assets.extend((path, (LOCALIZATION / path).read_bytes()) for path in paths)
    if len(assets) != 135:
        raise ValueError(f"Expected 135 patch assets, got {len(assets)}")
    for resource, (path, data) in enumerate(assets, start=2001):
        if data:
            rc.append(f'{resource} RCDATA "{stage(path, data)}"')
        rows.append((path.as_posix(), sha(data), resource if data else 0))

    with (LOCALIZATION / "original-sha256.csv").open(encoding="utf-8-sig", newline="") as stream:
        originals = [(row["Path"], row["SHA256"], 0) for row in csv.DictReader(stream) if row["Path"] != "Shenzhen.exe"]
    for path in paths:
        if path.name.endswith("_ko.png"):
            stock = path.with_name(path.name.replace("_ko.png", "_zh.png"))
            originals.append((stock.as_posix(), sha((game / stock).read_bytes()), 0))
    for path, digest, _ in originals:
        if sha((game / path).read_bytes()) != digest:
            raise ValueError(f"Unsupported original file: {path}")

    header = ["#pragma once", f'constexpr wchar_t Version[] = L"{VERSION}";',
              f'constexpr wchar_t OriginalExe[] = L"{STOCK_EXE}";',
              f'constexpr wchar_t PatchedExe[] = L"{sha(patched)}";',
              f'constexpr wchar_t DeltaHash[] = L"{sha(delta)}";',
              "constexpr unsigned DeltaResource = 2000;"]
    for name, entries in (("OriginalFiles", originals), ("PatchFiles", rows)):
        header.append(f"constexpr Entry {name}[] = {{")
        header.extend(f'    {{L"{path}", L"{digest}", {resource}}},' for path, digest, resource in entries)
        header.append("};")
    (BUILD / "manifest.generated.h").write_text("\n".join(header) + "\n", encoding="ascii")
    digits = VERSION.split(".") + ["0", "0"]
    version_number = ",".join(digits)
    rc.extend([
        "1 VERSIONINFO", f"FILEVERSION {version_number}", f"PRODUCTVERSION {version_number}",
        "FILEOS 0x40004", "FILETYPE 0x1", "BEGIN",
        ' BLOCK "StringFileInfo"', " BEGIN", '  BLOCK "040904b0"', "  BEGIN",
        '   VALUE "FileDescription", "SHENZHEN I/O Korean Patcher"',
        f'   VALUE "FileVersion", "{VERSION}"', f'   VALUE "ProductVersion", "{VERSION}"',
        "  END", " END", ' BLOCK "VarFileInfo"', " BEGIN",
        '  VALUE "Translation", 0x0409, 1200', " END", "END",
    ])
    (BUILD / "payload.generated.rc").write_text("\n".join(rc) + "\n", encoding="ascii")
    print(f"Embedded payload: {len(rows)} files. Patched SHA-256: {sha(patched)}")


def archive():
    output = RELEASE / f"ShenzhenKoreanPatch-v{VERSION}-win32.zip"
    with ZipFile(output, "w", ZIP_DEFLATED, compresslevel=9) as z:
        z.write(RELEASE / EXE, EXE)
    with ZipFile(output) as z:
        assert z.namelist() == [EXE] and z.testzip() is None
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", type=Path)
    parser.add_argument("--manual", type=Path)
    parser.add_argument("--zip", action="store_true")
    args = parser.parse_args()
    if args.zip:
        archive()
    elif args.game and args.manual:
        generate(args.game, args.manual)
    else:
        parser.error("--game and --manual are required when preparing resources")
