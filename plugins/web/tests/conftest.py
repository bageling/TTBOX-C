# conftest.py — plugins/web/tests 的收集配置
#
# 本目录混有两种测试风格：
#   ① pytest 风格（test_* 函数 + 断言），由 `python -m pytest plugins/web/tests` 收集执行：
#        test_web_brand / test_web_cloud_client / test_web_continuous_lead /
#        test_web_freeauth_gate / test_web_license_caps / test_web_model_input
#   ② 独立脚本风格（模块级顺序执行、末尾 sys.exit()），设计为 `python <file>` 直跑：
#        test_web_calibration_apply / test_web_recoil_translation
#
# 问题：脚本风格文件在 **import 期**就执行用例并 sys.exit()，会被 pytest 收集器
# 介入为 INTERNALERROR（"caught unexpected SystemExit"），从而阻断**整目录**收集。
# 故在此将脚本风格文件排除出收集面；它们按文档以 `python <file>` 方式单独运行。
collect_ignore = [
    'test_web_calibration_apply.py',
    'test_web_recoil_translation.py',
]
