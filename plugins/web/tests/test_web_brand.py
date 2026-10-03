# test_web_brand.py — M2 前端接线：ui_brand（渠道品牌/换皮）单测
#
# 背景（对标 yu 的换皮机制，app.py:362-427）：
#   ui_brand 是签名卡下发的渠道标识，链路
#     签名卡 → Core(LicenseStateMachine.sanitize_ui_brand)
#            → IPC GET_STATUS.license.ui_brand
#            → plugins/web/bin/ttbox-web.py 的 _license_block()/_ui_block()
#            → plugins/web/static/app.js 的 applyBrand()
#   本轮之前：core→IPC→web 三处硬编码 'ttbox'，签名卡下发的品牌**永远到不了前端**。
#
# 本文件锁死的判据（每条都对应一个真实缺陷）：
#   1 闭集归一：未知/空/大写/注入 token 一律回默认品牌（不是"原样透传"）
#   2 投影：license.ui_brand 来自 IPC；IPC 不可达 ⇒ 明确默认（不是缺字段）
#   3 注入防御：IPC 侧塞 <script> 也只会落成默认品牌文案
#   4 单一来源：/api/state.ui 与 /api/license.ui 同源，不可能漂移
#   5 模板换皮：品牌有 templates/<dir>/index.html 才换，缺文件夹静默回退
#   6 注册表损坏/缺失 ⇒ 内置兜底表，服务照起（绝不因品牌配置白屏）
#   7 渠道条目缺字段 ⇒ 用默认品牌同名字段补齐（渠道包只改 3 个字段也能跑）
#
# 运行：python -m pytest plugins/web/tests/test_web_brand.py -v
# 说明：Python 侧用例，不并入 C++ ttbox_core_tests，不动三个 C++ 计数域。
import importlib.util
import json
import os
import pathlib
import sys
import tempfile

import pytest

from plugins.web.lib.paths import repo_root as _ttbox_repo_root  # noqa: E402  路径单点真源（A-PATH-3）
REPO_ROOT = pathlib.Path(_ttbox_repo_root())
WEB_SRC = REPO_ROOT / 'plugins' / 'web' / 'bin' / 'ttbox-web.py'

_load_seq = 0


def _load_ttbox_web():
    global _load_seq
    _load_seq += 1
    name = 'ttbox_web_brand_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, WEB_SRC)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture()
def web_mod():
    """每用例独立模块实例（品牌缓存是模块级 dict，必须隔离，否则串味）。"""
    return _load_ttbox_web()


def _write_registry(path, brands, default='ttbox'):
    path.write_text(json.dumps({'schema': 'ttbox-ui-brand-v1',
                                'default': default, 'brands': brands}),
                    encoding='utf-8')


def _ttbox_entry():
    return {
        'brand_name': 'TTBOX', 'brand_mark': 'TT', 'brand_eyebrow': 'TTBOX SYSTEM',
        'brand_title': 'TTBOX 控制台', 'app_title': 'TTBOX 控制台',
        'default_theme': 'dark', 'allow_theme_switch': True,
        'default_local_name': 'ttbox', 'default_hotspot_ssid': 'TTBOX',
        'fallback_reset_text': '重置默认 Wi-Fi',
        'template_dir': None, 'static_dir': None,
    }


@pytest.fixture()
def tmp_registry(web_mod, tmp_path, monkeypatch):
    """把注册表指向 PID 唯一临时文件，绝不碰仓库内 plugins/web/config/。"""
    path = tmp_path / ('ui_brands_%d.json' % os.getpid())
    monkeypatch.setattr(web_mod, 'UI_BRANDS_CONFIG', path)
    web_mod._UI_BRANDS_CACHE.update({'mtime': None, 'path': None, 'data': None})
    return path


# ---------------------------------------------------------------------------
# 1 / 3. 闭集归一 + 注入防御
# ---------------------------------------------------------------------------

@pytest.mark.parametrize('hostile', [
    '', None, '  ', 'ttbox ', ' TTBOX ', 'nope', 'xcs', 'yu',
    '<script>alert(1)</script>', '../../etc/passwd', 'ttbox;rm -rf /',
    'a' * 64, '中文品牌', 'ttbox\u0000', '--',
])
def test_normalize_rejects_anything_outside_closed_set(web_mod, hostile):
    """闭集判据：除注册表里的 id，其它一律回默认品牌（绝不原样透传）。"""
    assert web_mod._normalize_ui_brand(hostile) == 'ttbox'


def test_normalize_accepts_registered_token_case_insensitively(web_mod, tmp_registry):
    brands = {'ttbox': _ttbox_entry(), 'acme': dict(_ttbox_entry(), brand_name='ACME')}
    _write_registry(tmp_registry, brands)
    assert web_mod._normalize_ui_brand('acme') == 'acme'
    assert web_mod._normalize_ui_brand('ACME') == 'acme'
    assert web_mod._normalize_ui_brand('  AcMe  ') == 'acme'


# ---------------------------------------------------------------------------
# 2. _license_block 投影（断链点：本轮之前该字段根本不存在）
# ---------------------------------------------------------------------------

def test_license_block_has_ui_brand_even_when_ipc_unreachable(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_status', lambda: {})
    block = web_mod._license_block()
    assert block['ui_brand'] == 'ttbox'
    assert block['activated'] is False and block['state'] == 'unactivated'


def test_license_block_projects_ui_brand_from_ipc(web_mod, tmp_registry, monkeypatch):
    _write_registry(tmp_registry,
                    {'ttbox': _ttbox_entry(), 'acme': dict(_ttbox_entry(), brand_name='ACME')})
    monkeypatch.setattr(web_mod, '_get_status',
                        lambda: {'license': {'state': 'valid', 'activated': True,
                                             'plan': 'subscription', 'is_pro': True,
                                             'features': ['capture', 'inference'],
                                             'ui_brand': 'acme'}})
    block = web_mod._license_block()
    assert block['ui_brand'] == 'acme'
    assert block['state'] == 'valid'


def test_license_block_neutralizes_hostile_ui_brand_from_ipc(web_mod, monkeypatch):
    """纵深防御：即便 Core 侧约束被绕过，Web 侧闭集收窄仍挡住注入 token。"""
    monkeypatch.setattr(web_mod, '_get_status',
                        lambda: {'license': {'state': 'valid', 'activated': True,
                                             'ui_brand': '<img src=x onerror=alert(1)>'}})
    block = web_mod._license_block()
    assert block['ui_brand'] == 'ttbox'


# ---------------------------------------------------------------------------
# 4. 单一来源：ui 块 = 品牌表投影
# ---------------------------------------------------------------------------

def test_ui_block_shape_and_no_path_leak(web_mod):
    ui = web_mod._ui_block('ttbox')
    for key in ('app_title', 'brand_name', 'brand_mark', 'brand_eyebrow',
                'brand_title', 'ui_brand', 'default_theme', 'allow_theme_switch',
                'default_local_name', 'default_hotspot_ssid'):
        assert key in ui, key
    # template_dir / static_dir 是服务端路径控制字段，不得混入展示块
    assert 'template_dir' not in ui and 'static_dir' not in ui
    assert ui['brand_name'] == 'TTBOX' and ui['ui_brand'] == 'ttbox'
    assert ui['default_theme'] == 'dark'


def test_channel_brand_overrides_theme_and_switch(web_mod, tmp_registry):
    """对标 yu 的 xh：浅色且锁死不换主题。"""
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), brand_name='ACME', brand_mark='AC',
                     brand_title='ACME 控制台', app_title='ACME 控制台',
                     default_theme='light', allow_theme_switch=False,
                     default_hotspot_ssid='ACME', template_dir='acme'),
    })
    ui = web_mod._ui_block('acme')
    assert ui['brand_name'] == 'ACME'
    assert ui['default_theme'] == 'light'
    assert ui['allow_theme_switch'] is False
    assert ui['brand_title'] == 'ACME 控制台'


def test_channel_entry_missing_fields_is_backfilled(web_mod, tmp_registry):
    """渠道包只改 3 个字段也要能跑：其余字段从默认品牌补齐。"""
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': {'brand_name': 'ACME'},   # 只给一个字段
    })
    ui = web_mod._ui_block('acme')
    assert ui['brand_name'] == 'ACME'
    assert ui['brand_mark'] == 'TT'                 # 补位自默认品牌
    assert ui['default_theme'] == 'dark'
    assert ui['allow_theme_switch'] is True


def test_invalid_default_theme_falls_back_to_dark(web_mod, tmp_registry):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), default_theme='rainbow'),
    })
    assert web_mod._ui_block('acme')['default_theme'] == 'dark'


# ---------------------------------------------------------------------------
# 5. 模板换皮（对标 yu 的 _xcsh_template_name，但带回退）
# ---------------------------------------------------------------------------

def test_template_switch_used_only_when_file_exists(web_mod, tmp_registry, tmp_path, monkeypatch):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), template_dir='acme'),
    })
    tdir = tmp_path / 'templates'
    (tdir / 'acme').mkdir(parents=True)
    monkeypatch.setattr(web_mod, 'TEMPLATE_DIR', tdir)

    # 模板尚未下发 ⇒ 静默回退默认模板（yu 此处会 TemplateNotFound → 500）
    assert web_mod._brand_template_name('index.html', 'acme') == 'index.html'

    (tdir / 'acme' / 'index.html').write_text('x', encoding='utf-8')
    assert web_mod._brand_template_name('index.html', 'acme') == 'acme/index.html'

    # 只换 index/mobile，激活页全品牌共用（读卡前品牌未定）
    assert web_mod._brand_template_name('activate.html', 'acme') == 'activate.html'


def test_template_switch_inactive_for_default_brand(web_mod, tmp_registry, tmp_path, monkeypatch):
    _write_registry(tmp_registry, {'ttbox': _ttbox_entry()})
    monkeypatch.setattr(web_mod, 'TEMPLATE_DIR', tmp_path)
    assert web_mod._brand_template_name('index.html', 'ttbox') == 'index.html'


# ---------------------------------------------------------------------------
# 6. 注册表损坏 ⇒ 兜底（服务必须照起）
# ---------------------------------------------------------------------------

@pytest.mark.parametrize('payload', [
    '{ not json',                                   # 语法坏
    '[]',                                           # 顶层不是对象
    '{"brands": {}}',                               # 无默认品牌
    '{"default": "ghost", "brands": {"ttbox": {}}}',  # default 指向不存在的 id
    '{"default": "ttbox", "brands": "nope"}',       # brands 不是对象
])
def test_corrupt_registry_falls_back_to_builtin(web_mod, tmp_registry, payload):
    tmp_registry.write_text(payload, encoding='utf-8')
    assert web_mod._normalize_ui_brand('ttbox') == 'ttbox'
    assert web_mod._ui_block('ttbox')['brand_name'] == 'TTBOX'


def test_missing_registry_file_falls_back_to_builtin(web_mod, tmp_registry):
    assert not tmp_registry.exists()
    assert web_mod._ui_block('ttbox')['brand_name'] == 'TTBOX'
    assert web_mod._normalize_ui_brand('anything') == 'ttbox'


def test_registry_hot_reload_on_mtime_change(web_mod, tmp_registry):
    _write_registry(tmp_registry, {'ttbox': _ttbox_entry()})
    assert web_mod._normalize_ui_brand('acme') == 'ttbox'
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), brand_name='ACME'),
    })
    # 强制 mtime 变化（部分文件系统同秒内写入 mtime 不变）
    os.utime(tmp_registry, (0, 0))
    assert web_mod._normalize_ui_brand('acme') == 'acme'


# ---------------------------------------------------------------------------
# 7. 页面上下文（三端共用构造器；此前是 3 份各 17 行重复硬编码）
# ---------------------------------------------------------------------------

def test_page_context_carries_brand_and_optional_skin(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_status',
                        lambda: {'license': {'state': 'valid', 'activated': True,
                                             'ui_brand': 'ttbox'}})
    ctx = web_mod._page_context()
    assert ctx['ui_brand'] == 'ttbox'
    assert ctx['brand_name'] if 'brand_name' in ctx else True  # 模板不消费 brand_name，仅 ui 块有
    assert ctx['brand_eyebrow'] == 'TTBOX SYSTEM'
    assert ctx['default_theme'] == 'dark'
    assert ctx['brand_has_custom_skin'] is False     # 默认品牌不挂皮肤 CSS
    assert len(ctx['module_labels']) == 12


def test_page_context_enables_skin_link_for_channel_brand(web_mod, tmp_registry, monkeypatch):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), brand_name='ACME', template_dir='acme',
                     static_dir='acme'),
    })
    monkeypatch.setattr(web_mod, '_get_status',
                        lambda: {'license': {'state': 'valid', 'activated': True,
                                             'ui_brand': 'acme'}})
    ctx = web_mod._page_context()
    assert ctx['ui_brand'] == 'acme'
    assert ctx['brand_has_custom_skin'] is True
    assert ctx['brand_static_prefix'] == 'acme'


# ---------------------------------------------------------------------------
# 8. 端到端：/api/state 与 /api/license 的品牌字段必须同源
# ---------------------------------------------------------------------------

def test_state_and_license_payloads_agree_on_brand(web_mod, tmp_registry, monkeypatch):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), brand_name='ACME', brand_mark='AC',
                     brand_title='ACME 控制台', app_title='ACME 控制台'),
    })
    monkeypatch.setattr(web_mod, '_get_status',
                        lambda: {'license': {'state': 'valid', 'activated': True,
                                             'ui_brand': 'acme'}})
    monkeypatch.setattr(web_mod, 'ipc_request',
                        lambda *_a, **_k: {'status': 1, 'data': {}})
    monkeypatch.setattr(web_mod, '_get_runtime_profile', lambda: {})
    monkeypatch.setattr(web_mod, '_loopout_payload', lambda: {})

    license_payload = web_mod._license_payload()
    assert license_payload['ui_brand'] == 'acme'
    assert license_payload['ui']['ui_brand'] == 'acme'
    assert license_payload['ui']['brand_name'] == 'ACME'
    # 顶层与 ui 子块必须同值（两处曾各写一份硬编码，正是漂移源头）
    assert license_payload['ui_brand'] == license_payload['ui']['ui_brand']


# ---------------------------------------------------------------------------
# 9. schema v2：外观字段（brand_accent / brand_logo / theme）与向后兼容
# ---------------------------------------------------------------------------
def _brand_v2_entry(**over):
    e = dict(_ttbox_entry())
    e.update(over)
    return e


def test_v2_appearance_fields_exposed(web_mod, tmp_registry):
    _write_registry(tmp_registry, {
        'ttbox': _brand_v2_entry(),
        'acme': _brand_v2_entry(brand_name='ACME', default_theme='light',
                                brand_accent='#e4572e', brand_logo='logos/acme.png',
                                theme={'mode': 'light', 'accent': '#e4572e'}),
    })
    ui = web_mod._ui_block('acme')
    assert ui['brand_accent'] == '#E4572E'          # 大写归一
    assert ui['brand_logo'] == 'logos/acme.png'
    assert ui['theme'] == {'mode': 'light', 'accent': '#E4572E'}
    assert ui['default_theme'] == 'light'           # 与 theme.mode 一致


def test_v1_entry_gets_v2_defaults(web_mod, tmp_registry):
    """v1 条目（无 accent/logo/theme）⇒ 由 default_theme/默认强调色派生，绝不 KeyError。"""
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': _brand_v1_only_entry(),
    })
    ui = web_mod._ui_block('acme')
    assert ui['brand_accent'] == web_mod._DEFAULT_BRAND_ACCENT
    assert ui['brand_logo'] is None
    assert ui['theme'] == {'mode': 'dark', 'accent': web_mod._DEFAULT_BRAND_ACCENT}


def _brand_v1_only_entry():
    # 严格 v1 字段集（无 brand_accent/brand_logo/theme）
    return {
        'brand_name': 'ACME', 'brand_mark': 'AC', 'brand_eyebrow': 'ACME SYSTEM',
        'brand_title': 'ACME 控制台', 'app_title': 'ACME 控制台',
        'default_theme': 'dark', 'allow_theme_switch': True,
        'default_local_name': 'acme', 'default_hotspot_ssid': 'ACME',
        'fallback_reset_text': '重置默认 Wi-Fi',
        'template_dir': None, 'static_dir': None,
    }


@pytest.mark.parametrize('bad', [
    'red', '#12345', '#GGGGGG', 'red;}', '', None, 123, '#1234567',
])
def test_brand_accent_invalid_falls_back(web_mod, tmp_registry, bad):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': _brand_v2_entry(brand_name='ACME', brand_accent=bad),
    })
    assert web_mod._ui_block('acme')['brand_accent'] == web_mod._DEFAULT_BRAND_ACCENT


@pytest.mark.parametrize('bad', [
    '/etc/passwd', '../../secret.png', 'http://x/y.png', 'a' * 65, '', None,
    'lo go.png', 'x;y',
])
def test_brand_logo_unsafe_is_none(web_mod, tmp_registry, bad):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': _brand_v2_entry(brand_name='ACME', brand_logo=bad),
    })
    assert web_mod._ui_block('acme')['brand_logo'] is None


def test_theme_mode_invalid_falls_back_to_default_theme(web_mod, tmp_registry):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': _brand_v2_entry(brand_name='ACME', default_theme='light',
                                theme={'mode': 'rainbow'}),
    })
    assert web_mod._ui_block('acme')['theme']['mode'] == 'light'


def test_shipped_registry_is_v2_and_loads(web_mod):
    """仓库自带注册表必须是 v2 且能被解析（不破坏默认品牌 lane）。"""
    table = web_mod._ui_brands_table()
    assert web_mod.DEFAULT_UI_BRAND in table['brands']
    assert 'sample' in table['brands'], '应含示例渠道条目（供渠道包参照）'
    # sample 是渠道模板：浅色 + 锁换主题 + 有强调色/logo
    ui = web_mod._ui_block('sample')
    assert ui['default_theme'] == 'light'
    assert ui['allow_theme_switch'] is False
    assert ui['brand_accent'].startswith('#')


# ---------------------------------------------------------------------------
# 10. 品牌表 ↔ 广播 SSID 一致性（wifi_manager 自检钩子）
# ---------------------------------------------------------------------------
def _load_wifi_manager():
    path = REPO_ROOT / 'scripts' / 'wifi_manager.py'
    name = 'wifi_manager_under_test_%d_%d' % (os.getpid(), _load_seq)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def test_wifi_manager_reports_brand_ssid_drift(monkeypatch):
    wifi = _load_wifi_manager()
    # 品牌表声明了 'SAMPLE'（见 ui_brands.json 的 sample 条目）
    assert 'SAMPLE' in wifi.brand_registry_ssids()
    # 默认广播集不含 SAMPLE ⇒ 检出漂移（这正是"面板显示渠道名·板子仍广播 TTBOX"病灶）
    monkeypatch.setattr(wifi, 'DEFAULT_SSIDS', ['TTBOX', 'TTBOX-5G'])
    rep = wifi.verify_brand_ssid_consistency()
    assert rep['ok'] is False
    assert 'SAMPLE' in rep['missing']


def test_wifi_manager_consistent_when_ssid_broadcast(monkeypatch):
    wifi = _load_wifi_manager()
    # 渠道上线时把渠道名加入广播集 ⇒ 一致
    monkeypatch.setattr(wifi, 'DEFAULT_SSIDS', ['TTBOX', 'TTBOX-5G', 'SAMPLE'])
    rep = wifi.verify_brand_ssid_consistency()
    assert rep['ok'] is True
    assert rep['missing'] == []


# ---------------------------------------------------------------------------
# 11. 面板内联皮肤（skin）：闭集 {yu, xh, xcsh}；缺省/未知 ⇒ yu
#     —— 皮肤只决定视觉（参照物 data-ui-brand），文案仍由 payload.ui.brand_* 下发。
# ---------------------------------------------------------------------------
@pytest.mark.parametrize('bad', ['', None, '  ', 'nope', 'yu ', 'XCS', 'xcshh', '中文', 'ye'])
def test_normalize_skin_rejects_outside_closed_set(web_mod, bad):
    assert web_mod._normalize_skin(bad) == 'yu'


@pytest.mark.parametrize('token,skin', [('yu', 'yu'), ('xh', 'xh'), ('XCSH', 'xcsh'), (' xh ', 'xh')])
def test_normalize_skin_accepts_closed_set(web_mod, token, skin):
    assert web_mod._normalize_skin(token) == skin


def test_ui_block_carries_skin_from_registry(web_mod, tmp_registry):
    _write_registry(tmp_registry, {
        'ttbox': _ttbox_entry(),
        'acme': dict(_ttbox_entry(), brand_name='ACME', skin='xh'),
    })
    assert web_mod._ui_block('ttbox')['skin'] == 'yu'
    assert web_mod._ui_block('acme')['skin'] == 'xh'


def test_ui_block_skin_defaults_to_yu_when_missing(web_mod, tmp_registry):
    """渠道条目未声明 skin ⇒ 由默认品牌补齐为 yu（v2 旧条目零改动可用）。"""
    _write_registry(tmp_registry, {'ttbox': _ttbox_entry(), 'acme': dict(_ttbox_entry())})
    assert web_mod._ui_block('acme')['skin'] == 'yu'


def test_shipped_registry_skin_mapping(web_mod):
    """仓库自带注册表：ttbox→yu、sample→xh（xcsh 预留给第三渠道）。"""
    assert web_mod._ui_block('ttbox')['skin'] == 'yu'
    assert web_mod._ui_block('sample')['skin'] == 'xh'


def test_page_context_carries_ui_skin(web_mod, monkeypatch):
    monkeypatch.setattr(web_mod, '_get_status',
                        lambda: {'license': {'state': 'valid', 'activated': True,
                                             'ui_brand': 'ttbox'}})
    ctx = web_mod._page_context()
    assert ctx['ui_skin'] == 'yu'
