<?php
/**
 * CLI模式使用示例 - 消息队列消费者
 */

echo "=== CLI模式 - 消息队列消费者示例 ===\n\n";

// 设置白名单
trace_set_callback_whitelist([
    ['file_contains' => 'cli_example.php'],
]);

// 设置回调
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    return ['operation_name' => $func];
});

// 模拟消息队列消息
$messages = [
    ['id' => 'msg_001', 'user_id' => 123, 'action' => 'register', 'trace_id' => 'parent_trace_001'],
    ['id' => 'msg_002', 'user_id' => 456, 'action' => 'login', 'trace_id' => 'parent_trace_002'],
    ['id' => 'msg_003', 'user_id' => 789, 'action' => 'logout', 'trace_id' => null],
];

// 处理函数
function process_message($message) {
    trace_add_tag('message_id', $message['id']);
    trace_add_tag('user_id', (string)$message['user_id']);
    trace_add_tag('action', $message['action']);
    
    trace_add_log('info', "处理消息: {$message['id']}");
    
    handle_user_action($message['user_id'], $message['action']);
    
    trace_add_log('info', "消息处理完成");
}

function handle_user_action($user_id, $action) {
    trace_add_log('debug', "执行用户操作: {$action}");
    
    // 模拟业务处理
    usleep(50000); // 50ms
    
    trace_add_tag('status', 'success');
}

// 模拟消息队列消费者（常驻进程）
foreach ($messages as $i => $message) {
    echo "\n--- 处理消息 " . ($i + 1) . " ---\n";
    
    // 关键：为每个消息创建独立的trace
    if ($message['trace_id']) {
        // 使用消息携带的TraceID（分布式追踪）
        trace_reset($message['trace_id']);
        echo "使用消息TraceID: {$message['trace_id']}\n";
    } else {
        // 生成新的TraceID
        trace_reset();
        echo "生成新TraceID: " . trace_get_trace_id() . "\n";
    }
    
    // 处理消息
    process_message($message);
    
    // 获取trace数据
    $traces = trace_get_spans();
    echo "  TraceID: " . $traces['trace_id'] . "\n";
    echo "  Spans数: " . count($traces['spans']) . "\n";
    
    // 显示tags
    foreach ($traces['spans'] as $span) {
        if (!empty($span['tags'])) {
            echo "  Tags: ";
            foreach ($span['tags'] as $k => $v) {
                echo "{$k}={$v} ";
            }
            echo "\n";
        }
    }
    
    // 上报到监控系统
    // send_to_jaeger($traces);
    
    echo "消息处理完成\n";
}

echo "\n✅ CLI模式演示完成！\n\n";

echo "💡 CLI模式要点:\n";
echo "1. 使用 trace_reset() 为每个任务创建新trace\n";
echo "2. 可传入trace_id参数实现分布式追踪\n";
echo "3. trace_reset() 会清理所有span并重新开始\n";
echo "4. 适用于: 队列消费者、定时任务、长连接服务等\n";
