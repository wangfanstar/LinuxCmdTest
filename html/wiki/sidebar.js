(function () {
  var CUR_ID = window.WIKI_CUR_ID || '';

  // ── 左右面板折叠 ──────────────────────────────────────────────────
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

  initToggle('sidebar', 'sb-toggle',  'wiki-sb',  '\u25C0', '\u25B6');
  initToggle('toc',     'toc-toggle', 'wiki-toc', '\u25B6', '\u25C0');

  // ── 工具 ──────────────────────────────────────────────────────────
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

  // ── 目录文件夹折叠状态（key=分类路径，value=1表示折叠） ──────────
  var collapsedCats = {};
  try { collapsedCats = JSON.parse(localStorage.getItem('wiki-cats') || '{}'); } catch (e) {}
  function saveCats() { localStorage.setItem('wiki-cats', JSON.stringify(collapsedCats)); }

  // 用计数器给每个分类生成唯一 DOM id（支持中文等任意路径）
  var catSeq = 0;
  var catIdMap = {};
  function getCatDomId(cat) {
    if (!catIdMap[cat]) catIdMap[cat] = 'cat-' + (catSeq++);
    return catIdMap[cat];
  }

  function renderArt(a, indent) {
    var cls = 'st-art' + (a.id === CUR_ID ? ' active' : '');
    return '<a class="' + cls + '" style="padding-left:' + indent + '"'
         + ' href="' + artUrl(a) + '" title="' + escH(a.title || a.id) + '">'
         + escH(a.title || a.id) + '</a>';
  }

  function renderCat(cat, depth, bycat) {
    var label     = cat.split('/').pop();
    var pi        = (10 + depth * 14) + 'px';
    var ai        = (22 + depth * 14) + 'px';
    var domId     = getCatDomId(cat);
    var collapsed = !!collapsedCats[cat];
    var arrow     = collapsed ? '\u25B6' : '\u25BC';

    var h = '<div class="st-cat" style="padding-left:' + pi + '" data-domid="' + domId + '">'
          + '<span class="cat-arrow">' + arrow + '</span>'
          + escH(label) + '</div>';

    h += '<div class="cat-body" id="' + domId + '"' + (collapsed ? ' style="display:none"' : '') + '>';
    (bycat[cat] || []).forEach(function (a) { h += renderArt(a, ai); });
    h += renderChildren(bycat, cat, depth + 1);
    h += '</div>';
    return h;
  }

  function renderChildren(bycat, parent, depth) {
    var prefix = parent ? parent + '/' : '';
    var h = '';
    Object.keys(bycat).sort().forEach(function (cat) {
      if (cat === '' || cat === parent) return;
      var ok = parent
        ? (cat.indexOf(prefix) === 0 && cat.slice(prefix.length).indexOf('/') === -1)
        : (cat.indexOf('/') === -1);
      if (!ok) return;
      h += renderCat(cat, depth, bycat);
    });
    return h;
  }

  // ── 左侧文章目录 ──────────────────────────────────────────────────
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
      (bycat[''] || []).forEach(function (a) { h += renderArt(a, '12px'); });
      h += renderChildren(bycat, '', 0);

      var sbBody = document.getElementById('sidebar-body');
      if (!sbBody) return;
      sbBody.innerHTML = h;

      // 点击分类头折叠/展开
      sbBody.querySelectorAll('.st-cat').forEach(function (el) {
        el.addEventListener('click', function () {
          var domId  = el.dataset.domid;
          var bodyEl = document.getElementById(domId);
          var arrowEl = el.querySelector('.cat-arrow');
          if (!bodyEl || !arrowEl) return;
          var nowHidden = bodyEl.style.display === 'none';
          bodyEl.style.display = nowHidden ? '' : 'none';
          arrowEl.textContent  = nowHidden ? '\u25BC' : '\u25B6';
          // 反查 cat 路径
          var cat = Object.keys(catIdMap).find(function (k) { return catIdMap[k] === domId; });
          if (cat !== undefined) {
            if (nowHidden) delete collapsedCats[cat];
            else collapsedCats[cat] = 1;
            saveCats();
          }
        });
      });

      var act = sbBody.querySelector('.active');
      if (act) act.scrollIntoView({ block: 'nearest' });
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
      links.forEach(function (lk, i) { lk.classList.toggle('active', i === active); });
    }, { passive: true });
  }());
})();
