# -*- coding: utf-8 -*-
"""S9 安全网：路由快照比对。

用法：
    python scripts/check_route_snapshot.py            # 比对（有漂移则非 0 退出）
    python scripts/check_route_snapshot.py --dump     # 重新导出当前路由为快照

原理：AST 扫入口（以及 api/ 下所有 Blueprint）的装饰器，抽出 (METHOD, URL, 视图名)，
      与 scripts/web_route_snapshot.txt 逐条比对。
      ★ S9 拆完后入口里不再有路由，要同时扫 api/**/*.py。
"""
import argparse
import ast
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_BIN = os.path.join(REPO, 'plugins', 'web', 'bin', 'ttbox-web.py')
API_DIR = os.path.join(REPO, 'plugins', 'web', 'api')
SNAPSHOT = os.path.join(REPO, 'scripts', 'web_route_snapshot.txt')

ROUTE_RE = re.compile(
    r'\.(get|post|put|delete|patch)\(\s*[\'"]([^\'"]+)[\'"]'
    r'(?:.*?[\'"]([^\'"]+)[\'"]\s*)?\)', re.S)


def collect_from_file(path):
    """抽一个文件里的 (METHOD, URL, 视图名)。"""
    with open(path, encoding='utf-8') as fh:
        src = fh.read()
    try:
        tree = ast.parse(src)
    except SyntaxError as exc:
        print('[route] 解析失败 %s: %s' % (path, exc))
        return []
    out = []
    for node in tree.body:
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        for dec in node.decorator_list:
            # 渲染装饰器源码（跨行也能拿到）
            try:
                dsrc = ast.get_source_segment(src, dec) or ''
            except Exception:
                dsrc = ''
            if 'before_request' in dsrc:
                out.append(('BEFORE_REQUEST', '(hook)', node.name))
                continue
            if 'errorhandler' in dsrc:
                m = re.search(r'errorhandler\(\s*[\'"]?(\w+)', dsrc)
                out.append(('ERRORHANDLER', m.group(1) if m else '?', node.name))
                continue
            # @bp.get('/x') / @app.get('/x') / @bp.route(...)
            m = ROUTE_RE.search(dsrc.replace('\n', ''))
            if m:
                out.append((m.group(1).upper(), m.group(2), node.name))
                break
    return out


def collect_all():
    rows = list(collect_from_file(WEB_BIN))
    if os.path.isdir(API_DIR):
        for name in sorted(os.listdir(API_DIR)):
            if not name.endswith('.py') or name == '__init__.py':
                continue
            rows += collect_from_file(os.path.join(API_DIR, name))
    return rows


def load_snapshot():
    rows = []
    if not os.path.exists(SNAPSHOT):
        return None
    with open(SNAPSHOT, encoding='utf-8') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(None, 2)
            if len(parts) == 3:
                rows.append(tuple(parts))
    return rows


def key(rows):
    """比对键：(METHOD, URL) -> 视图名。视图名允许变（拆域会改函数名）。"""
    d = {}
    for method, url, view in rows:
        d.setdefault((method, url), []).append(view)
    return d


def collect_runtime():
    """★ 更强的一条：直接读 Flask 运行时 url_map（真注册了什么就是什么）。

    比 AST 可靠 —— AST 只看得到装饰器字面量，看不到 Blueprint 实际注册结果。
    需要能 import 入口模块（会连带起 lib，但不会起服务）。
    返回 {(URL, METHOD), ...}；import 失败返回 None。
    """
    import importlib.util
    if REPO not in sys.path:
        sys.path.insert(0, REPO)
    try:
        spec = importlib.util.spec_from_file_location(
            'ttbox_web_route_probe', WEB_BIN)
        mod = importlib.util.module_from_spec(spec)
        sys.modules['ttbox_web_route_probe'] = mod
        spec.loader.exec_module(mod)
    except Exception as exc:                       # pragma: no cover
        print('[route] 运行时 import 入口失败：%s' % exc)
        return None
    out = set()
    for rule in mod.app.url_map.iter_rules():
        for m in (rule.methods - {'HEAD', 'OPTIONS'}):
            out.add((rule.rule, m))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dump', action='store_true',
                    help='重新导出当前路由为快照（改基线时才用）')
    ap.add_argument('--runtime', action='store_true',
                    help='额外用 Flask 运行时 url_map 比对（需能 import 入口）')
    args = ap.parse_args()

    cur = collect_all()
    if not cur:
        print('[route] ★一条路由都没抽到 —— 解析器可能失效了，不要相信本结果')
        return 2

    if args.dump:
        hdr = (
            '# TTBOX web 路由快照 — S9 拆 Blueprint 的安全网\n'
            '# 用途：S9 拆分前后逐条比对，任何一条漂移即 FAIL。URL 一字不改是铁律。\n'
            '# 格式：METHOD<space>URL<space>视图函数名\n'
            '# 比对键 = (METHOD, URL)；视图名允许变（拆域会改函数名）。\n'
            '# 条数：%d\n' % len(cur))
        body = '\n'.join('%s %s %s' % r for r in sorted(cur, key=lambda x: (x[0], x[1])))
        with open(SNAPSHOT, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(hdr + body + '\n')
        print('[route] 已导出 %d 条到 %s' % (len(cur), SNAPSHOT))
        return 0

    snap = load_snapshot()
    if snap is None:
        print('[route] ★快照文件不存在：%s' % SNAPSHOT)
        return 2

    ck, sk = key(cur), key(snap)
    print('=== 路由快照比对 ===')
    print('  快照 %d 条 / 当前 %d 条' % (len(sk), len(ck)))

    # URL 集合比对（最关键：URL 一字不能改）
    only_snap = sorted(set(sk) - set(ck))
    only_cur = sorted(set(ck) - set(sk))

    ok = True
    if not only_snap and not only_cur:
        print('  [OK ] (METHOD, URL) 集合完全一致 —— URL零漂移')
    else:
        ok = False
        if only_snap:
            print('  [FAIL] 快照里有、当前没了（%d 条）：' % len(only_snap))
            for m, u in only_snap:
                print('         %-6s %s' % (m, u))
        if only_cur:
            print('  [FAIL] 当前新增了（%d 条）：' % len(only_cur))
            for m, u in only_cur:
                print('         %-6s %s' % (m, u))

    # 重复登记（同一 METHOD+URL 出现两次 = 路由冲突）
    dup = [k for k, v in ck.items() if len(v) > 1]
    if dup:
        ok = False
        print('  [FAIL] 同一 (METHOD, URL) 登记了多次（路由冲突）：')
        for k in sorted(dup):
            print('         %-6s %s -> %s' % (k[0], k[1], ck[k]))

    # 视图函数名变化（不算 FAIL，但要报）
    renamed = []
    for k in sorted(set(sk) & set(ck)):
        if sorted(ck[k]) != sorted(sk[k]):
            renamed.append((k, sk[k], ck[k]))
    if renamed:
        print('  [INFO] 视图函数名有变（不影响 URL，共 %d 条）：' % len(renamed))
        for (m, u), a, b in renamed:
            print('         %-6s %-42s %s -> %s' % (m, u, ','.join(a), ','.join(b)))

    print('  结论：%s' % ('PASS' if ok else 'FAIL'))

    # ---- 运行时校验（更强）：快照里的每条真路由都必须真注册在 url_map 里 ----
    if args.runtime:
        print()
        print('=== 运行时校验（Flask url_map）===')
        live = collect_runtime()
        if live is None:
            print('  SKIP：入口 import 失败，跳过运行时校验')
        else:
            # 快照里的钩子/错误处理器不在 url_map（Flask 就不放进去），要排除
            # 方法名在快照里是 BEFORE_REQUEST / ERRORHANDLER
            NON_ROUTE = ('HOOK', 'ERROR', 'BEFORE_REQUEST', 'ERRORHANDLER')
            snap_rt = {(u, m) for m, u, v in snap if m not in NON_ROUTE}
            # Flask 自带的静态路由不在快照里
            extra = {x for x in live - snap_rt if not x[0].startswith('/static/')}
            miss = snap_rt - live
            print('  快照真路由 %d 条 / url_map 实际 %d 条'
                  % (len(snap_rt), len(live)))
            if miss:
                ok = False
                print('  [FAIL] 快照里有、url_map 里没有（%d 条）：' % len(miss))
                for u, m in sorted(miss):
                    print('         %-6s %s' % (m, u))
            else:
                print('  [OK ] 快照里每条真路由都已真注册')
            if extra:
                ok = False
                print('  [FAIL] url_map 里有、快照里没有（%d 条）：' % len(extra))
                for u, m in sorted(extra):
                    print('         %-6s %s' % (m, u))
            else:
                print('  [OK ] 没有多注册出来的路由')
            print('  运行时结论：%s' % ('PASS' if ok else 'FAIL'))

    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
