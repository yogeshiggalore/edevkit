// edevkit docs — minimal self-contained syntax highlighter.
// No CDN, no dependencies. ~200 lines. Intentionally small — we want
// "looks like real code" not "perfectly correct" tokenization.
//
// Strategy: wrap each <pre><code>…</code></pre> body's textContent in
// language-aware <span class="tok-…"> tags. Auto-detect language by
// content sniffing (shebang, opening token, file extension hint via
// data-lang or class="language-x"). Emits classes:
//   tok-cmt  comment
//   tok-str  string literal
//   tok-num  number
//   tok-kw   keyword
//   tok-typ  type name (C/C++ in particular)
//   tok-pp   preprocessor (#include, #define)
//   tok-fn   function call
//   tok-pun  punctuation accent (rare)
//
// CSS lives in /static/syntax.css.
(function () {
  if (window.__edevkitSyntaxLoaded) return;
  window.__edevkitSyntaxLoaded = true;

  const KW = {
    c: new Set([
      'auto','break','case','char','const','continue','default','do','double',
      'else','enum','extern','float','for','goto','if','inline','int','long',
      'register','restrict','return','short','signed','sizeof','static','struct',
      'switch','typedef','union','unsigned','void','volatile','while','_Alignas',
      '_Alignof','_Atomic','_Bool','_Complex','_Generic','_Imaginary','_Noreturn',
      '_Static_assert','_Thread_local','true','false','NULL','bool',
    ]),
    cpp: new Set([
      'class','public','private','protected','virtual','override','final',
      'namespace','template','typename','using','this','new','delete','nullptr',
    ]),
    js: new Set([
      'async','await','break','case','catch','class','const','continue','debugger',
      'default','delete','do','else','export','extends','finally','for','from',
      'function','get','if','import','in','instanceof','let','new','null','of',
      'return','set','static','super','switch','this','throw','try','typeof',
      'undefined','var','void','while','with','yield','true','false',
    ]),
    py: new Set([
      'False','None','True','and','as','assert','async','await','break','class',
      'continue','def','del','elif','else','except','finally','for','from',
      'global','if','import','in','is','lambda','nonlocal','not','or','pass',
      'raise','return','try','while','with','yield','self','cls','match','case',
    ]),
    sh: new Set([
      'if','then','else','elif','fi','for','do','done','while','until','case','esac',
      'function','return','in','true','false','break','continue','export','source',
      'echo','cd','pwd','ls','mkdir','rm','cp','mv','cat','grep','sed','awk',
      'find','curl','wget','git','make','cmake','ninja','python','python3','pip',
      'west','probe-rs','mcumgr','kibot','cargo','tauri',
    ]),
    pio: new Set([
      'jmp','wait','in','out','push','pull','mov','irq','set','nop','side','delay',
      'pins','pindirs','x','y','osr','isr','status','null','exec','pc',
      'block','noblock','iffull','ifempty',
    ]),
  };

  const C_TYPES = new Set([
    'uint8_t','uint16_t','uint32_t','uint64_t',
    'int8_t','int16_t','int32_t','int64_t',
    'size_t','ssize_t','ptrdiff_t','intptr_t','uintptr_t',
    'pio_sm_config','PIO','dma_channel_config',
    'k_msgq','k_pipe','k_thread','ring_buf','k_sem',
  ]);

  function escapeHtml(s) {
    return s
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;');
  }

  function detectLang(code, hint) {
    if (hint) return hint;
    const t = code.trimStart();
    if (/^#!/.test(t)) {
      if (/python/.test(t.slice(0, 80))) return 'py';
      return 'sh';
    }
    if (/^\.program\s+\w/.test(t) || /^\.side_set\s/.test(t)) return 'pio';
    if (/^(#include|#define|#ifdef|#ifndef|#pragma)/m.test(t)) return 'c';
    if (/^(import|from|def|class)\s/m.test(t)) return 'py';
    if (/^---\s*$/m.test(t) || /^[a-zA-Z_]+:\s*[\w"'\-\d]/m.test(t)) {
      // Could be YAML — but distinguish from "label:" in C.
      // YAML usually has multiple top-level keys.
      const yamlKeys = (t.match(/^[a-zA-Z_]+:\s/gm) || []).length;
      if (yamlKeys >= 2) return 'yaml';
    }
    if (/^(west|cargo|cmake|make|probe-rs|mcumgr|python3?|pip|git|cd|ls|mkdir|rm|kibot|kicad-cli|sudo|brew|apt)\s/m.test(t)) return 'sh';
    if (/^\$\s/m.test(t)) return 'sh';
    if (/\bvoid\s+\w+\s*\(|\bstatic\s+\w+\s+\w+\s*\(|\b#include\b/.test(t)) return 'c';
    if (/^\{\s*$|^\[\s*$/.test(t) && /[\{\}\[\]"]/.test(t)) return 'json';
    return null;
  }

  /* -- per-language highlighters -------------------------------------- */

  function highlightCFamily(code, kw) {
    // Order: comments → strings → preprocessor → numbers → keywords/types → identifiers (function calls)
    return splitAndJoin(code, [
      // /* block comment */
      [/\/\*[\s\S]*?\*\//g, (m) => `<span class="tok-cmt">${escapeHtml(m)}</span>`],
      // // line comment
      [/\/\/[^\n]*/g, (m) => `<span class="tok-cmt">${escapeHtml(m)}</span>`],
      // String / char literals
      [/(["'])(?:\\.|(?!\1)[^\\\n])*\1/g, (m) => `<span class="tok-str">${escapeHtml(m)}</span>`],
      // Preprocessor (line starting with #)
      [/^[ \t]*#[ \t]*\w+(?:[ \t]+[^\n]*)?/gm, (m) => `<span class="tok-pp">${escapeHtml(m)}</span>`],
      // Numbers (hex / decimal / float)
      [/\b(?:0[xX][0-9a-fA-F]+|0b[01]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)[uUlLfF]*\b/g,
       (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
      // Identifiers — keyword / type / function
      [/\b[a-zA-Z_]\w*\b(\s*\()?/g, (m, paren) => {
        const word = paren ? m.slice(0, -paren.length) : m;
        if (kw.has(word)) return `<span class="tok-kw">${escapeHtml(word)}</span>${paren || ''}`;
        if (C_TYPES.has(word) || /^[A-Z_][A-Z0-9_]+$/.test(word) /* MACRO_CASE */)
          return `<span class="tok-typ">${escapeHtml(word)}</span>${paren || ''}`;
        if (paren) return `<span class="tok-fn">${escapeHtml(word)}</span>${paren}`;
        return escapeHtml(m);
      }],
    ]);
  }

  function highlightPython(code) {
    return splitAndJoin(code, [
      [/(#[^\n]*)/g,                                (m) => `<span class="tok-cmt">${escapeHtml(m)}</span>`],
      [/(['"])(?:\\.|(?!\1).)*\1/g,                 (m) => `<span class="tok-str">${escapeHtml(m)}</span>`],
      [/\b(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?)\b/g,  (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
      [/\b[a-zA-Z_]\w*\b(\s*\()?/g, (m, paren) => {
        const word = paren ? m.slice(0, -paren.length) : m;
        if (KW.py.has(word)) return `<span class="tok-kw">${escapeHtml(word)}</span>${paren || ''}`;
        if (paren) return `<span class="tok-fn">${escapeHtml(word)}</span>${paren}`;
        return escapeHtml(m);
      }],
    ]);
  }

  function highlightShell(code) {
    return splitAndJoin(code, [
      [/(#[^\n]*)/g,                          (m) => `<span class="tok-cmt">${escapeHtml(m)}</span>`],
      [/(["'])(?:\\.|(?!\1).)*\1/g,           (m) => `<span class="tok-str">${escapeHtml(m)}</span>`],
      [/\$\{?[a-zA-Z_][a-zA-Z0-9_]*\}?/g,     (m) => `<span class="tok-typ">${escapeHtml(m)}</span>`],
      [/\b\d+\b/g,                            (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
      [/^\s*[-]{1,2}[a-zA-Z][\w-]*/gm,        (m) => `<span class="tok-kw">${escapeHtml(m)}</span>`],
      [/(?<=\s|^)[-]{1,2}[a-zA-Z][\w-]*/g,    (m) => `<span class="tok-kw">${escapeHtml(m)}</span>`],
      [/\b[a-zA-Z_][\w-]*\b/g, (m) => {
        if (KW.sh.has(m)) return `<span class="tok-kw">${escapeHtml(m)}</span>`;
        return escapeHtml(m);
      }],
    ]);
  }

  function highlightYaml(code) {
    return splitAndJoin(code, [
      [/(#[^\n]*)/g,                         (m) => `<span class="tok-cmt">${escapeHtml(m)}</span>`],
      [/(["'])(?:\\.|(?!\1).)*\1/g,          (m) => `<span class="tok-str">${escapeHtml(m)}</span>`],
      [/^[ \t]*([a-zA-Z_][\w-]*)(?=:)/gm,    (m) => `<span class="tok-kw">${escapeHtml(m)}</span>`],
      [/\b(?:true|false|null|yes|no|on|off)\b/gi,
                                              (m) => `<span class="tok-kw">${escapeHtml(m)}</span>`],
      [/\b(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?)\b/g,
                                              (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
    ]);
  }

  function highlightJson(code) {
    return splitAndJoin(code, [
      [/(["])(?:\\.|(?!\1).)*\1(\s*:)?/g, (m, _q, colon) => {
        if (colon) return `<span class="tok-kw">${escapeHtml(m.slice(0, -colon.length))}</span>${colon}`;
        return `<span class="tok-str">${escapeHtml(m)}</span>`;
      }],
      [/\b(?:true|false|null)\b/g, (m) => `<span class="tok-kw">${escapeHtml(m)}</span>`],
      [/-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b/g, (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
    ]);
  }

  function highlightPio(code) {
    return splitAndJoin(code, [
      [/(;[^\n]*)/g,                         (m) => `<span class="tok-cmt">${escapeHtml(m)}</span>`],
      [/^\.[a-z_]+/gm,                       (m) => `<span class="tok-pp">${escapeHtml(m)}</span>`],
      [/\[(\d+)\]/g,                         (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
      [/\b(?:0[xX][0-9a-fA-F]+|\d+)\b/g,     (m) => `<span class="tok-num">${escapeHtml(m)}</span>`],
      [/^[ \t]*([a-zA-Z_]\w*):/gm,           (m) => `<span class="tok-typ">${escapeHtml(m)}</span>`],
      [/\b[a-zA-Z_!][\w!]*\b/g, (m) => {
        const lc = m.toLowerCase();
        if (KW.pio.has(lc)) return `<span class="tok-kw">${escapeHtml(m)}</span>`;
        return escapeHtml(m);
      }],
    ]);
  }

  /* Tokenize using a list of (regex, replacer) pairs.
     Important: avoid re-processing already-tokenized HTML by interleaving
     plain segments (escaped) with replaced segments (already HTML). */
  function splitAndJoin(input, rules) {
    // We process rules in order: each rule operates on the CURRENT tokens
    // (where some are wrapped HTML, others raw). We only run regex against
    // raw segments and leave wrapped ones untouched.
    let tokens = [{ raw: true, text: input }];
    for (const [re, fn] of rules) {
      const next = [];
      for (const t of tokens) {
        if (!t.raw) { next.push(t); continue; }
        let lastEnd = 0;
        const text = t.text;
        re.lastIndex = 0;
        let m;
        const reGlobal = re.global ? re : new RegExp(re.source, re.flags + (re.flags.includes('g') ? '' : 'g'));
        while ((m = reGlobal.exec(text)) !== null) {
          if (m.index > lastEnd) next.push({ raw: true, text: text.slice(lastEnd, m.index) });
          next.push({ raw: false, text: fn(...m) });
          lastEnd = m.index + m[0].length;
          if (m[0].length === 0) reGlobal.lastIndex++;
        }
        if (lastEnd < text.length) next.push({ raw: true, text: text.slice(lastEnd) });
      }
      tokens = next;
    }
    return tokens.map((t) => (t.raw ? escapeHtml(t.text) : t.text)).join('');
  }

  /* -- entry point ---------------------------------------------------- */

  function langFromClass(el) {
    const m = (el.className || '').match(/language-(\w+)/);
    if (m) return m[1];
    if (el.dataset.lang) return el.dataset.lang;
    return null;
  }

  function highlightOne(codeEl) {
    if (codeEl.dataset.hl === 'done') return;
    const raw = codeEl.textContent;
    const hint = langFromClass(codeEl);
    const lang = detectLang(raw, hint);
    let html;
    switch (lang) {
      case 'c': case 'cpp':  html = highlightCFamily(raw, KW.c); break;
      case 'js':             html = highlightCFamily(raw, KW.js); break;
      case 'py':             html = highlightPython(raw); break;
      case 'sh': case 'bash':html = highlightShell(raw); break;
      case 'yaml':           html = highlightYaml(raw); break;
      case 'json':           html = highlightJson(raw); break;
      case 'pio':            html = highlightPio(raw); break;
      default:               return;
    }
    codeEl.innerHTML = html;
    codeEl.dataset.hl = 'done';
    codeEl.classList.add('hl-' + (lang || 'auto'));
  }

  function highlightAll(root = document) {
    root.querySelectorAll('pre > code').forEach(highlightOne);
  }

  /* -- copy buttons (paired with highlighting) ----------------------- */

  function addCopyButton(pre) {
    if (pre.dataset.copy === 'done') return;
    pre.dataset.copy = 'done';
    pre.style.position = pre.style.position || 'relative';
    const btn = document.createElement('button');
    btn.className = 'code-copy';
    btn.type = 'button';
    btn.setAttribute('aria-label', 'Copy code');
    btn.innerHTML = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg><span>Copy</span>`;
    btn.addEventListener('click', async () => {
      const code = pre.querySelector('code')?.textContent ?? pre.textContent;
      try {
        await navigator.clipboard.writeText(code);
        btn.classList.add('ok');
        btn.querySelector('span').textContent = 'Copied';
        setTimeout(() => {
          btn.classList.remove('ok');
          btn.querySelector('span').textContent = 'Copy';
        }, 1400);
      } catch {
        btn.querySelector('span').textContent = 'Failed';
      }
    });
    pre.appendChild(btn);
  }

  function addCopyButtons(root = document) {
    root.querySelectorAll('pre').forEach(addCopyButton);
  }

  /* -- reading progress bar ------------------------------------------ */

  function installProgressBar() {
    if (document.getElementById('edevkit-progress')) return;
    // Skip on extremely short docs (no value) — heuristic.
    if (document.body.scrollHeight < window.innerHeight * 1.5) return;
    const bar = document.createElement('div');
    bar.id = 'edevkit-progress';
    bar.setAttribute('aria-hidden', 'true');
    document.body.appendChild(bar);
    let raf = 0;
    const update = () => {
      const max = document.documentElement.scrollHeight - window.innerHeight;
      const pct = max > 0 ? Math.min(100, (window.scrollY / max) * 100) : 0;
      bar.style.transform = `scaleX(${pct / 100})`;
      raf = 0;
    };
    window.addEventListener('scroll', () => {
      if (!raf) raf = requestAnimationFrame(update);
    }, { passive: true });
    window.addEventListener('resize', update, { passive: true });
    update();
  }

  /* -- heading anchor § affordance ----------------------------------- */

  function installHeadingAnchors() {
    const headings = document.querySelectorAll('h2[id], h3[id]');
    for (const h of headings) {
      if (h.querySelector(':scope > .heading-anchor')) continue;
      const a = document.createElement('a');
      a.className = 'heading-anchor';
      a.href = '#' + h.id;
      a.setAttribute('aria-label', `Link to “${h.textContent.trim()}”`);
      a.textContent = '§';
      a.addEventListener('click', (e) => {
        if (!navigator.clipboard) return; // let default anchor jump happen
        // copy the absolute URL with hash; still allow the smooth scroll
        e.preventDefault();
        const url = location.origin + location.pathname + '#' + h.id;
        navigator.clipboard.writeText(url).catch(() => {});
        history.replaceState(null, '', '#' + h.id);
        h.scrollIntoView({ behavior: 'smooth', block: 'start' });
        a.classList.add('copied');
        setTimeout(() => a.classList.remove('copied'), 1200);
      });
      h.appendChild(a);
    }
  }

  /* -- Breadcrumbs (top of doc body) --------------------------------- */

  // Same prefix → category mapping as server.py CATEGORIES.
  const CATEGORY_MAP = [
    ['edevkit1',      { name: 'Design Spec',  icon: '📐' }],
    ['installation',  { name: 'Onboarding',   icon: '🧭' }],
    ['shell_helpers', { name: 'Workflow',     icon: '⚙️' }],
    ['arm',           { name: 'Architecture', icon: '🧠' }],
    ['usb',           { name: 'USB stack',    icon: '🔌' }],
    ['debug',         { name: 'Debug',        icon: '🔍' }],
    ['proto',         { name: 'Protocols',    icon: '🔁' }],
    ['platform',      { name: 'Platform',     icon: '🏗️' }],
    ['bringup',       { name: 'Bring-up',     icon: '⚡' }],
    ['recovery',      { name: 'Recovery',     icon: '🛟' }],
    ['firmware',      { name: 'Firmware',     icon: '🔧' }],
    ['hardware',      { name: 'Hardware',     icon: '📟' }],
    ['host',          { name: 'Host tools',   icon: '💻' }],
    ['enclosure',     { name: 'Enclosure',    icon: '📦' }],
  ];

  function categoryFor(stem) {
    for (const [key, meta] of CATEGORY_MAP) {
      if (stem === key || stem.startsWith(key + '_') || stem.startsWith(key + '-')) return meta;
    }
    return { name: 'General', icon: '📄' };
  }

  function installBreadcrumbs() {
    if (document.querySelector('.doc-breadcrumbs')) return;
    // Find the doc title — look for the first <h1> in <header.hero> or any h1.
    const h1 = document.querySelector('header.hero h1, .hero h1, body > .wrap h1, body h1');
    if (!h1) return;
    // Determine slug from URL.
    const path = location.pathname || '';
    const file = path.split('/').pop() || '';
    const stem = file.replace(/\.html?$/, '');
    if (!stem) return;
    const cat = categoryFor(stem);
    const home = (location.protocol === 'file:') ? null : '/';
    const titleText = h1.textContent.trim().slice(0, 60);

    const nav = document.createElement('nav');
    nav.className = 'doc-breadcrumbs';
    nav.setAttribute('aria-label', 'Breadcrumb');
    nav.innerHTML = `
      ${home ? `<a href="${home}">edevkit</a><span class="sep">›</span>` : '<span>edevkit</span><span class="sep">›</span>'}
      <span>${cat.icon} ${cat.name}</span>
      <span class="sep">›</span>
      <span class="here">${titleText}</span>
    `;
    h1.parentNode.insertBefore(nav, h1);
  }

  /* -- Doc-page mini search box ------------------------------------- */

  function installMiniSearch() {
    if (document.querySelector('.doc-mini-search')) return;
    if (location.protocol === 'file:') return;  // hub URL won't work via file://
    // Insert near the top of the doc body. Prefer just below breadcrumbs / above hero.
    const target = document.querySelector('header.hero, .hero, body > .wrap > h1, body h1');
    if (!target) return;
    const wrap = document.createElement('form');
    wrap.className = 'doc-mini-search';
    wrap.setAttribute('role', 'search');
    wrap.action = '/';
    wrap.method = 'GET';
    wrap.innerHTML = `
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/></svg>
      <input type="search" name="q" placeholder="Search all docs…" aria-label="Search all docs" autocomplete="off">
      <kbd>⏎</kbd>
    `;
    wrap.style.margin = '0 0 14px';
    // Place between any breadcrumbs we just inserted and the H1.
    const before = document.querySelector('.doc-breadcrumbs');
    if (before && before.parentNode === target.parentNode) {
      target.parentNode.insertBefore(wrap, target);
    } else if (target.tagName === 'HEADER') {
      target.parentNode.insertBefore(wrap, target);
    } else {
      target.parentNode.insertBefore(wrap, target);
    }
    // On submit, navigate to the hub with the query as a hash so future
    // hub JS can pre-fill (we don't actually parse it client-side yet).
    wrap.addEventListener('submit', (e) => {
      e.preventDefault();
      const q = wrap.querySelector('input').value.trim();
      const url = q ? `/?q=${encodeURIComponent(q)}` : '/';
      location.href = url;
    });
  }

  /* -- Floating TOC right-rail (≥1280px) ---------------------------- */

  function installFloatingToc() {
    if (window.matchMedia('(max-width: 1199px)').matches) return;
    if (document.querySelector('.doc-toc-rail')) return;

    // Skip if the doc already has its own sticky/sidebar TOC.
    // (edevkit1.html has a built-in `nav.toc` on the left as part of its
    //  `.layout` grid — don't duplicate.)
    const existing = document.querySelector('nav.toc');
    if (existing) {
      const cs = window.getComputedStyle(existing);
      if (cs.position === 'sticky' || cs.position === 'fixed') return;
    }

    const headings = Array.from(document.querySelectorAll('h2[id]'));
    if (headings.length < 3) return;  // not worth showing for very short docs

    const rail = document.createElement('aside');
    rail.className = 'doc-toc-rail';
    rail.setAttribute('aria-label', 'On this page');
    rail.innerHTML = `
      <h2 class="toc-title">On this page</h2>
      <ul>
        ${headings.map((h) => `
          <li><a href="#${h.id}" data-anchor="${h.id}">${h.textContent.trim()}</a></li>
        `).join('')}
      </ul>
    `;
    document.body.appendChild(rail);

    // Scrollspy: highlight the active heading.
    const links = new Map();
    for (const a of rail.querySelectorAll('a[data-anchor]')) {
      links.set(a.dataset.anchor, a);
    }
    const visible = new Set();
    const setActive = (id) => {
      for (const [k, a] of links) a.classList.toggle('active', k === id);
    };
    const observer = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          if (e.isIntersecting) visible.add(e.target.id);
          else                  visible.delete(e.target.id);
        }
        const first = headings.find((h) => visible.has(h.id));
        if (first) setActive(first.id);
      },
      { rootMargin: '-15% 0px -70% 0px', threshold: 0 }
    );
    for (const h of headings) observer.observe(h);

    // Hide rail when window resizes below breakpoint.
    matchMedia('(min-width: 1200px)').addEventListener('change', (ev) => {
      rail.style.display = ev.matches ? '' : 'none';
    });
  }

  /* -- Inject Tailwind link for use in injected UI (no-op if file 404s) -- */

  function ensureTailwindLink() {
    // Only when served via http(s); file:// can't reach /static reliably from
    // arbitrary working directories. We use a relative path so http-served
    // /docs/foo.html → /static/tailwind.css resolves correctly.
    if (location.protocol === 'file:') return;
    if (document.getElementById('edevkit-tailwind')) return;
    const link = document.createElement('link');
    link.id = 'edevkit-tailwind';
    link.rel = 'stylesheet';
    link.href = '../static/tailwind.css';
    // Don't block render if file isn't there yet.
    link.media = 'all';
    link.onerror = () => link.remove();
    document.head.appendChild(link);
  }

  function bootstrap() {
    ensureTailwindLink();
    highlightAll();
    addCopyButtons();
    installProgressBar();
    installHeadingAnchors();
    installBreadcrumbs();
    installMiniSearch();
    installFloatingToc();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', bootstrap);
  } else {
    bootstrap();
  }

  // Expose for ad-hoc rerun on dynamically inserted code blocks.
  window.edevkitSyntax = {
    highlightAll, addCopyButtons,
    installProgressBar, installHeadingAnchors,
    installBreadcrumbs, installMiniSearch, installFloatingToc,
  };
})();
