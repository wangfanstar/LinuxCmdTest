;(function (global) {
  'use strict';

  function loadScriptOnce(src) {
    return new Promise(function (resolve, reject) {
      var old = document.querySelector('script[data-rich-src="' + src + '"],script[src="' + src + '"]');
      if (old) {
        if (old.getAttribute('data-rich-ready') === '1') { resolve(); return; }
        old.addEventListener('load', function () { resolve(); }, { once: true });
        old.addEventListener('error', function () { reject(new Error('load ' + src + ' failed')); }, { once: true });
        return;
      }
      var s = document.createElement('script');
      s.src = src;
      s.defer = true;
      s.setAttribute('data-rich-src', src);
      s.onload = function () { s.setAttribute('data-rich-ready', '1'); resolve(); };
      s.onerror = function () { reject(new Error('load ' + src + ' failed')); };
      document.head.appendChild(s);
    });
  }

  function createRichRenderer(opts) {
    opts = opts || {};
    var mathjaxSrc = opts.mathjaxSrc || '/wiki/vendor/mathjax/tex-svg-full.js';
    var mermaidSrc = opts.mermaidSrc || '/wiki/vendor/mermaid/mermaid.min.js';
    var mermaidTheme = opts.mermaidTheme || 'dark';
    var libsReady = false;
    var loading = false;
    var waiters = [];
    var mermaidInited = false;
    var hlStyleInited = false;

    function escHtml(s) {
      return String(s == null ? '' : s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
    }

    function ensureHighlightStyle() {
      if (hlStyleInited) return;
      if (document.getElementById('rr-code-highlight-style')) {
        hlStyleInited = true;
        return;
      }
      var st = document.createElement('style');
      st.id = 'rr-code-highlight-style';
      st.textContent =
        '.rr-code-comment{color:#6e7681;font-style:italic}' +
        '.rr-code-string{color:#a5d6ff}' +
        '.rr-code-keyword{color:#ff7b72;font-weight:600}' +
        '.rr-code-number{color:#79c0ff}' +
        '.rr-code-key{color:#d2a8ff}' +
        '.rr-code-bool{color:#ffa657;font-weight:600}';
      document.head.appendChild(st);
      hlStyleInited = true;
    }

    function highlightByRegex(code, tokenRe, classify) {
      var out = '';
      var last = 0;
      var m;
      while ((m = tokenRe.exec(code)) !== null) {
        if (m.index > last) out += escHtml(code.slice(last, m.index));
        var tok = m[0];
        var cls = classify(tok, m);
        if (cls) out += '<span class="' + cls + '">' + escHtml(tok) + '</span>';
        else out += escHtml(tok);
        last = tokenRe.lastIndex;
      }
      if (last < code.length) out += escHtml(code.slice(last));
      return out;
    }

    function highlightJson(code) {
      var re = /"(?:\\.|[^"\\])*"(?=\s*:)|"(?:\\.|[^"\\])*"|\b(?:true|false|null)\b|-?\b\d+(?:\.\d+)?(?:[eE][+\-]?\d+)?\b/g;
      return highlightByRegex(code, re, function (tok, m) {
        if (/^"/.test(tok) && /\s*:$/.test(code.slice(m.index + tok.length, m.index + tok.length + 4))) return 'rr-code-key';
        if (/^"/.test(tok)) return 'rr-code-string';
        if (/^(true|false|null)$/.test(tok)) return 'rr-code-bool';
        return 'rr-code-number';
      });
    }

    function highlightCLike(code) {
      var kw = 'if|else|for|while|switch|case|break|continue|return|class|struct|enum|typedef|static|const|void|int|long|short|float|double|char|unsigned|signed|bool|new|delete|try|catch|throw|public|private|protected|import|from|export|default|function|var|let|const|async|await|package|include';
      var re = new RegExp(
        '(\\/\\/[^\n]*|\\/\\*[\\s\\S]*?\\*\\/|"(?:\\\\.|[^"\\\\])*"|\'(?:\\\\.|[^\'\\\\])*\'|`(?:\\\\.|[^`\\\\])*`|\\b(?:' +
        kw + ')\\b|\\b\\d+(?:\\.\\d+)?\\b)', 'g'
      );
      return highlightByRegex(code, re, function (tok) {
        if (/^(\/\/|\/\*)/.test(tok)) return 'rr-code-comment';
        if (/^["'`]/.test(tok)) return 'rr-code-string';
        if (/^\d/.test(tok)) return 'rr-code-number';
        return 'rr-code-keyword';
      });
    }

    function highlightPyLike(code) {
      var kw = 'def|class|if|elif|else|for|while|break|continue|return|try|except|finally|raise|import|from|as|with|lambda|yield|pass|global|nonlocal|True|False|None|in|is|and|or|not';
      var re = new RegExp(
        '(#[^\n]*|"(?:\\\\.|[^"\\\\])*"|\'(?:\\\\.|[^\'\\\\])*\'|\\b(?:' + kw + ')\\b|\\b\\d+(?:\\.\\d+)?\\b)', 'g'
      );
      return highlightByRegex(code, re, function (tok) {
        if (/^#/.test(tok)) return 'rr-code-comment';
        if (/^["']/.test(tok)) return 'rr-code-string';
        if (/^\d/.test(tok)) return 'rr-code-number';
        return 'rr-code-keyword';
      });
    }

    function detectLang(codeEl) {
      var cls = codeEl.className || '';
      var m = cls.match(/(?:^|\s)(?:lang|language)-([a-zA-Z0-9_+-]+)/);
      return (m && m[1] ? m[1] : '').toLowerCase();
    }

    function highlightCode(code, lang) {
      if (lang === 'json') return highlightJson(code);
      if (lang === 'py' || lang === 'python' || lang === 'sh' || lang === 'bash' || lang === 'zsh') return highlightPyLike(code);
      return highlightCLike(code);
    }

    function highlightCodeBlocks(root) {
      ensureHighlightStyle();
      var list = root.querySelectorAll('.code-block pre code, pre code');
      for (var i = 0; i < list.length; i++) {
        var el = list[i];
        if (el.getAttribute('data-rr-hl') === '1') continue;
        var code = el.textContent || '';
        if (!code.trim()) { el.setAttribute('data-rr-hl', '1'); continue; }
        var lang = detectLang(el);
        el.innerHTML = highlightCode(code, lang);
        el.setAttribute('data-rr-hl', '1');
      }
    }

    function ensureMathJaxConfig() {
      if (!global.MathJax) {
        global.MathJax = {
          tex: {
            inlineMath: [['\\(', '\\)'], ['$', '$']],
            displayMath: [['\\[', '\\]'], ['$$', '$$']]
          },
          svg: { fontCache: 'global' },
          startup: { typeset: false }
        };
      }
    }

    function ensure(done) {
      if (libsReady) { if (done) done(); return; }
      if (done) waiters.push(done);
      if (loading) return;
      loading = true;
      ensureMathJaxConfig();
      Promise.all([
        (global.MathJax && global.MathJax.typesetPromise)
          ? Promise.resolve()
          : loadScriptOnce(mathjaxSrc),
        global.mermaid
          ? Promise.resolve()
          : loadScriptOnce(mermaidSrc)
      ]).then(function () {
        libsReady = true;
        loading = false;
        var q = waiters.slice();
        waiters.length = 0;
        q.forEach(function (fn) { try { fn(); } catch (e) {} });
      }).catch(function () {
        loading = false;
        setTimeout(function () { ensure(); }, 500);
      });
    }

    function render(root) {
      if (!root) return;
      ensure(function () {
        highlightCodeBlocks(root);

        if (global.MathJax && global.MathJax.typesetPromise) {
          try { if (global.MathJax.typesetClear) global.MathJax.typesetClear([root]); } catch (e) {}
          var mjTask = (global.MathJax.startup && global.MathJax.startup.promise)
            ? global.MathJax.startup.promise.then(function () { return global.MathJax.typesetPromise([root]); })
            : global.MathJax.typesetPromise([root]);
          mjTask.catch(function () {});
        }

        if (global.mermaid) {
          if (!mermaidInited) {
            try {
              global.mermaid.initialize({ startOnLoad: false, theme: mermaidTheme, securityLevel: 'loose' });
              mermaidInited = true;
            } catch (e) {}
          }
          var all = Array.prototype.slice.call(root.querySelectorAll('.mermaid'));
          var nodes = all.filter(function (n) { return n.getAttribute('data-processed') !== 'true'; });
          if (nodes.length) {
            try {
              if (global.mermaid.run) global.mermaid.run({ nodes: Array.prototype.slice.call(nodes) });
              else if (global.mermaid.init) global.mermaid.init(undefined, Array.prototype.slice.call(nodes));
            } catch (e) {}
          }
        }
      });
    }

    return {
      ensure: ensure,
      render: render
    };
  }

  global.createRichRenderer = createRichRenderer;
})(window);

