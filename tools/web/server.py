"""edevkit documentation hub — FastAPI server.

Serves the project's HTML docs through a hub interface with grouped navigation,
client-side search, and theme toggle. Doc metadata (title, summary, modified
date, reading time) is extracted from each HTML file at request time, so
adding a new doc is as simple as dropping a `.html` file into `pages/`.

Branding: "edevkit" is the project umbrella. Specific products (edevkit1,
edevkit2, …) keep their version-specific naming inside their own docs only.

Run from this directory:

    python server.py                 # auto-reload, http://127.0.0.1:8765, opens browser
    uvicorn server:app --reload      # equivalent (no browser auto-open)
    uvicorn server:app --host 0.0.0.0 --port 8000   # bind to LAN

Set EDEVKIT_DOCS_NO_BROWSER=1 to suppress browser auto-open (e.g. headless / SSH).
"""

from __future__ import annotations

import re
from datetime import datetime
from html import unescape
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates

ROOT = Path(__file__).parent.resolve()
PAGES_DIR = ROOT / "pages"
STATIC_DIR = ROOT / "static"
TEMPLATES_DIR = ROOT / "templates"

# --------------------------------------------------------------------------- #
# Doc categorisation                                                          #
# --------------------------------------------------------------------------- #
# Map filename stem → category metadata. Extend as new docs land.
# `order` controls grouping order on the hub page (lower = earlier).

CATEGORIES: dict[str, dict] = {
    "edevkit1":      {"name": "Design Spec",  "icon": "📐", "order": 0},
    "installation":  {"name": "Onboarding",   "icon": "🧭", "order": 1},
    "shell_helpers": {"name": "Workflow",     "icon": "⚙️", "order": 2},
    "usb_book":      {"name": "USB Book",     "icon": "📚", "order": 3},
    "bringup":       {"name": "Bring-up",     "icon": "⚡", "order": 4},
    "recovery":      {"name": "Recovery",     "icon": "🛟", "order": 5},
    "firmware":      {"name": "Firmware",     "icon": "🔧", "order": 6},
    "hardware":      {"name": "Hardware",     "icon": "🔌", "order": 7},
    "host":          {"name": "Host tools",   "icon": "💻", "order": 8},
    "enclosure":     {"name": "Enclosure",    "icon": "📦", "order": 9},
}
DEFAULT_CATEGORY = {"name": "General", "icon": "📄", "order": 99}

# --------------------------------------------------------------------------- #
# HTML metadata extraction                                                    #
# --------------------------------------------------------------------------- #

_TITLE_RE   = re.compile(r"<title[^>]*>(.*?)</title>", re.IGNORECASE | re.DOTALL)
_DESC_RE    = re.compile(r'<meta\s+name=["\']description["\']\s+content=["\']([^"\']+)["\']', re.IGNORECASE)
_HERO_P_RE  = re.compile(r'<header[^>]*class="hero"[^>]*>.*?<p[^>]*class="[^"]*small[^"]*"[^>]*>(.*?)</p>', re.IGNORECASE | re.DOTALL)
_TAG_RE     = re.compile(r"<[^>]+>")


def _strip(html: str) -> str:
    return re.sub(r"\s+", " ", _TAG_RE.sub("", unescape(html))).strip()


def _category_for(stem: str) -> dict:
    # Exact stem match first, then prefix match (e.g. firmware_arch.html → firmware).
    if stem in CATEGORIES:
        return CATEGORIES[stem]
    for key, cat in CATEGORIES.items():
        if stem.startswith(f"{key}_") or stem.startswith(f"{key}-"):
            return cat
    return DEFAULT_CATEGORY


def extract_metadata(path: Path) -> dict:
    """Pull title, summary, size, mtime, reading time out of a doc HTML file."""
    text = path.read_text(encoding="utf-8", errors="ignore")

    title_m = _TITLE_RE.search(text)
    title = _strip(title_m.group(1)) if title_m else path.stem.replace("_", " ").title()
    # Project prefix is noise on the hub — strip it.
    # Matches "edevkit", "edevkit1", "edevkit2", … followed by a separator.
    title = re.sub(r"^\s*edevkit\d*\s*[—\-:·]\s*", "", title, flags=re.IGNORECASE)

    desc = ""
    desc_m = _DESC_RE.search(text)
    if desc_m:
        desc = _strip(desc_m.group(1))
    if not desc:
        hero_m = _HERO_P_RE.search(text)
        if hero_m:
            desc = _strip(hero_m.group(1))
    if len(desc) > 280:
        desc = desc[:277].rstrip() + "…"

    cat = _category_for(path.stem)

    word_count = len(_strip(text).split())
    read_min = max(1, round(word_count / 250))

    stat = path.stat()

    return {
        "slug":           path.stem,
        "filename":       path.name,
        "title":          title,
        "summary":        desc,
        "category":       cat["name"],
        "category_icon":  cat["icon"],
        "category_order": cat.get("order", 99),
        "size_kb":        round(stat.st_size / 1024, 1),
        "modified":       datetime.fromtimestamp(stat.st_mtime).isoformat(),
        "modified_human": datetime.fromtimestamp(stat.st_mtime).strftime("%Y-%m-%d"),
        "read_minutes":   read_min,
        "url":            f"/docs/{path.name}",
    }


def list_docs() -> list[dict]:
    if not PAGES_DIR.exists():
        return []
    docs = [extract_metadata(p) for p in sorted(PAGES_DIR.glob("*.html"))]
    docs.sort(key=lambda d: (d["category_order"], d["title"].lower()))
    return docs


def group_by_category(docs: list[dict]) -> list[dict]:
    """Stable ordered groups for sidebar + main grid."""
    seen: dict[str, dict] = {}
    for d in docs:
        cat = seen.setdefault(d["category"], {
            "name":  d["category"],
            "icon":  d["category_icon"],
            "order": d["category_order"],
            "docs":  [],
        })
        cat["docs"].append(d)
    return sorted(seen.values(), key=lambda c: c["order"])


# --------------------------------------------------------------------------- #
# App                                                                          #
# --------------------------------------------------------------------------- #

app = FastAPI(
    title="edevkit documentation hub",
    version="0.1.0",
    docs_url=None,           # disable Swagger; we don't expose a public API
    redoc_url=None,
    openapi_url=None,
)

if STATIC_DIR.exists():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

templates = Jinja2Templates(directory=str(TEMPLATES_DIR))


@app.get("/", response_class=HTMLResponse)
async def index(request: Request) -> HTMLResponse:
    docs = list_docs()
    groups = group_by_category(docs)
    return templates.TemplateResponse(
        request,
        "index.html",
        {
            "docs":        docs,
            "groups":      groups,
            "doc_count":   len(docs),
            "now":         datetime.now().strftime("%Y-%m-%d"),
        },
    )


@app.get("/docs/{path:path}")
async def serve_doc(path: str):
    # Allow nested paths (e.g. usb/partI/01_why_usb.html, usb/_assets/book.css)
    # but defend against traversal: resolve and assert it stays under PAGES_DIR.
    if "\\" in path or path.startswith("/") or path.startswith("."):
        raise HTTPException(status_code=400, detail="bad path")
    target = (PAGES_DIR / path).resolve()
    try:
        target.relative_to(PAGES_DIR.resolve())
    except ValueError:
        raise HTTPException(status_code=400, detail="path escapes pages root")
    if not target.is_file():
        raise HTTPException(status_code=404, detail="doc not found")
    return FileResponse(target)


@app.get("/api/docs")
async def api_docs() -> JSONResponse:
    """JSON list of docs — powers client-side search."""
    return JSONResponse(list_docs())


@app.get("/api/health")
async def api_health() -> JSONResponse:
    return JSONResponse({"status": "ok", "docs": len(list_docs())})


@app.exception_handler(404)
async def not_found(request: Request, _exc):
    return templates.TemplateResponse(
        request,
        "404.html",
        {"path": request.url.path},
        status_code=404,
    )


# --------------------------------------------------------------------------- #
# Entrypoint                                                                   #
# --------------------------------------------------------------------------- #

def _open_browser_when_ready(host: str, port: int, *, timeout: float = 10.0) -> None:
    """Wait for the server port to accept a connection, then open the user's
    default browser at the docs hub. Runs in a background thread so it doesn't
    block uvicorn startup. Silently gives up after `timeout` seconds.

    Lives in the parent (supervisor) process when uvicorn runs with reload=True,
    so it only fires once per `python server.py` invocation — not on every
    file-save reload.
    """
    import socket
    import time
    import webbrowser

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.2):
                webbrowser.open_new_tab(f"http://{host}:{port}/")
                return
        except OSError:
            time.sleep(0.15)


if __name__ == "__main__":
    import os
    import threading
    import uvicorn

    HOST, PORT = "127.0.0.1", 8765

    # Auto-open the docs hub in the user's browser on first start. Suppressed
    # by EDEVKIT_DOCS_NO_BROWSER=1 (headless / SSH / CI use). The supervisor
    # process runs this block exactly once; uvicorn's reload child inherits the
    # parent's behaviour but never re-runs __main__, so reloads don't re-open.
    if os.environ.get("EDEVKIT_DOCS_NO_BROWSER", "").strip() not in ("1", "true", "yes"):
        threading.Thread(
            target=_open_browser_when_ready,
            args=(HOST, PORT),
            daemon=True,
        ).start()

    uvicorn.run(
        "server:app",
        host=HOST,
        port=PORT,
        reload=True,
        log_level="info",
    )
