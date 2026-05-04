"""edevkit documentation hub — FastAPI server.

Serves the project's HTML docs through a hub interface with grouped navigation,
client-side search, and theme toggle. Doc metadata (title, summary, modified
date, reading time) is extracted from each HTML file at request time, so
adding a new doc is as simple as dropping a `.html` file into `pages/`.

Branding: "edevkit" is the project umbrella. Specific products (edevkit1,
edevkit2, …) keep their version-specific naming inside their own docs only.

Run from this directory:

    python server.py                 # auto-reload, http://127.0.0.1:8765
    uvicorn server:app --reload      # equivalent
    uvicorn server:app --host 0.0.0.0 --port 8000   # bind to LAN
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
    "edevkit1":      {"name": "Design Spec",   "icon": "📐", "order":  0},
    "installation":  {"name": "Onboarding",    "icon": "🧭", "order":  1},
    "shell_helpers": {"name": "Workflow",      "icon": "⚙️", "order":  2},
    "arm":           {"name": "Architecture",  "icon": "🧠", "order":  3},
    "usb":           {"name": "USB stack",     "icon": "🔌", "order":  4},
    "debug":         {"name": "Debug",         "icon": "🔍", "order":  5},
    "proto":         {"name": "Protocols",     "icon": "🔁", "order":  6},
    "platform":      {"name": "Platform",      "icon": "🏗️", "order":  7},
    "bringup":       {"name": "Bring-up",      "icon": "⚡", "order":  8},
    "recovery":      {"name": "Recovery",      "icon": "🛟", "order":  9},
    "firmware":      {"name": "Firmware",      "icon": "🔧", "order": 10},
    "hardware":      {"name": "Hardware",      "icon": "📟", "order": 11},
    "host":          {"name": "Host tools",    "icon": "💻", "order": 12},
    "enclosure":     {"name": "Enclosure",     "icon": "📦", "order": 13},
}
DEFAULT_CATEGORY = {"name": "General", "icon": "📄", "order": 99}

# --------------------------------------------------------------------------- #
# HTML metadata extraction                                                    #
# --------------------------------------------------------------------------- #

_TITLE_RE   = re.compile(r"<title[^>]*>(.*?)</title>", re.IGNORECASE | re.DOTALL)
_DESC_RE    = re.compile(r'<meta\s+name=["\']description["\']\s+content=["\']([^"\']+)["\']', re.IGNORECASE)
_HERO_P_RE  = re.compile(r'<header[^>]*class="hero"[^>]*>.*?<p[^>]*class="[^"]*small[^"]*"[^>]*>(.*?)</p>', re.IGNORECASE | re.DOTALL)
_TAG_RE     = re.compile(r"<[^>]+>")
_BODY_RE    = re.compile(r"<body[^>]*>(.*?)</body>", re.IGNORECASE | re.DOTALL)
_SECTION_RE = re.compile(
    r'<h2[^>]*id=["\']([^"\']+)["\'][^>]*>(.*?)</h2>(.*?)(?=<h2|</body)',
    re.IGNORECASE | re.DOTALL,
)
_STRIP_TAGS_BLOCK_RE = re.compile(
    r"<(script|style|nav|header|footer)\b[^>]*>.*?</\1\s*>",
    re.IGNORECASE | re.DOTALL,
)


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


# --------------------------------------------------------------------------- #
# Full-text search index                                                      #
# --------------------------------------------------------------------------- #
# Per-doc extraction of <h2 id="…">…</h2> sections + their following body text,
# stripped of nav/header/footer/script/style. Cached on first build, invalidated
# when any pages/*.html mtime changes.

_search_index_cache: tuple[float, list[dict]] | None = None


def _doc_sections(path: Path, doc_meta: dict) -> list[dict]:
    text = path.read_text(encoding="utf-8", errors="ignore")
    body_m = _BODY_RE.search(text)
    body = body_m.group(1) if body_m else text
    # drop noise blocks before section-scanning
    body = _STRIP_TAGS_BLOCK_RE.sub("", body)

    sections: list[dict] = []
    for m in _SECTION_RE.finditer(body):
        sec_id, heading_html, content_html = m.group(1), m.group(2), m.group(3)
        heading = _strip(heading_html)
        content = _strip(content_html)
        if len(content) > 1200:
            content = content[:1200]
        sections.append({
            "doc_url":      doc_meta["url"],
            "doc_title":    doc_meta["title"],
            "doc_category": doc_meta["category"],
            "anchor":       sec_id,
            "heading":      heading,
            "text":         content,
        })
    return sections


def build_search_index() -> list[dict]:
    """List of section entries across all docs, suitable for JSON streaming."""
    global _search_index_cache
    if not PAGES_DIR.exists():
        return []
    files = sorted(PAGES_DIR.glob("*.html"))
    fingerprint = sum(p.stat().st_mtime for p in files)
    if _search_index_cache and _search_index_cache[0] == fingerprint:
        return _search_index_cache[1]

    by_path = {p: extract_metadata(p) for p in files}
    index: list[dict] = []
    for p, meta in by_path.items():
        index.extend(_doc_sections(p, meta))
    _search_index_cache = (fingerprint, index)
    return index


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
    # Top N most-recently-modified docs for the "Recently updated" row.
    recent = sorted(docs, key=lambda d: d["modified"], reverse=True)[:4]
    return templates.TemplateResponse(
        request,
        "index.html",
        {
            "docs":        docs,
            "groups":      groups,
            "recent":      recent,
            "doc_count":   len(docs),
            "now":         datetime.now().strftime("%Y-%m-%d"),
        },
    )


@app.get("/docs/{filename}")
async def serve_doc(filename: str):
    # Prevent path traversal — only flat filenames inside `pages/` are allowed.
    if "/" in filename or "\\" in filename or ".." in filename or filename.startswith("."):
        raise HTTPException(status_code=400, detail="bad filename")
    path = PAGES_DIR / filename
    if not path.is_file():
        raise HTTPException(status_code=404, detail="doc not found")
    return FileResponse(path)


@app.get("/api/docs")
async def api_docs() -> JSONResponse:
    """JSON list of docs — powers client-side search (metadata only)."""
    return JSONResponse(list_docs())


@app.get("/api/search-index")
async def api_search_index() -> JSONResponse:
    """Per-section text index across all docs — powers full-text search."""
    return JSONResponse(build_search_index())


@app.get("/api/health")
async def api_health() -> JSONResponse:
    return JSONResponse({"status": "ok", "docs": len(list_docs())})


@app.get("/sw.js", include_in_schema=False)
async def service_worker():
    """Serve the service worker from the site root so its scope covers /docs/*.

    Must include a Service-Worker-Allowed header that matches the desired
    scope ('/'). Browsers cache aggressively, so we also turn off caching
    for the SW file itself — the worker handles its own resource caching."""
    sw_path = STATIC_DIR / "sw.js"
    if not sw_path.is_file():
        raise HTTPException(status_code=404, detail="service worker missing")
    return FileResponse(
        sw_path,
        media_type="application/javascript",
        headers={
            "Service-Worker-Allowed": "/",
            "Cache-Control": "no-cache, no-store, must-revalidate",
        },
    )


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

if __name__ == "__main__":
    import os
    import threading
    import webbrowser

    import uvicorn

    HOST = "127.0.0.1"
    PORT = 8765
    URL  = f"http://{HOST}:{PORT}/"

    def _open_browser() -> None:
        # Suppressed by EDEVKIT_NO_BROWSER=1 (CI, headless, repeated reloads).
        # Also suppressed in uvicorn's reload child process — it re-imports
        # this module under the name "server", not "__main__", so the
        # whole block is naturally skipped on reload.
        if os.environ.get("EDEVKIT_NO_BROWSER"):
            return
        try:
            webbrowser.open(URL)
        except Exception:
            pass  # opening the browser is a nicety, never fatal

    # Give uvicorn ~1 s to bind the socket before the browser tries to load.
    threading.Timer(1.0, _open_browser).start()

    print(f"\n  edevkit docs hub serving at {URL}\n  (set EDEVKIT_NO_BROWSER=1 to skip auto-open)\n")

    uvicorn.run(
        "server:app",
        host=HOST,
        port=PORT,
        reload=True,
        log_level="info",
    )
