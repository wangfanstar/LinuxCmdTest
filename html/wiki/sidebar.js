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

  function buildSecNums(hs) {
    var minLv = 5;
    for (var m = 0; m < hs.length; m++) {
      var lv0 = parseInt(hs[m].tagName.slice(1), 10) || 1;
      if (lv0 < minLv) minLv = lv0;
    }
    if (minLv > 4) minLv = 1;
    var ctrs = [0, 0, 0, 0];
    var nums = [];
    for (var i = 0; i < hs.length; i++) {
      var lv = parseInt(hs[i].tagName.slice(1), 10) || 1;
      var nl = Math.min(Math.max(lv - minLv + 1, 1), 4);
      ctrs[nl - 1]++;
      for (var j = nl; j < 4; j++) ctrs[j] = 0;
      nums.push(ctrs.slice(0, nl).join('.') + ' ');
    }
    return nums;
  }

  function buildStablePdfBookmarkBody(bodyHtml) {
    var root = document.createElement('div');
    root.innerHTML = String(bodyHtml || '');
    var hs = root.querySelectorAll('h1,h2,h3,h4');
    if (!hs.length) return root.innerHTML;

    var secNums = buildSecNums(hs);

    var minLv = 5;
    for (var m = 0; m < hs.length; m++) {
      var lv0 = parseInt(hs[m].tagName.slice(1), 10) || 1;
      if (lv0 < minLv) minLv = lv0;
    }
    if (minLv > 4) minLv = 1;

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
      hd.setAttribute('data-secnum', secNums[i]);
      var txt = (hd.textContent || '').replace(/\s+/g, ' ').trim();
      if (!txt) continue;
      var lv = parseInt(hd.tagName.slice(1), 10) || 1;
      var normLv = Math.min(Math.max(lv - minLv + 1, 1), 4);
      var id = hd.getAttribute('id') || slug(txt);
      hd.setAttribute('id', id);
      shadow.push('<h' + normLv + ' class="pdf-bookmark-shadow">' + escH(secNums[i] + txt) + '</h' + normLv + '>');
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

    // 计算 h1-h4 章节号并写入 data-secnum（与 PDF 保持一致）
    var hs4 = Array.prototype.slice.call(ab.querySelectorAll('h1,h2,h3,h4'));
    if (hs4.length) {
      var snums = buildSecNums(hs4);
      hs4.forEach(function (el, i) { el.setAttribute('data-secnum', snums[i]); });

      // 动态注入修正样式，覆盖旧 HTML 中嵌入的 CSS counter 方案（0.1 问题）
      // 使用属性选择器 [data-secnum]（specificity 0-2-1）> 元素选择器 h2（0-1-2），确保覆盖旧规则
      if (!document.getElementById('_ab_secnum_fix')) {
        var _st = document.createElement('style');
        _st.id = '_ab_secnum_fix';
        _st.textContent =
          '.ab [data-secnum]::before{' +
          'content:attr(data-secnum)!important;color:#8b949e;font-weight:400;font-size:.88em;margin-right:.1em}';
        document.head.appendChild(_st);
      }
    }

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
        var _sn = child.el.getAttribute('data-secnum') || '';
        h += '<a class="toc-item" data-idx="' + child.idx + '" href="#toc-h-' + child.idx + '">'
           + escH(_sn + child.el.textContent) + '</a>';
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

  // ── 客户端 PDF 构建（服务端 wkhtmltopdf 不可用时的回退）──────────────────
  function _wkBuildPdfHtml(title, meta, bodyHtml) {
    var esc = function (s) {
      return String(s || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    };
    var css = [
      '@page{size:A4;margin:14mm 14mm 18mm}',
      '*{box-sizing:border-box;margin:0;padding:0}',
      'body{background:#fff;color:#1a1a2e;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:13px;line-height:1.7}',
      '.wrap{padding:20px 28px}',
      'h1.art-title{font-size:1.6rem;color:#111;margin-bottom:5px;border-bottom:2px solid #e0e0e0;padding-bottom:8px}',
      '.meta{font-size:.72rem;color:#666;margin-bottom:16px}',
      '.pdf-layout{display:flex;gap:18px;align-items:flex-start}',
      '.pdf-toc{width:190px;flex:0 0 190px;border:1px solid #e5e7eb;border-radius:6px;padding:10px;background:#fafbfc;position:sticky;top:0;max-height:calc(100vh - 40px);overflow:auto}',
      '.pdf-toc-title{font-size:.78rem;font-weight:700;color:#444;margin-bottom:8px}',
      '.pdf-toc-empty{font-size:.72rem;color:#999}',
      '.pdf-toc a{display:block;font-size:.72rem;line-height:1.45;color:#555;text-decoration:none;padding:2px 0;word-break:break-word}',
      '.pdf-toc a.lv2{padding-left:10px}.pdf-toc a.lv3{padding-left:20px}.pdf-toc a.lv4{padding-left:30px}',
      '.pdf-main{flex:1;min-width:0}',
      '.art-content h1,.art-content h2,.art-content h3,.art-content h4{color:#111;margin:1.2em 0 .4em;line-height:1.3;page-break-after:avoid}',
      '.art-content h1{font-size:1.3rem;border-bottom:1px solid #ddd;padding-bottom:4px}',
      '.art-content h2{font-size:1.15rem}.art-content h3{font-size:1.05rem}',
      '.art-content h1::before,.art-content h2::before,.art-content h3::before,.art-content h4::before{content:attr(data-secnum);color:#57606a;font-weight:400;font-size:.88em;margin-right:.1em}',
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
      '.art-content .copy-btn{display:none!important}',
      '.pdf-bookmark-shadow-root{position:absolute;left:-100000px;top:0;width:1px;height:1px;overflow:hidden}',
      '.pdf-bookmark-shadow{margin:0;padding:0;font-size:1px;line-height:1px;color:transparent;border:0}',
      '@media print{.pdf-layout{display:block}.pdf-toc{position:static;width:auto;max-height:none;margin-bottom:12px;page-break-inside:avoid}}'
    ].join('\n');

    var boot = '<script>(function(){'
      + 'function eh(s){return String(s||"").replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;");}'
      + 'function sid(t,u){var b=String(t||"").toLowerCase().replace(/[^\\w\\-\\u4e00-\\u9fa5]+/g,"-").replace(/^-+|-+$/g,"")||"sec";var id=b,n=1;while(u[id]){id=b+"-"+(n++);}u[id]=1;return id;}'
      + 'function buildToc(){var root=document.getElementById("_pb"),list=document.getElementById("_ptl");if(!root||!list)return;'
      + 'var hs=root.querySelectorAll("h1,h2,h3,h4");if(!hs.length){list.innerHTML="<div class=\\"pdf-toc-empty\\">无目录</div>";return;}'
      + 'var used={},h="";'
      + 'for(var i=0;i<hs.length;i++){var hd=hs[i],txt=(hd.textContent||"").trim();if(!txt)continue;var id=hd.id||sid(txt,used);hd.id=id;var lv=parseInt(hd.tagName.slice(1),10)||1;var cls=lv>1?(" class=\\"lv"+lv+"\\""):"";var sn=hd.getAttribute("data-secnum")||"";h+="<a"+cls+" href=\\"#"+id+"\\">"+sn+eh(txt)+"</a>";}'
      + 'list.innerHTML=h||"<div class=\\"pdf-toc-empty\\">无目录</div>";}'
      + 'try{buildToc();}catch(e){}window.__pdfReady=1;'
      + '})();<\\/script>';

    return '<!DOCTYPE html>\n<html lang="zh-CN">\n<head>\n<meta charset="UTF-8">\n'
      + '<title>' + esc(title) + '</title>\n'
      + '<style>\n' + css + '\n</style>\n</head>\n<body>\n'
      + '<div class="wrap">\n'
      + '<h1 class="art-title">' + esc(title) + '</h1>\n'
      + (meta ? '<div class="meta">' + esc(meta) + '</div>\n' : '')
      + '<div class="pdf-layout">'
      + '<aside class="pdf-toc"><div class="pdf-toc-title">目录</div>'
      + '<div id="_ptl"><div class="pdf-toc-empty">生成中…</div></div></aside>'
      + '<div class="pdf-main"><div class="art-content" id="_pb">\n'
      + bodyHtml + '\n</div></div></div>\n</div>\n'
      + boot + '\n</body>\n</html>';
  }

  function _wkPrintPdf(html) {
    var ifr = document.createElement('iframe');
    ifr.style.cssText = 'position:fixed;right:0;bottom:0;width:0;height:0;border:0;opacity:0;pointer-events:none;';
    ifr.setAttribute('aria-hidden', 'true');
    var url = null;
    if (String(html).length < 2000000) {
      try { ifr.srcdoc = html; } catch (e) { ifr.removeAttribute('srcdoc'); }
    }
    if (!ifr.srcdoc) {
      var blob = new Blob(['﻿', html], { type: 'text/html;charset=utf-8' });
      url = URL.createObjectURL(blob);
      ifr.src = url;
    }
    var cleaned = false;
    function cleanup() {
      if (cleaned) return; cleaned = true;
      try { if (url) URL.revokeObjectURL(url); } catch (e) {}
      if (ifr.parentNode) ifr.parentNode.removeChild(ifr);
    }
    ifr.onload = function () {
      setTimeout(function () {
        var cw = ifr.contentWindow;
        if (!cw) { cleanup(); return; }
        try { cw.addEventListener('afterprint', cleanup); } catch (e) {}
        var tries = 0;
        (function waitAndPrint() {
          var ready = false;
          try { ready = !!cw.__pdfReady; } catch (e) {}
          if (ready || tries > 20) { try { cw.print(); } catch (e2) {} return; }
          tries++;
          setTimeout(waitAndPrint, 80);
        }());
        setTimeout(cleanup, 60000);
      }, 80);
    };
    document.body.appendChild(ifr);
  }

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
        var msg = '服务端 PDF 导出失败，已回退本地导出：' + (err && err.message ? err.message : err);
        if (typeof showToast === 'function') showToast(msg);
        try { _wkPrintPdf(_wkBuildPdfHtml(title, metaTxt, body)); } catch (e2) {}
      });
    };
  }());

  (function installWikiPageSearch() {
    var pageInput = document.getElementById('wk-page-search-input');
    var pageBtn = document.getElementById('wk-page-search-btn');
    if (!pageInput || !pageBtn) return;

    var isFile = window.location.protocol === 'file:';
    var prefix = (function () {
      var scripts = document.getElementsByTagName('script');
      for (var i = scripts.length - 1; i >= 0; i--) {
        var raw = scripts[i].getAttribute('src') || '';
        var pos = raw.lastIndexOf('sidebar.js');
        if (pos >= 0) return raw.slice(0, pos);
      }
      return '';
    }());
    var indexData = null;
    var fuse = null;
    var indexLabel = '';
    var indexPromise = null;
    var fusePromise = null;
    var cacheKey = 'wiki-page-search-index-v1';

    function assetUrl(path) {
      return prefix + path;
    }

    function indexUrl() {
      return isFile ? assetUrl('wiki-index.json') : '/wiki/wiki-index.json';
    }

    function injectSearchCss() {
      if (document.getElementById('wk-page-search-css')) return;
      var st = document.createElement('style');
      st.id = 'wk-page-search-css';
      st.textContent =
        '.modal-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:999;align-items:center;justify-content:center}' +
        '.modal-overlay.open{display:flex}' +
        '.modal-box{background:#161b22;border:1px solid #30363d;border-radius:10px;width:340px;max-width:92vw;box-shadow:0 8px 32px rgba(0,0,0,.5)}' +
        '.modal-box.modal-wide{width:min(920px,96vw)}' +
        '.modal-head{padding:13px 16px 9px;border-bottom:1px solid #30363d;font-weight:600;font-size:.88rem;color:#f0f6fc;display:flex;justify-content:space-between;align-items:center}' +
        '.modal-body{padding:14px 16px}.modal-foot{padding:10px 16px 13px;border-top:1px solid #30363d;display:flex;gap:8px;justify-content:flex-end}' +
        '.modal-close{background:none;border:none;color:#8b949e;cursor:pointer;font-size:1.1rem;line-height:1}' +
        '.btn{padding:5px 12px;border:none;border-radius:5px;font-size:.78rem;cursor:pointer;display:inline-flex;align-items:center;gap:4px;transition:opacity .15s;white-space:nowrap}' +
        '.btn:hover{opacity:.85}.btn-primary{background:#238636;color:#fff}.btn-secondary{background:#27334d;color:#c9d1d9;border:1px solid #30363d}' +
        '.wk-search-row{display:flex;gap:8px;align-items:center;margin-bottom:10px}' +
        '.wk-search-row input{flex:1;min-width:0;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:8px 10px;font-size:.86rem;outline:none}' +
        '.wk-search-row input:focus{border-color:#4a90e2;box-shadow:0 0 0 2px rgba(74,144,226,.18)}' +
        '.wk-search-actions{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}' +
        '.wk-search-summary{font-size:.76rem;color:#8b949e;margin-bottom:10px}' +
        '.wk-search-results{max-height:min(62vh,560px);overflow:auto;border:1px solid #30363d;border-radius:8px;background:#0d1117}' +
        '.wk-search-empty{padding:28px 14px;text-align:center;color:#8b949e;font-size:.8rem}' +
        '.wk-search-item{display:block;padding:12px 14px;border-bottom:1px solid #21262d;text-decoration:none;color:#c9d1d9}' +
        '.wk-search-item:last-child{border-bottom:none}.wk-search-item:hover{background:#111827}' +
        '.wk-search-title{display:flex;gap:8px;justify-content:space-between;align-items:flex-start;color:#f0f6fc;font-weight:600;line-height:1.45}' +
        '.wk-search-cat{font-size:.72rem;color:#8b949e;font-weight:400;white-space:nowrap;max-width:38%;overflow:hidden;text-overflow:ellipsis}' +
        '.wk-search-snippet{font-size:.8rem;color:#b6c2d1;margin-top:5px;line-height:1.55}' +
        '.wk-search-meta{font-size:.72rem;color:#6e7681;margin-top:5px;word-break:break-all}' +
        '.wk-search-item mark{background:rgba(210,153,34,.32);color:#ffd68a;border-radius:3px;padding:0 2px}' +
        '@media(max-width:640px){.wk-search-row{flex-wrap:wrap}.wk-search-row .btn{width:100%;justify-content:center}.wk-search-cat{max-width:44%}}';
      document.head.appendChild(st);
    }

    function ensureModal() {
      injectSearchCss();
      var modal = document.getElementById('modal-wiki-search');
      if (modal) return modal;
      modal = document.createElement('div');
      modal.className = 'modal-overlay';
      modal.id = 'modal-wiki-search';
      modal.innerHTML =
        '<div class="modal-box modal-wide">' +
          '<div class="modal-head">' +
            '<span>Wiki 搜索</span>' +
            '<button type="button" class="modal-close" id="wk-search-close">✕</button>' +
          '</div>' +
          '<div class="modal-body">' +
            '<div class="wk-search-row">' +
              '<input id="wk-search-modal-input" type="search" autocomplete="off" spellcheck="false" placeholder="输入关键字">' +
              '<button type="button" class="btn btn-primary" id="wk-search-run">搜索</button>' +
            '</div>' +
            '<div class="wk-search-actions">' +
              '<button type="button" class="btn btn-secondary" id="wk-search-load-local">加载本地索引</button>' +
              '<input id="wk-search-local-file" type="file" accept="application/json,.json" style="display:none">' +
            '</div>' +
            '<div class="wk-search-summary" id="wk-search-summary">未加载索引</div>' +
            '<div class="wk-search-results" id="wk-search-results">' +
              '<div class="wk-search-empty">输入关键字后点击搜索。</div>' +
            '</div>' +
          '</div>' +
          '<div class="modal-foot">' +
            '<button type="button" class="btn btn-secondary" id="wk-search-foot-close">关闭</button>' +
          '</div>' +
        '</div>';
      document.body.appendChild(modal);

      document.getElementById('wk-search-close').addEventListener('click', closeSearchModal);
      document.getElementById('wk-search-foot-close').addEventListener('click', closeSearchModal);
      document.getElementById('wk-search-run').addEventListener('click', runSearch);
      document.getElementById('wk-search-modal-input').addEventListener('keydown', function (e) {
        if (e.key === 'Enter') {
          e.preventDefault();
          runSearch();
        }
      });
      document.getElementById('wk-search-load-local').addEventListener('click', function () {
        document.getElementById('wk-search-local-file').click();
      });
      document.getElementById('wk-search-local-file').addEventListener('change', loadLocalIndex);
      modal.addEventListener('click', function (e) {
        if (e.target === modal) closeSearchModal();
      });
      document.addEventListener('keydown', function (e) {
        if (e.key === 'Escape' && modal.classList.contains('open')) closeSearchModal();
      });
      return modal;
    }

    function setSummary(msg) {
      var el = document.getElementById('wk-search-summary');
      if (el) el.textContent = msg;
    }

    function setResults(html) {
      var el = document.getElementById('wk-search-results');
      if (el) el.innerHTML = html;
    }

    function parseIndex(text) {
      var data = JSON.parse(String(text || '').replace(/^\uFEFF/, ''));
      if (!data || typeof data !== 'object' || !Array.isArray(data.articles)) {
        throw new Error('索引格式无效');
      }
      return data;
    }

    function fetchText(url) {
      return fetch(url, { cache: 'no-store' })
        .then(function (r) {
          if (!r.ok) throw new Error('HTTP ' + r.status);
          return r.text();
        })
        .catch(function (fetchErr) {
          return new Promise(function (resolve, reject) {
            try {
              var xhr = new XMLHttpRequest();
              xhr.open('GET', url, true);
              xhr.onreadystatechange = function () {
                if (xhr.readyState !== 4) return;
                if ((xhr.status >= 200 && xhr.status < 300) || (xhr.status === 0 && xhr.responseText)) {
                  resolve(xhr.responseText);
                } else {
                  reject(fetchErr || new Error('XHR ' + xhr.status));
                }
              };
              xhr.onerror = function () { reject(fetchErr || new Error('XHR error')); };
              xhr.send();
            } catch (e) {
              reject(fetchErr || e);
            }
          });
        });
    }

    function loadFuse() {
      if (window.Fuse) return Promise.resolve(window.Fuse);
      if (fusePromise) return fusePromise;
      fusePromise = new Promise(function (resolve, reject) {
        var script = document.createElement('script');
        script.src = assetUrl('vendor/fuse/fuse.min.js');
        script.onload = function () {
          if (window.Fuse) resolve(window.Fuse);
          else reject(new Error('Fuse global missing'));
        };
        script.onerror = function () {
          reject(new Error('无法加载 Fuse.js'));
        };
        document.head.appendChild(script);
      });
      return fusePromise;
    }

    function readCache() {
      try {
        var raw = localStorage.getItem(cacheKey);
        if (!raw) return null;
        var obj = JSON.parse(raw);
        return obj && obj.data ? obj.data : null;
      } catch (e) {
        return null;
      }
    }

    function saveCache(data) {
      try {
        localStorage.setItem(cacheKey, JSON.stringify({ ts: Date.now(), data: data }));
      } catch (e) {}
    }

    function setIndex(data, label) {
      var articles = (data.articles || []).filter(function (a) { return a && a.id; });
      indexData = {
        version: data.version || 1,
        generatedAt: data.generatedAt || '',
        articles: articles,
        categories: Array.isArray(data.categories) ? data.categories : []
      };
      indexLabel = label || '';
      fuse = new window.Fuse(articles, {
        includeScore: true,
        includeMatches: true,
        ignoreLocation: true,
        threshold: 0.36,
        minMatchCharLength: 2,
        keys: [
          { name: 'title', weight: 0.5 },
          { name: 'category', weight: 0.2 },
          { name: 'content', weight: 0.3 }
        ]
      });
      return indexData;
    }

    function loadIndex(force) {
      if (indexData && !force) return Promise.resolve(indexData);
      if (indexPromise && !force) return indexPromise;
      setSummary('正在加载离线索引...');
      indexPromise = loadFuse()
        .then(function () { return fetchText(indexUrl()); })
        .then(parseIndex)
        .then(function (data) {
          saveCache(data);
          return setIndex(data, isFile ? 'wiki-index.json' : '/wiki/wiki-index.json');
        })
        .catch(function (err) {
          var cached = readCache();
          if (cached) return loadFuse().then(function () { return setIndex(cached, '浏览器缓存'); });
          throw err;
        })
        .then(function (ret) {
          indexPromise = null;
          return ret;
        }, function (err) {
          indexPromise = null;
          throw err;
        });
      return indexPromise;
    }

    function renderIntro() {
      var n = indexData ? indexData.articles.length : 0;
      var gen = indexData && indexData.generatedAt ? (' · ' + indexData.generatedAt) : '';
      setSummary('已加载 ' + n + ' 篇文章' + gen + (indexLabel ? (' · ' + indexLabel) : ''));
      setResults('<div class="wk-search-empty">输入关键字后点击搜索。</div>');
    }

    function renderError(msg) {
      setSummary(msg || '搜索失败');
      setResults('<div class="wk-search-empty">可在 NoteWiki 中刷新索引，或在离线模式下加载本地 wiki-index.json。</div>');
    }

    function articleHref(a) {
      var id = encodeURIComponent(a.id || '');
      var cat = String(a.category || '');
      var catPath = cat ? cat.split('/').filter(Boolean).map(encodeURIComponent).join('/') + '/' : '';
      return isFile ? assetUrl(catPath + id + '.html') : ('/wiki/' + catPath + id + '.html');
    }

    function highlight(text, q) {
      text = String(text || '');
      q = String(q || '').trim();
      if (!q) return escH(text);
      var lower = text.toLowerCase();
      var needle = q.toLowerCase();
      var out = '';
      var pos = 0;
      var idx;
      while ((idx = lower.indexOf(needle, pos)) >= 0) {
        out += escH(text.slice(pos, idx));
        out += '<mark>' + escH(text.slice(idx, idx + q.length)) + '</mark>';
        pos = idx + q.length;
      }
      out += escH(text.slice(pos));
      return out;
    }

    function snippet(item, matches, q) {
      var src = '';
      var idx = -1;
      (matches || []).some(function (m) {
        if (m.key === 'content' && m.value) {
          src = m.value;
          idx = m.indices && m.indices[0] ? m.indices[0][0] : -1;
          return true;
        }
        return false;
      });
      if (!src) {
        src = item.content || item.title || '';
        idx = q ? src.toLowerCase().indexOf(q.toLowerCase()) : -1;
      }
      if (idx < 0) idx = 0;
      var start = Math.max(0, idx - 70);
      var end = Math.min(src.length, idx + Math.max(q.length, 1) + 160);
      var text = src.slice(start, end).replace(/\s+/g, ' ').trim();
      if (start > 0) text = '...' + text;
      if (end < src.length) text += '...';
      return highlight(text, q);
    }

    function renderResults(rows, q) {
      var total = indexData ? indexData.articles.length : 0;
      setSummary('搜索 "' + q + '" · ' + rows.length + ' 个结果 · 索引 ' + total + ' 篇' + (indexLabel ? (' · ' + indexLabel) : ''));
      if (!rows.length) {
        setResults('<div class="wk-search-empty">没有匹配结果。</div>');
        return;
      }
      var html = '';
      rows.forEach(function (row) {
        var item = row.item || row;
        var href = articleHref(item);
        var cat = item.category || '根目录';
        var upd = String(item.updated || '').replace('T', ' ').replace('Z', '').slice(0, 16);
        html += '<a class="wk-search-item" href="' + escH(href) + '" target="_blank" rel="noopener">' +
          '<div class="wk-search-title"><span>' + highlight(item.title || item.id, q) + '</span>' +
          '<span class="wk-search-cat">' + escH(cat) + '</span></div>' +
          '<div class="wk-search-snippet">' + snippet(item, row.matches, q) + '</div>' +
          '<div class="wk-search-meta">' + escH(upd || '') + ' · ' + escH(href) + '</div>' +
          '</a>';
      });
      setResults(html);
    }

    function runSearch() {
      var input = document.getElementById('wk-search-modal-input');
      var q = input ? input.value.trim() : '';
      if (!q) {
        renderIntro();
        return;
      }
      loadIndex(false)
        .then(function () {
          if (!fuse) throw new Error('Fuse not ready');
          renderResults(fuse.search(q, { limit: 50 }), q);
        })
        .catch(function (e) {
          renderError('搜索失败：' + (e && e.message ? e.message : e));
        });
    }

    function openSearchModal(seed) {
      var modal = ensureModal();
      var input = document.getElementById('wk-search-modal-input');
      var loadBtn = document.getElementById('wk-search-load-local');
      if (loadBtn) loadBtn.style.display = isFile ? '' : 'none';
      if (input) input.value = seed || pageInput.value || '';
      modal.classList.add('open');
      setTimeout(function () { if (input) input.focus(); }, 30);
      loadIndex(false)
        .then(function () {
          var q = input ? input.value.trim() : '';
          if (q) runSearch();
          else renderIntro();
        })
        .catch(function (e) {
          renderError('索引加载失败：' + (e && e.message ? e.message : e));
          if (loadBtn) loadBtn.style.display = '';
        });
    }

    function closeSearchModal() {
      var modal = document.getElementById('modal-wiki-search');
      if (modal) modal.classList.remove('open');
    }

    function loadLocalIndex(e) {
      var file = e && e.target && e.target.files ? e.target.files[0] : null;
      if (!file) return;
      var reader = new FileReader();
      reader.onload = function () {
        loadFuse()
          .then(function () {
            var data = parseIndex(reader.result || '');
            saveCache(data);
            setIndex(data, file.name || 'wiki-index.json');
            var input = document.getElementById('wk-search-modal-input');
            if (input && input.value.trim()) runSearch();
            else renderIntro();
          })
          .catch(function (err) {
            renderError('本地索引加载失败：' + (err && err.message ? err.message : err));
          });
      };
      reader.onerror = function () {
        renderError('无法读取本地索引文件。');
      };
      reader.readAsText(file, 'utf-8');
      e.target.value = '';
    }

    pageBtn.addEventListener('click', function () {
      openSearchModal(pageInput.value.trim());
    });
    pageInput.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') {
        e.preventDefault();
        openSearchModal(pageInput.value.trim());
      }
    });
  }());

  // ── 左右面板拖拽调整宽度 ─────────────────────────────────────────────
  (function initResizable() {
    var sidebar = document.getElementById('sidebar');
    var toc     = document.getElementById('toc');
    if (!sidebar || !toc) return;

    var st = document.createElement('style');
    st.textContent =
      '.resize-handle{width:5px;cursor:col-resize;flex-shrink:0;background:transparent;' +
      'transition:background .15s;z-index:10;user-select:none}' +
      '.resize-handle:hover,.resize-handle.dragging{background:rgba(74,144,226,.45)}';
    document.head.appendChild(st);

    function makeHandle() {
      var h = document.createElement('div');
      h.className = 'resize-handle';
      return h;
    }

    var lh = makeHandle();
    var rh = makeHandle();
    sidebar.parentNode.insertBefore(lh, sidebar.nextSibling);
    toc.parentNode.insertBefore(rh, toc);

    function attachDrag(handle, getPanel, sign, lsKey, minW, maxW) {
      handle.addEventListener('mousedown', function (e) {
        if (getPanel().classList.contains('collapsed')) return;
        e.preventDefault();
        var startX = e.clientX;
        var startW = getPanel().offsetWidth;
        handle.classList.add('dragging');
        getPanel().style.transition = 'none';

        function onMove(ev) {
          var newW = Math.max(minW, Math.min(maxW, startW + sign * (ev.clientX - startX)));
          getPanel().style.width = newW + 'px';
        }
        function onUp() {
          handle.classList.remove('dragging');
          getPanel().style.transition = '';
          localStorage.setItem(lsKey, String(getPanel().offsetWidth));
          document.removeEventListener('mousemove', onMove);
          document.removeEventListener('mouseup', onUp);
        }
        document.addEventListener('mousemove', onMove);
        document.addEventListener('mouseup', onUp);
      });
    }

    attachDrag(lh, function() { return sidebar; },  1, 'wiki-sb-width',  80, 520);
    attachDrag(rh, function() { return toc; },      -1, 'wiki-toc-width', 80, 520);

    // 恢复上次保存的宽度（仅在非折叠状态）
    var sbW  = parseInt(localStorage.getItem('wiki-sb-width')  || '0', 10);
    var tocW = parseInt(localStorage.getItem('wiki-toc-width') || '0', 10);
    if (sbW  > 80 && sbW  < 520 && !sidebar.classList.contains('collapsed')) sidebar.style.width  = sbW  + 'px';
    if (tocW > 80 && tocW < 520 && !toc.classList.contains('collapsed'))     toc.style.width     = tocW + 'px';
  }());
})();
