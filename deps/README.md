# deps —— 三方库供给（共享清单 + 拉取脚本）

本目录是 paint-pc / paint-android 消费者所需三方库的**单一事实来源**。

## 用法
```bash
# 从 SDK 主仓库（本仓库）运行
scripts/fetch-deps.sh --list    # 查看清单
scripts/fetch-deps.sh --check   # 探测本机缺哪些大库
scripts/fetch-deps.sh --fetch   # 拉取/解包缺失大库到 deps/usr，导出 DGCPAIN_DEPS_ROOT
```

## 镜像源
大库（Vulkan/shaderc/glslc）从**国内镜像**下载离线 deb：清华 → 阿里。
镜像 URL 池内置在 manifest.yaml，无需手动配置。

## 无 sudo 设计
全部拉取/解包在仓库内 `deps/` 完成（curl + dpkg-deb -x），不写系统目录，无需 root。

## Windows 策略（诚实标注）
- Windows 下大库 Vulkan/shaderc 由 **LunarG Vulkan SDK** 提供（VULKAN_SDK 环境变量）。
- `fetch-deps.sh --fetch` 在 Windows 仅探测 `VULKAN_SDK` 是否已装：已装即满足；未装则打印国内加速安装指引（LunarG SDK 安装器交互/体积大，**不承诺脚本全自动静默安装**），并尝试从镜像拉 shaderc 预编译产物（如可得）。
- 消费者 setup.sh 在 Windows 下调用 `--check` 给出指引。

## 布局
- `manifest.yaml` — 三方库清单（单一事实来源）
- `usr/` — 解包后的 include/lib/bin（DGCPAIN_DEPS_ROOT 指向这里；由 deb 的 `usr/*` 合并而来）
- `cache/` — 下载的 deb 缓存
