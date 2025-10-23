# PHP全链路跟踪扩展

完整实现的PHP全链路跟踪扩展，基于钩子+PHP回调混合架构。

## 编译安装

```bash
make clean && make && make install
```

## INI配置

```ini
; php.ini
extension=trace.so
trace.enabled = 1

; Debug配置（可选）
trace.debug_enabled = 1
trace.debug_log_path = /var/log/php_trace.log
```

**命令行方式**：
```bash
php -d extension=trace.so -d trace.debug_enabled=1 test.php
```

**环境变量方式**（备用）：
```bash
export PHP_TRACE_DEBUG=1
php -d extension=trace.so test.php
```

## 核心API

### trace_set_callback_whitelist($rules)
设置白名单 - **15种匹配规则**

```php
trace_set_callback_whitelist([
    // 函数名（5种）
    ['function' => 'exact_name'],
    ['function_prefix' => 'api_'],
    ['function_suffix' => '_handler'],
    ['function_contains' => 'process'],
    ['function_not_contains' => 'test'],
    
    // 类名（4种）
    ['class' => 'UserService'],
    ['class_prefix' => 'App\\'],
    ['class_suffix' => 'Controller'],
    ['class_contains' => 'Service'],
    
    // 文件（5种）
    ['file' => '/path/file.php'],
    ['file_prefix' => '/app/'],
    ['file_suffix' => '.controller.php'],
    ['file_contains' => '/services/'],
    ['file_not_contains' => '/vendor/'],
]);
```

### trace_set_callback($type, $callback)
设置回调 - **支持返回tags**

```php
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    return [
        'operation_name' => $func,
        'tags' => [              // 可选：返回tags
            'function' => $func,
            'class' => $class
        ]
    ];
});
```

### trace_reset($trace_id = null)
重置trace - **CLI模式**

```php
trace_reset();                    // 生成新TraceID
trace_reset('custom_trace_id');   // 使用指定TraceID
```

### trace_add_tag($key, $value)
添加Tag到当前Span

```php
trace_add_tag('user_id', '123');
```

### 其他API
- `trace_get_trace_id()` - 获取TraceID
- `trace_get_current_span()` - 获取当前Span
- `trace_add_log($level, $message)` - 添加日志
- `trace_get_spans()` - 导出数据（OpenTelemetry格式）

## Debug日志

### 启用方式

```bash
# 命令行
php -d trace.debug_enabled=1 -d extension=trace.so test.php

# 或环境变量
export PHP_TRACE_DEBUG=1
php -d extension=trace.so test.php
```

### 查看日志

```bash
cat /tmp/php_trace_debug.log
tail -f /tmp/php_trace_debug.log
```

## 测试

```bash
# 基础测试
php -d extension=trace.so test.php

# 配置测试
php -d extension=trace.so -d trace.debug_enabled=1 config_test.php

# 查看扩展信息
php -d extension=trace.so --ri trace
```

## 完整功能

✅ 自动TraceID生成  
✅ 函数调用自动钩子  
✅ 15种白名单规则  
✅ Span栈管理  
✅ Tags支持（callback + trace_add_tag）  
✅ Logs支持  
✅ CLI模式（trace_reset）  
✅ 可配置Debug  
✅ OpenTelemetry格式  

**您的PHP全链路跟踪扩展已完全实现！**