from pathlib import Path
import gzip
import sys
Import("env")

SRC = Path("src")
WEB = Path("src/web/build")

PAGES_CPP = SRC / "web_pages.cpp"
PAGES_H = SRC / "web_pages.h"
ROUTER_CPP = SRC / "page_router.cpp"

MIME = {
    ".html": "text/html",
    ".css": "text/css",
    ".js": "application/javascript",
}


def symbol(name: str) -> str:
    return name.upper().replace(".", "_").replace("-", "_")


def is_nogz(filename: str) -> bool:
    return ".nogz." in filename


def gen():
    pages = []

    for f in sorted(WEB.iterdir()):
        if not f.is_file():
            continue

        ext = f.suffix.lower()
        if ext not in MIME:
            continue

        url = "/" if f.name == "index.html" else f"/{f.name.replace('.nogz', '')}"
        raw = f.read_bytes()

        gzip_enabled = not is_nogz(f.name)

        if gzip_enabled:
            data = gzip.compress(raw, compresslevel=9)
        else:
            data = raw

        pages.append(
            {
                "url": url,
                "symbol": symbol(f.name) + ("_GZ" if gzip_enabled else ""),
                "data": data,
                "len": len(data),
                "mime": MIME[ext],
                "gzip": gzip_enabled,
            }
        )

    # ---------- web_pages.h ----------
    with PAGES_H.open("w", newline="\n") as f:
        f.write("#ifndef WEB_PAGES_H\n")
        f.write("#define WEB_PAGES_H\n\n")
        f.write("#include <Arduino.h>\n\n")

        for p in pages:
            f.write(f"extern const uint8_t {p['symbol']}[];\n")
            f.write(f"extern const size_t {p['symbol']}_LEN;\n\n")

        f.write("#endif\n")

    # ---------- web_pages.cpp ----------
    with PAGES_CPP.open("w", newline="\n") as f:
        f.write("#include <Arduino.h>\n")
        f.write('#include "web_pages.h"\n\n')

        for p in pages:
            f.write(f"const uint8_t {p['symbol']}[] PROGMEM = {{\n")
            for i, b in enumerate(p["data"]):
                f.write(f"0x{b:02x},")
                if i % 12 == 11:
                    f.write("\n")
            f.write("\n};\n")
            f.write(f"const size_t {p['symbol']}_LEN = {p['len']};\n\n")

    # ---------- page_router.cpp ----------
    with ROUTER_CPP.open("w", newline="\n") as f:
        f.write("#include <string.h>\n")
        f.write('#include "page_router.h"\n')
        f.write('#include "web_pages.h"\n\n')

        f.write("static const Page pages[] = {\n")
        for p in pages:
            f.write(
                f'  {{ "{p["url"]}", {p["symbol"]}, {p["symbol"]}_LEN, "{p["mime"]}", {str(p["gzip"]).lower()} }},\n'
            )
        f.write("};\n\n")

        f.write(
            """\
const Page *findPage(const char *path)
{
  size_t plen = strlen(path);

  for (auto &p : pages)
  {
    const char *ppath = p.path;
    size_t flen = strlen(ppath);

    // 1) Exact match: "/foo.js" == "/foo.js"
    if (plen == flen && memcmp(path, ppath, plen) == 0)
      return &p;

    // 2) Extensionless match: "/foo" == "/foo.<ext>"
    // Find '.' in stored path
    const char *dot = strchr(ppath, '.');
    if (!dot)
      continue;

    size_t baseLen = dot - ppath;

    if (plen == baseLen && memcmp(path, ppath, baseLen) == 0)
      return &p;
  }

  return nullptr;
}
"""
        )

    print(f"Generated {len(pages)} web assets")


# Check if minified files exist (only consider files with MIME extensions)
if not WEB.exists() or not any(f.is_file() and f.suffix.lower() in MIME for f in WEB.iterdir()):
    print("Warning: No minified files found in src/web/build/", file=sys.stderr)
    print("Skipping code generation. Using existing pre-generated files from repository.", file=sys.stderr)
    print("To regenerate, ensure minify.py runs first or minified files exist.", file=sys.stderr)
else:
    # Only generate pages if minified files exist
    gen()
