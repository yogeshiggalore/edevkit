// edevkit1 docs hub — vanilla ES module
// - theme toggle (light / dark / system) persisted in localStorage
// - client-side fuzzy search across cards
// - keyboard shortcuts: /, Cmd+K / Ctrl+K, T, Esc

const STORAGE_KEY = 'edev_theme';
const root = document.documentElement;

/* ----------------------------------------------------------------------- *
 * Theme                                                                    *
 * ----------------------------------------------------------------------- */

function applyStoredTheme() {
  const stored = localStorage.getItem(STORAGE_KEY);
  if (stored === 'dark' || stored === 'light') {
    root.setAttribute('data-theme', stored);
  } else {
    // 'system' — let CSS prefers-color-scheme decide
    root.removeAttribute('data-theme');
  }
  syncThemeIcons();
}

function syncThemeIcons() {
  const dark = effectiveTheme() === 'dark';
  const sun  = document.querySelector('#theme-toggle .sun');
  const moon = document.querySelector('#theme-toggle .moon');
  if (sun)  sun.style.display  = dark ? 'block' : 'none';
  if (moon) moon.style.display = dark ? 'none'  : 'block';
}

function nextTheme(current) {
  // light → dark → system → light
  if (current === 'light') return 'dark';
  if (current === 'dark')  return 'system';
  return 'light';
}

function effectiveTheme() {
  const stored = localStorage.getItem(STORAGE_KEY);
  if (stored === 'dark' || stored === 'light') return stored;
  return matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

function toggleTheme() {
  const stored = localStorage.getItem(STORAGE_KEY) ?? 'system';
  const next = nextTheme(stored);
  if (next === 'system') {
    localStorage.removeItem(STORAGE_KEY);
    root.removeAttribute('data-theme');
  } else {
    localStorage.setItem(STORAGE_KEY, next);
    root.setAttribute('data-theme', next);
  }
  syncThemeIcons();
}

applyStoredTheme();

document.getElementById('theme-toggle')?.addEventListener('click', toggleTheme);

// React to OS-level theme changes when the user is on "system"
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
  if (!localStorage.getItem(STORAGE_KEY)) applyStoredTheme();
});

/* ----------------------------------------------------------------------- *
 * Search                                                                   *
 * ----------------------------------------------------------------------- */

const searchInput = document.getElementById('search-input');
const cards       = Array.from(document.querySelectorAll('[data-card]'));
const groups      = Array.from(document.querySelectorAll('[data-group]'));
const emptyState  = document.getElementById('empty-state');

// Index every card's searchable text up front for fast filtering.
const cardIndex = cards.map((card) => ({
  el:      card,
  title:   (card.dataset.title    ?? '').toLowerCase(),
  summary: (card.dataset.summary  ?? '').toLowerCase(),
  cat:     (card.dataset.category ?? '').toLowerCase(),
}));

function escapeRegExp(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function highlight(card, query) {
  // Reset prior highlights.
  card.querySelectorAll('mark').forEach((m) => {
    const t = document.createTextNode(m.textContent);
    m.parentNode.replaceChild(t, m);
  });
  if (!query) return;
  const re = new RegExp(`(${escapeRegExp(query)})`, 'gi');
  for (const sel of ['h3', 'p.summary']) {
    const node = card.querySelector(sel);
    if (!node) continue;
    const text = node.textContent;
    if (!re.test(text)) continue;
    re.lastIndex = 0;
    node.innerHTML = text.replace(re, '<mark>$1</mark>');
  }
}

function score(entry, query) {
  if (!query) return 1;
  const q = query.toLowerCase();
  let s = 0;
  if (entry.title.includes(q))   s += 5;
  if (entry.cat.includes(q))     s += 3;
  if (entry.summary.includes(q)) s += 2;
  // sub-token matches — split query on whitespace
  const parts = q.split(/\s+/).filter(Boolean);
  for (const p of parts) {
    if (entry.title.includes(p))   s += 1;
    if (entry.summary.includes(p)) s += 1;
  }
  return s;
}

function applyFilter(query) {
  const q = query.trim().toLowerCase();
  let anyVisible = false;

  for (const entry of cardIndex) {
    const s = score(entry, q);
    const visible = !q || s > 0;
    entry.el.hidden = !visible;
    if (visible) anyVisible = true;
    highlight(entry.el, q);
  }

  // Hide empty groups so the page doesn't look broken.
  for (const g of groups) {
    const visibleInGroup = g.querySelectorAll('[data-card]:not([hidden])').length;
    g.hidden = visibleInGroup === 0;
  }

  if (emptyState) emptyState.hidden = anyVisible;
}

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
  // Skip when the user is typing into a form field.
  const tag = (e.target?.tagName ?? '').toUpperCase();
  const inField = tag === 'INPUT' || tag === 'TEXTAREA' || e.target?.isContentEditable;

  // Cmd/Ctrl+K → focus search
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'k') {
    e.preventDefault();
    searchInput?.focus();
    searchInput?.select();
    return;
  }

  if (inField) return;

  // / → focus search
  if (e.key === '/') {
    e.preventDefault();
    searchInput?.focus();
  }

  // T → toggle theme
  if (e.key === 't' || e.key === 'T') {
    toggleTheme();
  }
});

/* ----------------------------------------------------------------------- *
 * View Transitions API enhancement (Chrome / Edge / Safari TP)             *
 * Falls back gracefully when unsupported.                                  *
 * ----------------------------------------------------------------------- */

if ('startViewTransition' in document) {
  for (const card of cards) {
    card.addEventListener('click', (e) => {
      // Let middle-click / cmd-click open in new tab without animation
      if (e.metaKey || e.ctrlKey || e.button === 1) return;
      const href = card.getAttribute('href');
      if (!href) return;
      e.preventDefault();
      document.startViewTransition(() => {
        window.location.href = href;
      });
    });
  }
}
