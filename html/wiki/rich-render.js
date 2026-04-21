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

