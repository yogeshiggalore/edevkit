/* edevkit USB Book — page helpers
   ----------------------------------------------------------------------------
   Self-contained: theme toggle uses the same `edev_theme` localStorage key
   as the docs hub (so flipping theme on the hub or the book is consistent).

   Three responsibilities:
     1. theme toggle
     2. lazy-load Mermaid only on pages that have .mermaid blocks
     3. (no sidebar in the new layout, so no current-chapter highlighter)
*/

(function () {
  'use strict';

  const KEY = 'edev_theme';
  const root = document.documentElement;

  function effectiveDark() {
    const a = root.getAttribute('data-theme');
    if (a === 'dark') return true;
    if (a === 'light') return false;
    return matchMedia('(prefers-color-scheme: dark)').matches;
  }

  function applyStored() {
    const t = localStorage.getItem(KEY);
    if (t === 'dark' || t === 'light') root.setAttribute('data-theme', t);
    else root.removeAttribute('data-theme');
    syncIcons();
  }

  function syncIcons() {
    const dark = effectiveDark();
    document.querySelectorAll('.theme-btn .sun').forEach(el => el.style.display = dark ? 'block' : 'none');
    document.querySelectorAll('.theme-btn .moon').forEach(el => el.style.display = dark ? 'none' : 'block');
  }

  function toggleTheme() {
    const cur = localStorage.getItem(KEY);
    // light → dark → system → light  (matches hub script.js)
    let next;
    if (cur === 'light') next = 'dark';
    else if (cur === 'dark') next = null;
    else next = 'light';
    if (next) { localStorage.setItem(KEY, next); root.setAttribute('data-theme', next); }
    else      { localStorage.removeItem(KEY);    root.removeAttribute('data-theme'); }
    syncIcons();
  }

  function maybeLoadMermaid() {
    if (!document.querySelector('.mermaid')) return;
    const dark = effectiveDark();
    const s = document.createElement('script');
    s.src = 'https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.min.js';
    s.onload = () => {
      window.mermaid.initialize({
        startOnLoad: true,
        theme: dark ? 'dark' : 'default',
        securityLevel: 'loose',
        flowchart: { curve: 'basis' },
        sequence: { mirrorActors: false, showSequenceNumbers: true },
      });
    };
    document.head.appendChild(s);
  }

  // ── boot ──────────────────────────────────────────────────────────────────
  applyStored();   // pre-paint script in <head> already set data-theme; this just syncs icons
  document.addEventListener('DOMContentLoaded', () => {
    document.querySelectorAll('.theme-btn').forEach(btn => btn.addEventListener('click', toggleTheme));
    maybeLoadMermaid();

    // T = toggle theme (skip when typing in a field)
    document.addEventListener('keydown', (e) => {
      if ((e.key === 't' || e.key === 'T') && !e.metaKey && !e.ctrlKey) {
        const tag = (e.target?.tagName ?? '').toUpperCase();
        if (tag === 'INPUT' || tag === 'TEXTAREA' || e.target?.isContentEditable) return;
        toggleTheme();
      }
    });
  });

  // React to OS theme changes when user is on system
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
    if (!localStorage.getItem(KEY)) applyStored();
  });
})();
