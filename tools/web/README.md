# edevkit — documentation hub

A small FastAPI server that serves every project HTML doc through one
discoverable hub: searchable card grid, sidebar grouped by category,
light/dark theme, keyboard shortcuts. Drop a new `.html` into `pages/` and
it appears on the hub automatically — no config edits required.

Branding convention: **edevkit** is the project umbrella; specific
products (`edevkit1`, future `edevkit2`, etc.) only carry version-specific
naming inside their own pages. Don't introduce `edevkit1` into the hub
chrome unless the content is genuinely v1-only.

## Layout

```
tools/web/
├── server.py                FastAPI app
├── requirements.txt         fastapi, uvicorn, jinja2
├── package.json             Tailwind build (devDeps only — needed to rebuild static/tailwind.css)
├── tailwind.config.js       brand palette + custom animations
├── tailwind.input.css       @tailwind directives
├── pages/                   ← project HTML docs go here
│   ├── edevkit1.html
│   └── installation.html
├── static/                  ← hub UI assets (served at /static)
│   ├── tailwind.css         vendored Tailwind build (committed)
│   ├── style.css            hub custom CSS (brand mark, search results, scrollbar, …)
│   ├── script.js            hub interactivity (theme, search, sort, chips, …)
│   ├── syntax.{js,css}      doc-page enhancements (highlighter, TOC rail, breadcrumbs, …)
│   ├── manifest.webmanifest PWA metadata
│   ├── sw.js                service worker (served from /sw.js so its scope covers /docs/*)
│   └── favicon.svg
└── templates/               Jinja2 templates for the hub itself
    ├── base.html
    ├── index.html
    └── 404.html
```

The hub is *just* the entry point: each doc in `pages/` is served as-is
(its embedded styling is preserved). Internal `<a href="other.html">`
links between docs continue to work because every doc lives in the same
URL prefix `/docs/…`.

## Rebuilding Tailwind

`static/tailwind.css` is vendored (≈54 KB minified) so the hub has zero
runtime dependency on the Tailwind CDN. After editing
`tailwind.config.js` or adding new class names in `templates/` /
`static/script.js`, rebuild:

```bash
cd tools/web
npm install                    # one-time, populates node_modules/ (gitignored)
npm run build:css              # rebuilds static/tailwind.css
# or:
npm run watch:css              # rebuilds on every save during dev
```

## Run it (any OS)

### Recommended — use the project venv

If you've followed the project's installation guide
(`tools/web/pages/installation.html` step 3), the docs-hub deps
(`fastapi`, `uvicorn`, `jinja2`) are already installed in `$EDEV_VENV`
alongside `west`, `pyocd`, etc. Just activate it:

```bash
source "$EDEV_VENV/bin/activate"             # Windows: & "$env:EDEV_VENV\Scripts\Activate.ps1"
cd "$EDEVKIT_REPO/tools/web"
python server.py                             # or: uvicorn server:app --reload
```

### Standalone — separate venv for the docs hub only

If you want the hub without setting up the rest of the project (e.g. for
a doc-only contributor), a private venv works fine:

```bash
cd tools/web
python3 -m venv .venv
source .venv/bin/activate                    # Windows: .venv\Scripts\Activate.ps1
pip install -r requirements.txt
python server.py
```

### Either way

Open <http://127.0.0.1:8765/>. The `--reload` flag picks up doc changes
without restart; metadata (title, summary, mtime, reading time) is
re-extracted on every request, so editing a doc reflects immediately.

### Bind to LAN (e.g. demo to a colleague)

```bash
uvicorn server:app --host 0.0.0.0 --port 8765
# Browse from another machine: http://<your-ip>:8765/
```

## Adding a new doc

1. Create `pages/<slug>.html` — a complete HTML5 document with a
   `<title>` tag and (optionally) a `<p class="small">` inside
   `<header class="hero">` for the summary, or a
   `<meta name="description">` tag.
2. (Optional) Add `<slug>` to `CATEGORIES` in `server.py` to give it a
   category, icon, and ordering. Without an entry the doc lands in
   *General* with the 📄 icon.
3. Reload <http://127.0.0.1:8765/>. The card appears.

Filenames matching a category prefix (e.g. `firmware_arch.html`) are
auto-grouped under that category.

## Keyboard shortcuts (hub page)

| Key | Action |
|-----|--------|
| <kbd>/</kbd> or <kbd>⌘K</kbd> / <kbd>Ctrl K</kbd> | Focus the search box |
| <kbd>Esc</kbd> | Clear search |
| <kbd>T</kbd>   | Toggle theme |

## Endpoints

| Path                    | What it does                                    |
|-------------------------|-------------------------------------------------|
| `GET /`                 | Hub landing page                                |
| `GET /docs/{filename}`  | Serve the HTML at `pages/{filename}` as-is      |
| `GET /api/docs`         | JSON list of all docs (powers client-side search) |
| `GET /api/health`       | `{"status": "ok", "docs": N}` — for liveness checks |
| `GET /static/…`         | Hub CSS/JS/favicon                              |

## Why FastAPI

- Async-first; doc serving and the hub render share one event loop with
  zero ceremony.
- Built-in static + Jinja2 support.
- Trivial to extend later — search, project-status JSON, OTA-trigger
  endpoints, KiBot job results, etc. — without rewriting the host.

## Conventions

- **Source of truth for the design spec** is now
  `tools/web/pages/edevkit1.html` (moved from `documents/`). Update the
  parent `CLAUDE.md` reference if you re-locate it.
- The `documents/` folder remains for non-HTML artifacts (PDFs,
  diagrams, captures) that aren't part of the hub.
- Don't commit `pages/` build outputs (none today; just static HTML).
- Don't commit the `.venv/` directory created by the bootstrap step.
