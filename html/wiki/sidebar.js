(function () {
  var CUR_ID = window.WIKI_CUR_ID || '';

  // ── 折叠面板 ──────────────────────────────────────────────────────
  function initToggle(navId, btnId, lsKey, iconOpen, iconClosed) {
    var nav = document.getElementById(navId);
    var btn = document.getElementById(btnId);
    if (!nav || !btn) return;
    var collapsed = localStorage.getItem(lsKey) === '1';
    if (collapsed) nav.classList.add('collapsed');
    btn.textContent = collapsed ? iconClosed : iconOpen;
    btn.addEventListener('click', function () {
      var now = nav.classList.toggle('collapsed');
      localStorage.setItem(lsKey, now ? '1' : '0');
      btn.textContent = now ? iconClosed : iconOpen;
    });
  }

  // sidebar: ◀ 折叠 / ▶ 展开；toc: ▶ 折叠 / ◀ 展开
  initToggle('sidebar', 'sb-toggle',  'wiki-sb',  '\u25C0', '\u25B6');
  initToggle('toc',     'toc-toggle', 'wiki-toc', '\u25B6', '\u25C0');

  // ── 左侧文章目录 ──────────────────────────────────────────────────
  function escH(s) {
    return ('' + s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;')
      .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  function artUrl(a) {
    var c = a.category || '';
    return c
      ? '/wiki/' + encodeURIComponent(c) + '/' + a.id + '.html'
      : '/wiki/' + a.id + '.html';
  }

  function renderChildren(bycat, parent, depth) {
    var prefix = parent ? parent + '/' : '';
    var h = '';
    Object.keys(bycat).sort().forEach(function (cat) {
      if (cat === parent || cat === '') return;
      var ok = parent
        ? (cat.indexOf(prefix) === 0 && cat.slice(prefix.length).indexOf('/') === -1)
        : (cat.indexOf('/') === -1);
      if (!ok) return;
      var label = cat.split('/').pop();
      var pi = (10 + depth * 14) + 'px';
      h += '<div class="st-cat" style="padding-left:' + pi + '">' + escH(label) + '</div>';
      (bycat[cat] || []).forEach(function (a) {
        var ai = (22 + depth * 14) + 'px';
        var cls = 'st-art' + (a.id === CUR_ID ? ' active' : '');
        h += '<a class="' + cls + '" style="padding-left:' + ai + '"'
           + ' href="' + artUrl(a) + '" title="' + escH(a.title || a.id) + '">'
           + escH(a.title || a.id) + '</a>';
      });
      h += renderChildren(bycat, cat, depth + 1);
    });
    return h;
  }

  fetch('/api/wiki-list')
    .then(function (r) { return r.json(); })
    .then(function (data) {
      var arts = data.articles || [];
      var bycat = {};
      arts.forEach(function (a) {
        var c = a.category || '';
        if (!bycat[c]) bycat[c] = [];
        bycat[c].push(a);
      });
      var h = '';
      (bycat[''] || []).forEach(function (a) {
        var cls = 'st-art' + (a.id === CUR_ID ? ' active' : '');
        h += '<a class="' + cls + '" style="padding-left:12px"'
           + ' href="' + artUrl(a) + '" title="' + escH(a.title || a.id) + '">'
           + escH(a.title || a.id) + '</a>';
      });
      h += renderChildren(bycat, '', 0);
      var body = document.getElementById('sidebar-body');
      if (body) {
        body.innerHTML = h;
        var act = body.querySelector('.active');
        if (act) act.scrollIntoView({ block: 'nearest' });
      }
    })
    .catch(function () {});

  // ── 右侧本文目录（TOC） ───────────────────────────────────────────
  (function buildToc() {
    var ab  = document.getElementById('article-body');
    var box = document.getElementById('toc-body');
    var toc = document.getElementById('toc');
    if (!ab || !box || !toc) return;

    var headings = ab.querySelectorAll('h1,h2,h3,h4,h5,h6');
    if (!headings.length) { toc.style.display = 'none'; return; }

    var h = '';
    headings.forEach(function (el, i) {
      var level  = parseInt(el.tagName[1], 10);
      var indent = (8 + (level - 1) * 12) + 'px';
      el.id = 'toc-h-' + i;
      h += '<a class="toc-item" style="padding-left:' + indent + '"'
         + ' href="#toc-h-' + i + '">' + escH(el.textContent) + '</a>';
    });
    box.innerHTML = h;

    var content = document.querySelector('.content');
    if (!content) return;
    var links = box.querySelectorAll('.toc-item');
    content.addEventListener('scroll', function () {
      var top = content.scrollTop;
      var active = 0;
      headings.forEach(function (el, i) {
        if (el.offsetTop - content.offsetTop <= top + 80) active = i;
      });
      links.forEach(function (lk, i) {
        lk.classList.toggle('active', i === active);
      });
    }, { passive: true });
  }());
})();
