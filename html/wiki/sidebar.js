(function () {
  var CUR_ID = window.WIKI_CUR_ID || '';

  function escH(s) {
    return ('' + s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
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
      var nav = document.getElementById('sidebar');
      nav.innerHTML = '<div class="st-top">\u6587\u7ae0\u76ee\u5f55</div>' + h;
      var act = nav.querySelector('.active');
      if (act) act.scrollIntoView({ block: 'nearest' });
    })
    .catch(function () {});

  // ── 右侧本文目录（TOC） ────────────────────────────────────────────
  (function buildToc() {
    var body = document.getElementById('article-body');
    var toc  = document.getElementById('toc');
    if (!body || !toc) return;

    var headings = body.querySelectorAll('h1,h2,h3,h4,h5,h6');
    if (!headings.length) { toc.style.display = 'none'; return; }

    var h = '<div class="toc-top">\u672c\u6587\u76ee\u5f55</div>';
    headings.forEach(function (el, i) {
      var level  = parseInt(el.tagName[1], 10);
      var indent = (8 + (level - 1) * 12) + 'px';
      var id     = 'toc-h-' + i;
      el.id = id;
      h += '<a class="toc-item" style="padding-left:' + indent + '"'
         + ' href="#' + id + '">' + escH(el.textContent) + '</a>';
    });
    toc.innerHTML = h;

    // 滚动高亮：当前可见标题对应条目加 active 类
    var content = document.querySelector('.content');
    if (!content) return;
    var links = toc.querySelectorAll('.toc-item');
    var ids   = Array.prototype.map.call(headings, function (el) { return el.id; });

    content.addEventListener('scroll', function () {
      var scrollTop = content.scrollTop;
      var active = 0;
      headings.forEach(function (el, i) {
        if (el.offsetTop - content.offsetTop <= scrollTop + 80) active = i;
      });
      links.forEach(function (lk, i) {
        lk.classList.toggle('active', i === active);
      });
    }, { passive: true });
  }());
})();
