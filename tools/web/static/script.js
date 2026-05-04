// edevkit docs hub — vanilla ES module
// - theme toggle (light / dark / system) persisted in localStorage
// - client-side fuzzy search across cards (title / summary / category)
// - lazy-fetched full-text search across doc bodies via /api/search-index
// - keyboard shortcuts: /, Cmd+K / Ctrl+K, T, Esc
// - arrow-key navigation across the doc-card grid
// - scrollspy on the in-page section anchors
// - View Transitions API enhancement for card → doc navigation

const STORAGE_KEY = 'edev_theme';
const root = document.documentElement;

/* ----------------------------------------------------------------------- *
 * Theme — Tailwind class-based dark mode                                   *
 * ----------------------------------------------------------------------- */

function applyStoredTheme() {
  const stored = localStorage.getItem(STORAGE_KEY);
  let dark;
  if (stored === 'dark')      dark = true;
  else if (stored === 'light') dark = false;
  else dark = matchMedia('(prefers-color-scheme: dark)').matches;
  root.classList.toggle('dark', dark);
}

function nextTheme(current) {
  // light → dark → system → light
  if (current === 'light') return 'dark';
  if (current === 'dark')  return 'system';
  return 'light';
}

function toggleTheme() {
  const stored = localStorage.getItem(STORAGE_KEY) ?? 'system';
  const next = nextTheme(stored);
  if (next === 'system') {
    localStorage.removeItem(STORAGE_KEY);
  } else {
    localStorage.setItem(STORAGE_KEY, next);
  }
  applyStoredTheme();
}

applyStoredTheme();

document.getElementById('theme-toggle')?.addEventListener('click', toggleTheme);

// React to OS-level theme changes when the user is on "system"
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
  if (!localStorage.getItem(STORAGE_KEY)) applyStoredTheme();
});

/* ----------------------------------------------------------------------- *
 * Search — card-level filter (title / summary / category)                  *
 * ----------------------------------------------------------------------- */

const searchInput = document.getElementById('search-input');
const cards       = Array.from(document.querySelectorAll('[data-card]'));
const groups      = Array.from(document.querySelectorAll('[data-group]'));
const emptyState  = document.getElementById('empty-state');

const cardIndex = cards.map((card) => ({
  el:      card,
  title:   (card.dataset.title    ?? '').toLowerCase(),
  summary: (card.dataset.summary  ?? '').toLowerCase(),
  cat:     (card.dataset.category ?? '').toLowerCase(),
}));

// Active category chip — empty string = "All".
let activeCategory = '';

function escapeRegExp(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function highlight(card, query) {
  card.querySelectorAll('mark').forEach((m) => {
    const t = document.createTextNode(m.textContent);
    m.parentNode.replaceChild(t, m);
  });
  if (!query) return;
  const re = new RegExp(`(${escapeRegExp(query)})`, 'gi');
  for (const sel of ['h3', 'p']) {
    const node = card.querySelector(sel);
    if (!node) continue;
    const text = node.textContent;
    if (!re.test(text)) continue;
    re.lastIndex = 0;
    node.innerHTML = text.replace(re, '<mark class="bg-brand-200 dark:bg-brand-500/40 text-inherit px-0.5 rounded">$1</mark>');
  }
}

function score(entry, query) {
  if (!query) return 1;
  const q = query.toLowerCase();
  let s = 0;
  if (entry.title.includes(q))   s += 5;
  if (entry.cat.includes(q))     s += 3;
  if (entry.summary.includes(q)) s += 2;
  const parts = q.split(/\s+/).filter(Boolean);
  for (const p of parts) {
    if (entry.title.includes(p))   s += 1;
    if (entry.summary.includes(p)) s += 1;
  }
  return s;
}

/* ----------------------------------------------------------------------- *
 * Full-text section index (lazy fetched)                                   *
 * ----------------------------------------------------------------------- */

let sectionIndex = null;
let sectionIndexLoading = null;

function setSearchLoading(loading) {
  const sp = document.getElementById('search-spinner');
  const kb = document.getElementById('search-kbd');
  if (sp) sp.hidden = !loading;
  if (kb) kb.hidden = loading;
}

async function ensureSectionIndex() {
  if (sectionIndex) return sectionIndex;
  if (sectionIndexLoading) return sectionIndexLoading;
  setSearchLoading(true);
  sectionIndexLoading = fetch('/api/search-index')
    .then((r) => (r.ok ? r.json() : []))
    .then((idx) => {
      sectionIndex = idx.map((s) => ({
        ...s,
        _hay: (s.heading + ' ' + s.text).toLowerCase(),
      }));
      return sectionIndex;
    })
    .catch(() => (sectionIndex = []))
    .finally(() => setSearchLoading(false));
  return sectionIndexLoading;
}

function makeSnippet(text, q, max = 160) {
  if (!q) return text.slice(0, max);
  const lo = text.toLowerCase();
  const i = lo.indexOf(q);
  if (i < 0) return text.slice(0, max);
  const start = Math.max(0, i - 40);
  const end   = Math.min(text.length, start + max);
  let snip = text.slice(start, end);
  if (start > 0) snip = '…' + snip;
  if (end < text.length) snip = snip + '…';
  const re = new RegExp(`(${escapeRegExp(q)})`, 'gi');
  return snip.replace(re, '<mark>$1</mark>');
}

function renderSectionResults(query) {
  const host = document.getElementById('section-results');
  if (!host) return;
  if (!query || !sectionIndex) {
    host.hidden = true;
    host.innerHTML = '';
    return;
  }
  const q = query.toLowerCase();
  const hits = [];
  for (const s of sectionIndex) {
    const idx = s._hay.indexOf(q);
    if (idx < 0) continue;
    const headHit = s.heading.toLowerCase().includes(q);
    hits.push({ s, headHit, idx });
  }
  hits.sort((a, b) => (b.headHit - a.headHit) || (a.idx - b.idx));
  const top = hits.slice(0, 12);

  if (!top.length) { host.hidden = true; host.innerHTML = ''; return; }

  host.hidden = false;
  host.innerHTML = `
    <div class="section-results-head">
      <h3>Found in <strong>${hits.length}</strong> section${hits.length !== 1 ? 's' : ''}</h3>
      <span class="text-xs text-ink-500 dark:text-ink-400 ml-auto">showing top ${Math.min(12, hits.length)}</span>
    </div>
    <ul class="section-result-list">
      ${top.map(({ s }) => `
        <li>
          <a href="${s.doc_url}#${s.anchor}">
            <span class="inline-flex items-center gap-1.5 px-2 py-0.5 text-[10px] font-mono font-semibold uppercase tracking-wider rounded-full bg-brand-100 text-brand-700 dark:bg-brand-500/15 dark:text-brand-300">${s.doc_category}</span>
            <span class="result-doc">${s.doc_title}</span>
            <span class="result-arrow" aria-hidden="true">›</span>
            <strong class="result-heading">${s.heading}</strong>
            <p class="result-snippet">${makeSnippet(s.text, q)}</p>
          </a>
        </li>
      `).join('')}
    </ul>
  `;
}

function applyFilter(query) {
  const q = query.trim().toLowerCase();
  const cat = activeCategory.toLowerCase();
  let anyVisible = false;

  for (const entry of cardIndex) {
    const matchesQuery = !q || score(entry, q) > 0;
    const matchesCat   = !cat || entry.cat === cat;
    const visible = matchesQuery && matchesCat;
    entry.el.hidden = !visible;
    if (visible) anyVisible = true;
    highlight(entry.el, q);
  }

  for (const g of groups) {
    const visibleInGroup = g.querySelectorAll('[data-card]:not([hidden])').length;
    g.hidden = visibleInGroup === 0;
  }

  const countEl = document.getElementById('search-count');
  if (countEl) {
    const visibleCards = cardIndex.filter((e) => !e.el.hidden).length;
    const showCount = !!q || !!cat;
    countEl.hidden = !showCount;
    countEl.textContent = showCount ? `${visibleCards} of ${cardIndex.length}` : '';
  }

  // Hide the "Recently updated" row whenever any filter / search is active.
  const recentRow = document.getElementById('recent-row');
  if (recentRow) recentRow.hidden = !!q || !!cat;

  if (q) ensureSectionIndex().then(() => renderSectionResults(q));
  else   renderSectionResults('');

  if (emptyState) emptyState.hidden = anyVisible || !!q;
}

/* ----------------------------------------------------------------------- *
 * Category filter chips                                                    *
 * ----------------------------------------------------------------------- */

(function setupChips() {
  const host = document.getElementById('category-chips');
  if (!host) return;
  const chips = Array.from(host.querySelectorAll('.chip'));

  function setActive(value) {
    activeCategory = value;
    for (const c of chips) {
      const on = (c.dataset.category ?? '') === value;
      c.setAttribute('aria-pressed', on ? 'true' : 'false');
      c.classList.toggle('chip-active', on);
      // Tailwind active style — toggle via classList.
      if (on) {
        c.className = 'chip chip-active inline-flex items-center gap-1.5 px-3 py-1.5 text-[12px] font-medium rounded-full bg-brand-600 text-white ring-1 ring-brand-600 hover:bg-brand-700 transition-colors';
      } else {
        c.className = 'chip inline-flex items-center gap-1.5 px-3 py-1.5 text-[12px] font-medium rounded-full bg-ink-100 dark:bg-ink-800/60 text-ink-700 dark:text-ink-300 ring-1 ring-ink-200/60 dark:ring-ink-700/60 hover:bg-ink-200 dark:hover:bg-ink-700/60 transition-colors';
      }
    }
    applyFilter(searchInput?.value ?? '');
  }

  for (const c of chips) {
    c.addEventListener('click', () => setActive(c.dataset.category ?? ''));
  }
})();

/* ----------------------------------------------------------------------- *
 * Sort modes                                                               *
 * ----------------------------------------------------------------------- */

(function setupSort() {
  const sel = document.getElementById('sort-select');
  if (!sel || !cards.length) return;

  // Snapshot the original DOM positions so we can restore "By category".
  const originalParents = new Map();
  for (const c of cards) originalParents.set(c, { parent: c.parentElement, next: c.nextSibling });

  // Read meta from the card's data + computed values for sort keys.
  const meta = new Map();
  for (const c of cards) {
    const sizeMatch = c.querySelector('[title="File size"]')?.textContent?.match(/[\d.]+/);
    const minsMatch = c.querySelector('[title="Estimated reading time"]')?.textContent?.match(/\d+/);
    const dateMatch = c.querySelector('[title="Last modified"]')?.textContent?.trim();
    meta.set(c, {
      title: (c.dataset.title || '').toLowerCase(),
      size:  sizeMatch ? parseFloat(sizeMatch[0]) : 0,
      mins:  minsMatch ? parseInt(minsMatch[0], 10) : 0,
      date:  dateMatch || '',
    });
  }

  // A single flat container we use whenever sort != 'category'.
  let flatHost = null;
  function ensureFlatHost() {
    if (flatHost) return flatHost;
    flatHost = document.createElement('div');
    flatHost.id = 'cards-flat';
    flatHost.className = 'grid sm:grid-cols-2 lg:grid-cols-3 gap-4';
    // Insert at the top of the docs section, after the toolbar.
    const docs = document.getElementById('docs');
    const ref  = document.getElementById('section-results') ||
                 document.getElementById('recent-row') ||
                 docs.querySelector('.doc-group');
    docs.insertBefore(flatHost, ref?.nextSibling || ref);
    return flatHost;
  }

  function restoreCategoryView() {
    if (flatHost) flatHost.hidden = true;
    for (const g of groups) g.hidden = false;
    for (const c of cards) {
      const o = originalParents.get(c);
      if (o?.parent && c.parentElement !== o.parent) {
        if (o.next && o.next.parentNode === o.parent) o.parent.insertBefore(c, o.next);
        else                                          o.parent.appendChild(c);
      }
    }
    applyFilter(searchInput?.value ?? '');
  }

  function flatSort(keyFn, asc = true) {
    const host = ensureFlatHost();
    host.hidden = false;
    for (const g of groups) g.hidden = true;
    const sorted = cards.slice().sort((a, b) => {
      const av = keyFn(meta.get(a)), bv = keyFn(meta.get(b));
      if (av < bv) return asc ? -1 : 1;
      if (av > bv) return asc ? 1 : -1;
      return 0;
    });
    for (const c of sorted) host.appendChild(c);
    applyFilter(searchInput?.value ?? '');
  }

  sel.addEventListener('change', (e) => {
    const v = e.target.value;
    switch (v) {
      case 'category': restoreCategoryView(); break;
      case 'recent':   flatSort((m) => m.date, false); break;
      case 'size':     flatSort((m) => m.size, true);  break;
      case 'reading':  flatSort((m) => m.mins, true);  break;
      case 'alpha':    flatSort((m) => m.title, true); break;
    }
  });
})();

let searchDebounce = null;
searchInput?.addEventListener('input', (e) => {
  clearTimeout(searchDebounce);
  searchDebounce = setTimeout(() => applyFilter(e.target.value), 50);
});
searchInput?.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    e.target.value = '';
    applyFilter('');
    e.target.blur();
  }
});

/* ----------------------------------------------------------------------- *
 * Keyboard shortcuts                                                       *
 * ----------------------------------------------------------------------- */

document.addEventListener('keydown', (e) => {
  const tag = (e.target?.tagName ?? '').toUpperCase();
  const inField = tag === 'INPUT' || tag === 'TEXTAREA' || e.target?.isContentEditable;

  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault();
    searchInput?.focus();
    searchInput?.select();
    return;
  }

  if (inField) return;

  if (e.key === '/') {
    e.preventDefault();
    searchInput?.focus();
  }
  if (e.key === 't' || e.key === 'T') {
    toggleTheme();
  }
});

/* ----------------------------------------------------------------------- *
 * Card keyboard navigation — arrow keys move focus across the doc grid    *
 * ----------------------------------------------------------------------- */

(function setupCardKeys() {
  const all = Array.from(document.querySelectorAll('[data-card]'));
  if (!all.length) return;

  const visibleCards = () => all.filter((c) => !c.hidden);

  function focusBy(deltaIdx, fromEl) {
    const v = visibleCards();
    if (!v.length) return;
    const i = v.indexOf(fromEl);
    const target = v[Math.min(v.length - 1, Math.max(0, i + deltaIdx))];
    target?.focus();
  }

  function focusVertical(dir, fromEl) {
    const v = visibleCards();
    const r = fromEl.getBoundingClientRect();
    const myCenterX = r.left + r.width / 2;
    const myCenterY = r.top + r.height / 2;
    const candidates = v
      .filter((c) => c !== fromEl)
      .map((c) => {
        const cr = c.getBoundingClientRect();
        return { c, cr, dy: (cr.top + cr.height / 2) - myCenterY };
      })
      .filter(({ dy }) => (dir === 'down' ? dy > 8 : dy < -8));
    if (!candidates.length) return;
    candidates.sort((a, b) => Math.abs(a.dy) - Math.abs(b.dy));
    const closestDy = candidates[0].dy;
    const sameRow = candidates.filter(({ dy }) => Math.abs(dy - closestDy) < 16);
    sameRow.sort((a, b) => {
      const ax = a.cr.left + a.cr.width / 2;
      const bx = b.cr.left + b.cr.width / 2;
      return Math.abs(ax - myCenterX) - Math.abs(bx - myCenterX);
    });
    sameRow[0].c.focus();
  }

  document.addEventListener('keydown', (e) => {
    const tag = (e.target?.tagName ?? '').toUpperCase();
    if (tag === 'INPUT' || tag === 'TEXTAREA' || e.target?.isContentEditable) return;
    const focused = document.activeElement;
    const onCard = focused && focused.matches?.('[data-card]');
    if (!onCard) {
      if (['ArrowDown', 'ArrowRight'].includes(e.key)) {
        const first = visibleCards()[0];
        if (first) { e.preventDefault(); first.focus(); }
      }
      return;
    }
    switch (e.key) {
      case 'ArrowLeft':  e.preventDefault(); focusBy(-1, focused);              break;
      case 'ArrowRight': e.preventDefault(); focusBy( 1, focused);              break;
      case 'ArrowUp':    e.preventDefault(); focusVertical('up',   focused);    break;
      case 'ArrowDown':  e.preventDefault(); focusVertical('down', focused);    break;
      case 'Home':       e.preventDefault(); visibleCards()[0]?.focus();        break;
      case 'End':        e.preventDefault();
                         { const v = visibleCards(); v[v.length - 1]?.focus(); } break;
    }
  });
})();

/* ----------------------------------------------------------------------- *
 * Service worker — registers from the site root so scope covers /docs/*    *
 * ----------------------------------------------------------------------- */

if ('serviceWorker' in navigator && location.protocol !== 'file:') {
  window.addEventListener('load', () => {
    navigator.serviceWorker
      .register('/sw.js', { scope: '/' })
      .catch((err) => console.warn('[edevkit] service worker registration failed:', err));
  });
}

/* ----------------------------------------------------------------------- *
 * View Transitions API enhancement (Chrome / Edge / Safari TP)             *
 * ----------------------------------------------------------------------- */

if ('startViewTransition' in document) {
  const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
  for (const card of cards) {
    card.addEventListener('click', (e) => {
      if (e.metaKey || e.ctrlKey || e.button === 1) return;
      if (reducedMotion.matches) return;
      const href = card.getAttribute('href');
      if (!href) return;
      e.preventDefault();
      document.startViewTransition(() => {
        window.location.href = href;
      });
    });
  }
}
