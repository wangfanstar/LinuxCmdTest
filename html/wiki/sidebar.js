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
      var css = [
        '@page{size:A4;margin:0;}',
        '*{box-sizing:border-box;margin:0;padding:0}',
        'html,body{height:auto!important;max-height:none!important;overflow:visible!important}',
        'body{background:#fff;color:#1a1a2e;font-family:-apple-system,BlinkMacSystemFont,\'Segoe UI\',sans-serif;font-size:13px;line-height:1.7;margin:0;padding:14mm}',
        '.wrap{max-width:100%;padding:20px 28px;position:relative;min-height:1px}',
        'h1.art-title{font-size:1.6rem;color:#111;margin-bottom:5px;border-bottom:2px solid #e0e0e0;padding-bottom:8px}',
        '.meta{font-size:.72rem;color:#666;margin-bottom:16px}',
        '.art-content h1,.art-content h2,.art-content h3,.art-content h4{color:#111;margin:1.2em 0 .4em;line-height:1.3;page-break-after:avoid}',
        '.art-content h1{font-size:1.3rem;border-bottom:1px solid #ddd;padding-bottom:4px}',
        '.art-content h2{font-size:1.15rem}.art-content h3{font-size:1.05rem}',
        '.art-content p{margin:.6em 0;line-height:1.75}',
        '.art-content pre{background:#f6f8fa;border:1px solid #d0d7de;border-radius:4px;padding:10px;overflow-x:auto;margin:.8em 0;font-size:.8rem;page-break-inside:avoid}',
        '.art-content code{font-family:"SFMono-Regular",Consolas,monospace;font-size:.85em}',
        '.art-content pre code{color:#1a1a2e}',
        '.art-content :not(pre)>code{background:#f6f8fa;padding:1px 5px;border-radius:3px;color:#0550ae;border:1px solid #d0d7de}',
        '.art-content blockquote{border-left:3px solid #0969da;padding:.4em 1em;color:#57606a;margin:.8em 0}',
        '.art-content table{border-collapse:collapse;width:100%;margin:.8em 0;page-break-inside:avoid}',
        '.art-content th,.art-content td{border:1px solid #d0d7de;padding:5px 9px;text-align:left}',
        '.art-content th{background:#f6f8fa;font-weight:600}',
        '.art-content img{max-width:100%;page-break-inside:avoid}',
        '.art-content ul,.art-content ol{padding-left:1.5em;margin:.5em 0}',
        '.art-content li{margin:.15em 0}',
        '.art-content a{color:#0969da;text-decoration:none}',
        '.art-content hr{border:none;border-top:1px solid #d0d7de;margin:1em 0}',
        '.art-content .code-block{position:relative;margin:1em 0}',
        '.art-content .code-block .copy-btn{display:none!important}',
        '.art-content{counter-reset:sc1 sc2 sc3 sc4}',
        '.art-content h1{counter-reset:sc2 sc3 sc4;counter-increment:sc1}',
        '.art-content h2{counter-reset:sc3 sc4;counter-increment:sc2}',
        '.art-content h3{counter-reset:sc4;counter-increment:sc3}',
        '.art-content h4{counter-increment:sc4}',
        '.art-content h1::before{content:counter(sc1)". ";color:#57606a;font-weight:400;font-size:.88em}',
        '.art-content h2::before{content:counter(sc1)"."counter(sc2)" ";color:#57606a;font-weight:400;font-size:.88em}',
        '.art-content h3::before{content:counter(sc1)"."counter(sc2)"."counter(sc3)" ";color:#57606a;font-weight:400;font-size:.88em}',
        '.art-content h4::before{content:counter(sc1)"."counter(sc2)"."counter(sc3)"."counter(sc4)" ";color:#57606a;font-weight:400;font-size:.88em}',
        '.pdf-page-num{display:none;position:absolute;left:0;right:0;color:#666;font-size:10px;text-align:right;white-space:nowrap}',
        '.pdf-page-num .num{font-weight:600;color:#333}',
        '@media print{html,body{height:auto!important;overflow:visible!important}body{font-size:11px;padding:12mm 14mm!important;margin:0!important;-webkit-print-color-adjust:exact;print-color-adjust:exact}.pdf-page-num{display:block}}'
      ].join('\n');

      var baseHref = '';
      try {
        baseHref = new URL('.', window.location.href).href;
      } catch (e) {
        baseHref = window.location.href;
      }
      var baseTag = baseHref
        ? '<base href="' + escH(baseHref) + '">\n'
        : '';
      var boot = '<script src="/wiki/rich-render.js"><\\/script>\n' +
        '<script>(function(){' +
        'function mmPx(v){var d=document.createElement("div");d.style.cssText="position:absolute;visibility:hidden;height:"+v+"mm";document.body.appendChild(d);var h=d.offsetHeight||0;d.remove();return h;}' +
        'function injectPaging(){var wrap=document.getElementById("pdf-print-root"),root=document.getElementById("pdf-article-body");if(!wrap||!root)return;Array.prototype.forEach.call(wrap.querySelectorAll(".pdf-page-num"),function(n){n.remove();});' +
        'var pageH=mmPx(269),footerGap=18;if(!pageH)return;var total=Math.max(1,Math.ceil(wrap.scrollHeight/pageH));' +
        'wrap.style.paddingBottom=(footerGap+10)+"px";' +
        'for(var i=1;i<=total;i++){var el=document.createElement("div");el.className="pdf-page-num";el.innerHTML="当前页 <span class=\\"num\\">"+i+"</span> / 总页数 <span class=\\"num\\">"+total+"</span>";el.style.top=Math.max(0,i*pageH-footerGap)+"px";wrap.appendChild(el);}}' +
        'function done(){window.__pdfReady=1;}' +
        'try{var root=document.getElementById("pdf-article-body");if(!root||!window.createRichRenderer){injectPaging();done();return;}' +
        'var rr=createRichRenderer({mathjaxSrc:"/wiki/vendor/mathjax/tex-svg-full.js",mermaidSrc:"/wiki/vendor/mermaid/mermaid.min.js",mermaidTheme:"dark"});' +
        'rr.ensure(function(){rr.render(root);setTimeout(function(){injectPaging();done();},700);});setTimeout(function(){injectPaging();done();},4200);' +
        '}catch(e){injectPaging();done();}})();<\\/script>\n';

      var html =
        '<!DOCTYPE html>\n<html lang="zh-CN">\n<head>\n' +
        '<meta charset="UTF-8">\n' +
        baseTag +
        '<title>\u200b</title>\n<style>\n' +
        css +
        '\n</style>\n</head>\n<body>\n' +
        '<div class="wrap" id="pdf-print-root">\n' +
        '<h1 class="art-title">' +
        escH(title) +
        '</h1>\n' +
        '<div class="meta">' +
        escH(metaTxt) +
        '</div>\n' +
        '<div class="art-content" id="pdf-article-body">\n' +
        body +
        '\n</div>\n</div>\n' +
        boot +
        '</body>\n</html>';

      if (typeof showToast === 'function') {
        showToast('\u82e5\u9884\u89c8\u5de6\u4e0a\u6709\u65e5\u671f/\u5de6\u4e0b\u6709\u7f51\u5740\uff0c\u8bf7\u5728\u6253\u5370\u8bbe\u7f6e\u4e2d\u53d6\u6d88\u300c\u9875\u7709\u548c\u9875\u811a\u300d');
      }

      var url = null;
      var ifr = document.createElement('iframe');
      ifr.style.cssText =
        'position:fixed;right:0;bottom:0;width:0;height:0;border:0;opacity:0;pointer-events:none;';
      ifr.setAttribute('aria-hidden', 'true');
      var useSrcdoc = (String(html).length < 2000000);
      if (useSrcdoc) {
        try { ifr.removeAttribute('src'); ifr.srcdoc = html; } catch (e0) { useSrcdoc = false; }
      }
      if (!useSrcdoc) {
        var blob = new Blob(['\ufeff', html], { type: 'text/html;charset=utf-8' });
        url = URL.createObjectURL(blob);
        ifr.removeAttribute('srcdoc');
        ifr.src = url;
      }
      var cleaned = false;
      function cleanupPdf() {
        if (cleaned) return;
        cleaned = true;
        try { if (url) URL.revokeObjectURL(url); } catch (e) {}
        if (ifr.parentNode) ifr.parentNode.removeChild(ifr);
      }
      ifr.onload = function () {
        setTimeout(function () {
          var cw = ifr.contentWindow;
          if (!cw) {
            cleanupPdf();
            return;
          }
          try {
            cw.addEventListener('afterprint', cleanupPdf);
          } catch (e) {}
          try {
            cw.focus();
            var tries = 0;
            (function waitReadyAndPrint() {
              var ready = false;
              try { ready = !!cw.__pdfReady; } catch (e0) {}
              if (ready || tries > 50) {
                try { cw.print(); } catch (e1) {}
                return;
              }
              tries++;
              setTimeout(waitReadyAndPrint, 120);
            })();
          } catch (e) {
            if (!url) {
              try {
                var b = new Blob(['\ufeff', html], { type: 'text/html;charset=utf-8' });
                url = URL.createObjectURL(b);
              } catch (e0) { cleanupPdf(); if (typeof showToast === 'function') showToast('\u65e0\u6cd5\u51c6\u5907\u6253\u5370\u5185\u5bb9'); return; }
            }
            var w = window.open(url, '_blank');
            if (w) {
              try {
                w.addEventListener('afterprint', cleanupPdf);
              } catch (e2) {}
              setTimeout(function () {
                try {
                  w.focus();
                  w.print();
                } catch (e3) {}
              }, 300);
              setTimeout(cleanupPdf, 60000);
              return;
            }
            if (typeof showToast === 'function') {
              showToast('\u8bf7\u5141\u8bb8\u5f39\u51fa\u7a97\u53e3\u540e\u91cd\u8bd5');
            } else {
              alert('\u8bf7\u5141\u8bb8\u5f39\u51fa\u7a97\u53e3\u540e\u91cd\u8bd5');
            }
            cleanupPdf();
            return;
          }
          setTimeout(cleanupPdf, 60000);
        }, 150);
      };
      document.body.appendChild(ifr);
    };
  }());
})();
