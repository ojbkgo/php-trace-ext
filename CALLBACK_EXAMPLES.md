# 回调函数参数说明

## function_enter 回调

当函数被调用时触发，参数如下：

### 参数列表

```php
function onFunctionEnter(
    string $function,      // 函数名
    ?string $class,        // 类名（如果是方法）
    ?string $callerFile,   // 调用方的文件路径
    int $callerLine,       // 调用方的行号
    ?string $parentSpanId, // 父Span ID
    array $args            // 函数参数数组
): ?array
```

### 返回值

返回一个数组来创建Span，或返回 `null`/`false` 不创建Span：

```php
return [
    'operation_name' => 'User::login',  // Span名称
    'tags' => [                         // 可选：初始tags
        'user_id' => '123',
        'method' => 'POST'
    ]
];
```

---

## function_exit 回调

当函数执行完毕时触发，参数如下：

### 参数列表

```php
function onFunctionExit(
    string $spanId,        // 当前Span ID
    float $duration,       // 执行时长（秒）
    mixed $returnValue     // 函数返回值
): ?array
```

### 返回值

返回一个数组来添加tags到Span：

```php
return [
    'tags' => [
        'result' => 'success',
        'rows_affected' => $returnValue
    ]
];
```

---

## 完整示例

### 示例 1：基本追踪

```php
<?php

// 设置进入回调
trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    // 打印调用信息
    echo sprintf(
        "[ENTER] %s%s%s called from %s:%d with %d args\n",
        $class ? $class : '',
        $class ? '::' : '',
        $func,
        basename($callerFile ?? 'unknown'),
        $callerLine,
        count($args)
    );
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => [
            'caller' => basename($callerFile ?? 'unknown') . ':' . $callerLine
        ]
    ];
});

// 设置退出回调
trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    echo sprintf(
        "[EXIT] span=%s duration=%.4fs return=%s\n",
        substr($spanId, 0, 8),
        $duration,
        json_encode($returnValue)
    );
    
    return [
        'tags' => [
            'duration_ms' => round($duration * 1000, 2),
            'has_return' => $returnValue !== null ? 'yes' : 'no'
        ]
    ];
});
```

---

### 示例 2：参数和返回值追踪

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    $tags = [
        'caller_file' => basename($callerFile ?? ''),
        'caller_line' => $callerLine,
        'arg_count' => count($args)
    ];
    
    // 记录特定参数
    if ($func === 'query' && isset($args[0])) {
        $tags['sql'] = $args[0];
    }
    
    if ($func === 'login' && isset($args[0])) {
        $tags['username'] = $args[0];
    }
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => $tags
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = [
        'duration_ms' => round($duration * 1000, 2)
    ];
    
    // 根据返回值类型添加不同的tags
    if (is_bool($returnValue)) {
        $tags['result'] = $returnValue ? 'success' : 'failure';
    } elseif (is_array($returnValue)) {
        $tags['result_count'] = count($returnValue);
    } elseif (is_int($returnValue)) {
        $tags['result_value'] = $returnValue;
    }
    
    return ['tags' => $tags];
});
```

---

### 示例 3：调用链追踪

```php
<?php

// 全局计数器
$callDepth = 0;

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) use (&$callDepth) {
    $callDepth++;
    
    $indent = str_repeat('  ', $callDepth - 1);
    echo sprintf(
        "%s→ %s%s%s(%s) at %s:%d\n",
        $indent,
        $class ? $class : '',
        $class ? '::' : '',
        $func,
        implode(', ', array_map('json_encode', $args)),
        basename($callerFile ?? 'unknown'),
        $callerLine
    );
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => [
            'depth' => $callDepth,
            'caller' => basename($callerFile ?? '') . ':' . $callerLine
        ]
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) use (&$callDepth) {
    $indent = str_repeat('  ', $callDepth - 1);
    echo sprintf(
        "%s← return %s (%.4fs)\n",
        $indent,
        json_encode($returnValue),
        $duration
    );
    
    $callDepth--;
    
    return [
        'tags' => [
            'duration_ms' => round($duration * 1000, 2),
            'return_type' => gettype($returnValue)
        ]
    ];
});
```

**输出示例：**
```
→ UserController::login("admin") at index.php:10
  → UserService::authenticate("admin", "***") at UserController.php:25
    → Database::query("SELECT * FROM users WHERE...") at UserService.php:40
    ← return [...] (0.0023s)
  ← return true (0.0045s)
← return {...} (0.0067s)
```

---

### 示例 4：性能分析

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => [
            'caller' => basename($callerFile ?? '') . ':' . $callerLine,
            'memory_before' => memory_get_usage(true)
        ]
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $memoryAfter = memory_get_usage(true);
    
    $tags = [
        'duration_ms' => round($duration * 1000, 2),
        'memory_after' => $memoryAfter
    ];
    
    // 性能警告
    if ($duration > 0.1) {
        $tags['performance'] = 'slow';
        error_log("Slow function detected: span=$spanId duration={$duration}s");
    }
    
    return ['tags' => $tags];
});
```

---

### 示例 5：错误追踪

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => [
            'caller' => basename($callerFile ?? '') . ':' . $callerLine
        ]
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = ['duration_ms' => round($duration * 1000, 2)];
    
    // 检测错误返回值
    if ($returnValue === false) {
        $tags['status'] = 'error';
        $tags['error'] = 'function returned false';
    } elseif ($returnValue === null) {
        $tags['status'] = 'null_return';
    } else {
        $tags['status'] = 'success';
    }
    
    return ['tags' => $tags];
});
```

---

### 示例 6：数据库查询追踪

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    $tags = [
        'caller' => basename($callerFile ?? '') . ':' . $callerLine
    ];
    
    // 特殊处理数据库查询
    if ($func === 'query' || $func === 'execute') {
        if (isset($args[0]) && is_string($args[0])) {
            // 提取SQL类型
            $sql = $args[0];
            if (preg_match('/^\s*(SELECT|INSERT|UPDATE|DELETE)/i', $sql, $matches)) {
                $tags['sql_type'] = strtoupper($matches[1]);
                $tags['sql'] = substr($sql, 0, 100); // 只记录前100字符
            }
        }
    }
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => $tags
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = ['duration_ms' => round($duration * 1000, 2)];
    
    // 记录受影响的行数
    if (is_int($returnValue)) {
        $tags['affected_rows'] = $returnValue;
    } elseif (is_array($returnValue)) {
        $tags['result_rows'] = count($returnValue);
    }
    
    // 慢查询警告
    if ($duration > 0.05) { // 50ms
        $tags['slow_query'] = 'yes';
    }
    
    return ['tags' => $tags];
});
```

---

### 示例 7：HTTP请求追踪

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    $tags = [
        'caller' => basename($callerFile ?? '') . ':' . $callerLine
    ];
    
    // 追踪HTTP请求
    if ($class === 'GuzzleHttp\\Client' || $func === 'file_get_contents') {
        if (isset($args[0])) {
            $url = parse_url($args[0]);
            $tags['http_host'] = $url['host'] ?? 'unknown';
            $tags['http_path'] = $url['path'] ?? '/';
        }
    }
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => $tags
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = ['duration_ms' => round($duration * 1000, 2)];
    
    // HTTP响应分析
    if (is_string($returnValue)) {
        $tags['response_size'] = strlen($returnValue);
    }
    
    // 慢请求警告
    if ($duration > 1.0) {
        $tags['slow_request'] = 'yes';
    }
    
    return ['tags' => $tags];
});
```

---

## 核心概念

### 调用方上下文（Caller Context）

**`callerFile` 和 `callerLine` 指的是调用当前函数的位置，而不是函数定义的位置。**

```php
// UserController.php
class UserController {
    public function login() {
        $this->authenticate();  // ← 这是caller (line 10)
    }
    
    public function authenticate() {
        // 当追踪这个方法时：
        // $callerFile = "UserController.php"
        // $callerLine = 10  (调用authenticate的位置)
    }
}
```

### 函数参数（$args）

**完整的参数数组，按顺序传递：**

```php
function example($a, $b, $c = 'default') {
    // ...
}

example(1, 'hello');

// 回调中 $args = [1, 'hello']
// 注意：默认值如果没传，不会在$args中
```

### 返回值（$returnValue）

**函数的实际返回值：**

```php
function calculate($a, $b) {
    return $a + $b;
}

$result = calculate(10, 20);

// exit回调中 $returnValue = 30
```

---

## 性能提示

1. **避免在回调中执行重操作**
   - 回调会在每个被追踪的函数执行时调用
   - 保持回调逻辑简单快速

2. **合理使用白名单**
   - 只追踪需要的函数
   - 避免追踪高频调用的简单函数（如getter/setter）

3. **参数和返回值的复制开销**
   - 参数和返回值会被复制，大对象可能有性能影响
   - 在回调中可以选择性地记录参数

4. **Tags数量控制**
   - 不要在tags中存储大量数据
   - Tags主要用于索引和过滤，不是数据存储

---

## 最佳实践

### ✅ 推荐

```php
// 简洁的操作名
'operation_name' => "$class::$func"

// 有意义的tags
'tags' => [
    'caller' => basename($callerFile) . ':' . $callerLine,
    'arg_count' => count($args),
    'duration_ms' => round($duration * 1000, 2)
]

// 有选择地记录参数
if (isset($args[0]) && is_scalar($args[0])) {
    $tags['param_0'] = $args[0];
}
```

### ❌ 避免

```php
// 不要记录整个大对象
'tags' => ['all_args' => json_encode($args)] // 可能很大

// 不要在回调中执行IO操作
file_put_contents('/tmp/trace.log', ...);  // 太慢

// 不要在回调中抛异常
throw new Exception('...');  // 会影响原函数执行
```

---

## 总结

| 回调 | 触发时机 | 主要用途 |
|------|---------|---------|
| function_enter | 函数调用前 | 创建Span，记录输入参数 |
| function_exit | 函数返回后 | 记录返回值，补充tags |

**记住：回调应该快速、简洁、无副作用！** ⚡

