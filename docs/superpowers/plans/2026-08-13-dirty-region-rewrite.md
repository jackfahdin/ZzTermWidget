# 脏区算法重写（跨度级脏区 + 滚动像素搬迁）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 局部刷新只画脏跨度（行内 `[minX,maxX]` ±1 格），纯整屏滚动跳过已移位行的重复比对；双场景有 benchmark 前后对比证据；tst_rendering 增量重放像素等价零差异。
**架构：** 只改 `lib/src/display/TerminalDisplay.{h,cpp}`（updateImage 内部）与 `tests/tst_benchmark.cpp`、`tests/tst_rendering.cpp`；`drawContents`、QRegion 合并策略、公共头、`lib/third_party/` 零改动。
**技术栈：** Qt 6.11.1（前缀 `/home/zz/Qt/6.11.1/gcc_64`）、C++20、QTest；构建 `cmake --build build --parallel`；测试 `ctest --test-dir build --output-on-failure`。

---

## 勘察结论（实施前必读，行号以勘察时为准、可能漂移，以符号锚点为准）

1. **滚动基础设施已存在**：`TerminalDisplay::updateImage()` 入口（约 :1336）已调用
   `scrollImage(_screenWindow->scrollCount(), _screenWindow->scrollRegion())` 并立即
   `resetScrollCount()`。`scrollImage()`（约 :1173-1261）已完成 `_image` memmove 移位 +
   `QWidget::scroll()` 像素搬迁（含 scrollbar 宽度、`_topMargin`、`SCROLLBAR_CONTENT_GAP` 处理）。
   因此纯滚动帧的现有 dirtyRegion **本就只含新进 N 行**；任务 3 的收益仅是跳过已移位行的
   逐格比对（`Character::operator!=`），不改变任何可观测行为（脏区形状、像素均不变）。
2. **scrollCount 时序**：`updateImage` 由 `ScreenWindow::outputChanged` 直连触发
   （`TerminalDisplay.cpp:159-162`），链路为 `receiveData → Emulation::bufferedUpdate
   （_bulkTimer1 10ms / _bulkTimer2 40ms）→ showBulk → outputChanged → notifyOutputChanged
   → outputChanged → updateImage`；测试中用 `emu.receiveData(...)` + `display.updateImage()`
   同步驱帧绕过定时器（tst_sixel 已有先例）。
3. **备选屏切换**：`Emulation::setScreen()`（Emulation.cpp:120-127）调 `window->setScreen()`
   但 `ScreenWindow::setScreen` **不重置 `_scrollCount`** → 快路径须以屏幕指针变化检测回退
   （新成员 `Screen* _lastImageScreen`，帧尾更新）。
4. **错位检测不可用 memcmp**：`Character` 布局 = char32_t(4) + quint16(2) + 2×CharacterColor(各 4B)
   = 14 有效字节 + 2 尾部填充（`CharacterColor` 为 4×quint8，见 CharacterColor.h:227-232）；
   `Screen::copyFromScreen`（Screen.cpp:442-461）逐格赋值不同步填充字节 → 验证必须逐格
   `operator!=`（早退），验证循环顺带做 RE_BLINK 扫描（零额外成本，避免 blink 粘滞问题）。
5. **选区/光标烤进 newimg**：`Screen::getImage`（Screen.cpp:463-497）把选区
   `reverseRendition` 与 `RE_CURSOR` 直接写进返回缓冲 → 选区变化天然被逐格比对捕获；
   快路径仍以 `_screenWindow->isClearSelection()` 为显式回退条件（双保险）。
6. **宽字符**：尾格 `character == 0`（Screen.cpp:914-921 注释证实）；`drawContents` 对
   脏 rect 左边界有"回退找宽字符头"（TerminalDisplay.cpp:2157-2158）、右边界尾格补偿
   （:2242-2243），±1 格跨度扩展为第一道防线。
7. **benchmark 现状问题**：`testTuiPartialRepaint`（tst_benchmark.cpp:106-126）用
   `render(&image)` 全区域渲染计量，且帧负载固定——第二次迭代起屏幕内容相同、无脏区，
   计量的实为重复全量渲染。基线文件无落盘代码，靠 QTest `-o` 输出人工保存。
8. **像素等价设施**：tst_rendering 有 `initRenderEnv`/`renderDisplay`/`buildRenderContent`/
   `QCOMPARE(image, image)`；tst_sixel 有 `DirtyProbeDisplay`（paintEvent 区域累积）与
   行带断言模式（`fontHeight()`、`contentsRect().top() + display.margin()` 行几何）。
   本计划新增"增量重放"：`render(&base, QPoint(), display.lastDirtyRegion())` 与
   全新全量 render 逐像素比对。
9. **公开访问器**：`fontHeight()`/`fontWidth()`（TerminalDisplay.h:268/:273）、
   `cellPixelWidth()/cellPixelHeight()`（:276/:278）、`margin()`（:203，返回 `_topBaseMargin`）。
10. **行几何同源**：脏行矩形 = `_leftMargin + tLx + x*_fontWidth` 起（updateImage :1437-1439）；
    测试行带 = `contentsRect().top() + margin() + y*fontHeight()`（tst_sixel.cpp:287-297 同款）。
11. **测试注册**：tests/CMakeLists.txt 为 foreach 列表注册，本计划**不新增测试文件**
    （用例追加进 tst_rendering/tst_benchmark），tests/CMakeLists.txt 与 lib/CMakeLists.txt 不动。
12. **图像行例外语义**：原代码 `hasImages && !updateLine && (图查询)` 的 `!updateLine` 只是
    短路优化——含图行无论字符脏否最终都整行脏；新代码对含图行直接 `fullLineDirty = true`，
    行为完全一致（逐行图查询仅在 `hasImages()` 为真时发生，无图屏零开销）。

## 跨任务命名一致性（自检项）

| 名称 | 种类 | 引入任务 |
|------|------|----------|
| `QRegion lastDirtyRegion() const` / 成员 `_lastDirtyRegion` | TerminalDisplay 公共测试钩子 | 1 |
| `setScrollOptimizationEnabled(bool)` / `isScrollOptimizationEnabled()` / 成员 `_scrollOptimizationEnabled`（默认 true） | TerminalDisplay 公共内部开关（镜像 setTextBatchingEnabled，TerminalDisplay.h:420 旁） | 3 |
| `Screen* _lastImageScreen = nullptr` | TerminalDisplay 私有成员 | 3 |
| `renderFull(display)` / `replayDirtyRegion(display, base)` | tst_rendering 静态设施 | 2 |
| `buildScrollPayload(from, count)` | tst_rendering 与 tst_benchmark 各自的静态函数（测试间无共享头，沿用 initRenderEnv/initDisplayEnv 重复惯例） | 1/3 |
| 用例名 | `testLocalRefresh`、`testFullScreenScroll(_data)`（benchmark）；`testSpanDirtyPixelEquivalence(_data)`、`testSpanDirtySelectionFrame`、`testScrollFastPathPixelEquivalence`、`testScrollMixedContentFallback`、`testScrollImageFallback`、`testScrollSelectionFallback`、`testScrollOptimizationSwitchAB`（rendering） | 1/2/3 |

---

## 任务 1：benchmark 口径修正 + 双场景基线（先行，后续任务的对比证据）

**目标**：新增 `lastDirtyRegion()` 观测钩子；TUI 用例改为真实增量口径（updateImage 比对段 +
脏区离屏渲染段），修复帧负载幂等问题；新增整屏滚动场景（24x80 / 40x160 双尺寸）；落盘基线。

### 步骤

- [ ] **1.1 加观测钩子**（`lib/src/display/TerminalDisplay.h`、`TerminalDisplay.cpp`）

  TerminalDisplay.h：在 `setTextBatchingEnabled`（:420）旁插入：

  ```cpp
      /**
       * @brief 查询上一次 updateImage() 计算并提交给 QWidget::update() 的脏区。
       * @return 上次帧的脏区；updateImage() 未执行或被同步输出攒帧拦截时为上一次的值。
       * @note 测试与 benchmark 计量钩子（镜像 _drawTextTestFlag 的内部观测点惯例）；
       *       内部接口，不进公共头 qtermwidget.h。
       */
      QRegion lastDirtyRegion() const { return _lastDirtyRegion; }
  ```

  私有成员区 `_imageViewTopLine`（:930）旁插入：

  ```cpp
      /** @brief 上一次 updateImage() 提交的脏区；lastDirtyRegion() 测试钩子用。 */
      QRegion _lastDirtyRegion;
  ```

  TerminalDisplay.cpp `updateImage()` 中 `update(dirtyRegion);`（:1479）**之前**插入一行：

  ```cpp
      _lastDirtyRegion = dirtyRegion; // 测试/benchmark 观测钩子：记录本次帧脏区
  ```

  构建：`cmake --build build --parallel` → 预期无错误无新告警。

- [ ] **1.2 重写 `tests/tst_benchmark.cpp` 全文**（保留 testParseThroughput/testDrawFullRepaint 原文，
  替换 TUI 用例、新增滚动用例）：

  ```cpp
  #include <QtTest>
  #include <QFontDatabase>
  #include <QImage>
  #include "Vt102Emulation.h"
  #include "ScreenWindow.h"
  #include "Screen.h"
  #include "TerminalDisplay.h"

  /**
   * @brief 渲染性能 benchmark 基线：解析吞吐 / 全量重绘 / 局部刷新 / 整屏滚动四用例。
   * @note 不设硬性性能断言（机器差异大），仅保证可编译可运行；优化前后各跑一遍，
   *       数字人工对比并记入 CHANGELOG。数值仅在 Release 构建下有参考意义。
   * @note 局部刷新与整屏滚动为真实增量口径：receiveData 产帧 → updateImage()（脏区比对段）
   *       → 按 lastDirtyRegion() 离屏渲染（渲染段）；帧负载交替变化保证每次迭代都有真实脏区。
   */
  class TestBenchmark : public QObject
  {
      Q_OBJECT
  private slots:
      void testParseThroughput();
      void testDrawFullRepaint();
      void testLocalRefresh();
      void testFullScreenScroll_data();
      void testFullScreenScroll();
  private:
      int m_frame = 0; ///< 帧奇偶计数：交替负载保证每次迭代都产生真实脏区
  };

  /**
   * @brief 构造混合内容负载：普通文本 / SGR 颜色转义 / CJK 宽字符 / 样式与制表符混排。
   * @param lineCount 生成行数。
   * @return UTF-8 编码的字节流。
   */
  static QByteArray buildMixedPayload(int lineCount)
  {
      QByteArray payload;
      for (int i = 0; i < lineCount; i++) {
          switch (i % 4) {
          case 0:
              payload += "build/output line " + QByteArray::number(i)
                       + ": plain ascii text lorem ipsum dolor sit\r\n";
              break;
          case 1:
              payload += "\033[38;5;" + QByteArray::number(16 + (i % 216))
                       + "m256色前景 \033[48;2;30;120;200mRGB底色\033[0m 混合样式输出\r\n";
              break;
          case 2:
              payload += "CJK 宽字符混排：中文测试文本 abc 123 コンソール 端末エミュレータ\r\n";
              break;
          default:
              payload += "\033[1;3;4m粗斜下划线\033[0m 制表符 ─│┌┐└┘ 与 emoji 😀 混排\r\n";
              break;
          }
      }
      return payload;
  }

  /**
   * @brief 构造整屏滚动负载：持续行输出（行号递增）使视图匀速上滚。
   * @param from 起始行号。 @param count 行数。
   * @return UTF-8 编码的字节流。
   */
  static QByteArray buildScrollPayload(int from, int count)
  {
      QByteArray payload;
      for (int i = from; i < from + count; i++)
          payload += "scroll payload line " + QByteArray::number(i)
                   + " mixed 文本 abc 123 lorem ipsum\r\n";
      return payload;
  }

  /**
   * @brief 构造显示测试环境：仿真 + 窗口 + 离屏显示组件（等宽字体，关闪烁）。
   * @param lines/columns 终端行列数（默认 24x80；滚动场景另测 40x160 大屏变体）。
   */
  static void initDisplayEnv(Vt102Emulation &emu, ScreenWindow *&win, TerminalDisplay &display,
                             int lines = 24, int columns = 80)
  {
      emu.setCodec(QStringEncoder(QStringConverter::Utf8));
      emu.setImageSize(lines, columns);
      win = emu.createWindow();
      win->setWindowLines(lines);
      display.setVTFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
      display.setBlinkingCursor(false);
      display.setBlinkingTextEnabled(false);
      display.setScreenWindow(win);
      display.resize(columns * 10, lines * 25);
  }

  /**
   * @brief 解析吞吐：向 Emulation+Screen 喂大块混合输出，计量 receiveData 吞吐。
   */
  void TestBenchmark::testParseThroughput()
  {
      Vt102Emulation emu;
      emu.setCodec(QStringEncoder(QStringConverter::Utf8));
      emu.setImageSize(24, 80);
      ScreenWindow *win = emu.createWindow();
      win->setWindowLines(24);
      const QByteArray payload = buildMixedPayload(2000); // 约 200KB 混合内容
      QBENCHMARK {
          emu.receiveData(payload.constData(), int(payload.size()));
      }
  }

  /**
   * @brief 绘制吞吐：预填满屏混合内容后，循环全量重绘（render 到 QImage）。
   */
  void TestBenchmark::testDrawFullRepaint()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initDisplayEnv(emu, win, display);
      const QByteArray content = buildMixedPayload(24);
      emu.receiveData(content.constData(), int(content.size()));
      display.updateImage();
      QImage image(display.size(), QImage::Format_ARGB32);
      display.render(&image); // warmup：吃掉 _drawTextTestFlag 一次性度量
      QBENCHMARK {
          display.render(&image);
      }
  }

  /**
   * @brief 局部刷新：模拟 nvim 帧负载——光标行内容 + 状态行时钟交替小区域更新。
   * @note 真实增量口径：updateImage() 比对段 + 按 lastDirtyRegion() 离屏渲染段；
   *       帧 A/B 仅时钟字符不同，每次迭代都产生单行少格真实脏区。
   */
  void TestBenchmark::testLocalRefresh()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initDisplayEnv(emu, win, display);
      const QByteArray content = buildMixedPayload(24);
      emu.receiveData(content.constData(), int(content.size()));
      display.updateImage();
      QImage image(display.size(), QImage::Format_ARGB32);
      image.fill(Qt::black);
      display.render(&image); // warmup
      const QByteArray frameA =
              "\033[2;5H\033[38;5;45mmain.cpp\033[0m"            // 光标行内容更新
              "\033[24;1H\033[7m NORMAL  main.cpp  12:5  utf-8 \033[0m" // 状态行重写
              "\033[2;10H";                                       // 光标归位
      const QByteArray frameB =
              "\033[2;5H\033[38;5;45mmain.cpp\033[0m"
              "\033[24;1H\033[7m NORMAL  main.cpp  12:6  utf-8 \033[0m"
              "\033[2;10H";
      QBENCHMARK {
          const QByteArray &frame = ((m_frame++ & 1) == 0) ? frameA : frameB;
          emu.receiveData(frame.constData(), int(frame.size()));
          display.updateImage();   // 脏区比对段（public slot，绕过 bufferedUpdate 定时器）
          display.render(&image, QPoint(), display.lastDirtyRegion()); // 脏区渲染段
      }
  }

  void TestBenchmark::testFullScreenScroll_data()
  {
      QTest::addColumn<int>("lines");
      QTest::addColumn<int>("columns");
      QTest::newRow("24x80") << 24 << 80;
      QTest::newRow("40x160") << 40 << 160; // 大屏变体：比对段开销随格数放大，增强区分度
  }

  /**
   * @brief 整屏滚动：持续行输出使视图匀速上滚，每次迭代滚 4 行。
   * @note 滚动帧的脏区本就只有新进 4 行（scrollImage 像素搬迁为既有行为）；
   *       本用例的对比价值在 updateImage() 比对段耗时（任务 3 快路径的裁决证据）。
   */
  void TestBenchmark::testFullScreenScroll()
  {
      QFETCH(int, lines);
      QFETCH(int, columns);
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initDisplayEnv(emu, win, display, lines, columns);
      const QByteArray content = buildScrollPayload(0, lines + 10); // 填满并进入滚动态
      emu.receiveData(content.constData(), int(content.size()));
      display.updateImage();
      QImage image(display.size(), QImage::Format_ARGB32);
      image.fill(Qt::black);
      display.render(&image); // warmup
      int lineNo = lines + 10;
      QBENCHMARK {
          const QByteArray out = buildScrollPayload(lineNo, 4);
          lineNo += 4;
          emu.receiveData(out.constData(), int(out.size()));
          display.updateImage();
          display.render(&image, QPoint(), display.lastDirtyRegion());
      }
  }

  QTEST_MAIN(TestBenchmark)
  #include "tst_benchmark.moc"
  ```

- [ ] **1.3 构建、跑基线、落盘**

  ```bash
  cmake --build build --parallel
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark -o -,txt -o build/benchmark-baseline.txt,txt
  ```

  预期：4 个用例全 PASS，输出含 `RESULT : TestBenchmark::testLocalRefresh(): ... msecs per iteration`
  与 `testFullScreenScroll():24x80 / 40x160` 两条 RESULT 行。把 `build/benchmark-baseline.txt`
  中 testLocalRefresh 与 testFullScreenScroll 的数字抄到工作笔记（任务 2/3 后对比用）。

- [ ] **1.4 全量回归 + commit**

  ```bash
  ctest --test-dir build --output-on-failure
  ```

  预期 9 套件全绿。提交：

  ```
  test(benchmark): 修正局部刷新计量口径并新增整屏滚动场景

  - TerminalDisplay 新增 lastDirtyRegion() 观测钩子（镜像 _drawTextTestFlag 惯例）
  - TUI 用例改真实增量口径（updateImage 比对段 + lastDirtyRegion 离屏渲染段），
    帧负载 A/B 交替修复二次迭代起无脏区的计量失真
  - 新增整屏滚动场景（24x80 / 40x160 双尺寸），基线落盘 build/benchmark-baseline.txt
  ```

---

## 任务 2：行内跨度级脏区（updateImage 改造 + 像素等价）

**目标**：dirtyMask 两趟扫描合并为一趟 `[minX,maxX]` 跨度聚合（±1 格钳位扩展）；双高行/含图行/
收缩区/graphicsDirty/preedit 例外保持整行脏；先失败测试（区域形状）+ 常驻像素等价。

### 步骤

- [ ] **2.1 先写失败测试**：`tests/tst_rendering.cpp` 追加设施与用例（完整代码）。

  文件头槽位列表 `private slots:` 追加：

  ```cpp
      void testSpanDirtyPixelEquivalence_data();
      void testSpanDirtyPixelEquivalence();
      void testSpanDirtySelectionFrame();
  ```

  文件尾部（`QTEST_MAIN` 之前）追加：

  ```cpp
  /**
   * @brief 全量离屏渲染：黑底全新一帧（调用方负责首帧 warmup 吃掉 _drawTextTestFlag）。
   */
  static QImage renderFull(TerminalDisplay &display)
  {
      QImage image(display.size(), QImage::Format_ARGB32);
      image.fill(Qt::black);
      display.render(&image);
      return image;
  }

  /**
   * @brief 增量重放：仅把上一次 updateImage() 的脏区渲染到上一帧图像上。
   * @param display 目标显示组件（须刚调用过 updateImage()）。
   * @param base 上一帧的全量渲染结果。
   * @return 重放后的图像；与 renderFull() 逐像素相等即增量路径无遗漏脏区。
   */
  static QImage replayDirtyRegion(TerminalDisplay &display, const QImage &base)
  {
      QImage image = base;
      display.render(&image, QPoint(), display.lastDirtyRegion());
      return image;
  }

  void TestRendering::testSpanDirtyPixelEquivalence_data()
  {
      QTest::addColumn<QByteArray>("setup");     // 首帧内容后的追加构造（双高行/图像等）
      QTest::addColumn<QByteArray>("edit");      // 本帧编辑负载
      QTest::addColumn<int>("editRow");          // 编辑所在行（0 起）
      QTest::addColumn<int>("editCells");        // 预期脏跨度格数上限；0 = 不做形状断言

      QTest::newRow("单行少格")
              << QByteArray()
              << QByteArray("\033[5;10H\033[38;5;45mEDIT\033[0m") << 4 << 6;
      QTest::newRow("宽字符跨界")
              << QByteArray()
              << QByteArray("\033[4;21H\xe7\x95\xbb") << 3 << 0; // CJK 行内改写宽字符"画"
      QTest::newRow("斜体越界")
              << QByteArray()
              << QByteArray("\033[2;10H\033[3mITAL\033[0m") << 1 << 0;
      QTest::newRow("双高行整行脏")
              << QByteArray("\033[10;1H\033#3double high line\r\n\033#4double high line\r\n")
              << QByteArray("\033[10;3HZ") << 9 << 0;
      QTest::newRow("含图像行")
              << QByteArray("\033[12;1H\033Pq#0;2;100;0;0#0!8~-!8~-!8~\033\\")
              << QByteArray("\033[12;20HIMG") << 11 << 0;
  }

  /**
   * @brief 跨度脏区：编辑帧的增量重放与全量渲染逐像素相等；单行少格场景的脏带
   *        宽度不得超过（编辑格数 + 两侧各 1 格扩展 + 1 格余量）× 格宽（形状证据）。
   * @note 形状断言即本任务的先失败测试：整行脏区旧实现下脏带恒为整行宽，必然失败。
   */
  void TestRendering::testSpanDirtyPixelEquivalence()
  {
      QFETCH(QByteArray, setup);
      QFETCH(QByteArray, edit);
      QFETCH(int, editRow);
      QFETCH(int, editCells);

      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initRenderEnv(emu, win, display);
      // sixel 场景需要单元格像素尺寸（镜像 tst_sixel 的 initSixelRenderEnv）
      emu.setCellPixelSize(display.cellPixelWidth(), display.cellPixelHeight());
      const QByteArray content = buildRenderContent();
      emu.receiveData(content.constData(), int(content.size()));
      if (!setup.isEmpty())
          emu.receiveData(setup.constData(), int(setup.size()));
      display.updateImage();
      renderFull(display); // warmup：吃掉 _drawTextTestFlag 一次性度量
      const QImage base = renderFull(display);

      emu.receiveData(edit.constData(), int(edit.size()));
      display.updateImage();

      if (editCells > 0) {
          const int fh = display.fontHeight();
          const int rowTop = display.contentsRect().top() + display.margin() + editRow * fh;
          const QRect band(0, rowTop, display.width(), fh);
          int maxWidth = 0;
          const auto rects = display.lastDirtyRegion();
          for (const QRect &r : rects)
              if (r.intersects(band))
                  maxWidth = qMax(maxWidth, r.width());
          QVERIFY2(maxWidth > 0 && maxWidth <= (editCells + 3) * display.fontWidth(),
                   qPrintable(QStringLiteral("编辑行脏带宽 %1 超出跨度上限 %2")
                              .arg(maxWidth).arg((editCells + 3) * display.fontWidth())));
      }

      const QImage incremental = replayDirtyRegion(display, base);
      const QImage full = renderFull(display);
      if (incremental != full) { // 排障辅助：落盘人工比对
          incremental.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-span-incremental.png")));
          full.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-span-full.png")));
      }
      QCOMPARE(incremental, full);
  }

  /**
   * @brief 选区高亮帧：selectAll 把选区 rendition 烤进 newimg，逐格比对捕获，
   *        增量重放与全量渲染仍逐像素相等。
   */
  void TestRendering::testSpanDirtySelectionFrame()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initRenderEnv(emu, win, display);
      const QByteArray content = buildRenderContent();
      emu.receiveData(content.constData(), int(content.size()));
      display.updateImage();
      renderFull(display); // warmup
      const QImage base = renderFull(display);

      display.selectAll();
      display.updateImage();

      const QImage incremental = replayDirtyRegion(display, base);
      const QImage full = renderFull(display);
      QCOMPARE(incremental, full);
  }
  ```

  构建并运行，确认**先失败**：

  ```bash
  cmake --build build --parallel
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_rendering
  ```

  预期：`testSpanDirtyPixelEquivalence():单行少格` FAIL（脏带为整行宽 ≈ 80×fontWidth，
  超出 `(6+3)×fontWidth` 上限）；其余行 PASS（像素等价对旧实现也成立——它是常驻安全网）。

- [ ] **2.2 改造 `updateImage()` 比对循环**（TerminalDisplay.cpp :1365-1448 段）。

  删除 `char *dirtyMask = new char[columnsToUpdate + 2];`（:1365）与末尾 `delete[] dirtyMask;`
  （:1487），逐行循环体（:1381-1448）整体替换为：

  ```cpp
      // debugging variable, this records the number of lines that are found to
      // be 'dirty' ( ie. have changed from the old _image to the new _image ) and
      // which therefore need to be repainted
      for (y = 0; y < linesToUpdate; ++y) {
          const Character *currentLine = &_image[y * this->_columns];
          const Character *const newLine = &newimg[y * columns];

          // 跨度聚合：一趟完成逐格比对、blink 扫描与 [minX,maxX] 聚合，取代原 dirtyMask
          // 两趟扫描与每帧堆分配。脏格邻居不再单独置位，改为出循环后跨度两侧各扩 1 格——
          // 吸收宽字符尾部变脏与斜体/衬线字形 ±1 格越界（与原 dirtyMask 注释语义一致）
          int minX = columnsToUpdate;
          int maxX = -1;
          if (!_resizing) // not while _resizing, we're expecting a paintEvent
              for (x = 0; x < columnsToUpdate; ++x) {
                  if ((newLine[x].rendition & RE_BLINK) != 0)
                      _hasBlinker = true;
                  if (newLine[x] != currentLine[x]) {
                      minX = qMin(minX, x);
                      maxX = qMax(maxX, x);
                  }
              }
          else
              for (x = 0; x < columnsToUpdate; ++x) {
                  if (newLine[x] != currentLine[x]) {
                      minX = qMin(minX, x);
                      maxX = qMax(maxX, x);
                  }
              }
          bool updateLine = maxX >= 0;
          bool fullLineDirty = false; // 例外行保持整行脏（行为与改造前一致）

          // both the top and bottom halves of double height _lines must always be
          // redrawn although both top and bottom halves contain the same characters,
          // only the top one is actually drawn.
          if (_lineProperties.count() > y) {
              if ((_lineProperties[y] & LINE_DOUBLEHEIGHT) != 0) {
                  updateLine = true;
                  fullLineDirty = true;
              }
          }

          // 滚动前后两个视图中实际含图像放置的行强制整行标脏（sixel 切片与 kitty 放置同理）：
          // 新视图含图行补画，滚动前视图含图行抹除残留。原实现的 !updateLine 短路只是省查询，
          // 含图行无论字符脏否最终都整行脏，此处直接判定、行为不变；无图时 hasImages() 短路
          if (hasImages
                  && (!scr->imagePlacements(viewTopLine + y).isEmpty()
                      || !scr->kittyRefs(viewTopLine + y).isEmpty()
                      || (prevViewTopLine != viewTopLine
                          && (!scr->imagePlacements(prevViewTopLine + y).isEmpty()
                              || !scr->kittyRefs(prevViewTopLine + y).isEmpty())))) {
              updateLine = true;
              fullLineDirty = true;
          }

          // if the characters on the line are different in the old and the new _image
          // then this line must be repainted.
          if (updateLine) {
              // 非例外行只重绘脏跨度：两侧各扩 1 格并钳到 [0, columnsToUpdate-1]
              int spanMin = 0;
              int spanMax = columnsToUpdate - 1;
              if (!fullLineDirty) {
                  spanMin = qMax(0, minX - 1);
                  spanMax = qMin(columnsToUpdate - 1, maxX + 1);
              }
              const QRect dirtyRect =
                      QRect(_leftMargin + tLx + spanMin * _fontWidth,
                            _topMargin + tLy + _fontHeight * y,
                            (spanMax - spanMin + 1) * _fontWidth, _fontHeight);

              dirtyRegion |= dirtyRect;
          }

          // replace the line of characters in the old _image with the
          // current line of the new _image
          memcpy((void *)currentLine, (const void *)newLine,
                       columnsToUpdate * sizeof(Character));
      }
  ```

  `_usedLines/_usedColumns` 收缩区、graphicsDirty、preedit、`update(dirtyRegion)`、blink 定时器
  段（:1450-1486）**原样保留**（含任务 1 加的 `_lastDirtyRegion = dirtyRegion;`）。

- [ ] **2.3 验证**

  ```bash
  cmake --build build --parallel
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_rendering
  ctest --test-dir build --output-on-failure
  ```

  预期：tst_rendering 全绿（含"单行少格"形状断言转 PASS）；9 套件全绿。
  跑 benchmark 对比任务 1 基线：

  ```bash
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark -o -,txt
  ```

  预期：`testLocalRefresh` 每迭代耗时较基线显著下降（脏区渲染段从整行缩到跨度）；
  `testFullScreenScroll` 基本持平（比对段尚未改造）。数字抄入工作笔记。

- [ ] **2.4 commit**（先测试后实现两个提交）

  ```
  test(rendering): 跨度脏区像素等价与区域形状测试（先失败）

  增量重放设施（renderFull/replayDirtyRegion）+ 单行少格/宽字符跨界/斜体越界/
  双高行/含图像行/选区帧六场景；单行少格含脏带宽度形状断言，对整行脏区旧实现失败。
  ```
  ```
  perf(display): updateImage 行内跨度级脏区

  dirtyMask 两趟扫描合并为一趟 [minX,maxX] 跨度聚合（顺带消除每帧堆分配），
  脏 rect 只盖跨度 ±1 格；双高行/含图行例外保持整行脏，行为不变。
  tst_rendering 增量重放像素等价零差异；testLocalRefresh 较基线下降 __%。
  ```

---

## 任务 3：纯整屏滚动快路径（跳过已移位行重复比对 + 保守回退 + 内部开关）

**目标**：纯整屏滚动帧对 moved 行只做一次逐格验证（错位检测，顺带 blink 扫描）+ memcpy 同步，
不再走跨度聚合；新进 N 行走任务 2 跨度比对。任一回退条件命中即走现有全量路径。

**TDD 让步说明（勘察结论）**：scrollImage 的像素搬迁 + `_image` 移位是既有行为，纯滚动帧的
脏区形状在改造前后相同（都只有新进 N 行），本任务**无可观测行为差异、无法构造先失败测试**。
测试为等价/回退安全网（对现状即绿、防快路径实现错误），收益由 benchmark `testFullScreenScroll`
比对段耗时裁决。

### 步骤

- [ ] **3.1 加开关与成员**（TerminalDisplay.h）

  `setTextBatchingEnabled` 旁插入：

  ```cpp
      /**
       * @brief 开关纯整屏滚动快路径（已移位行跳过一次重复比对，新进 N 行走跨度级脏区）。
       * @param enabled true = 启用（默认）；false = 回退全屏逐格比对。
       * @note 供 benchmark A/B 与故障回退；内部接口，不进公共头 qtermwidget.h。
       */
      void setScrollOptimizationEnabled(bool enabled) { _scrollOptimizationEnabled = enabled; }

      /** @brief 查询纯整屏滚动快路径是否启用。 */
      bool isScrollOptimizationEnabled() const { return _scrollOptimizationEnabled; }
  ```

  私有成员区 `_lastDirtyRegion` 旁插入：

  ```cpp
      /** @brief 纯整屏滚动快路径开关（setScrollOptimizationEnabled()）。 */
      bool _scrollOptimizationEnabled = true;
      /** @brief 上一次 updateImage() 的 Screen 指针；备选屏切换帧检测（快路径回退条件）。 */
      Screen *_lastImageScreen = nullptr;
  ```

  确认 TerminalDisplay.cpp 已 include Screen.h（现有代码已用 `Screen` 完整类型，:1372，无需新增）。

- [ ] **3.2 写安全网测试**：`tests/tst_rendering.cpp` 追加。

  槽位列表追加：

  ```cpp
      void testScrollFastPathPixelEquivalence();
      void testScrollMixedContentFallback();
      void testScrollImageFallback();
      void testScrollSelectionFallback();
      void testScrollOptimizationSwitchAB();
  ```

  文件尾部追加：

  ```cpp
  /**
   * @brief 构造整屏滚动负载：持续行输出（行号递增）使视图匀速上滚。
   */
  static QByteArray buildScrollPayload(int from, int count)
  {
      QByteArray payload;
      for (int i = from; i < from + count; i++)
          payload += "scroll payload line " + QByteArray::number(i)
                   + " mixed 文本 abc 123\r\n";
      return payload;
  }

  /**
   * @brief 把显示组件推进滚动态：填满屏幕并多输出 10 行使视图开始上滚，
   *        完成 warmup 后返回首帧全量渲染结果。
   */
  static QImage enterScrollingState(Vt102Emulation &emu, TerminalDisplay &display, int lines)
  {
      const QByteArray content = buildScrollPayload(0, lines + 10);
      emu.receiveData(content.constData(), int(content.size()));
      display.updateImage();
      renderFull(display); // warmup：吃掉 _drawTextTestFlag
      return renderFull(display);
  }

  /**
   * @brief 纯滚动帧（1 行与 N 行）：增量重放与全量渲染逐像素相等，
   *        且脏区仅覆盖新进 N 行行带（远小于全屏）。
   * @note 快路径正确性安全网：若 moved 行被错误跳过（漏脏），重放结果将与全量渲染不符。
   */
  void TestRendering::testScrollFastPathPixelEquivalence()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initRenderEnv(emu, win, display);
      QImage base = enterScrollingState(emu, display, 24);

      int lineNo = 34;
      const int scrollSteps[] = {1, 4, 3}; // 连续滚动 1 行与 N 行
      for (const int n : scrollSteps) {
          const QByteArray out = buildScrollPayload(lineNo, n);
          lineNo += n;
          emu.receiveData(out.constData(), int(out.size()));
          display.updateImage();

          const int band = display.lastDirtyRegion().boundingRect().height();
          QVERIFY2(band > 0 && band <= (n + 1) * display.fontHeight(),
                   qPrintable(QStringLiteral("滚动 %1 行脏区高度 %2 超出新进 N 行行带")
                              .arg(n).arg(band)));

          const QImage incremental = replayDirtyRegion(display, base);
          const QImage full = renderFull(display);
          QCOMPARE(incremental, full);
          base = full;
      }
  }

  /**
   * @brief 滚动+局部修改混合帧：滚动外修改必须触发错位回退，修改行进入脏区，
   *        增量重放与全量渲染逐像素相等。
   */
  void TestRendering::testScrollMixedContentFallback()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initRenderEnv(emu, win, display);
      const QImage base = enterScrollingState(emu, display, 24);

      // 一帧内：先输出 3 行（滚 3 行），再绝对定位回 moved 区改两格
      QByteArray frame = buildScrollPayload(34, 3);
      frame += "\033[10;5H\033[38;5;45mXX\033[0m";
      emu.receiveData(frame.constData(), int(frame.size()));
      display.updateImage();

      // 回退证据：视图第 10 行（0 起 9）行带必须在脏区内
      const int fh = display.fontHeight();
      const int rowTop = display.contentsRect().top() + display.margin();
      const QRect band9(0, rowTop + 9 * fh, display.width(), fh);
      QVERIFY2(display.lastDirtyRegion().intersects(band9),
               "滚动外修改行未进入脏区（错位回退失效）");

      const QImage incremental = replayDirtyRegion(display, base);
      const QImage full = renderFull(display);
      QCOMPARE(incremental, full);
  }

  /**
   * @brief 含图滚动帧：hasImages() 回退条件命中，滚动前后含图行都在脏区内，
   *        增量重放与全量渲染逐像素相等（行带断言镜像 tst_sixel 模式）。
   */
  void TestRendering::testScrollImageFallback()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initRenderEnv(emu, win, display);
      emu.setCellPixelSize(display.cellPixelWidth(), display.cellPixelHeight());

      // 填满进入滚动态后，在屏幕底部锚定 8x18 sixel 图（约占 2 个网格行）
      QImage base = enterScrollingState(emu, display, 24);
      const QByteArray seq = QByteArray("\033Pq#0;2;100;0;0#0!8~-!8~-!8~\033\\");
      emu.receiveData(seq.constData(), int(seq.size()));
      display.updateImage();
      base = renderFull(display);

      // 滚 2 行：图像随内容上滚，hasImages() 使快路径回退
      const QByteArray out = buildScrollPayload(34, 2);
      emu.receiveData(out.constData(), int(out.size()));
      display.updateImage();

      // 回退证据：新视图含图行（补画）与滚动前视图含图行（抹除残留）都在脏区内
      Screen *scr = win->screen();
      const int fh = display.fontHeight();
      const int rowTop = display.contentsRect().top() + display.margin();
      const int newTop = win->currentLine();
      int checkedRows = 0;
      for (int y = 0; y < win->windowLines(); y++) {
          const bool inNewView = !scr->imagePlacements(newTop + y).isEmpty();
          const bool inOldView = !scr->imagePlacements(newTop + 2 + y).isEmpty();
          if (!inNewView && !inOldView)
              continue;
          checkedRows++;
          const QRect band(0, rowTop + fh * y, display.width(), fh);
          QVERIFY2(display.lastDirtyRegion().intersects(band),
                   qPrintable(QStringLiteral("含图视图行 %1 未进入脏区").arg(y)));
      }
      QVERIFY(checkedRows >= 1); // 防空断言

      const QImage incremental = replayDirtyRegion(display, base);
      const QImage full = renderFull(display);
      QCOMPARE(incremental, full);
  }

  /**
   * @brief 选区回退帧：活动选区下滚动帧回退全量路径，选区 rendition 反转行大面积进脏区，
   *        增量重放与全量渲染逐像素相等。
   */
  void TestRendering::testScrollSelectionFallback()
  {
      Vt102Emulation emu;
      ScreenWindow *win = nullptr;
      TerminalDisplay display;
      initRenderEnv(emu, win, display);
      QImage base = enterScrollingState(emu, display, 24);

      display.selectAll();
      display.updateImage(); // 选区高亮帧落账
      base = renderFull(display);

      const QByteArray out = buildScrollPayload(34, 2);
      emu.receiveData(out.constData(), int(out.size()));
      display.updateImage();

      // 回退证据：选区随内容滚动反转 rendition，脏区远大于新进 2 行行带
      QVERIFY(display.lastDirtyRegion().boundingRect().height() >= 20 * display.fontHeight());

      const QImage incremental = replayDirtyRegion(display, base);
      const QImage full = renderFull(display);
      QCOMPARE(incremental, full);
  }

  /**
   * @brief 开关 A/B：两套独立环境喂相同滚动负载，开/关快路径的增量重放结果
   *        逐像素相等且都与全量渲染一致。
   */
  void TestRendering::testScrollOptimizationSwitchAB()
  {
      Vt102Emulation emuA, emuB;
      ScreenWindow *winA = nullptr, *winB = nullptr;
      TerminalDisplay displayA, displayB;
      initRenderEnv(emuA, winA, displayA);
      initRenderEnv(emuB, winB, displayB);
      displayB.setScrollOptimizationEnabled(false);
      QVERIFY(displayA.isScrollOptimizationEnabled());
      QVERIFY(!displayB.isScrollOptimizationEnabled());

      QImage baseA = enterScrollingState(emuA, displayA, 24);
      QImage baseB = enterScrollingState(emuB, displayB, 24);

      for (int round = 0; round < 3; round++) {
          const QByteArray out = buildScrollPayload(34 + round * 3, 3);
          emuA.receiveData(out.constData(), int(out.size()));
          emuB.receiveData(out.constData(), int(out.size()));
          displayA.updateImage();
          displayB.updateImage();

          const QImage incrementalA = replayDirtyRegion(displayA, baseA);
          const QImage incrementalB = replayDirtyRegion(displayB, baseB);
          const QImage full = renderFull(displayA);
          QCOMPARE(incrementalA, full);
          QCOMPARE(incrementalB, full);
          QCOMPARE(incrementalA, incrementalB);
          baseA = full;
          baseB = renderFull(displayB);
      }
  }
  ```

  构建运行：

  ```bash
  cmake --build build --parallel
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_rendering
  ```

  预期：**全部 PASS**（安全网性质，对现状即绿——此时开关尚无效果，A/B 走的是同一条路径）。

- [ ] **3.3 实现快路径**：改造 `updateImage()`（TerminalDisplay.cpp）。在任务 2 终态基础上：

  a) 函数头部 scrollImage 调用段（:1333-1337）改为捕获滚动量：

  ```cpp
      // optimization - scroll the existing image where possible and
      // avoid expensive text drawing for parts of the image that
      // can simply be moved up or down
      const int scrollLines = _screenWindow->scrollCount();
      const QRect scrollWindowRegion = _screenWindow->scrollRegion();
      scrollImage(scrollLines, scrollWindowRegion);
      _screenWindow->resetScrollCount();
  ```

  b) 在图像视图顶行记录段（`_imageViewTopLine = viewTopLine;` 之后）插入快路径判定：

  ```cpp
      // 纯整屏滚动快路径判定：scrollImage 已完成像素搬迁与 _image 移位，
      // 满足全部条件时 moved 行跳过跨度聚合（仅验证 + memcpy 同步）。
      // 双高行探测：两半身必须成对重绘，不做像素搬迁假设（回退条件）
      bool hasDoubleHeight = false;
      for (int i = 0; i < _lineProperties.count() && i < linesToUpdate; ++i) {
          if ((_lineProperties[i] & LINE_DOUBLEHEIGHT) != 0) {
              hasDoubleHeight = true;
              break;
          }
      }
      // scrollImage 把滚动区下沿钳到 _lines-2，全屏滚动时区域高度为 linesToUpdate-1
      const bool fullScreenScroll = scrollWindowRegion.top() == 0
              && scrollWindowRegion.height() >= linesToUpdate - 1;
      bool fastScroll = _scrollOptimizationEnabled && scrollLines != 0
              && abs(scrollLines) < linesToUpdate
              && fullScreenScroll                       // 滚动区覆盖全内容区
              && scr != nullptr && scr == _lastImageScreen // 备选屏切换帧回退
              && !hasImages && !scr->graphicsDirty()    // 图像帧回退
              && !hasDoubleHeight && !_resizing         // 双高行/缩放中回退
              && _screenWindow->isClearSelection()      // 活动选区回退
              && linesToUpdate == _usedLines && columnsToUpdate == _usedColumns; // 收缩区回退

      // 行区间划分：上滚（scrollLines>0，图像上移）新进行在底部；
      // 下滚（用户回看历史，图像下移）新进行在顶部
      const int newRows = abs(scrollLines);
      const int movedBegin = (fastScroll && scrollLines < 0) ? newRows : 0;
      const int movedEnd = fastScroll
              ? (scrollLines > 0 ? linesToUpdate - newRows : linesToUpdate) : 0;

      if (fastScroll) {
          // 错位检测：已移位的 moved 行与 newimg 逐格比对（顺带 blink 扫描）；
          // 任一格存在滚动外修改即回退全量路径
          for (y = movedBegin; y < movedEnd && fastScroll; ++y) {
              const Character *cur = &_image[y * this->_columns];
              const Character *nw = &newimg[y * columns];
              for (x = 0; x < columnsToUpdate; ++x) {
                  if ((nw[x].rendition & RE_BLINK) != 0)
                      _hasBlinker = true;
                  if (nw[x] != cur[x]) {
                      fastScroll = false;
                      break;
                  }
              }
          }
      }
  ```

  c) 主循环开头（任务 2 终态的循环体第一行 `const Character *currentLine = ...` 之后）插入：

  ```cpp
          if (fastScroll && y >= movedBegin && y < movedEnd) {
              // 纯滚动快路径：像素已由 scrollImage 搬迁、内容经错位检测确认一致；
              // 仍 memcpy 同步，保持 _image 为 newimg 字节镜像的既有语义
              memcpy((void *)currentLine, (const void *)newLine,
                           columnsToUpdate * sizeof(Character));
              continue;
          }
  ```

  d) `update(dirtyRegion);` 之前（`_lastDirtyRegion` 赋值旁）插入：

  ```cpp
      _lastImageScreen = scr; // 备选屏切换帧检测（快路径回退条件）
  ```

  e) 自检：任务 2 的跨度聚合循环、例外行逻辑、收缩区/graphicsDirty/preedit 段原样保留；
  快路径帧 blink 由错位检测循环（moved 行）与跨度聚合循环（新进 N 行）共同覆盖，无遗漏。

- [ ] **3.4 验证 + benchmark 裁决**

  ```bash
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark -o -,txt
  ```

  预期：9 套件全绿；`testFullScreenScroll`（尤其 40x160 行）较任务 1 基线的比对段耗时下降。
  若数字无显著改善（24x80 下 µs 级差异可能被噪声淹没）：以 40x160 数字为准记录；
  若两尺寸均无改善，保持开关默认开（快路径不劣化、回退清单保证正确性），在 commit
  message 与 CHANGELOG 如实记录"收益低于测量噪声"。

- [ ] **3.5 commit**（测试与实现两个提交）

  ```
  test(rendering): 滚动快路径等价与回退安全网

  连续滚动 1/N 行像素等价、滚动+局部修改错位回退、含图回退、选区回退、
  setScrollOptimizationEnabled 开关 A/B 五用例（对现状即绿的安全网，
  快路径无可观测行为差异，收益由 benchmark 比对段裁决）。
  ```
  ```
  perf(display): 纯整屏滚动快路径跳过已移位行重复比对

  scrollImage 像素搬迁与 _image 移位为既有行为；本提交在纯整屏滚动帧
  对 moved 行只做错位检测（逐格验证 + blink 扫描）与 memcpy 同步，
  新进 N 行走跨度级脏区。回退清单：图像/graphicsDirty/活动选区/双高行/
  备选屏切换/_resizing/收缩区/滚动外修改错位。testFullScreenScroll 比对段
  较基线：24x80 __ms→__ms，40x160 __ms→__ms。
  ```

---

## 任务 4：文档收尾 + 全量验证 + 基线前后对比记录

- [ ] **4.1 README.md**：:76-79 性能与渲染回归段更新两条描述：

  ```markdown
  - `tst_benchmark`：渲染性能基线（解析吞吐 / 全量重绘 / 局部刷新 / 整屏滚动），真实增量口径（updateImage 比对段 + lastDirtyRegion 离屏渲染段），进 ctest 但无硬性性能断言，数字仅 Release 构建下有参考意义。
  - `tst_rendering`：像素等价性测试，批次聚合与 Legacy 两条绘制路径双渲染逐像素比对，以及跨度脏区/滚动快路径的增量重放等价（脏区渲染到上帧 vs 全量渲染），是绘制路径改造的安全网。
  ```

- [ ] **4.2 CHANGELOG** 顶部新增条目（沿用现有"标题 / 日期 + ====="格式）：

  ```
  ZzQTermWidget 脏区算法重写 / 2026-08-13
  =============================================
   * updateImage 行内跨度级脏区：dirtyMask 两趟扫描合并为一趟 [minX,maxX] 跨度聚合，
     脏 rect 只盖跨度 ±1 格（吸收宽字符尾部与字形越界）；双高行/含图行例外保持整行脏。
   * 纯整屏滚动快路径：复用 scrollImage 既有像素搬迁与 _image 移位，moved 行仅错位检测
     + memcpy 同步，新进 N 行走跨度比对；图像/选区/双高/备选屏/缩放/错位任一命中即回退。
   * 内部开关 setScrollOptimizationEnabled（默认开，镜像 setTextBatchingEnabled 模式）与
     lastDirtyRegion 观测钩子，均不进公共头。
   * 实测（本机，Release）：局部刷新 __ms → __ms；整屏滚动 24x80 __ms → __ms、
     40x160 __ms → __ms；tst_rendering 增量重放像素等价零差异。
  ```

- [ ] **4.3 最终验证**

  ```bash
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure
  QT_QPA_PLATFORM=offscreen ./build/tests/tst_benchmark -o -,txt -o build/benchmark-baseline.txt,txt
  ```

  预期：9 套件全绿（tst_rendering 用例数 2→12）；基线文件更新为改造后数字；
  把任务 1 基线与最终数字的前后对比填入 CHANGELOG 实测行。

- [ ] **4.4 commit**

  ```
  docs: 脏区算法重写收尾（README/CHANGELOG + benchmark 前后对比）
  ```

---

## 风险与对策

| 风险 | 对策 |
|------|------|
| 字形越界超 ±1 格被逐 rect setClipRect 裁掉（轮 6 F2 叠加跨度脏区） | tst_rendering 增量重放逐像素比对强制裁决；不足时升级 `QFontMetrics::overhang()` 像素级扩展（规格预留路径，单独 commit） |
| 快路径错位漏检导致陈旧像素 | 不可能漏检：moved 行逐格 `operator!=` 全量验证，任一格不同即回退；混合帧用例常驻 |
| `Character` 尾部填充使整 16B memcmp 失效 | 不用 memcmp，逐格验证（勘察结论 4）；严禁实施者"优化"为 memcmp |
| 快路径收益低于测量噪声 | 如实记录，不夸大；开关默认开（不劣化、回退清单保正确性），40x160 变体增强区分度 |
| offscreen 平台 `QWidget::scroll` 不搬真实像素 | 测试口径不依赖真实投递：增量重放用 `render(&img, QPoint(), lastDirtyRegion())` 手工驱帧 |
| blink 定时器状态漂移 | 错位检测循环顺带扫描 RE_BLINK，moved 行无覆盖空洞；测试环境关闪烁，不影响测试 |
| 图像行逐行查询开销 | 仅 `hasImages()` 为真时发生（有图屏），无图屏短路零开销；与原短路优化等价 |
| 备选屏切换帧 `_scrollCount` 陈旧 | `_lastImageScreen` 指针变化检测回退（ScreenWindow::setScreen 不重置计数，勘察结论 3） |

## 自检清单（提交前逐项确认）

- [ ] `lastDirtyRegion`/`_lastDirtyRegion`、`setScrollOptimizationEnabled`/`isScrollOptimizationEnabled`/`_scrollOptimizationEnabled`、`_lastImageScreen`、`renderFull`/`replayDirtyRegion`/`buildScrollPayload` 命名与本计划一致
- [ ] 所有新增/修改注释为简体中文 Doxygen 风格；`lib/third_party/` 与 `lib/include/qtermwidget.h` 零改动
- [ ] tests/CMakeLists.txt、lib/CMakeLists.txt 未改动（无新文件）
- [ ] 每个 commit 前 `ctest --test-dir build --output-on-failure` 9 套件全绿
- [ ] benchmark 数字均来自 Release 构建，前后对比记入 CHANGELOG，无硬断言
- [ ] 未引入计划外重构（drawContents 内部、QRegion 合并、tile、多线程均不动）
