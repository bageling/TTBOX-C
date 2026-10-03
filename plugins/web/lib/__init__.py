# plugins/web/lib — Web 层公共库（M2.07 起）
#
# ttbox-web.py 已把本目录加入 sys.path 末尾（append，不用 insert(0)：顶到 stdlib
# 前会遮蔽同名标准库模块）；systemd 单元的 PYTHONPATH 也是同一个目录。
# 因此可直接 `from lib.cloud_client import CloudLicenseClient`。
#
# 本包遵循两条红线：
#   1. cloud_client.py 仅用 Python 标准库（零新第三方依赖，板端依赖面最小化）；
#   2. 授权语义零推导：web 层只做"云端转发 + 投影"，执法权在 core（LicenseGate）。
