#!/usr/bin/env python3
"""Build PkgManager's demonstration storefront: Demo/storefront.json and its
pictures, from the sources listed in assets/source/demo-store/sources.json.

    make-demo-store.py OUT_DIR

Every picture is cut and scaled to the fixed places of the window's picture
panel (DEMO_SHOT_W x DEMO_SHOT_H for a screenshot, DEMO_THUMB_* for its
thumbnail, src/pkgmanager/demo.h), so the panel never changes size and
never pushes the buttons off a 1024x768 screen.

Screenshots come from real runs of the programs, named with where and when
they were taken; the fictional paid application has a MOCKUP picture drawn
here, which says so in the picture itself. Nothing is invented to look like
a screenshot of a program.

Needs netpbm (pngtopam, pamcut, pamscale, pnmtopng, pbmtext). pamcut provides explicit crop offsets for reproducible thumbnails.
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SOURCES = ROOT / "assets/source/demo-store/sources.json"
SHOT, THUMB, ICON = (320, 200), (80, 50), (48, 48)


def run(cmd, data=None):
    return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def cut_scale(src, box, size):
    """PNG bytes: `box` (x, y, w, h) of `src`, scaled to exactly `size`."""
    pam = run(["pngtopam", str(src)])
    x, y, w, h = box
    pam = run(["pamcut", "-left", str(x), "-top", str(y), "-width", str(w), "-height", str(h)], pam)
    pam = run(["pamscale", "-width", str(size[0]), "-height", str(size[1])], pam)
    return run(["pnmtopng", "-force", "-compression", "9"], pam)


def mockup_png(size, label, colour):
    """A MOCKUP picture with netpbm only: the label in black on a white
    panel, centred on a coloured field. Plain on purpose; it must not look
    like a screenshot of a program."""
    import tempfile
    w, h = size
    text = run(["pbmtext", "-builtin", "fixed", label])
    text = run(["pnmpad", "-white", "-left=4", "-right=4", "-top=3", "-bottom=3"], text)
    lab = run(["pgmtoppm", "white"], run(["pamscale", "-xyfit", str(w - 12), str(h - 12)], text))
    lw, lh = pam_size(lab)
    with tempfile.TemporaryDirectory() as d:
        bg = Path(d) / "bg.ppm"
        bg.write_bytes(run(["ppmmake", colour, str(w), str(h)]))
        res = run(["pnmpaste", "-", str((w - lw) // 2), str((h - lh) // 2), str(bg)], lab)
    # -force: truecolour, never a palette (see demo_png_check in demo.c).
    return run(["pnmtopng", "-force", "-compression", "9"], res)


def pam_size(data):
    head = run(["pamfile"], data).decode()
    import re
    m = re.search(r"(\d+) by (\d+)", head)
    return int(m.group(1)), int(m.group(2))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    spec = json.loads(SOURCES.read_text())
    (out / "media").mkdir(parents=True, exist_ok=True)
    store = {"schema": 1, "demo": True, "banner": spec["banner"], "media": [], "offers": spec["offers"]}
    for m in spec["media"]:
        # Two short lines rather than one long one: the panel is 320 pixels
        # wide and a cut provenance line would hide where a picture is from.
        import textwrap
        prov = "\n".join(textwrap.wrap(m["provenance"], 46)[:3])
        entry = {"id": m["id"], "kind": m["kind"], "provenance": prov, "screenshots": []}
        d = out / "media" / m["id"]
        d.mkdir(parents=True, exist_ok=True)
        if m.get("icon"):
            # The program's own icon, copied as it is: AROS PNG icons are PNG
            # files with an extra chunk the picture datatype skips.
            (d / "icon.png").write_bytes((ROOT / m["icon"]).read_bytes())
            entry["icon"] = f"media/{m['id']}/icon.png"
        for i, s in enumerate(m["screenshots"]):
            if m["kind"] == "screenshot":
                src = (ROOT / s["source"]).resolve()
                full, thumb = cut_scale(src, s["box"], SHOT), cut_scale(src, s["box"], THUMB)
            else:
                full, thumb = mockup_png(SHOT, s["label"], s["colour"]), mockup_png(THUMB, "MOCKUP", s["colour"])
            (d / f"{i + 1}.png").write_bytes(full)
            (d / f"{i + 1}-thumb.png").write_bytes(thumb)
            entry["screenshots"].append({"file": f"media/{m['id']}/{i + 1}.png",
                                         "thumb": f"media/{m['id']}/{i + 1}-thumb.png",
                                         "caption": s["caption"]})
        store["media"].append(entry)
    (out / "storefront.json").write_text(json.dumps(store, indent=2) + "\n")
    print(f"written {out}/storefront.json and {sum(len(m['screenshots']) for m in store['media'])} picture(s)")


if __name__ == "__main__":
    main()
