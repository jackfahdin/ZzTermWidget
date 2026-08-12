# 测试目标统一注册：链接静态库与 Qt6::Test，offscreen 平台运行
set(QTERMWIDGET_TESTS
    tst_charwidth
    tst_emulation
    tst_osc52
    tst_history
    tst_protocols
    tst_benchmark
)

foreach(test_name IN LISTS QTERMWIDGET_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE
        ZzQTermWidget::qtermwidget
        Qt${QT_VERSION_MAJOR}::Test
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
    # 无显示环境下运行（CI）；QClipboard 在 offscreen 下为进程内实现，可用
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()
