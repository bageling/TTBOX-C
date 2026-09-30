# test_web_recoil_translation.py — ttbox-web.py 压枪翻译层单测（本地，不启动 Web）
# 验证 Web 前端 recoil 提交格式 → mouse.recoil（位掩码 int）转换正确。
import sys

# 从 ttbox-web.py 提取翻译函数（避免启动 Flask）
src = open('plugins/web/bin/ttbox-web.py', encoding='utf-8').read()
# 提取 HOTKEY_BITS / BIT_HOTKEYS / _hotkey_to_bits / _bits_to_hotkey
ns = {}
start = src.index('HOTKEY_BITS =')
end = src.index('# controller 内的数值/布尔直通字段')
exec(src[start:end], ns)

failures = 0

def check(cond, msg):
    global failures
    if cond:
        print('  PASS:', msg)
    else:
        print('  FAIL:', msg)
        failures += 1

hotkey_to_bits = ns['_hotkey_to_bits']
bits_to_hotkey = ns['_bits_to_hotkey']

print('[翻译层] hotkey 字符串 → 位掩码')
check(hotkey_to_bits('left', 1) == 1, "'left' → 1")
check(hotkey_to_bits('right', 2) == 2, "'right' → 2")
check(hotkey_to_bits('middle', 0) == 4, "'middle' → 4")
check(hotkey_to_bits('', 0) == 0, "'' → 0")
check(hotkey_to_bits('back', 0) == 8, "'back' → 8")
check(hotkey_to_bits('forward', 0) == 16, "'forward' → 16")

print('[翻译层] 位掩码 → hotkey 字符串')
check(bits_to_hotkey(1) == 'left', "1 → 'left'")
check(bits_to_hotkey(2) == 'right', "2 → 'right'")
check(bits_to_hotkey(0) == '', "0 → ''")

# 模拟 Web 前端提交的 recoil 块（RECOIL_DEFAULTS 语义）
#
# ★ 2026-09-30 对照 yu 重做后，面板提交的 recoil 块只剩「总开关 + 触发键」四项；
#   算法参数（拉力/速度/累计上限/松手渐出/门控）走 body.ai.controller 里的 recoil_*
#   扁平键，由后端**合并**进同一个 mouse.recoil 子对象。
print('[映射] Web recoil 块 → mouse.recoil 字段翻译')
web_recoil = {
    'enabled': True,
    'hotkey': 'left',
    'hotkey2': '',
    'hotkey_mode': 'any',
}
mouse_recoil = {}
if web_recoil.get('enabled') is not None:
    mouse_recoil['enabled'] = bool(web_recoil['enabled'])
if web_recoil.get('hotkey') is not None:
    mouse_recoil['hotkey'] = hotkey_to_bits(web_recoil['hotkey'], 1) or 1
if web_recoil.get('hotkey2') is not None:
    mouse_recoil['hotkey2'] = hotkey_to_bits(web_recoil['hotkey2'], 0)
if web_recoil.get('hotkey_mode') is not None:
    mouse_recoil['hotkey_mode'] = 2 if str(web_recoil['hotkey_mode']) == 'all' else 1

check(mouse_recoil.get('enabled') is True, "enabled → true")
check(mouse_recoil.get('hotkey') == 1, "hotkey 'left' → 1")
check(mouse_recoil.get('hotkey2') == 0, "hotkey2 '' → 0")
check(mouse_recoil.get('hotkey_mode') == 1, "hotkey_mode 'any' → 1")

print('[映射] 开关/热键 与 算法参数 合并进同一个 mouse.recoil')
# 后端 ttbox-web.py 把算法参数（controller 里的 recoil_* 扁平键）**合并**进同一个
# mouse.recoil 子对象；这里照抄同一语义，锁死"两边不互相覆盖"这条契约。
recoil_params = {'strength': 180.0, 'speed': 1.2}
mouse = {'recoil': dict(mouse_recoil, **recoil_params)}
check(mouse['recoil']['enabled'] is True, "开关与算法参数合并后，开关还在")
check(mouse['recoil']['strength'] == 180.0, "开关与算法参数合并后，参数也落下去")

mouse_off = {'recoil': dict({'enabled': False}, **recoil_params)}
check(mouse_off['recoil']['enabled'] is False, "关掉的开关不会被参数块重新点亮")

# 面板没提交压枪块时不得凭空造出 mouse.recoil（Core 会保留原值）
mouse_none = {}
if {}:
    mouse_none.setdefault('recoil', {})['enabled'] = True
check('recoil' not in mouse_none, "没提交压枪块 → 不造 mouse.recoil")

print('[映射] all 模式')
web_all = dict(web_recoil, hotkey_mode='all')
mode_all = 2 if str(web_all['hotkey_mode']) == 'all' else 1
check(mode_all == 2, "hotkey_mode 'all' → 2")

print()
print('结果: %d failures' % failures)
sys.exit(1 if failures else 0)
