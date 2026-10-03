"""branding.py — 品牌（ui_brand）/ 换皮 / 主题归一与投影（2026-10-02 web 换写法 S3）。

从 ``plugins/web/bin/ttbox-web.py`` 整块搬出。**行为逐字保留**。

## 这一层干什么

``ui_brand`` 是**签名卡下发的渠道标识**（M2 三件套 features/plan/ui_brand 之一），流向：
签名卡 → Core(LicenseStateMachine) → IPC GET_STATUS.license.ui_brand → 本层投影 →
前端 applyBrand()。

三件事，各自加固：
  ① 闭集归一：token 必须在注册表命中，否则回默认品牌。注册表**数据驱动**
     （config/ui_brands.json）⇒ 渠道新增品牌 = 加一条 JSON，零代码改动。
  ② 安全边界只收窄不放大：Core 侧 sanitize_ui_brand 已挡注入（`<script>`、非 ASCII、
     超长、首字符非字母数字）；本层再挡"token 合法但无皮肤"。两层叠加。
  ③ 模板换皮带**回退**：品牌声明 template_dir 后，若 templates/<dir>/index.html
     不存在则用默认模板。缺目录会 render_template 抛 TemplateNotFound → 500、面板白屏，
     此处刻意做得更稳：渠道包只下静态皮肤、忘带模板也不会白屏。

## 与入口模块的关系（见 lib/hub.py）

``UI_BRANDS_CONFIG`` / ``TEMPLATE_DIR`` / ``_get_status`` 是**测试的 monkeypatch 锚点**，
定义在入口模块；本模块在**调用时**经 hub 取，补丁因此照旧生效。
"""
from __future__ import annotations

import json
from typing import Any

from plugins.web.lib import hub
from plugins.web.lib.timefmt import _unix_to_iso


# 品牌（ui_brand）解析 —— 对标 yu 的 _normalize_ui_brand / _brand_payload
# ====================================================================
# 为什么需要这一层（对标 yu 的换皮机制）：
#   `ui_brand` 是**签名卡下发的渠道标识**（M2 三件套 features/plan/ui_brand 之一），
#   流向：签名卡 → Core(LicenseStateMachine) → IPC GET_STATUS.license.ui_brand
#        → 本层投影 → 前端 app.js applyBrand()。
#
# yu 的做法（app.py:362-427）：闭集 {"yu","xh","xcsh"}，未知一律回 "yu"；
#   然后 _brand_payload() 用 if 链把 token 摊成 9 个展示字段；
#   _xcsh_template_name() 再把 index/mobile 指到 templates/xcsh/ 换皮。
#
# 本层做三件事与之对齐，并各自加固：
#   ① 闭集归一：token 必须在注册表命中，否则回默认品牌。
#      —— 与 yu 同语义，但注册表**数据驱动**（config/ui_brands.json）：
#         渠道新增品牌 = 加一条 JSON，零代码改动（yu 需改 Python）。
#   ② 安全边界不放松：Core 侧 sanitize_ui_brand 已做字符集/长度硬约束
#      （拒 `<script>`、非 ASCII、超长、首字符非字母数字）。本层**只收窄不放大**：
#      Core 挡"注入"，本层挡"token 合法但无皮肤"。两层叠加。
#   ③ 模板换皮带**回退**：品牌声明 template_dir 后，若 templates/<dir>/index.html
#      不存在则静默用默认模板。yu 的 _xcsh_template_name 无此回退 ——
#      缺目录会 render_template 抛 TemplateNotFound → 500，面板直接打不开。
#      此处刻意做得更稳：渠道包只下静态皮肤、忘带模板也不会白屏。
# --------------------------------------------------------------------
# ★ UI_BRANDS_CONFIG **定义在入口模块**（测试的 monkeypatch 锚点），本模块一律经
#   ``hub.get('UI_BRANDS_CONFIG')`` 在调用时取 —— 定义在这里会被"from 入口 import"
#   的补丁覆盖不到，测试改了注册表路径也不生效。

DEFAULT_UI_BRAND = 'ttbox'

# ★ M2.04（schema v2）：品牌外观默认值（单一默认点）。
_DEFAULT_BRAND_ACCENT = '#2F81F7'
_VALID_THEME_MODES = ('dark', 'light')

# ★ 面板内联皮肤闭集（与参照物 html[data-ui-brand=...] 覆盖段一一对应）。
#   注册表条目可声明 "skin"；缺省/未知一律归一为默认皮肤 'yu'。皮肤**只决定视觉**
#   （documentElement.dataset.uiBrand + body.ui-brand-*），文案永远由品牌注册表投影
#   下发（payload.ui.brand_*），面板内不再硬编码任何品牌文案。
DEFAULT_UI_SKIN = 'yu'
_VALID_SKINS = ('yu', 'xh', 'xcsh')


def _normalize_skin(value: Any) -> str:
    """token → 皮肤闭集 {yu, xh, xcsh}；空/未知/非法一律回默认皮肤（对标参照物 normalizeUiBrand）。"""
    token = str(value or '').strip().lower()
    return token if token in _VALID_SKINS else DEFAULT_UI_SKIN


# 内置兜底表：注册表文件缺失/损坏时仍能起服务（绝不因品牌配置问题白屏）
_UI_BRAND_FALLBACK = {
    DEFAULT_UI_BRAND: {
        'brand_name': 'TTBOX',
        'brand_mark': 'TT',
        'brand_eyebrow': 'TTBOX SYSTEM',
        'brand_title': 'TTBOX 控制台',
        'app_title': 'TTBOX 控制台',
        'default_theme': 'dark',
        'allow_theme_switch': True,
        'default_local_name': 'ttbox',
        'default_hotspot_ssid': 'TTBOX',
        'fallback_reset_text': '重置默认 Wi-Fi',
        # ★ v2 外观字段（v1 条目缺省时由此补齐；向后兼容）
        'brand_accent': _DEFAULT_BRAND_ACCENT,
        'brand_logo': None,
        # ★ 面板皮肤（缺省 yu）；渠道条目可覆盖以套用 xh/xcsh 内联皮肤。
        'skin': DEFAULT_UI_SKIN,
        'template_dir': None,
        'static_dir': None,
    },
}

_UI_BRANDS_CACHE: dict = {'mtime': None, 'path': None, 'data': None}


def _ui_brands_table() -> dict:
    """品牌注册表，按 mtime 失效热重载。任何异常 ⇒ 内置兜底表。

    返回 {'default': str, 'brands': {id: {...}}}，保证：
      · default 一定在 brands 里；
      · brands 一定非空；
      · 每个 entry 都是 dict。
    这样下游 `_normalize_ui_brand` 永不返回"查不到的 id"。
    """
    fallback = {'default': DEFAULT_UI_BRAND, 'brands': dict(_UI_BRAND_FALLBACK)}
    path = hub.get('UI_BRANDS_CONFIG')
    try:
        mtime = path.stat().st_mtime
    except OSError:
        return fallback

    cache = _UI_BRANDS_CACHE
    if (cache['data'] is not None and cache['mtime'] == mtime
            and cache['path'] == str(path)):
        return cache['data']

    try:
        raw = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(raw, dict) or not isinstance(raw.get('brands'), dict):
            return fallback
        brands = {k: v for k, v in raw['brands'].items()
                  if isinstance(k, str) and k and isinstance(v, dict)}
        # 默认品牌必须存在，否则注册表不可用（宁可整体回退，不做半残选择）
        if DEFAULT_UI_BRAND not in brands:
            return fallback
        default = raw.get('default')
        if not isinstance(default, str) or default not in brands:
            default = DEFAULT_UI_BRAND
    except (OSError, json.JSONDecodeError, ValueError):
        return fallback

    table = {'default': default, 'brands': brands}
    cache.update({'mtime': mtime, 'path': str(path), 'data': table})
    return table


def _normalize_ui_brand(value: Any) -> str:
    """token → 闭集内的品牌 id；空/未知/非法一律回默认品牌（对标 yu 同函数）。"""
    table = _ui_brands_table()
    token = str(value or '').strip().lower()
    if token and token in table['brands']:
        return token
    return table['default']


def _normalize_hex_color(value: Any, fallback: str = _DEFAULT_BRAND_ACCENT) -> str:
    """★ M2.04：品牌强调色归一 —— 仅接受 #RRGGBB（大小写皆可，统一大写）；其余回默认。

    安全边界：色值只进 CSS 变量，仍严格收窄为 7 字符 hex，杜绝注入（如 `red;}`）。
    """
    if isinstance(value, str):
        v = value.strip()
        if len(v) == 7 and v[0] == '#':
            try:
                int(v[1:], 16)
                return '#' + v[1:].upper()
            except ValueError:
                return fallback
    return fallback


def _normalize_brand_logo(value: Any):
    """★ M2.04：品牌 logo 归一 —— 仅接受安全**相对**路径，否则 None（前端不显示）。

    拒绝：空 / 超长(>64) / 以 '/' 开头（绝对路径）/ 含 '..'（上跳）/ 含 URL scheme 或
    非法字符。返回值恒为 `str|None`（None = 不挂 logo，前端零兜底）。
    """
    if not isinstance(value, str):
        return None
    v = value.strip()
    if not v or len(v) > 64 or v.startswith('/') or '..' in v:
        return None
    for ch in v:
        if not (ch.isalnum() or ch in '._-/'):
            return None
    return v


def _resolve_theme_fields(entry: dict, base: dict) -> dict:
    """★ M2.04：把 v1/v2 两种写法归一到 (mode, accent, logo)。

    兼容矩阵：
      · v1 条目（无 theme / brand_accent）⇒ mode 取 default_theme，accent 取默认色。
      · v2 平铺（brand_accent / brand_logo）⇒ 分别归一。
      · v2 子对象（theme.mode / theme.accent）⇒ 优先级高于平铺。
    """
    theme_raw = entry.get('theme') if isinstance(entry.get('theme'), dict) else {}
    mode = base.get('default_theme') if base.get('default_theme') in _VALID_THEME_MODES else 'dark'
    if isinstance(theme_raw.get('mode'), str) and theme_raw['mode'].strip().lower() in _VALID_THEME_MODES:
        mode = theme_raw['mode'].strip().lower()
    accent_src = theme_raw.get('accent')
    if accent_src is None:
        accent_src = base.get('brand_accent')
    accent = _normalize_hex_color(accent_src)
    logo = _normalize_brand_logo(base.get('brand_logo'))
    return {'mode': mode, 'accent': accent, 'logo': logo}


def _brand_payload(brand: Any = None) -> dict:
    """品牌 id → 展示字段全集（对标 yu `_brand_payload`）。

    brand=None ⇒ 取当前授权卡下发的品牌。
    template_dir / static_dir 是**路径控制字段**，不对外暴露给前端
    （它们只用于服务端选模板/静态前缀，进 JSON 会被误当展示字段）。
    """
    table = _ui_brands_table()
    ui_brand = _current_ui_brand() if brand is None else _normalize_ui_brand(brand)

    entry = dict(table['brands'].get(ui_brand) or {})
    template_dir = entry.pop('template_dir', None)
    static_dir = entry.pop('static_dir', None)

    # 缺字段的渠道条目用默认品牌同名字段补齐 —— 渠道包只改 3 个字段也能跑
    base = dict(_UI_BRAND_FALLBACK[DEFAULT_UI_BRAND])
    base.update({k: v for k, v in entry.items() if v is not None})

    # ★ M2.04：v2 外观（向后兼容 v1 —— 缺字段由 default_theme/默认强调色派生）
    theme = _resolve_theme_fields(entry, base)

    payload = {
        'ui_brand': ui_brand,
        # ★ 面板内联皮肤（闭集 yu/xh/xcsh；缺省 yu）。只决定视觉，不承载文案。
        'skin': _normalize_skin(base.get('skin')),
        'brand_name': str(base.get('brand_name') or 'TTBOX'),
        'brand_mark': str(base.get('brand_mark') or 'TT'),
        'brand_eyebrow': str(base.get('brand_eyebrow') or 'TTBOX SYSTEM'),
        'brand_title': str(base.get('brand_title') or 'TTBOX 控制台'),
        'app_title': str(base.get('app_title') or 'TTBOX 控制台'),
        'default_theme': theme['mode'],
        'allow_theme_switch': bool(base.get('allow_theme_switch', True)),
        'default_local_name': str(base.get('default_local_name') or 'ttbox'),
        'default_hotspot_ssid': str(base.get('default_hotspot_ssid') or 'TTBOX'),
        'fallback_reset_text': str(base.get('fallback_reset_text') or '重置默认 Wi-Fi'),
        # ★ v2 外观字段（前端消费；v1 只认 default_theme 也不会崩 —— 只增不改）
        'brand_accent': theme['accent'],
        'brand_logo': theme['logo'],
        'theme': {'mode': theme['mode'], 'accent': theme['accent']},
    }
    payload['template_dir'] = str(template_dir) if template_dir else None
    payload['static_dir'] = str(static_dir) if static_dir else None
    return payload


def _license_block() -> dict:
    """Web `license` 子块 = core IPC `GET_STATUS.license` 的**投影**（T1.07b：单一真相源）。

    - 字段映射表写死于 `t1.07b-impl-spec.md §3`，wire 形状来自
      `t1.09-t1.10-impl-spec.md §3`（= `core/src/ipc/IpcServer.cpp` 序列化块）；
      **本层零推导**，禁止新增/改写授权语义。
    - core 不可达 / 无 license 块 ⇒ **诚实报未激活**（`activated:false` +
      `state:'unactivated'`）；**不得** fallback 到任何"默认激活"形态 ——
      M1 = AUTH=OFF + NullLicenseClient，未激活本就是**正确的**形态（A23）。
    """
    lic = hub.call('_get_status').get('license', {})
    if not isinstance(lic, dict) or not lic:
        # core 不可达 ⇒ 诚实默认：无签名卡 ⇒ 无短码、无任何能力位（**绝不**回退"默认激活"）。
        return {'activated': False, 'state': 'unactivated', 'plan': 'none', 'is_pro': False,
                'features': [], 'expires_at': '', 'grace_until': '', 'message': '',
                'heartbeat_interval_s': 60, 'status': 'unactivated', 'valid': False,
                # 未激活 ⇒ 无签名卡 ⇒ 无渠道品牌 ⇒ 默认原厂皮（不是"缺字段"，是明确默认）
                'ui_brand': DEFAULT_UI_BRAND,
                # ★ M2.05：卡号短码（core 不可达 ⇒ 空串，前端不得自行拼造）。
                'short_code': '',
                # ★ M2.03：能力位（core 不可达 ⇒ 四项全 False；前端据此隐藏功能入口）。
                'capabilities': {'capture': False, 'inference': False, 'aim': False, 'ota': False}}
    state = str(lic.get('state', 'unactivated'))
    activated = bool(lic.get('activated', False))
    feats = lic.get('features') or []
    if not isinstance(feats, (list, tuple)):
        feats = []
    try:
        heartbeat = int(lic.get('heartbeat_interval_s', 60))
    except (TypeError, ValueError):
        heartbeat = 60
    # ★ M2.03：能力位 = core IPC 的 capabilities 投影（零推导）；缺失/非 dict 一律视为全 False。
    caps_raw = lic.get('capabilities')
    if not isinstance(caps_raw, dict):
        caps_raw = {}
    capabilities = {k: bool(caps_raw.get(k, False))
                    for k in ('capture', 'inference', 'aim', 'ota')}
    return {
        # ---- 授权语义：逐字段来自 IPC，零推导 ----
        'activated': activated,
        'state': state,
        'plan': str(lic.get('plan', 'none')),
        'is_pro': bool(lic.get('is_pro', False)),
        'features': list(feats),                              # 空数组合法（M1 无文档）
        'expires_at': _unix_to_iso(lic.get('expires_at', 0)),
        'grace_until': _unix_to_iso(lic.get('grace_until', 0)),
        'message': str(lic.get('last_error', '')),             # 诊断文案
        'heartbeat_interval_s': heartbeat,
        # ---- M2.05：卡号可读短码（core 派生；不可信态 = 空串）。仅投影，禁止本层再派生。----
        'short_code': str(lic.get('short_code', '') or ''),
        # ---- M2.03：能力位（capture/inference/aim/ota）。前端据此做功能可见性门控。----
        'capabilities': capabilities,
        # ---- 渠道品牌（M2 三件套之一）----
        # 例外说明：这不是"发明授权语义"，而是把 Core 下发的 token **收窄到闭集**。
        # Core 已做字符集/长度硬约束；这里再判"注册表里有没有这个皮肤"，
        # 使本字段对前端**恒可用**：拿到的要么是真实品牌 id，要么是默认品牌，
        # 永远不会是"合法但打不开"的悬空 token。见上方品牌解析段注释。
        'ui_brand': _normalize_ui_brand(lic.get('ui_brand')),
        # ---- 旧前端兼容别名（不新增语义）：status ≡ state，valid ≡ activated（两者同值）----
        'status': state,
        'valid': activated,
    }


def _current_ui_brand() -> str:
    """当前生效品牌 = license 投影的 ui_brand（单一真相源 = Core IPC，经闭集归一）。

    ★ 2026-10-02 复核实测：本文件的 `_license_block` 是**实现体**、入口只是再导出，
    模块内直调本地那份会让测试 `monkeypatch web_mod._license_block` 打不进来
    （实测假函数被调用 0 次）。故这里经 hub 取入口属性 —— 无死循环，
    入口那份本身就再往下一层 `_get_status`。
    """
    return _normalize_ui_brand(hub.call('_license_block').get('ui_brand'))


def _ui_block(brand: Any = None) -> dict:
    """Web `ui` 子块 = 品牌表投影（唯一来源，杜绝 collect_web_state 与
    _license_payload 两处各写一份硬编码 —— 那正是本次要消灭的分叉）。

    brand=None ⇒ 取当前授权卡品牌；显式传入 ⇒ 解析该品牌（供 /designer 预览用）。
    可显式传 `license_block['ui_brand']`，避免重复走 IPC。
    """
    b = _brand_payload(brand)
    return {
        'app_title': b['app_title'],
        'brand_name': b['brand_name'],
        'brand_mark': b['brand_mark'],
        'brand_eyebrow': b['brand_eyebrow'],
        'brand_title': b['brand_title'],
        'ui_brand': b['ui_brand'],
        # ★ 面板内联皮肤（参照物 data-ui-brand 闭集）；与文案解耦，只负责选皮。
        'skin': b['skin'],
        'default_theme': b['default_theme'],
        'allow_theme_switch': b['allow_theme_switch'],
        'default_local_name': b['default_local_name'],
        'default_hotspot_ssid': b['default_hotspot_ssid'],
        'fallback_reset_text': b['fallback_reset_text'],
        # ★ M2.04（v2）：品牌外观字段（强调色 / logo / theme 子对象）。前端据此上色与挂 logo。
        'brand_accent': b['brand_accent'],
        'brand_logo': b['brand_logo'],
        'theme': b['theme'],
        # 静态资源前缀：渠道品牌可把皮肤放 static/<id>/（对标 yu static/xcsh/）。
        # 默认品牌 ⇒ 'static'（= 现网路径，行为零变化）。
        'static_prefix': b['static_dir'] or '',
    }


def _brand_template_name(template: str, brand: Any = None) -> str:
    """按品牌解析模板名（对标 yu `_xcsh_template_name`，但带**存在性回退**）。

    仅对 index.html / mobile.html 做换皮（与 yu 一致：activate.html 全品牌共用，
    因为激活页必须在"还没读到卡"时就能打开 —— 此时品牌尚未确定）。

    回退链：templates/<brand_dir>/<tpl> 存在 ⇒ 用它；否则用默认 templates/<tpl>。
    缺皮肤文件夹**不是错误**：渠道包只下静态资源、忘带模板时面板照常打开。
    """
    try:
        b = _brand_payload(brand)
    except Exception:
        return template
    brand_dir = b.get('template_dir')
    if not brand_dir or brand_dir == DEFAULT_UI_BRAND:
        return template
    if template not in ('index.html', 'mobile.html'):
        return template
    candidate = hub.get('TEMPLATE_DIR') / brand_dir / template
    try:
        if candidate.is_file():
            return f'{brand_dir}/{template}'
    except OSError:
        pass
    return template

# ====================================================================
# 参数翻译（Web 格式 ↔ RuntimeProfile 格式）
# 映射依据：Web 前端 collectConfig()（web/static/app.js:5663）+ Web daemon
# 二进制字段名实测。predict_x/y 是 Pid1Controller 的 I 通道增益（无量纲），
# rate_x/y 是 kp_gain_rate —— 全部直通。
# ★ V1.0.13（2026-09-30）：smooth_x/y 已随 core 一起删除（折叠进 kp/kd），
#   aim_offset_x/y 与 capture.offset_x/y 也一并删除（落点只留「瞄点」一个入口）。
#   老配置里若还带这些键，core 一律静默忽略。
# ====================================================================
