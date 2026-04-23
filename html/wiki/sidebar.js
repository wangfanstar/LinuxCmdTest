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

  function buildStablePdfBookmarkBody(bodyHtml) {
    var root = document.createElement('div');
    root.innerHTML = String(bodyHtml || '');
    var hs = root.querySelectorAll('h1,h2,h3,h4');
    if (!hs.length) return root.innerHTML;

    var used = {};
    function slug(t) {
      var b = String(t || '')
        .toLowerCase()
        .replace(/[^\w\-\u4e00-\u9fa5]+/g, '-')
        .replace(/^-+|-+$/g, '') || 'sec';
      var id = b, n = 1;
      while (used[id]) id = b + '-' + (n++);
      used[id] = 1;
      return id;
    }

    var shadow = [];
    for (var i = 0; i < hs.length; i++) {
      var hd = hs[i];
      var txt = (hd.textContent || '').replace(/\s+/g, ' ').trim();
      if (!txt) continue;
      var lv = parseInt(hd.tagName.slice(1), 10) || 1;
      if (lv < 1) lv = 1;
      if (lv > 4) lv = 4;
      var id = hd.getAttribute('id') || slug(txt);
      hd.setAttribute('id', id);
      shadow.push('<h' + lv + ' class="pdf-bookmark-shadow">' + escH(txt) + '</h' + lv + '>');
    }
    if (shadow.length) {
      var box = document.createElement('div');
      box.className = 'pdf-bookmark-shadow-root';
      box.setAttribute('aria-hidden', 'true');
      box.innerHTML = shadow.join('');
      root.insertBefore(box, root.firstChild);
    }
    return root.innerHTML;
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

  // ── 右侧本文目录（TOC，支持层级缩进与折叠） ─────────────────────
  (function buildToc() {
    var ab  = document.getElementById('article-body');
    var box = document.getElementById('toc-body');
    var toc = document.getElementById('toc');
    if (!ab || !box || !toc) return;

    var headings = Array.prototype.slice.call(ab.querySelectorAll('h1,h2,h3,h4,h5,h6'));
    if (!headings.length) { toc.style.display = 'none'; return; }

    // 给每个标题打上 id
    headings.forEach(function (el, i) { el.id = 'toc-h-' + i; });

    // 构建树形结构
    function buildTree(items) {
      var root = { children: [], level: 0 };
      var stack = [root];
      items.forEach(function (el, i) {
        var level = parseInt(el.tagName[1], 10);
        var node  = { el: el, idx: i, level: level, children: [] };
        while (stack.length > 1 && stack[stack.length - 1].level >= level) stack.pop();
        stack[stack.length - 1].children.push(node);
        stack.push(node);
      });
      return root;
    }

    // 渲染树节点（递归）
    function renderNode(node, depth) {
      var h = '';
      node.children.forEach(function (child) {
        var hasCh  = child.children.length > 0;
        var indent = (6 + depth * 12) + 'px';
        h += '<div class="toc-node">';
        // 折叠按钮 + 链接行
        h += '<div class="toc-row" style="padding-left:' + indent + '">';
        if (hasCh) {
          h += '<span class="toc-tog open" data-cid="toc-c-' + child.idx + '">▾</span>';
        } else {
          h += '<span class="toc-tog-sp"></span>';
        }
        h += '<a class="toc-item" data-idx="' + child.idx + '" href="#toc-h-' + child.idx + '">'
           + escH(child.el.textContent) + '</a>';
        h += '</div>';
        if (hasCh) {
          h += '<div class="toc-children" id="toc-c-' + child.idx + '">';
          h += renderNode(child, depth + 1);
          h += '</div>';
        }
        h += '</div>';
      });
      return h;
    }

    var tree = buildTree(headings);
    box.innerHTML = renderNode(tree, 0);

    // 折叠 / 展开
    function tocToggle(tog) {
      var cid  = tog.dataset ? tog.dataset.cid : tog.getAttribute('data-cid');
      var body = document.getElementById(cid);
      if (!body) return;
      var collapsed = body.style.display === 'none';
      body.style.display = collapsed ? '' : 'none';
      tog.textContent = collapsed ? '▾' : '▸';
      tog.classList.toggle('open', collapsed);
    }

    function findParent(el, cls, stop) {
      while (el && el !== stop) {
        if (el.classList && el.classList.contains(cls)) return el;
        el = el.parentNode;
      }
      return null;
    }

    box.addEventListener('click', function (e) {
      // 点箭头：仅折叠，阻止链接跳转
      var tog = findParent(e.target, 'toc-tog', box);
      if (tog) { e.preventDefault(); tocToggle(tog); return; }

      // 点标题文字或整行：折叠同时允许链接正常跳转
      var row = findParent(e.target, 'toc-row', box);
      if (row) {
        var rowTog = row.querySelector('.toc-tog');
        if (rowTog) tocToggle(rowTog);
      }
    });

    // 滚动高亮
    var content = document.querySelector('.content');
    if (!content) return;
    var links = box.querySelectorAll('.toc-item');
    content.addEventListener('scroll', function () {
      var top    = content.scrollTop;
      var active = 0;
      headings.forEach(function (el, i) {
        if (el.offsetTop - content.offsetTop <= top + 80) active = i;
      });
      links.forEach(function (lk) {
        var on = parseInt(lk.dataset.idx, 10) === active;
        lk.classList.toggle('active', on);
        // 确保激活项的父级展开
        if (on) {
          var p = lk.parentNode;
          while (p && p !== box) {
            if (p.classList && p.classList.contains('toc-children') && p.style.display === 'none') {
              p.style.display = '';
              var tog2 = document.querySelector('[data-cid="' + p.id + '"]');
              if (tog2) { tog2.textContent = '▾'; tog2.classList.add('open'); }
            }
            p = p.parentNode;
          }
        }
      });
    }, { passive: true });
  }());

  // ── 标题自动章节编号（CSS 计数器注入，无需重建 HTML） ─────────────
  (function addSectionNumbers() {
    if (!document.getElementById('article-body')) return;
    var s = document.createElement('style');
    s.textContent =
      '.ab{counter-reset:sc1 sc2 sc3 sc4}' +
      '.ab h1{counter-reset:sc2 sc3 sc4;counter-increment:sc1}' +
      '.ab h2{counter-reset:sc3 sc4;counter-increment:sc2}' +
      '.ab h3{counter-reset:sc4;counter-increment:sc3}' +
      '.ab h4{counter-increment:sc4}' +
      '.ab h1::before{content:counter(sc1)". ";color:#8b949e;font-weight:400;font-size:.88em;margin-right:.2em}' +
      '.ab h2::before{content:counter(sc1)"."counter(sc2)" ";color:#8b949e;font-weight:400;font-size:.88em;margin-right:.2em}' +
      '.ab h3::before{content:counter(sc1)"."counter(sc2)"."counter(sc3)" ";color:#8b949e;font-weight:400;font-size:.88em;margin-right:.2em}' +
      '.ab h4::before{content:counter(sc1)"."counter(sc2)"."counter(sc3)"."counter(sc4)" ";color:#8b949e;font-weight:400;font-size:.88em;margin-right:.2em}';
    document.head.appendChild(s);
  }());

  // ── 公式/流程图渲染（MathJax + Mermaid）────────────────────────────
  (function renderMathAndMermaid() {
    var root = document.getElementById('article-body');
    if (!root) return;
    var base = (function () {
      var p = window.location.pathname || '';
      var i = p.indexOf('/wiki/');
      if (i >= 0) return p.slice(0, i + 6);
      return '/wiki/';
    }());
    var rr = createRichRenderer({
      mathjaxSrc: base + 'vendor/mathjax/tex-svg-full.js',
      mermaidSrc: base + 'vendor/mermaid/mermaid.min.js',
      mermaidTheme: 'dark'
    });
    rr.ensure();
    rr.render(root);
    window.addEventListener('load', function () { rr.render(root); });
  }());

  // ── 打印样式（Ctrl+P）：隐藏顶栏与双栏目录，正文多页展开 ─────────────
  (function injectWikiPrintCss() {
    if (!document.getElementById('article-body')) return;
    if (document.getElementById('wiki-print-media')) return;
    var st = document.createElement('style');
    st.id = 'wiki-print-media';
    st.textContent =
      '@media print{' +
      'html,body{height:auto!important;max-height:none!important;overflow:visible!important;' +
      'background:#fff!important;color:#1a1a2e!important;' +
      '-webkit-print-color-adjust:exact;print-color-adjust:exact}' +
      'body{display:block!important}' +
      'nav.topbar,nav#sidebar,nav#toc,.sidebar,.toc,' +
      '.st-top,.sidebar-body,.toc-top,.toc-body,' +
      '.edit-btn,.copy-btn,.panel-toggle,.panel-label{' +
      'display:none!important;visibility:hidden!important;width:0!important;height:0!important;' +
      'max-height:0!important;overflow:hidden!important;position:absolute!important;left:-9999px!important;' +
      'clip:rect(0,0,0,0)!important}' +
      '.layout{display:block!important;flex:none!important;height:auto!important;' +
      'max-height:none!important;overflow:visible!important}' +
      '.content{display:block!important;flex:none!important;width:100%!important;' +
      'height:auto!important;max-height:none!important;overflow:visible!important;' +
      'padding:12px 16px!important;position:static!important}' +
      'article,#article-body{overflow:visible!important;max-height:none!important}' +
      '}';
    document.head.appendChild(st);
  }());

  // ── 导出 PDF：新开窗口仅含正文（覆盖页面内联脚本，与 notewiki 行为一致）──
  (function installWikiExportPdf() {
    if (!document.getElementById('article-body')) return;

    window.exportPdf = function () {
      var b = document.getElementById('article-body');
      var h = document.querySelector('h1.at');
      var m = document.querySelector('.am');
      if (!b) return;
      var title = h && h.textContent ? String(h.textContent).trim() : '';
      var metaTxt = m && m.textContent ? String(m.textContent).trim() : '';
      var body = String(b.innerHTML || '').replace(/<\/script/gi, '<\\/script');
      body = buildStablePdfBookmarkBody(body);
      if (typeof showToast === 'function') {
        showToast('正在由服务端生成 PDF（含书签）...');
      }
      function parseErrText(t) {
        var s = String(t || '').trim();
        if (!s) return '';
        try {
          var obj = JSON.parse(s);
          if (obj && obj.error) return String(obj.error);
        } catch (e) {}
        return s;
      }
      fetch('/api/wiki-export-pdf', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json;charset=utf-8' },
        body: JSON.stringify({
          title: title || 'untitled',
          meta: metaTxt || '',
          body: body || ''
        })
      }).then(function(resp) {
        if (!resp.ok) {
          return resp.text().then(function(t) {
            var msg = parseErrText(t);
            throw new Error('HTTP ' + resp.status + (msg ? (' ' + msg) : ''));
          });
        }
        var dispo = resp.headers.get('Content-Disposition') || '';
        return resp.blob().then(function(blob) { return { blob: blob, dispo: dispo }; });
      }).then(function(ret) {
        var filename = 'wiki_export.pdf';
        var m1 = ret.dispo.match(/filename="?([^";]+)"?/i);
        if (m1 && m1[1]) filename = m1[1];
        var url = URL.createObjectURL(ret.blob);
        var a = document.createElement('a');
        a.href = url;
        a.download = filename;
        a.style.display = 'none';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(function () { URL.revokeObjectURL(url); }, 30000);
        if (typeof showToast === 'function') showToast('PDF 已下载（服务端书签版）');
      }).catch(function(err) {
        var msg = '服务端 PDF 导出失败：' + (err && err.message ? err.message : err);
        if (typeof showToast === 'function') showToast(msg);
        else alert(msg);
        try { window.print(); } catch (e2) {}
      });
    };
  }());
})();
