# TTBOX 交叉编译工具链文件（RK3588 / aarch64-linux-gnu）
# 由 WSL Ubuntu-22.04 提供，本机 Windows 调用 WSL 执行 cmake + make
#
# 用法：
#   wsl -d Ubuntu-22.04 -- cmake -B build-aarch64 -S <repo>/core -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<this_file> \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DTTBOX_PROJECT_ROOT=/opt/ttbox \
#     -DTTBOX_CROSS_AARCH64=ON \
#     -DCMAKE_SYSROOT=<sysroot_path>
#   wsl -d Ubuntu-22.04 -- cmake --build build-aarch64 -j8

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 交叉编译器
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc)

# 链接器
set(CMAKE_LINKER       aarch64-linux-gnu-ld)
set(CMAKE_AR           aarch64-linux-gnu-ar)
set(CMAKE_OBJCOPY      aarch64-linux-gnu-objcopy)
set(CMAKE_OBJDUMP      aarch64-linux-gnu-objdump)

# sysroot（从板端拉取）
set(CMAKE_SYSROOT "" CACHE PATH "Sysroot path for cross-compilation")

# 搜索路径只限 sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)  # 找 host 工具（cmake/make 等）
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)    # 只找 sysroot 库
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)    # 只找 sysroot 头文件
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake 3.22+ 支持 FindOpenCV 的 MODULE 模式，需显式设置
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake-modules")

# RKNN SDK 头文件路径（仓库内自带）
# ⚠️ 此处 third_party/rknn 仅含**头文件**（rknn_api.h / rknn_matmul_api.h），**不含 .so**；
#    丙案（lib-scope-ruling.md §6）之后仓库已无 librknnrt.so 可"同步"
#    （`git ls-files 'lib/*.so*'` 为空 —— 上轮"无需同步"的表述已随该案作废）。
#    librknnrt.so 的**链接期单一真源 = 交叉 sysroot**：由 find_library 解析，且上面的
#    FIND_ROOT_PATH_MODE_LIBRARY ONLY 保证只搜 sysroot；出货侧再由
#    scripts/ttbox_fhs_init.sh 的 `rknnrt_gate` 以**同源 sha256 硬门禁**把守。
set(RKNN_SDK_PATH "${CMAKE_SOURCE_DIR}/third_party/rknn" CACHE PATH "RKNN SDK include path (headers only, no .so)")

message(STATUS "=== Cross-compile toolchain ===")
message(STATUS "  CMAKE_SYSTEM_NAME:      ${CMAKE_SYSTEM_NAME}")
message(STATUS "  CMAKE_SYSTEM_PROCESSOR: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  CMAKE_SYSROOT:          ${CMAKE_SYSROOT}")
message(STATUS "  CMAKE_C_COMPILER:       ${CMAKE_C_COMPILER}")
message(STATUS "  CMAKE_CXX_COMPILER:     ${CMAKE_CXX_COMPILER}")
message(STATUS "  RKNN_SDK_PATH:          ${RKNN_SDK_PATH}")
message(STATUS "==============================")
