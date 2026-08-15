# 测试目标统一注册：链接静态库与 Qt6::Test，offscreen 平台运行
set(QTERMWIDGET_TESTS
    tst_charwidth
    tst_ligature
    tst_emulation
    tst_osc52
    tst_history
    tst_protocols
    tst_benchmark
    tst_rendering
    tst_sixel
    tst_kittygraphics
)

foreach(test_name IN LISTS QTERMWIDGET_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE
        ZzQTermWidget::qtermwidget
        Qt${QT_VERSION_MAJOR}::Test
    )
    if(WIN32)
        # Windows CI 上 ctest 捕获不到 QTest 输出（多轮日志实证零输出）：
        # 强制控制台子系统，并让 QTest 同时落盘文本日志（失败时随 artifact 上传）
        target_link_options(${test_name} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-mconsole>
            $<$<CXX_COMPILER_ID:MSVC>:/SUBSYSTEM:CONSOLE>)
        add_test(NAME ${test_name} COMMAND ${test_name}
                 -o -,txt -o ${test_name}-qtest.log,txt)
    else()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
    # 无显示环境下运行（CI）；QClipboard 在 offscreen 下为进程内实现，可用
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
