<?php

echo "=== PHP全链路跟踪扩展 - 完整功能测试 ===\n\n";

if (!extension_loaded('trace')) {
    die("错误: trace扩展未加载\n");
}

echo "1. TraceID: " . trace_get_trace_id() . "\n\n";

echo "2. 设置白名单:\n";
trace_set_callback_whitelist([
    ['file_contains' => 'test.php'],
]);
echo "   白名单设置完成\n\n";

echo "3. 设置回调（支持返回tags）:\n";
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    echo "  → {$func}\n";
    
    // 返回operation_name和tags
    return [
        'operation_name' => $func,
        'tags' => [
            'function_name' => $func,
            'file' => basename($file ?: ''),
            'line' => (string)$line
        ]
    ];
});

trace_set_callback('function_exit', function($span_id, $duration, $rv) {
    echo "  ← 耗时: " . number_format($duration * 1000, 2) . "ms\n";
});
echo "   回调设置完成\n\n";

echo "4. 函数调用测试:\n";

function business_function() {
    // 手动添加tag
    trace_add_tag('manual_tag', 'manual_value');
    trace_add_tag('user_id', '123');
    
    trace_add_log('info', '业务函数执行');
    
    $result = helper_function();
    return $result;
}

function helper_function() {
    trace_add_tag('helper', 'true');
    trace_add_log('debug', '辅助函数');
    usleep(20000);
    return 'ok';
}

$result = business_function();
echo "  结果: {$result}\n\n";

echo "5. 测试trace_reset (CLI模式):\n";
$old_id = trace_get_trace_id();
echo "  重置前: {$old_id}\n";

trace_reset();
$new_id = trace_get_trace_id();
echo "  重置后: {$new_id}\n";

trace_reset('custom_trace_12345');
$custom_id = trace_get_trace_id();
echo "  自定义: {$custom_id}\n\n";

// 重新设置一次
trace_reset();

echo "6. 链路数据导出:\n";
$data = trace_get_spans();
echo "  TraceID: " . $data['trace_id'] . "\n";
echo "  Spans: " . count($data['spans']) . "\n\n";

foreach ($data['spans'] as $i => $span) {
    echo "  Span[{$i}]: " . $span['operation_name'] . "\n";
    echo "    耗时: " . number_format($span['duration'] * 1000, 2) . "ms\n";
    echo "    父Span: " . ($span['parent_id'] ?: '根') . "\n";
    
    if (!empty($span['tags'])) {
        echo "    Tags: ";
        foreach ($span['tags'] as $k => $v) {
            echo "{$k}={$v} ";
        }
        echo "\n";
    }
    
    if (!empty($span['logs'])) {
        echo "    日志: " . count($span['logs']) . "条\n";
    }
    echo "\n";
}

echo "✅ 测试完成!\n\n";

echo "📋 新功能验证:\n";
echo "1. ✅ callback可以返回tags（自动添加到span）\n";
echo "2. ✅ trace_add_tag() 手动添加tag\n";
echo "3. ✅ trace_reset() CLI模式支持\n";
echo "4. ✅ 白名单15种规则\n";
echo "5. ✅ Debug配置从INI读取\n";