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
})();
