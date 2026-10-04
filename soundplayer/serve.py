#!/usr/bin/env python3
"""
Tiny static web server for the Floor Piano SoundFont player.
Serves this folder over http (so the WASM + SoundFonts load cleanly) and opens
the browser. Run it via start.bat / start.sh, or directly:  python serve.py
"""
import http.server
import socketserver
import threading
import webbrowser
import os

os.chdir(os.path.dirname(os.path.abspath(__file__)))


class Server(socketserver.ThreadingTCPServer):
    # threaded so the big .sf2 download doesn't block the page from loading
    allow_reuse_address = True
    daemon_threads = True


def main():
    httpd = None
    port = 8000
    for p in range(8000, 8010):
        try:
            httpd = Server(("", p), http.server.SimpleHTTPRequestHandler)
            port = p
            break
        except OSError:
            continue
    if httpd is None:
        print("No free port in 8000-8009. Close the other server and retry.")
        return

    url = f"http://localhost:{port}"
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    print(f"Floor Piano SoundFont player → {url}")
    print("Press Ctrl-C to stop.")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        httpd.shutdown()


if __name__ == "__main__":
    main()
