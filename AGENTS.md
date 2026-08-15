# ZzQTermWidget 项目约定

跨平台 Qt6 终端模拟器组件（lxqt/qtermwidget 的深度 fork），C++20 静态库，CMake 构建。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt6 前缀>
cmake --build build --parallel
```

本机 Qt 6.11.1 前缀：`/home/zz/Qt/6.11.1/gcc_64`。

Qt 基线版本 6.8+（`find_package` 已强制 6.8；CI 三平台装 6.8.2，本机用 6.11.1）。

CI 两个平台坑（已固化在 workflow，改动测试/CI 前须知）：
- MSVC job 的 Qt 无可用字体（offscreen 走通用字体库）：所有字符渲染为 tofu、
  度量退化。`windows.yml` 已在 Test 前把 DejaVu Sans Mono 部署到 Qt `lib/fonts`，
  勿删该步骤。
- Windows 上 ctest 捕获不到 QTest 输出：测试目标在 WIN32 下强制控制台子系统，
  且 QTest 同时写 `build/tests/*-qtest.log`，失败时随 artifact（含渲染对照 PNG
  `zzqtermwidget-*.png`）上传，排障以下载 artifact 为准。

## 测试

- 测试位于 `tests/`，使用 Qt 官方 QTest 框架（`Qt6::Test`），无第三方依赖。
- 选项 `-DZZQTERMWIDGET_BUILD_TESTS=ON`（默认开）；运行：`ctest --test-dir build --output-on-failure`。
- 新增核心逻辑（解析器、屏幕缓冲、宽度判定等）必须附带回归测试。
- 渲染像素等价比对（`verifyStructuralEqual`）只在 Linux/FreeType 执行：
  macOS/Windows 光栅器的亚像素字形定位与小数步进使跨光栅器像素恒等原理上
  不可达（片段内 1~4px 平移/AA 差异，非缺陷）；非 Linux 平台由脏区形状断言
  与确定性像素断言（同布局双路径、行带不变式）覆盖。

## 目录结构

- `lib/include/` — 对外公共头
- `lib/src/emulation|display|widget|util/` — 核心源码
- `lib/third_party/utf8proc|ptyqt/` — vendored 第三方（内部不改动风格）
- `lib/resources/` — 配色/键位/翻译/res.qrc（qrc 前缀 `:/lib/qtermwidget`）
- `example/` — 示例程序

## 注释与文档（强制）

- 所有新增和修改的代码注释**强制使用 Doxygen 风格、简体中文**：

```cpp
/**
 * @brief 计算两个整数的和。
 * @param a 第一个加数。
 * @param b 第二个加数。
 * @return 两数之和。
 * @note 如果结果溢出，行为未定义。
 * @see subtract()
 */
int add(int a, int b);
```

- 例外：`lib/third_party/` 内的 vendored 代码保持上游原样，不加注释不改风格。
- 移植上游代码时，其原有英文注释可保留或一并译为中文 Doxygen 风格；新写的说明性注释一律中文。

## Git

- 约定式提交（Conventional Commits），描述可用中文。
- 上游 lxqt/qtermwidget 已配置为 `upstream` remote，移植用 commit hash + 手工对照。
