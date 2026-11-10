#!/usr/bin/env python3
#
# The page, in a headless Chromium (docs/zsys.md, "In a browser"):
#
#   test_page.py --web out/web --flash out/boot.img [--card zeitlos.img]
#
# Serves the page and the images from a scratch directory, opens it,
# and checks what a visitor would see: the machine boots to the desktop
# (welcome on a card's first boot, or on a machine without one), the
# canvas shows every window wm placed (check_screen.py, on the canvas's
# own 640x480 pixels), a key on the page's keyboard reaches it, and a
# click with the page's mouse does, and what the machine wrote is there
# at the next visit when the visitor asks for that. Needs Playwright (Python) and its
# Chromium; skipped, and says so, without them.

import argparse
import base64
import functools
import gzip
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))


def serve(root):
    h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=root)
    h.log_message = lambda *a: None
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), h)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--web", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--card")
    ap.add_argument("--out", default="out")
    a = ap.parse_args(argv)
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("web-test: skipped -- no Playwright for Python")
        return 0

    root = tempfile.mkdtemp(prefix="zsys-web-")
    try:
        for f in os.listdir(a.web):
            shutil.copy(os.path.join(a.web, f), root)
        shutil.copy(a.flash, os.path.join(root, "flash.img"))
        card = "none"
        if a.card:
            # as a release would serve it: gzipped, unpacked by the page
            with open(a.card, "rb") as i, gzip.open(os.path.join(root, "card.img.gz"), "wb", compresslevel=1) as o:
                shutil.copyfileobj(i, o, 1 << 20)
            card = "card.img.gz"
        srv = serve(root)
        url = "http://127.0.0.1:%d/index.html?flash=flash.img&card=%s" % (srv.server_address[1], card)
        greeted = "firstrun removed" if a.card else "starting welcome"
        fails = 0

        def check(ok, what):
            nonlocal fails
            print("web-test: %s -- %s" % (what, "ok" if ok else "FAILED"))
            fails += not ok

        with sync_playwright() as p:
            b = p.chromium.launch()
            page = b.new_page(viewport={"width": 1400, "height": 1100})
            page.add_init_script("window.zsysSeen = ''; window.zsysAudioPeak = 0")
            page.goto(url)
            seen = lambda: page.evaluate("window.zsysSeen")
            try:
                page.wait_for_function("t => window.zsysSeen.includes(t)", arg=greeted, timeout=60000)
            except Exception:
                pass
            check(greeted in seen(), "the page boots the machine to the desktop (\"%s\")" % greeted)
            page.wait_for_timeout(1500)

            def canvas_png(name):
                data = page.evaluate("document.getElementById('screen').toDataURL('image/png')")
                path = os.path.join(a.out, name)
                with open(path, "wb") as f:
                    f.write(base64.b64decode(data.split(",", 1)[1]))
                return path

            log = os.path.join(a.out, "web.log")
            png = canvas_png("web.png")
            with open(log, "w") as f:
                f.write(seen())
            r = subprocess.run([sys.executable, os.path.join(HERE, "..", "check_screen.py"), log, png, "welcome"],
                               capture_output=True, text=True)
            check(r.returncode == 0, "the canvas shows every window wm placed (%s)" % png)
            if r.returncode:
                print(r.stdout, end="")

            # the page's keyboard: Tab, Enter on welcome's first page opens the guide
            page.focus("#screen")
            page.keyboard.press("Tab")
            page.wait_for_timeout(200)
            page.keyboard.press("Enter")
            # with a card the guide opens in read; without one in text, from flash
            guide = "'read' pid" if a.card else "text: starting"
            try:
                page.wait_for_function("t => window.zsysSeen.includes(t)", arg=guide, timeout=20000)
            except Exception:
                pass
            check(guide in seen(), "a key on the page reaches welcome: the guide opens (%s)" % guide.split()[0])

            box = page.locator("#screen").bounding_box()
            sx, sy = box["width"] / 640, box["height"] / 480

            # the page's wheel, over the guide the key opened: it scrolls
            page.wait_for_timeout(1500)
            page.mouse.click(box["x"] + 230 * sx, box["y"] + 250 * sy)
            page.wait_for_timeout(800)
            b0 = open(canvas_png("web-wheel0.png"), "rb").read()
            page.mouse.wheel(0, 500)
            page.wait_for_timeout(1500)
            b1 = open(canvas_png("web-wheel1.png"), "rb").read()
            check(b0 != b1, "the page's wheel reaches the window under the pointer: the guide scrolls")

            # sound: Super+S turns speech on, and tts says so -- through the
            # mixer, the worklet, to the page's audio (a card with tts on it)
            if a.card:
                page.keyboard.down("Meta")
                page.keyboard.press("KeyS")
                page.keyboard.up("Meta")
                try:
                    page.wait_for_function("() => window.zsysAudioPeak > 0.01", timeout=20000)
                except Exception:
                    pass
                peak = page.evaluate("window.zsysAudioPeak")
                check(peak > 0.01, "sound: Super+S, and tts is heard through the page (peak %.3f)" % peak)
                page.keyboard.down("Meta")
                page.keyboard.press("KeyS")
                page.keyboard.up("Meta")

            # the page's mouse: a click on the dock's second icon starts files
            page.mouse.click(box["x"] + 64 * sx, box["y"] + 450 * sy)
            try:
                page.wait_for_function("() => window.zsysSeen.includes(\"launched 'files'\")", timeout=20000)
            except Exception:
                pass
            check("launched 'files'" in seen(), "a click on the page reaches wm: the dock starts files")
            page.wait_for_timeout(1500)
            canvas_png("web-after.png")
            speed = page.evaluate("document.getElementById('speed').textContent")
            print("web-test: %s" % (speed or "no speed measured"))

            # colour: planedemo from the console line, in game mode's 256 colours
            if a.card:
                page.click("summary")
                page.fill("#type", "run /apps/planedemo")
                page.press("#type", "Enter")
                try:
                    page.wait_for_function("() => window.zsysSeen.includes(\"'planedemo' pid\")", timeout=20000)
                except Exception:
                    pass
                page.wait_for_timeout(4000)
                colours = page.evaluate("""() => {
                    const c = document.getElementById('screen').getContext('2d').getImageData(0, 0, 640, 480).data;
                    const seen = new Set();
                    for (let i = 0; i < c.length; i += 4 * 97) seen.add(c[i] << 16 | c[i + 1] << 8 | c[i + 2]);
                    return seen.size; }""")
                canvas_png("web-colour.png")
                check(colours > 16, "colour: planedemo fills the canvas in %d colours or more" % colours)

            # full screen: the button, and the mouse mapped through the letterbox
            page.click("#full")
            page.wait_for_timeout(800)
            fs = page.evaluate("document.fullscreenElement ? document.fullscreenElement.id : ''")
            check(fs == "screen", "the full screen button puts the screen full screen")
            if fs:
                page.evaluate("document.exitFullscreen()")
                page.wait_for_timeout(500)

            # keeping changes: with the box ticked, what the machine wrote
            # is there at the next visit -- welcome removed the card's
            # /sys/firstrun (or set the flash's key), so it does not start again
            page.check("#keep")
            page.wait_for_timeout(4500)
            page.reload()
            try:
                page.wait_for_function("() => window.zsysSeen.includes('wm: welcome:') || "
                                       "window.zsysSeen.includes('starting welcome')", timeout=60000)
            except Exception:
                pass
            page.wait_for_timeout(2000)
            s2 = seen()
            # the console's lines can interleave (two processes print), so
            # the line looked for is wm's own reason, which it prints whole
            check("wm: welcome:" in s2 and "starting welcome" not in s2,
                  "kept in the browser: the next visit's machine is the same one (no welcome again)")
            if "starting welcome" in s2 or "wm: welcome:" not in s2:
                print("".join("    | " + l + "\n" for l in s2.splitlines() if "welcome" in l or "zsys page" in l or "boot fin" in l))
            # new images on the server: what was kept is dropped, not laid over them
            if a.card:
                with gzip.open(os.path.join(root, "card.img.gz"), "rb") as i:
                    img = bytearray(i.read())
                img[-1] ^= 0xff					# past the filesystem: another image
                with gzip.open(os.path.join(root, "card2.img.gz"), "wb", compresslevel=1) as o:
                    o.write(img)
                page.goto(url.replace("card.img.gz", "card2.img.gz"))
                try:
                    page.wait_for_function("() => window.zsysSeen.includes('starting welcome')", timeout=60000)
                except Exception:
                    pass
                s3 = seen()
                check("images have changed" in s3 and "starting welcome" in s3,
                      "new images: what was kept is dropped, and the new card's first boot greets again")
            page.uncheck("#keep")
            page.click("#forget")
            page.wait_for_timeout(500)
            b.close()
        srv.shutdown()
        return 1 if fails else 0
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
