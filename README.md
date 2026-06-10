# QTermWidget

从 https://github.com/lxqt/qtermwidget.git fork 而来，为了自己的一些开源项目（[quardCRT](https://github.com/QQxiaoming/quardCRT.git)/[quard_star_tutorial](https://github.com/QQxiaoming/quard_star_tutorial.git)）而修改。复用了大量原始代码但同时也大量修改了很多，因此只能作为一个单独的项目存在。

主要修改如下：

- 从原项目中删除了pty部分的实现，因为原项目中的实现不支持windows，因此在这里引入ptyqt(同样是来自我的个人[Fork](https://github.com/QQxiaoming/ptyqt.git)版本)，这样就跨平台支持linux/windows/macOS了。windows环境同时支持mingw和msvc，这部分对应代码改动比较大。
- 清理了Session的代码，将部分代码向前或向后移动到了TerminalWidget和Vt102Emulation中，使得代码更加清晰。termwidget此时有两个主要的类，TerminalDisplay和Vt102Emulation，TerminalDisplay负责绘制，Vt102Emulation负责解析和处理终端数据，而更高层次的Session以及SessionManager、SessionGroup等类都应该交由上层应用自行实现。这样的设计使得termwidget更加灵活，可以适应更多的应用场景。
- 修改了部分东亚字符的特殊处理，修复方式比较hack，但是对中文用户体验更好。
- 增加了选中字符的强调颜色设置透明度的功能，而不仅仅是反色处理。
- 增加了zmodem检测的功能，可以自动检测zmodem的传输请求发送singal
- 增加了块选择和列选择组合按键
- 增加开放了一些已有的内部API接口对外，方便外部配置设置使用。
- 修复了一些可能的问题，以及在windows上的一些小问题。
- 从原项目中拣选了部分未完成的PR，进行了一些修改和整合。
- 去除全部的构建依赖，使用 CMake 构建，通过 `add_subdirectory(lib)` 引入即可，极为方便通过源码引入其他项目。

## 构建

依赖 Qt6（Core/Gui/Widgets/Network/Xml/Multimedia 模块）。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

构建完成后会得到静态库 `qtermwidget`（CMake 目标别名 `ZzQTermWidget::qtermwidget`）以及示例程序 `qtermwidget_example`。

常用选项：

- `-DZZQTERMWIDGET_BUILD_EXAMPLE=OFF` 关闭示例程序的构建。
- `-DZZQTERMWIDGET_INSTALL=OFF` 关闭安装规则的生成。

作为子项目引入时，只需在你的 `CMakeLists.txt` 中：

```cmake
add_subdirectory(path/to/ZzQTermWidget/lib)
target_link_libraries(your_target PRIVATE ZzQTermWidget::qtermwidget)
```

一些注意：

- 原始项目使用 CMake 构建，本项目同样使用 CMake 构建（顶层 `CMakeLists.txt`、`lib/CMakeLists.txt`、`lib/ptyqt/CMakeLists.txt`）。
- 在Qt6.6.1上测试通过。
- 本项目完全遵守原始项目的LICENSE，修改新增的代码也遵守原始项目的LICENSE。

以下为原始的README：

[README.md](./README-QTermWidget.md)
