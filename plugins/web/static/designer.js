/* 界面设计器
 * 工作方式：把真实的面板装进 iframe，点选元素 → 生成一个 CSS 选择器 →
 * 改属性时把「选择器 { 属性: 值 }」写进自定义 CSS → 实时注入 iframe 预览 →
 * 点保存后写到板端 /opt/ttbox/config/ui-custom.css，面板刷新即永久生效。
 */
(function () {
  'use strict';

  var API = '/api/v1/designer/css';
  var STYLE_ID = '__ttbox_designer__';

  var PROPS = [
    { key: 'width',          label: '宽度',     ph: 'auto' },
    { key: 'height',         label: '高度',     ph: 'auto' },
    { key: 'margin',         label: '外边距',   ph: '0' },
    { key: 'padding',        label: '内边距',   ph: '9px 16px' },
    { key: 'gap',            label: '间距',     ph: '10px' },
    { key: 'display',        label: '排列',     ph: 'flex' },
    { key: 'align-items',    label: '竖向对齐', ph: 'center' },
    { key: 'justify-content',label: '横向分布', ph: 'flex-end' },
    { key: 'border-radius',  label: '圆角',     ph: '8px' },
    { key: 'border',         label: '边框',     ph: '1px solid #333' },
    { key: 'font-size',      label: '字号',     ph: '14px' },
    { key: 'font-weight',    label: '字重',     ph: '650' },
    { key: 'color',          label: '文字色',   ph: '#ffffff', color: true },
    { key: 'background',     label: '背景',     ph: 'rgba(0,0,0,.3)', color: true }
  ];

  var frame = document.getElementById('stage');
  var selInput = document.getElementById('sel');
  var propsBox = document.getElementById('props');
  var cssBox = document.getElementById('css');
  var statusEl = document.getElementById('status');
  var pickBtn = document.getElementById('pickBtn');
  var undoBtn = document.getElementById('undoBtn');

  var cssText = '';
  var picking = false;
  var currentSel = '';
  var undoStack = [];
  var dirty = false;

  /* ---------------- 状态提示 ---------------- */
  function setStatus(msg, kind) {
    statusEl.textContent = msg;
    statusEl.className = 'bar__status' + (kind ? ' is-' + kind : '');
  }
  function markDirty() {
    dirty = true;
    undoBtn.disabled = undoStack.length === 0;
    setStatus('有未保存的改动', 'dirty');
  }
  function markSaved() {
    dirty = false;
    setStatus('已保存');
  }

  /* ---------------- CSS 规则读写（纯文本操作，保持单一真源） ---------------- */
  function escRe(s) { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

  // 在 CSS 文本里给某个选择器设置属性；值留空则删除该属性
  function setRule(css, selector, prop, value) {
    var blockRe = new RegExp('(^|\\n)([ \\t]*' + escRe(selector) + ')\\s*\\{([^}]*)\\}', 'm');
    var m = css.match(blockRe);
    if (!m) {
      if (!value) return css;
      var block = selector + ' {\n  ' + prop + ': ' + value + ';\n}\n';
      return css.replace(/\s*$/, function (t) { return t ? '\n\n' : ''; }) + block;
    }
    var body = m[3];
    var declRe = new RegExp('(^|;|\\n)([ \\t]*)' + escRe(prop) + '\\s*:\\s*[^;]*', 'm');
    if (value) {
      body = declRe.test(body)
        ? body.replace(declRe, function (mm, p1, p2) { return p1 + '\n  ' + prop + ': ' + value; })
        : body.replace(/\s*$/, '') + '\n  ' + prop + ': ' + value + ';';
    } else {
      body = body.replace(declRe, '');
    }
    if (!body.replace(/[\s;]/g, '')) {
      // 属性全删空了就把整块规则去掉，避免留下一堆空壳
      return css.replace(blockRe, '').replace(/\n{3,}/g, '\n\n').replace(/^\s*\n/, '');
    }
    return css.replace(blockRe, function (mm, p1, p2) { return p1 + p2 + ' {' + body + '\n}'; });
  }

  // 从 CSS 文本里读出某个选择器的某属性值
  function getRule(css, selector, prop) {
    var blockRe = new RegExp('(^|\\n)[ \\t]*' + escRe(selector) + '\\s*\\{([^}]*)\\}', 'm');
    var m = css.match(blockRe);
    if (!m) return '';
    var dm = m[2].match(new RegExp('(^|;|\\n)[ \\t]*' + escRe(prop) + '\\s*:\\s*([^;]+)', 'm'));
    return dm ? dm[2].trim() : '';
  }

  // 删掉某个选择器的整块规则
  function delRule(css, selector) {
    var blockRe = new RegExp('(^|\\n)[ \\t]*' + escRe(selector) + '\\s*\\{[^}]*\\}\\n?', 'm');
    return css.replace(blockRe, '').replace(/^\s*\n/, '');
  }

  /* ---------------- 选择器生成 ---------------- */
  function selectorOf(el) {
    if (!el || el.nodeType !== 1) return '';
    if (el.id) return '#' + el.id;
    var parts = [];
    var cur = el;
    var depth = 0;
    while (cur && cur.nodeType === 1 && depth < 3) {
      var seg = cur.tagName.toLowerCase();
      var cls = String(cur.className || '').trim().split(/\s+/).filter(Boolean).slice(0, 2);
      if (cls.length) seg += '.' + cls.join('.');
      parts.unshift(seg);
      if (cur.id) { parts[0] = '#' + cur.id; break; }
      cur = cur.parentElement;
      depth++;
    }
    return parts.join(' > ');
  }

  /* ---------------- 注入预览 ---------------- */
  function applyToFrame() {
    var doc;
    try { doc = frame.contentDocument; } catch (e) { return; }
    if (!doc || !doc.head) return;
    var tag = doc.getElementById(STYLE_ID);
    if (!tag) {
      tag = doc.createElement('style');
      tag.id = STYLE_ID;
      doc.head.appendChild(tag);
    }
    tag.textContent = cssText;
  }

  function cursorStyle(on) {
    var doc;
    try { doc = frame.contentDocument; } catch (e) { return; }
    if (!doc) return;
    var id = '__ttbox_pick_cursor__';
    var tag = doc.getElementById(id);
    if (on) {
      if (!tag) {
        tag = doc.createElement('style');
        tag.id = id;
        doc.head.appendChild(tag);
      }
      tag.textContent = '* { cursor: crosshair !important; }';
    } else if (tag) {
      tag.remove();
    }
  }

  function highlight(el) {
    var doc = frame.contentDocument;
    if (!doc) return;
    clearHighlight();
    if (el && el.style) {
      el.dataset.__hl = el.style.outline || '';
      el.style.outline = '2px solid #79d6c5';
      el.style.outlineOffset = '1px';
    }
  }
  function clearHighlight() {
    var doc = frame.contentDocument;
    if (!doc) return;
    var list = doc.querySelectorAll('[data-__hl]');
    for (var i = 0; i < list.length; i++) {
      list[i].style.outline = list[i].dataset.__hl || '';
      delete list[i].dataset.__hl;
    }
  }

  /* ---------------- 属性面板 ---------------- */
  function renderProps() {
    if (!currentSel) {
      propsBox.innerHTML = '<div class="empty">先拾取一个元素</div>';
      return;
    }
    var html = '';
    PROPS.forEach(function (p) {
      var val = getRule(cssText, currentSel, p.key);
      var ph = p.ph;
      // 没改过就显示元素当前的计算值，方便知道起点在哪
      if (!val) {
        var computed = computedOf(currentSel, p.key);
        if (computed) ph = computed;
      }
      if (p.color) {
        html += '<label for="p_' + p.key + '">' + p.label + '</label>' +
                '<input id="p_' + p.key + '" data-prop="' + p.key + '" type="color" value="' +
                normalizeColor(val) + '">';
      } else {
        html += '<label for="p_' + p.key + '">' + p.label + '</label>' +
                '<input id="p_' + p.key + '" data-prop="' + p.key + '" type="text" value="' +
                escapeAttr(val) + '" placeholder="' + escapeAttr(ph) + '">';
      }
    });
    propsBox.innerHTML = html;

    Array.prototype.forEach.call(propsBox.querySelectorAll('input'), function (inp) {
      inp.addEventListener('input', function () {
        pushUndo();
        cssText = setRule(cssText, currentSel, inp.dataset.prop, inp.value);
        syncCssBox();
        applyToFrame();
        markDirty();
      });
    });
  }

  function computedOf(selector, prop) {
    var doc;
    try { doc = frame.contentDocument; } catch (e) { return ''; }
    if (!doc) return '';
    var el;
    try { el = doc.querySelector(selector); } catch (e) { return ''; }
    if (!el) return '';
    var v = doc.defaultView.getComputedStyle(el)[prop];
    return v || '';
  }

  function normalizeColor(v) {
    if (/^#[0-9a-f]{6}$/i.test(v)) return v;
    if (/^#[0-9a-f]{3}$/i.test(v)) {
      return '#' + v[1] + v[1] + v[2] + v[2] + v[3] + v[3];
    }
    return '#000000';
  }
  function escapeAttr(s) {
    return String(s == null ? '' : s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
  }

  /* ---------------- 撤销 ---------------- */
  function pushUndo() {
    undoStack.push(cssText);
    if (undoStack.length > 50) undoStack.shift();
  }
  function undo() {
    if (!undoStack.length) return;
    cssText = undoStack.pop();
    syncCssBox();
    applyToFrame();
    renderProps();
    undoBtn.disabled = undoStack.length === 0;
    markDirty();
  }

  function syncCssBox() {
    cssBox.value = cssText;
  }

  /* ---------------- 拾取 ---------------- */
  function setPicking(on) {
    picking = on;
    pickBtn.classList.toggle('is-on', on);
    pickBtn.textContent = on ? '拾取中…（再点一次结束）' : '拾取元素';
    cursorStyle(on);
    if (!on) clearHighlight();
  }

  function pick(el) {
    var sel = selectorOf(el);
    if (!sel) return;
    currentSel = sel;
    selInput.value = sel;
    renderProps();
  }

  frame.addEventListener('load', function () {
    applyToFrame();
    if (picking) cursorStyle(true);
    var doc;
    try { doc = frame.contentDocument; } catch (e) { return; }
    if (!doc) return;
    doc.addEventListener('click', function (ev) {
      if (!picking) return;
      ev.preventDefault();
      ev.stopPropagation();
      pick(ev.target);
    }, true);
    doc.addEventListener('mouseover', function (ev) {
      if (picking) highlight(ev.target);
    }, true);
    doc.addEventListener('mouseout', function () {
      if (picking) clearHighlight();
    }, true);
  });

  /* ---------------- 交互 ---------------- */
  pickBtn.addEventListener('click', function () { setPicking(!picking); });
  undoBtn.addEventListener('click', undo);

  document.getElementById('copyBtn').addEventListener('click', function () {
    if (!selInput.value) return;
    selInput.select();
    try { document.execCommand('copy'); setStatus('选择器已复制'); } catch (e) {}
  });

  document.getElementById('clearBtn').addEventListener('click', function () {
    if (!currentSel) return;
    pushUndo();
    cssText = delRule(cssText, currentSel);
    syncCssBox();
    applyToFrame();
    renderProps();
    markDirty();
  });

  selInput.addEventListener('change', function () {
    currentSel = selInput.value.trim();
    renderProps();
  });

  // 手动改 CSS 源码：直接当作新的真源注入预览
  var timer = null;
  cssBox.addEventListener('input', function () {
    clearTimeout(timer);
    timer = setTimeout(function () {
      pushUndo();
      cssText = cssBox.value;
      applyToFrame();
      markDirty();
    }, 300);
  });

  document.getElementById('saveBtn').addEventListener('click', function () {
    setStatus('保存中…');
    fetch(API, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ css: cssBox.value })
    })
      .then(function (r) { return r.json().then(function (j) { return { ok: r.ok, j: j }; }); })
      .then(function (res) {
        if (!res.ok || !res.j.ok) {
          setStatus('保存失败：' + (res.j.error || '未知错误'), 'err');
          return;
        }
        cssText = cssBox.value;
        undoStack = [];
        undoBtn.disabled = true;
        markSaved();
        // 重新载入预览，验证保存的内容确实从服务端生效了（而不是只靠注入）
        frame.src = '/?_=' + Date.now();
      })
      .catch(function (e) { setStatus('保存失败：' + e.message, 'err'); });
  });

  document.getElementById('resetBtn').addEventListener('click', function () {
    if (!confirm('恢复出厂样式？你保存的所有界面改动都会被删除。')) return;
    fetch(API, { method: 'DELETE' })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (!j.ok) { setStatus('重置失败：' + j.error, 'err'); return; }
        cssText = '';
        currentSel = '';
        selInput.value = '';
        undoStack = [];
        undoBtn.disabled = true;
        syncCssBox();
        renderProps();
        markSaved();
        setStatus('已恢复出厂样式');
        frame.src = '/?_=' + Date.now();
      })
      .catch(function (e) { setStatus('重置失败：' + e.message, 'err'); });
  });

  window.addEventListener('beforeunload', function (e) {
    if (dirty) { e.preventDefault(); e.returnValue = ''; }
  });

  /* ---------------- 载入已保存的样式 ---------------- */
  fetch(API)
    .then(function (r) { return r.json(); })
    .then(function (j) {
      cssText = (j.ok && j.data && j.data.css) || '';
      syncCssBox();
      applyToFrame();
      undoStack = [];
      undoBtn.disabled = true;
      setStatus(cssText ? '已载入 ' + cssText.length + ' 个字符' : '尚无自定义样式');
    })
    .catch(function (e) { setStatus('读取失败：' + e.message, 'err'); });

  renderProps();
})();
