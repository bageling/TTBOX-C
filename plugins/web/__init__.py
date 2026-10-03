"""TTBOX Web 插件包。

本文件**必须保留**：``plugins`` 是包，``plugins.system_host`` 等子模块
要靠它拼出包路径。

★ 2026-10-01（P1 复核后的口径）：``framework_api.py`` / ``api_v1.py`` 的源码
**保留、且随出货包交付** —— 原先"S1 交付减法把它们移出出货包"的描述已过期
（那份减法已被业主整体回滚）。二者目前在 ``bin/ttbox-web.py`` 里**没有注册点**，
即"随包但不注册"；要不要恢复注册属业主决策，不在本轮结构收敛范围内。

★ 另：本包不再靠 sys.path 技巧互相找。``framework_api.py`` 自带根锚 bootstrap；
``lib`` 既可经 ``plugins.web.lib`` 全路径导入，也可经 unit 的 PYTHONPATH 直接导入。
"""

__all__: list[str] = []
