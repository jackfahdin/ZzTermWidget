# 正确性修复包（kitty 逐行切片 / DECDH 几何根治 / SGR 38/48 冒号真彩）实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 打包修复三个正确性问题：(1) kitty 放置绘制从"任意引用行回推锚定行画整图"改为逐行水平带切片，根治 DECSTBM 部分滚动切割错位并结构性消除多 rect 半透明重复混合；(2) DECDH 双高行行坐标→像素逆映射一致化，墨迹从 2× 行坐标落回本行行带，双高行纳入增量重放像素等价；(3) SGR 38/48 冒口子参数真彩（`38:5:n`/`38:2::r:g:b`/48 同款），复用轮 8 `argSeparators` 设施。

**架构：** 三组件互不依赖，各自独立可测独立 commit。kitty 切片只改 `TerminalDisplay::drawKittyPlacements` 一个函数（z 层双通道、X/Y 偏移、c/r 缩放、源矩形取交、右缘截断、同 z 按 id 排序全不变）；DECDH 根治只改 `TerminalDisplay::calculateTextArea` 一个函数（drawContents/drawContentsLegacy 双路径共享，双高双宽组合因 ESC#3/#4 同置双宽位而天然覆盖）；SGR 38/48 冒号分支插入 Vt102Emulation SGR 分发循环既有 38/48 分号定长分支之前。测试全部追加进现有套件文件，不新增测试文件。

**技术栈：** Qt6（本机 6.11.1，前缀 `/home/zz/Qt/6.11.1/gcc_64`，build/ 已配置）、C++20、QTest、CMake。构建 `cmake --build build --parallel`；测试 `ctest --test-dir build --output-on-failure`（9 套件全绿保持）。

**规格：** `docs/superpowers/specs/2026-08-14-correctness-pack-design.md`（已批准）。

---

## 类型/方法名跨任务一致性自检项

- `TerminalDisplay::drawKittyPlacements(QPainter &, const QRect &, bool aboveText)` 签名不变（TerminalDisplay.h:800）。
- 切片循环内局部结构体名 `RowItem`（字段 `pl`、`rowOffset`），仅存在于 drawKittyPlacements 函数体内。
- `Screen::kittyRefs(int absoluteLine)` 返回 `QVector<KittyPlacementRef>`（字段 `placementHandle`、`rowOffset`）；`Screen::kittyPlacement(quint32)` 返回 `const KittyPlacement *`（字段 `imageHandle/imageId/placementId/col/cols/rows/srcX/srcY/srcW/srcH/cellXOff/cellYOff/zIndex/serial`，Screen.h:96-120）。
- 测试辅助命名：tst_kittygraphics.cpp 复用 `initKittyRenderEnv`/`kittySeq`/`solidImage`，新用例内 lambda 名 `bandRed`；tst_rendering.cpp 复用 `initRenderEnv`/`renderDisplay`/`renderFull`/`replayDirtyRegion`/`pumpFrame`，新用例内 lambda 名 `brightPixels`。
- SGR 冒号分支消费槽写法与 58 分支逐字同构（`i += 2`、`i = j + 2`、`i += 1` 三种收尾）。
- 新测试方法名：`testSgrTrueColorColon`（tst_emulation）、`testDecstbmCutSlicesAligned`（tst_kittygraphics）、`testDoubleHeightInkGeometry`/`testDoubleHeightPixelEquivalence`（tst_rendering）。

---

## 任务 1：SGR 38/48 冒号真彩（最小、最独立，先行）

**文件：**
- 测试：`tests/tst_emulation.cpp`（槽声明 :56 附近；实现追加在 `testSgrUnderlineColor` 之后 :978）
- 实现：`lib/src/emulation/Vt102Emulation.cpp`（SGR 分发循环 :560-622，插入点 :607 之前）

### Step 1.1 写失败测试

- [ ] 在 `tests/tst_emulation.cpp` 槽声明区（:56 `void testSgrUnderlineColor();` 之后）追加：

```cpp
    void testSgrTrueColorColon();
```

- [ ] 在 `testSgrUnderlineColor()` 实现之后（:978 后、`QTEST_GUILESS_MAIN` 前）追加：

```cpp
/**
 * @brief SGR 38/48 冒口子参数形式：38:5:n、38:2[:色彩空间空位]:r:g:b 与 48 同款，
 *        与分号形式落到同一 CharacterColor；参数不足整体忽略、不吞后续独立 SGR。
 * @note 空位容忍与歧义口径同 58：恰好 3 分量的 38:2:0:g:b（r=0）解析正确；
 *       r=0 且其后 ≥3 槽的写法按空位误吞（已声明遗留，见实现注释）。
 */
void TestEmulation::testSgrTrueColorColon()
{
    Vt102Emulation emu;
    initEmu(emu, 24, 80);
    const char *seq =
            "\033[38:5:196mA"            // 冒号 256 色前景
            "\033[38:2::10:20:30mB"      // 冒号真彩前景（容忍色彩空间空位）
            "\033[38:2:1:2:3mC"          // 冒号真彩（恰好 3 分量，无空位）
            "\033[48:5:42mD"             // 冒号 256 色背景
            "\033[48:2::100:150:200mE"   // 冒号真彩背景（容忍空位）
            "\033[38;2;10;20;30mF"       // 分号真彩前景（与 B 等价性对照）
            "\033[0m\033[38:5mG"         // 256 色槽不足：忽略，不吞后续（G 保持默认）
            "\033[38:2:9:8mH"            // 真彩槽不足：忽略 38 与模式槽（9/8 按独立 SGR）
            "\033[0m\033[38:2:0:1:2mI";  // r=0 恰好 3 分量：RGB(0,1,2) 解析正确
    emu.receiveData(seq, int(std::strlen(seq)));
    const QVector<Character> line = firstLineChars(emu, 80);
    QCOMPARE(line[0].foregroundColor, CharacterColor(COLOR_SPACE_256, 196));
    QCOMPARE(line[1].foregroundColor, CharacterColor(COLOR_SPACE_RGB, (10 << 16) | (20 << 8) | 30));
    QCOMPARE(line[2].foregroundColor, CharacterColor(COLOR_SPACE_RGB, (1 << 16) | (2 << 8) | 3));
    QCOMPARE(line[3].backgroundColor, CharacterColor(COLOR_SPACE_256, 42));
    QCOMPARE(line[4].backgroundColor, CharacterColor(COLOR_SPACE_RGB, (100 << 16) | (150 << 8) | 200));
    QCOMPARE(line[5].foregroundColor, line[1].foregroundColor); // 分号/冒号等价
    QCOMPARE(line[6].foregroundColor, CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR));
    QCOMPARE(line[7].foregroundColor, CharacterColor(COLOR_SPACE_DEFAULT, DEFAULT_FORE_COLOR));
    QCOMPARE(line[8].foregroundColor, CharacterColor(COLOR_SPACE_RGB, (0 << 16) | (1 << 8) | 2));
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R emulation --output-on-failure`
- [ ] 预期：编译通过，`testSgrTrueColorColon` 失败（首个 QCOMPARE 即不符：冒号形式被拍平误解）。其余用例保持绿。

### Step 1.2 实现冒号分支

- [ ] 在 `lib/src/emulation/Vt102Emulation.cpp` :605-606 的"参数不足的 58"分支之后、:607 的 38/48 分号真彩分支之前，插入（消费槽写法与 :573-594 的 58 冒号分支逐字同构）：

```cpp
            } else if (cc == 'm' && (argv[i] == 38 || argv[i] == 48) && i + 1 <= argc
                       && argSeparators[i + 1] == ':') {
                // SGR 38/48 冒口子参数：38:5:n / 38:2[:色彩空间空位]:r:g:b（必须先于
                // 38/48 分号定长分支：38:2::r:g:b 拍平后同样满足 argv[i+1]==2；
                // 顺序红线同 58）
                const int colorCmd = argv[i];
                if (argv[i + 1] == 5 && i + 2 <= argc) {
                    processToken(TY_CSI_PS(cc, colorCmd), COLOR_SPACE_256, argv[i + 2]);
                    i += 2;
                } else if (argv[i + 1] == 2) {
                    int j = i + 2;
                    // 容忍色彩空间空位：38:2::r:g:b 拍平后该槽为 0（同 58 口径；
                    // 已知歧义：r=0 且其后仍够 ≥3 槽的真彩写法被当空位整体跳过，
                    // 与 58 一致，注释声明为遗留）
                    if (j <= argc && argv[j] == 0 && argc - j >= 3)
                        j++;
                    if (argc - j >= 2) {
                        processToken(TY_CSI_PS(cc, colorCmd), COLOR_SPACE_RGB,
                                             (argv[j] << 16) | (argv[j + 1] << 8) | argv[j + 2]);
                        i = j + 2;
                    } else {
                        i += 1; // 参数不足：忽略 38/48 与模式槽，其余参数按独立 SGR 解释
                    }
                } else {
                    i += 1; // 未知 38/48 模式：忽略该参数与模式槽
                }
            } else if (cc == 'm' && argc - i >= 4 && (argv[i] == 38 || argv[i] == 48) &&
```

（最后一行是既有 :607 分支的条件行，作为拼接上下文；保持其后内容不变。）

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R emulation --output-on-failure`
- [ ] 预期：`testSgrTrueColorColon` 转绿；`testSgrUnderlineStyles*`/`testSgrUnderlineSemicolonVsColon`/`testSgrUnderlineColor`（4;3 vs 4:3 区分与 58/59 回归）不受影响。
- [ ] 提交：`feat(emulation): SGR 38/48 冒口子参数真彩（38:5:n / 38:2::r:g:b）`

---

## 任务 2：kitty 逐行水平带切片 + DECSTBM 切割回归测试

**文件：**
- 测试：`tests/tst_kittygraphics.cpp`（槽声明 :111 之后；实现追加在 `testSemiTransparentPaintedOnceAcrossRects` 之后 :1146）
- 实现：`lib/src/display/TerminalDisplay.cpp`（`drawKittyPlacements` :2192-2259 整体改写；paintEvent 注释 :1919-1921 同步）

**勘察结论（改造形态）：** 每引用行 y（rowOffset=k）的目标带 = 本行单元格行带 ∩ 放置目标矩形纵向区间；`ty = rowTop(y) - k*fh + cellYOff` 的回推在切片模式下只是求带位置的代数（切割后各段引用行 k 跳变，带位置仍各自正确）。带恒落在本行行带内 → 不同行的带互不重叠 → paintEvent 逐 rect 裁剪下任一像素只画一次，`seenHandles` 去重删除。z 排序从全局 items 排序改为逐行内排序（比较器不变）。

### Step 2.1 写失败测试（DECSTBM 切割）

- [ ] 在 `tests/tst_kittygraphics.cpp` 槽声明区（:111 之后）追加：

```cpp
    // 离屏渲染（正确性修复包：DECSTBM 部分滚动切割逐行切片）
    void testDecstbmCutSlicesAligned();
```

- [ ] 在 `testSemiTransparentPaintedOnceAcrossRects()` 实现之后（:1146 后、`QTEST_MAIN` 前）追加：

```cpp
/**
 * @brief DECSTBM 部分滚动横切放置中部：切割后各段引用行各自画本行水平带切片，
 *        上下两段像素各归其位（图像随文本切割，与 sixel 行为一致）。
 * @note 回归：整图画法按"任意引用行回推锚定行"把整图锚在切割前的锚定行，
 *       下段引用行显示的是错误的图像带（偏移 = 滚动行数：带 3/4/5 错成带 1/2/3）。
 */
void TestKittyGraphics::testDecstbmCutSlicesAligned()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initKittyRenderEnv(emu, win, display);
    const int cw = display.cellPixelWidth();
    const int ch = display.cellPixelHeight();

    const QByteArray hideCursor = "\033[?25l"; // 隐藏光标：排除复绘通道干扰
    emu.receiveData(hideCursor.constData(), int(hideCursor.size()));
    // 每行带一种红色梯度（40/80/…/240）的 1 列 x 6 行图，放在行 2（0 起）第 0 列；
    // c=1,r=6 显式显示区，C=1 固定光标（避免放置后光标移动触发滚动）
    QImage img(cw, ch * 6, QImage::Format_ARGB32);
    for (int i = 0; i < 6; i++)
        for (int y = i * ch; y < (i + 1) * ch; y++)
            for (int x = 0; x < cw; x++)
                img.setPixelColor(x, y, QColor(40 * (i + 1), 0, 0));
    QByteArray png;
    QBuffer buf(&png);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    const QByteArray home = "\033[3;1H";
    emu.receiveData(home.constData(), int(home.size()));
    const QByteArray seq = kittySeq("a=T,f=100,i=11,z=0,C=1,c=1,r=6", png.toBase64());
    emu.receiveData(seq.constData(), int(seq.size()));
    display.updateImage();

    const int cellX = display.contentsRect().left() + display.margin();
    const int rowTop = display.contentsRect().top() + display.margin();
    const int px = cellX + cw / 2; // 断言列：放置中心
    const auto bandRed = [&](const QImage &f, int row) {
        return f.pixelColor(px, rowTop + row * ch + ch / 2).red();
    };

    // warmup 基线（顺带吃掉 _drawTextTestFlag）：行 2..7 依次为梯度带 0..5
    QImage warm(display.size(), QImage::Format_ARGB32);
    warm.fill(Qt::black);
    display.render(&warm);
    for (int i = 0; i < 6; i++)
        QVERIFY2(qAbs(bandRed(warm, 2 + i) - 40 * (i + 1)) <= 4,
                 qPrintable(QStringLiteral("基线行 %1 红值 %2 ≠ 带 %3")
                            .arg(2 + i).arg(bandRed(warm, 2 + i)).arg(i)));

    // DECSTBM 滚动区行 3..10（0 起）横切放置中部，区内上滚 2 行（CSI S 走
    // _topMargin，与光标位置无关）：行 2（带 0）留在区外不动；行 3..5 经 moveImage
    // 取得原行 5..7 的引用（带 3/4/5，k 在切割边界跳变 2）；行 6..7 引用移出
    const QByteArray cut = "\033[4;11r\033[2S";
    emu.receiveData(cut.constData(), int(cut.size()));
    display.updateImage();

    QImage frame(display.size(), QImage::Format_ARGB32);
    frame.fill(Qt::black);
    display.render(&frame);
    QVERIFY(qAbs(bandRed(frame, 2) - 40) <= 4);  // 区外上段原位
    QVERIFY(qAbs(bandRed(frame, 3) - 160) <= 4); // 下段：带 3（整图画法错成带 1 = 80）
    QVERIFY(qAbs(bandRed(frame, 4) - 200) <= 4); // 带 4（错成 120）
    QVERIFY(qAbs(bandRed(frame, 5) - 240) <= 4); // 带 5（错成 160）
    // 引用移出的行恢复背景（与未触碰的空行 12 逐像素一致，不假设具体背景色值）
    QCOMPARE(frame.pixelColor(px, rowTop + 6 * ch + ch / 2),
             frame.pixelColor(px, rowTop + 12 * ch + ch / 2));
    QCOMPARE(frame.pixelColor(px, rowTop + 7 * ch + ch / 2),
             frame.pixelColor(px, rowTop + 12 * ch + ch / 2));
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R kittygraphics --output-on-failure`
- [ ] 预期：`testDecstbmCutSlicesAligned` 失败于行 3 断言（实测红值 80 ≠ 160）；基线循环通过；其余 kitty 用例保持绿。

### Step 2.2 实现逐行切片

- [ ] 将 `lib/src/display/TerminalDisplay.cpp` :2205-2258（`const int topLine` 起至函数结束）整体替换为：

```cpp
    const int topLine = _screenWindow->currentLine();
    const int rightEdge = _leftMargin + tL.x() + _usedColumns * _fontWidth;

    // 逐行水平带切片（与 sixel 同式）：每个引用行只画放置目标矩形与本行单元格行带
    // 的纵向交集，源带按目标高度比例从源矩形取条。行一致性不再依赖"回推锚定行画
    // 整图"：DECSTBM 部分滚动（或 insertLine/deleteLine）切割跨边界放置后，各段
    // 引用行各自落在正确视图行，图像随文本切割。带恒落在本行行带内，多 rect 局部
    // 重绘的半透明重复混合结构性消除（无需 seenHandles 去重）。
    for (int y = luy; y <= rly; y++) {
        const auto refs = screen->kittyRefs(topLine + y);
        if (refs.isEmpty())
            continue;
        struct RowItem {
            const KittyPlacement *pl;
            int rowOffset; ///< 本行在放置内的行偏移（0 = 放置首行）
        };
        QVector<RowItem> rowItems;
        rowItems.reserve(refs.size());
        for (const KittyPlacementRef &ref : refs) {
            const KittyPlacement *pl = screen->kittyPlacement(ref.placementHandle);
            if (pl && (pl->zIndex >= 0) == aboveText)
                rowItems.append({pl, ref.rowOffset});
        }
        // 本行内 z-index 排序：z 升序；同 z 重叠时 id 小者更低层；同 z 同 id 按插入序
        std::sort(rowItems.begin(), rowItems.end(), [](const RowItem &a, const RowItem &b) {
            if (a.pl->zIndex != b.pl->zIndex)
                return a.pl->zIndex < b.pl->zIndex;
            if (a.pl->imageId != b.pl->imageId)
                return a.pl->imageId < b.pl->imageId;
            return a.pl->serial < b.pl->serial;
        });

        const int rowTop = _topMargin + tL.y() + y * _fontHeight;
        for (const RowItem &item : rowItems) {
            const KittyPlacement *pl = item.pl;
            const ScreenImage *img = screen->image(pl->imageHandle);
            if (!img)
                continue;
            const QRect src = QRect(pl->srcX, pl->srcY, pl->srcW, pl->srcH) & img->image.rect();
            if (src.isEmpty())
                continue;
            // 目标矩形（未截断）：(col, 锚定行) 单元格起 + X/Y 像素偏移，尺寸 c×r
            // 单元格。锚定视图行由本行回推（y - rowOffset，可为负），仅作带位置代数
            const int tx = _leftMargin + tL.x() + pl->col * _fontWidth + pl->cellXOff;
            const int ty = rowTop - item.rowOffset * _fontHeight + pl->cellYOff;
            const int fullW = pl->cols * _fontWidth;
            const int fullH = pl->rows * _fontHeight;
            const int targetW = qMin(fullW, rightEdge - tx); // 超右缘截断
            if (targetW <= 0)
                continue;
            // 目标带 = 本行单元格行带 ∩ 目标矩形纵向区间
            const int bandTop = qMax(rowTop, ty);
            const int bandBot = qMin(rowTop + _fontHeight, ty + fullH);
            if (bandTop >= bandBot)
                continue;
            const QRect target(tx, bandTop, targetW, bandBot - bandTop);
            if (!target.intersects(rect))
                continue;
            // 源带：纵向按目标高度比例取条，横向沿用右缘截断等比收缩（整数换算同旧式）
            const int srcBandTop = src.y() + int(qint64(src.height()) * (bandTop - ty) / fullH);
            const int srcBandBot = src.y() + int(qint64(src.height()) * (bandBot - ty) / fullH);
            if (srcBandTop >= srcBandBot)
                continue;
            QRect s(src.x(), srcBandTop, src.width(), srcBandBot - srcBandTop);
            if (fullW > targetW)
                s.setWidth(int(qint64(src.width()) * targetW / fullW));
            if (s.width() <= 0)
                continue;
            paint.drawImage(target, img->image, s); // 半透明按 z 序 alpha 混合
        }
    }
}
```

- [ ] 同步 paintEvent 逐 rect 裁剪注释（:1919-1921）：把"跨 rect 的半透明 kitty 放置（整图画法）会在每个 rect 轮次重复 alpha 混合（颜色偏深）；逐 rect 裁剪保证任一像素本轮只画一次"改为"kitty 逐行切片后带恒落在单行带内、天然不跨 rect；逐 rect 裁剪保留为防御，保证任一像素本轮只画一次"。
- [ ] 同步 `Screen.h:119` 的 `rowOffset` 注释："绘制层只从 0 行画一次"→"绘制层按本行偏移画该行的水平带切片"。
- [ ] 同步 `tst_kittygraphics.cpp:1033-1037`（testPartialRepaintKeepsNonAnchorRows 头注释）：把"任意引用行参与绘制，靠重绘区域裁剪补画"改为"逐行切片后该行画本行切片"。
- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R 'kittygraphics|rendering' --output-on-failure`
- [ ] 预期：`testDecstbmCutSlicesAligned` 转绿；既有 kitty 渲染用例（z 层双通道、同 z 按 id、光标复绘、非锚定行局部重绘、半透明多 rect 单次混合）与 rendering 套件全绿——不变项回归即切片等价性的证据。
- [ ] 提交：`fix(display): kitty 放置绘制改逐行水平带切片，根治 DECSTBM 切割错位`

---

## 任务 3：DECDH 几何根治 + 双高行像素等价/增量重放测试

**文件：**
- 测试：`tests/tst_rendering.cpp`（翻转既有数据行 :211-216；注释同步 :306-307；新用例槽声明 :35 之后、实现追加在文件末尾 `testStyledUnderlineDirtyRegion` 之后）
- 实现：`lib/src/display/TerminalDisplay.cpp`（`calculateTextArea` :2129-2143）、`lib/src/display/TerminalDisplay.h`（:770-772 声明注释）

**勘察结论（错位机制与最小改动点）：** `calculateTextArea` 只对原点逆映射（:2135），`top = _fontHeight * line`（:2138）未经逆映射直接相加。ESC#3/#4 同置 LINE_DOUBLEWIDTH+LINE_DOUBLEHEIGHT（Vt102Emulation.cpp:1246-1253），双高行 textScale 恒为 scale(2,2)，故墨迹设备坐标 y = `_topMargin+tLy + 2*_fontHeight*line`（2× 行坐标），x = `_leftMargin+tLx + 2*left`（2× 列坐标为 DECDWL 设计语义，保留）。根治：把 `top` 并入逆映射点，`left` 保持不逆映射。修正后推演：世界坐标 y = (`_topMargin+tLy+top`)/2，经 scale(1,2) 落到设备 y = `_topMargin+tLy + _fontHeight*line`（本行行带顶）；高 `_fontHeight` 经变换为 `2*_fontHeight`，恰好覆盖上下两半行；x/宽与旧式逐位相同（scale(2,1) 分量不变）；恒等变换下输出与旧式完全一致。

### Step 3.1 写失败测试（墨迹几何 + 翻转既有数据行）

- [ ] 在 `tests/tst_rendering.cpp` 槽声明区（:35 `void testStyledUnderlineDirtyRegion();` 之后）追加：

```cpp
    void testDoubleHeightInkGeometry();
    void testDoubleHeightPixelEquivalence();
```

- [ ] 翻转既有数据行：把 :211-216 的注释与行改为（`pixelCheck` false→true，形状断言逻辑 `editCells == -1` 分支不变，先形状后像素）：

```cpp
    // 双高行：DECDH 几何根治后墨迹落在该行自身行带（上下两半），两行均整行脏的
    // 行矩形脏区能完整盖住墨迹，故纳入逐像素比对（轮 7 时因 2× 行坐标怪癖被排除）
    QTest::newRow("双高行整行脏")
            << QByteArray("\033[10;1H\033#3double high line\r\n\033#4double high line\r\n")
            << QByteArray("\033[10;3HZ") << 9 << -1 << true;
```

- [ ] 在文件末尾（`testStyledUnderlineDirtyRegion` 实现之后、`QTEST_MAIN` 前）追加几何用例：

```cpp
/**
 * @brief DECDH 墨迹几何根治：双高行文本墨迹必须落在该行自身两行行带内，
 *        而非 2× 行坐标处。
 * @note 回归：旧 calculateTextArea 只逆映射原点，top 项未经逆映射，scale 世界
 *       变换下行 9 的墨迹落到行 18；任何行矩形脏区都盖不住，增量重绘必留残影。
 */
void TestRendering::testDoubleHeightInkGeometry()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    emu.setCellPixelSize(display.cellPixelWidth(), display.cellPixelHeight());
    const QByteArray content =
            "\033[?25l"                            // 隐藏光标：排除光标块干扰
            "\033[10;1H\033#3DH\r\n\033#4DH\r\n";  // 行 9/10（0 起）双高行
    emu.receiveData(content.constData(), int(content.size()));
    // 双泵就位行属性/视图几何（updateLineProperties 在信号链中先于 updateImage
    // 执行，单泵拿到的是旧几何下的行属性；同 testSpanDirtyPixelEquivalence）
    pumpFrame(win);
    pumpFrame(win);
    renderFull(display); // warmup：吃掉 _drawTextTestFlag
    const QImage frame = renderFull(display);

    const int fh = display.fontHeight();
    const int fw = display.fontWidth();
    const int top0 = display.contentsRect().top() + display.margin();
    const int left0 = display.contentsRect().left() + display.margin();
    // 亮像素计数：默认前景 0xB2B2B2 远超阈值，背景远低；DECDH 恒带双宽，
    // 两字符墨迹约 4 格宽，扫 8 格留余量
    const auto brightPixels = [&](int rowBegin, int rowEnd) {
        int n = 0;
        for (int y = top0 + rowBegin * fh; y < top0 + rowEnd * fh; y++)
            for (int x = left0; x < left0 + 8 * fw; x++) {
                const QColor c = frame.pixelColor(x, y);
                if (c.red() > 100 && c.green() > 100 && c.blue() > 100)
                    n++;
            }
        return n;
    };
    QVERIFY(brightPixels(9, 11) > 0);   // 墨迹落在双高行自身两行带内
    QCOMPARE(brightPixels(18, 20), 0);  // 2× 行坐标处无墨迹（根治回归）
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R rendering --output-on-failure`
- [ ] 预期：`testDoubleHeightInkGeometry` 失败于 `brightPixels(9, 11) > 0`（墨迹在行 18）；`testSpanDirtyPixelEquivalence` 的"双高行整行脏"行失败于末尾 `QCOMPARE(incremental, full)`（脏区盖不住 2× 处墨迹）；其余用例绿。

### Step 3.2 实现逆映射一致化

- [ ] 将 `lib/src/display/TerminalDisplay.cpp` :2129-2143 的 `calculateTextArea` 整体替换为：

```cpp
QRect TerminalDisplay::calculateTextArea(int topLeftX, int topLeftY,
                                            int startColumn, int line,
                                            int length,
                                            const QTransform &textScale) {
    const int left =
            _fixedFont ? _fontWidth * startColumn : textWidth(0, startColumn, line);
    const int top = _fontHeight * line;
    const int width =
            _fixedFont ? _fontWidth * length : textWidth(startColumn, length, line);
    // 逆映射一致化（DECDH 根治）：行顶 top 并入逆映射点，scale(1,2) 下墨迹落在
    // 该行自身行带（旧实现只逆映射原点，top 未经逆映射，墨迹落在 2× 行坐标处，
    // 行矩形脏区盖不住、增量重绘必留残影）。横向 left 保持不逆映射：DECDWL 下
    // 列 x 本就该经 scale(2,1) 映射到 2× 列像素位置（ESC#3/#4 恒同置双宽位，
    // 双高双宽组合由同一修正覆盖）。恒等变换下求逆不变，像素输出不受影响。
    const QPoint origin = textScale.inverted().map(
            QPoint(_leftMargin + topLeftX, _topMargin + topLeftY + top));
    return {origin.x() + left, origin.y(), width, _fontHeight};
}
```

- [ ] 同步 `lib/src/display/TerminalDisplay.h:770-771` 声明注释：保留英文原行不动，在其下补一行中文：`// 双高/双宽行：行顶随原点一并逆映射（横向列偏移不逆映射，DECDWL 语义）`。
- [ ] 同步 `tests/tst_rendering.cpp:306-307`（testSpanDirtySelectionFrame 头注释）：把"双倍宽/高行的世界变换墨迹会越出行矩形"改为"双倍宽行的世界变换墨迹横向映射到 2× 列像素位置、越出格矩形（DECDH 已根治，纵向不再越界）"。
- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R rendering --output-on-failure`
- [ ] 预期：两个新失败点转绿；`testBatchingPixelEquivalence*`（含 DECDWL 行内容）等全部既有用例保持绿——证明恒等/纯 DECDWL 路径像素零变化。

### Step 3.3 双高行批次 vs Legacy 等价用例（补轮 4 分支覆盖缺口）

- [ ] 在 `testDoubleHeightInkGeometry` 之后追加：

```cpp
/**
 * @brief 双高行（DECDH 恒带 DECDWL 双宽位，即双高双宽组合）：批次聚合与
 *        Legacy 逐片段路径逐像素等价。
 * @note 双路径共享 calculateTextArea，本用例是根治后双高行绘制的常驻安全网；
 *       双泵就位行属性，保证 _lineProperties 真实携带双高/双宽位（非空转）。
 */
void TestRendering::testDoubleHeightPixelEquivalence()
{
    Vt102Emulation emu;
    ScreenWindow *win = nullptr;
    TerminalDisplay display;
    initRenderEnv(emu, win, display);
    const QByteArray content =
            "\033[H"
            "normal row before\r\n"
            "\033#3double high line\r\n"
            "\033#4double high line\r\n"
            "normal row after\r\n";
    emu.receiveData(content.constData(), int(content.size()));
    pumpFrame(win);
    pumpFrame(win); // 行属性/视图几何就位（同 testSpanDirtyPixelEquivalence）
    const QImage batched = renderDisplay(display, true);
    const QImage legacy = renderDisplay(display, false);
    if (batched != legacy) { // 排障辅助：落盘人工比对
        batched.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-dh-batched.png")));
        legacy.save(QDir::temp().filePath(QStringLiteral("zzqtermwidget-dh-legacy.png")));
    }
    QCOMPARE(batched, legacy);
}
```

- [ ] 运行：`cmake --build build --parallel && ctest --test-dir build -R rendering --output-on-failure`
- [ ] 预期：全绿（含新增两个用例与翻转后的"双高行整行脏"行）。
- [ ] 提交：`fix(display): DECDH 行坐标逆映射一致化，双高行墨迹落回本行行带`

---

## 任务 4：文档收尾 + 全量验证

**文件：** `CHANGELOG`、`README.md`、`docs/superpowers/specs/2026-08-13-kitty-graphics-design.md`、`docs/superpowers/specs/2026-08-13-dirty-region-rewrite-design.md`

- [ ] `CHANGELOG` 顶部（:1 之前）新增条目：

```
ZzQTermWidget 正确性修复包（kitty 逐行切片 / DECDH 几何根治 / SGR 38/48 冒号真彩） / 2026-08-14
=============================================
 * kitty 放置绘制改逐行水平带切片（sixel 同款）：根治 DECSTBM 部分滚动（或
   insertLine/deleteLine）切割跨边界放置后的图像错位；带恒落在单行带内，
   多 rect 局部重绘的半透明重复混合结构性消除。z 层/XY 偏移/c/r 缩放/右缘截断/
   同 z 按 id 排序行为不变。
 * DECDH 双高行行坐标逆映射一致化：calculateTextArea 行顶并入逆映射点，
   scale 世界变换下墨迹从 2× 行坐标落回本行两行带（双高双宽组合同覆盖）；
   双高行纳入增量重放像素等价适用范围。
 * SGR 38/48 冒口子参数真彩：38:5:n、38:2[:空位]:r:g:b 与 48 同款，复用
   argSeparators 设施；空位容忍与 r=0 ≥5 槽歧义口径同 58；分号形式零变化。
 * 已知遗留：38/48 冒号形式 r=0 ≥5 槽歧义（同 58 口径，注释声明）；DECDH
   备选屏特殊行为；kitty 动画/占位符/相对放置（延续轮 6 §9）。
```

- [ ] `README.md` :32 下划线条目之后追加一行：

```
- 支持 SGR 38/48 冒口子参数真彩（38:5:n、38:2::r:g:b 与 48 同款，容忍色彩空间空位）。
```

- [ ] 核销轮 6 遗留：`docs/superpowers/specs/2026-08-13-kitty-graphics-design.md` §9 的"放置跨越 DECSTBM 滚动区边界…"条目标注 `（已根治：2026-08-14 正确性修复包，kitty 绘制改逐行水平带切片）`。
- [ ] 核销轮 7 登记：`docs/superpowers/specs/2026-08-13-dirty-region-rewrite-design.md` §7 的"kitty DECSTBM 错位根治"条目标注已根治同上；若文中他处提及 DECDH 墨迹 2× 怪癖，补注 `（DECDH 怪癖已根治：2026-08-14 正确性修复包，双高行已纳入像素等价）`。
- [ ] 全量验证：`cmake --build build --parallel && ctest --test-dir build --output-on-failure`
- [ ] 预期：9 套件全绿（含全部新增/翻转用例）。
- [ ] 提交：`docs: 正确性修复包收尾（CHANGELOG/README 与轮 6/7 遗留核销）`

---

## 全程红线（自检）

- 不动 `lib/include/qtermwidget.h` 与 `lib/third_party/`；新增/修改注释一律中文 Doxygen。
- 每个任务独立 commit，提交前该任务相关套件全绿；全程遵循 TDD（每任务先见红再见绿，预期失败点已逐条写明）。
- `drawKittyPlacements` 不变项复核：z<0/z≥0 双通道过滤（`(pl->zIndex >= 0) == aboveText`）、`cellXOff/cellYOff`、c×r 目标尺寸、源矩形与源图取交、`rightEdge` 截断与源等比收缩、比较器三键（zIndex/imageId/serial）逐字保留。
- DECDH 修正横向零变化复核：`left`/`width` 计算与逆映射处理逐字同旧式；`testBatchingPixelEquivalence`（含 ESC#6 DECDWL 行）保持绿即证据。
- SGR 38/48 分号形式零变化；4;3 vs 4:3 与 58/59 既有用例保持绿即证据。
