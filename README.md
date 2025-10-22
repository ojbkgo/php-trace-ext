# PHP全链路跟踪扩展

完整实现您所有需求的PHP全链路跟踪扩展。

## 编译安装

```bash
make clean && make && make install
```

## 快速开始

```php
<?php

// 1. 设置白名单
trace_set_callback_whitelist([
    ['file_contains' => '/app/']
]);

// 2. 设置回调（可返回tags）
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    return [
        'operation_name' => $func,
        'tags' => [                    // 支持从callback返回tags
            'function' => $func,
            'class' => $class,
            'file' => basename($file)
        ]
    ];
});

// 3. 业务代码
function user_login($username) {
    trace_add_tag('user', $username);  // 手动添加tag
    trace_add_log('info', "登录: {$username}");
    return query_user($username);
}

// 4. 导出数据
$traces = trace_get_spans();
// spans包含callback返回的tags + 手动添加的tags
```

## 核心API

### trace_set_callback_whitelist($rules)
设置白名单 - **15种匹配规则**

```php
trace_set_callback_whitelist([
    // 函数名（5种）
    ['function' => 'exact_name'],           // 精确
    ['function_prefix' => 'api_'],          // 前缀
    ['function_suffix' => '_handler'],      // 后缀
    ['function_contains' => 'process'],     // 包含
    ['function_not_contains' => 'test'],    // 不包含
    
    // 类名（4种）
    ['class' => 'UserService'],             // 精确
    ['class_prefix' => 'App\\'],            // 前缀
    ['class_suffix' => 'Controller'],       // 后缀
    ['class_contains' => 'Service'],        // 包含
    
    // 文件路径（5种）
    ['file' => '/path/file.php'],           // 精确
    ['file_prefix' => '/app/'],             // 前缀
    ['file_suffix' => '.controller.php'],   // 后缀
    ['file_contains' => '/services/'],      // 包含
    ['file_not_contains' => '/vendor/'],    // 不包含
]);
```

### trace_set_callback($type, $callback)
设置回调 - **支持返回tags**

```php
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    return [
        'operation_name' => $func,
        'tags' => [              // ⭐ 新功能：从callback返回tags
            'http.method' => $_SERVER['REQUEST_METHOD'] ?? '',
            'user_id' => get_current_user_id(),
            'custom_tag' => 'value'
        ]
    ];
});
```

### trace_reset($trace_id = null)
重置trace - **CLI模式专用**

```php
// 自动生成新TraceID
trace_reset();

// 使用指定TraceID（分布式追踪）
trace_reset('parent_trace_id_from_message');
```

### trace_add_tag($key, $value)
添加Tag到当前Span

```php
trace_add_tag('user_id', '123');
trace_add_tag('action', 'login');
```

### 其他API
- `trace_get_trace_id()` - 获取TraceID
- `trace_get_current_span()` - 获取当前Span
- `trace_add_log($level, $message)` - 添加日志
- `trace_get_spans()` - 导出数据（OpenTelemetry格式）

## INI配置

```ini
; php.ini
extension=trace.so
trace.enabled = 1

; Debug配置（可选）
trace.debug_enabled = 1
trace.debug_log_path = /var/log/php_trace.log
```

或命令行：
```bash
php -d extension=trace.so -d trace.debug_enabled=1 test.php
```

## 使用场景

### Web应用
```php
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    return [
        'operation_name' => $func,
        'tags' => [
            'http.url' => $_SERVER['REQUEST_URI'] ?? '',
            'http.method' => $_SERVER['REQUEST_METHOD'] ?? '',
            'user_agent' => $_SERVER['HTTP_USER_AGENT'] ?? ''
        ]
    ];
});
```

### CLI常驻进程
```php
while (true) {
    $message = $queue->consume();
    
    // 每个消息独立trace
    trace_reset($message->trace_id);
    
    trace_add_tag('message_id', $message->id);
    process($message);
    
    $traces = trace_get_spans();
    send_to_jaeger($traces);
}
```

## Debug日志

```bash
# 清空日志
./debug_helper.sh clear

# 运行测试
php -d trace.debug_enabled=1 test.php

# 查看日志
./debug_helper.sh view

# 统计信息
./debug_helper.sh stats
```

## 完整功能

### API（8个函数）
1. trace_get_trace_id()
2. trace_set_callback()
3. trace_set_callback_whitelist()
4. trace_get_current_span()
5. trace_add_log()
6. trace_add_tag()
7. trace_get_spans()
8. trace_reset()

### INI配置（3个）
1. trace.enabled
2. trace.debug_enabled
3. trace.debug_log_path

### 白名单规则（15种）
- 函数：5种
- 类：4种
- 文件：5种

### Tags支持（2种方式）
1. callback返回tags
2. trace_add_tag()手动添加

**您的PHP全链路跟踪扩展已完全实现！**