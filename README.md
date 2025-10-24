# PHP全链路跟踪扩展

完整实现的PHP全链路跟踪扩展，支持用户代码和扩展函数（MySQL、Redis、Curl等）的双重追踪。

## ✨ 核心特性

- ✅ 自动函数调用钩子（用户函数 + 扩展函数）
- ✅ 双白名单机制（独立控制用户代码和扩展）
- ✅ 通配符和反向匹配支持
- ✅ 完整的Span栈管理
- ✅ Tags和Logs支持
- ✅ 调用方上下文追踪
- ✅ 参数和返回值捕获
- ✅ OpenTelemetry格式导出
- ✅ FPM进程复用安全
- ✅ 重入保护（防止死循环）

---

## 🚀 快速开始

### 编译安装

```bash
./build.sh
# 或手动编译
phpize && ./configure && make && make install
```

### 配置

```ini
; php.ini
extension=trace.so
trace.enabled = 1

; Debug配置（可选）
trace.debug_enabled = 0
trace.debug_log_path = /tmp/php_trace_debug.log
```

### 基本使用

```php
<?php

// 1. 设置回调
trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => ['caller' => basename($callerFile) . ':' . $callerLine]
    ];
});

// 2. 设置白名单
trace_set_callback_whitelist([
    ['file_pattern' => '/var/www/app/*']
]);

// 3. 获取追踪数据
$spans = trace_get_spans();
echo json_encode($spans, JSON_PRETTY_PRINT);
```

---

## 📚 核心概念

### 双拦截机制

PHP Trace扩展使用两个独立的钩子来追踪不同类型的函数：

| 类型 | 拦截点 | 处理函数 | 白名单函数 | 主要字段 |
|------|--------|---------|-----------|---------|
| **用户函数** | `zend_execute_ex` | `trace_execute_ex()` | `trace_set_callback_whitelist()` | `file_pattern` |
| **内部函数** | `zend_execute_internal` | `trace_execute_internal()` | `trace_set_internal_whitelist()` | `module_pattern` |

**为什么需要两个白名单？**

- 用户函数（PHP代码）有**文件路径**，没有模块名
- 内部函数（MySQL/Redis等）有**模块名**，没有文件路径
- 两者匹配逻辑完全不同，需要独立配置

---

## 📖 API参考

### 核心API

#### `trace_set_callback(string $type, callable $callback)`

设置回调函数。

**支持的类型：**
- `function_enter` - 函数进入时调用
- `function_exit` - 函数退出时调用
- `curl` - Curl操作（预留）
- `database` - 数据库操作（预留）

**function_enter 回调参数：**
```php
function($function, $class, $callerFile, $callerLine, $parentSpanId, $args): ?array
```
- `$function` - 函数名
- `$class` - 类名（如果是方法）
- `$callerFile` - **调用方的文件路径**
- `$callerLine` - **调用方的行号**
- `$parentSpanId` - 父Span ID
- `$args` - **函数参数数组**

**返回值：**
```php
return [
    'operation_name' => 'User::login',  // 必需：Span名称
    'tags' => [...],                    // 可选：初始tags
    'logs' => [...]                     // 可选：初始logs
];
```

**function_exit 回调参数：**
```php
function($spanId, $duration, $returnValue): ?array
```
- `$spanId` - 当前Span ID
- `$duration` - 执行时长（秒）
- `$returnValue` - **函数返回值**

**返回值：**
```php
return [
    'tags' => [...],  // 可选：添加或更新tags
    'logs' => [...]   // 可选：添加logs
];
```

---

#### `trace_set_callback_whitelist(array $rules)`

设置**用户函数白名单**（PHP代码）。

**字段：**
- `file_pattern` - 文件路径模式（字符串或数组）
- `class_pattern` - 类名模式（字符串或数组）
- `function_pattern` - 函数名模式（字符串或数组）

```php
trace_set_callback_whitelist([
    [
        'file_pattern' => '/var/www/app/*',
        'class_pattern' => 'App\\*',
        'function_pattern' => '*'
    ]
]);
```

---

#### `trace_set_internal_whitelist(array $rules)`

设置**内部函数白名单**（扩展函数：MySQL、Redis、Curl等）。

**字段：**
- `module_pattern` - 扩展模块名模式（字符串或数组）
- `class_pattern` - 类名模式（字符串或数组）
- `function_pattern` - 函数名模式（字符串或数组）

```php
trace_set_internal_whitelist([
    [
        'module_pattern' => ['mysqli', 'pdo'],
        'function_pattern' => ['query', 'execute']
    ]
]);
```

---

#### 其他API

```php
trace_get_trace_id()               // 获取当前TraceID
trace_get_current_span()           // 获取当前Span信息
trace_add_tag($key, $value)        // 添加tag到当前span
trace_add_log($level, $message)    // 添加log到当前span
trace_get_spans()                  // 导出所有spans（OpenTelemetry格式）
trace_reset(?string $traceId)      // 重置trace（CLI模式使用）
```

---

## 🎯 白名单配置详解

### 匹配规则

**支持的特性：**
- ✅ 通配符 `*`（匹配任意字符）
- ✅ 反向匹配 `! pattern`（排除匹配）
- ✅ 字符串或数组（数组内AND关系）
- ✅ 多规则OR关系

**逻辑关系：**
```
规则1 (OR) 规则2 (OR) 规则3
  ↓         ↓         ↓
file (AND) class (AND) function
  ↓         ↓         ↓
[条件1 (AND) 条件2 (AND) 条件3]
```

### 通配符示例

| 模式 | 匹配 | 不匹配 |
|------|------|--------|
| `App\\*` | `App\\User`, `App\\Controller\\User` | `Root\\App` |
| `*Controller` | `UserController`, `App\\UserController` | `ControllerBase` |
| `handle*` | `handle`, `handleRequest` | `myHandle` |
| `/app/*/test.php` | `/app/admin/test.php` | `/app/test.php` |
| `*` | 任何内容 | - |

### 反向匹配示例

```php
// 语法：感叹号 + 空格 + 模式
'! pattern'

// 示例：在/app/下但排除vendor
[
    'file_pattern' => [
        '/app/*',           // ✅ 必须匹配
        '! */vendor/*',     // ❌ 不能匹配
        '! */Test.php'      // ❌ 不能匹配
    ]
]
```

---

## 💡 实战示例

### 示例1：只追踪业务代码

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => ['caller' => basename($callerFile) . ':' . $callerLine]
    ];
});

trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/var/www/app/*',
            '! */vendor/*',
            '! */cache/*'
        ],
        'class_pattern' => ['App\\*'],
        'function_pattern' => [
            '*',
            '! get*',      // 排除getter
            '! set*'       // 排除setter
        ]
    ]
]);
```

---

### 示例2：全链路追踪（业务 + 数据库 + 缓存）

```php
<?php

// 设置回调
trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    $tags = ['caller' => basename($callerFile) . ':' . $callerLine];
    
    // 记录SQL或Redis命令
    if (isset($args[0]) && is_string($args[0])) {
        $tags['query'] = substr($args[0], 0, 200);
    }
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => $tags
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = ['duration_ms' => round($duration * 1000, 2)];
    
    // 慢操作警告
    if ($duration > 0.1) {
        $tags['slow'] = 'yes';
        error_log("[SLOW] span=$spanId duration={$duration}s");
    }
    
    // 记录结果
    if (is_bool($returnValue)) {
        $tags['success'] = $returnValue ? 'yes' : 'no';
    } elseif (is_array($returnValue)) {
        $tags['count'] = count($returnValue);
    }
    
    return ['tags' => $tags];
});

// 用户代码白名单
trace_set_callback_whitelist([
    ['file_pattern' => '/var/www/app/Controllers/*', 'class_pattern' => '*Controller'],
    ['file_pattern' => '/var/www/app/Services/*', 'class_pattern' => '*Service']
]);

// 扩展函数白名单
trace_set_internal_whitelist([
    // MySQL
    ['module_pattern' => ['mysqli', 'pdo'], 'function_pattern' => ['query', 'execute']],
    // Redis（排除高频get/set）
    ['module_pattern' => 'redis', 'function_pattern' => ['*', '! get', '! set']],
    // Curl
    ['module_pattern' => 'curl', 'function_pattern' => 'curl_exec']
]);
```

**完整的调用链追踪：**
```
HTTP Request
  ↓
UserController::handleLogin()     [用户白名单: file_pattern]
  ↓
UserService::authenticate()       [用户白名单: file_pattern]
  ↓
PDO::query("SELECT...")          [内部白名单: module_pattern]
  ↓
Redis::hGet("session:123")       [内部白名单: module_pattern]
  ↓
curl_exec($ch)                   [内部白名单: module_pattern]
```

---

### 示例3：调用链可视化

```php
<?php

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
        implode(', ', array_map('json_encode', array_slice($args, 0, 3))),
        basename($callerFile ?? 'unknown'),
        $callerLine
    );
    
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => ['depth' => $callDepth]
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) use (&$callDepth) {
    $indent = str_repeat('  ', $callDepth - 1);
    echo sprintf("%s← %.4fs\n", $indent, $duration);
    $callDepth--;
    
    return ['tags' => ['duration_ms' => round($duration * 1000, 2)]];
});

trace_set_callback_whitelist([
    ['file_pattern' => '/var/www/app/*']
]);
```

**输出：**
```
→ UserController::login("admin") at index.php:10
  → UserService::authenticate("admin", "***") at UserController.php:25
    → PDO::query("SELECT * FROM users...") at UserService.php:40
    ← 0.0023s
  ← 0.0045s
← 0.0067s
```

---

### 示例4：性能分析

```php
<?php

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => [
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
        error_log("[SLOW] span=$spanId duration={$duration}s");
    }
    
    return ['tags' => $tags];
});

trace_set_callback_whitelist([
    ['file_pattern' => '/var/www/app/Services/*']
]);
```

---

## 🎨 白名单配置详解

### 用户函数白名单（PHP代码）

使用 `trace_set_callback_whitelist()` 配置。

#### 基本示例

```php
// 简单模式
trace_set_callback_whitelist([
    ['file_pattern' => '/app/Controllers/*']
]);

// 组合条件（AND关系）
trace_set_callback_whitelist([
    [
        'file_pattern' => '/app/Controllers/*',
        'class_pattern' => '*Controller',
        'function_pattern' => 'handle*'
    ]
]);

// 数组模式（AND关系）
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/app/*',           // 必须在/app下
            '! */vendor/*',     // 排除vendor
            '! */Test.php'      // 排除测试文件
        ]
    ]
]);

// 多规则（OR关系）
trace_set_callback_whitelist([
    ['file_pattern' => '/app/Controllers/*'],
    ['file_pattern' => '/app/Services/*'],
    ['file_pattern' => '/app/Repositories/*']
]);
```

#### 高级示例

```php
// 分层追踪，排除简单方法
trace_set_callback_whitelist([
    // Controller层：全部追踪
    [
        'file_pattern' => '/app/Http/*',
        'class_pattern' => 'App\\Http\\Controllers\\*'
    ],
    // Service层：排除getter/setter
    [
        'file_pattern' => '/app/Services/*',
        'function_pattern' => ['*', '! get*', '! set*', '! __*']
    ],
    // Repository层：只追踪复杂查询
    [
        'file_pattern' => '/app/Repositories/*',
        'function_pattern' => ['*Query', '*Report', 'bulk*']
    ]
]);
```

---

### 内部函数白名单（扩展函数）

使用 `trace_set_internal_whitelist()` 配置。

#### 支持的扩展

| 扩展 | module_pattern | 常见函数 |
|------|---------------|---------|
| MySQL | `mysqli`, `mysqlnd` | `query`, `real_query`, `execute` |
| PDO | `pdo`, `pdo_mysql`, `PDO` | `query`, `exec`, `execute` |
| Redis | `redis` | `get`, `set`, `hGet`, `zadd` |
| Curl | `curl` | `curl_exec`, `curl_init` |
| Memcached | `memcached` | `get`, `set`, `add` |

#### 基本示例

```php
// 追踪MySQL查询
trace_set_internal_whitelist([
    [
        'module_pattern' => ['mysqli', 'pdo'],
        'function_pattern' => ['query', 'execute']
    ]
]);

// 追踪Redis（排除高频操作）
trace_set_internal_whitelist([
    [
        'module_pattern' => 'redis',
        'function_pattern' => ['*', '! get', '! set', '! exists']
    ]
]);

// 追踪Curl
trace_set_internal_whitelist([
    [
        'module_pattern' => 'curl',
        'function_pattern' => 'curl_exec'
    ]
]);
```

#### 完整示例

```php
// 追踪所有基础设施调用
trace_set_internal_whitelist([
    // 数据库
    [
        'module_pattern' => ['mysqli', 'pdo', 'pdo_mysql'],
        'function_pattern' => ['query', 'execute', 'exec', 'prepare']
    ],
    // 缓存
    [
        'module_pattern' => 'redis',
        'class_pattern' => 'Redis',
        'function_pattern' => ['*', '! get', '! set']
    ],
    // HTTP客户端
    [
        'module_pattern' => 'curl',
        'function_pattern' => ['curl_exec', 'curl_multi_exec']
    ]
]);

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    $tags = ['caller' => basename($callerFile) . ':' . $callerLine];
    
    // 记录SQL
    if (in_array($func, ['query', 'execute', 'exec']) && isset($args[0]) && is_string($args[0])) {
        $tags['sql'] = substr($args[0], 0, 200);
    }
    
    // 记录Redis key
    if ($class === 'Redis' && isset($args[0])) {
        $tags['redis_key'] = (string)$args[0];
    }
    
    return [
        'operation_name' => $class ? "$class::$func" : "EXT::$func",
        'tags' => $tags
    ];
});
```

---

## ⚡ 性能优化

### 推荐做法

1. **精确的白名单**
```php
// 好：具体的路径
['file_pattern' => '/app/Controllers/UserController.php']

// 差：过于宽泛
['file_pattern' => '*']
```

2. **优先使用file_pattern**
```php
// 好：文件过滤最快
['file_pattern' => '/app/Controllers/*']

// 可以：但比文件过滤慢
['class_pattern' => 'App\\Controllers\\*']
```

3. **排除高频函数**
```php
[
    'file_pattern' => '/app/Services/*',
    'function_pattern' => ['*', '! get*', '! set*', '! is*']
]

// 内部函数也要排除
[
    'module_pattern' => 'redis',
    'function_pattern' => ['*', '! get', '! set']
]
```

4. **分层追踪**
```php
// Controller：全部
['file_pattern' => '/app/Controllers/*']

// Service：排除简单方法
['file_pattern' => '/app/Services/*', 'function_pattern' => ['*', '! get*', '! set*']]

// DB：只追踪查询
['module_pattern' => 'mysqli', 'function_pattern' => ['query', 'execute']]
```

### 避免的做法

```php
// ❌ 追踪所有函数
['file_pattern' => '*']

// ❌ 追踪高频getter/setter
['function_pattern' => 'get*']

// ❌ 在回调中执行IO
file_put_contents('/tmp/trace.log', ...);

// ❌ 在回调中记录大对象
'tags' => ['all_args' => json_encode($args)]
```

---

## 🔧 调试技巧

### 启用调试日志

```ini
; php.ini
trace.debug_enabled = 1
trace.debug_log_path = /tmp/php_trace_debug.log
```

### 查看日志

```bash
tail -f /tmp/php_trace_debug.log
```

**关键警告：**
```
[SPAN_CREATE] ⚠️ 创建无父级span: SomeFunction (current_span=0x0, root_span=0x123)
```
说明parent链被破坏，需要检查回调返回值。

### 测试白名单

```php
// 1. 先宽松配置，确认功能正常
trace_set_callback_whitelist([
    ['file_pattern' => '*']
]);

// 2. 逐步收窄
trace_set_callback_whitelist([
    ['file_pattern' => '/app/*']
]);

// 3. 添加排除条件
trace_set_callback_whitelist([
    ['file_pattern' => ['/app/*', '! */vendor/*']]
]);
```

### 查看扩展信息

```bash
# 查看phpinfo
php -d extension=trace.so --ri trace

# 会显示：
# - 当前TraceID
# - 回调设置状态
# - 白名单规则数量
```

---

## 📋 完整配置模板

```php
<?php

// ===================
// 1. 设置回调函数
// ===================

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    // 判断是用户代码还是扩展
    $isExtension = !str_starts_with($callerFile ?? '', '/var/www/');
    
    if ($isExtension) {
        $operationName = $class ? "$class::$func" : "EXT::$func";
        $layer = 'infrastructure';
    } else {
        $operationName = $class ? "$class::$func" : $func;
        $layer = 'business';
    }
    
    $tags = [
        'layer' => $layer,
        'caller' => basename($callerFile ?? 'unknown') . ':' . $callerLine
    ];
    
    // 记录关键参数
    if (isset($args[0]) && is_string($args[0]) && strlen($args[0]) < 200) {
        $tags['param_0'] = $args[0];
    }
    
    return [
        'operation_name' => $operationName,
        'tags' => $tags
    ];
});

trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = ['duration_ms' => round($duration * 1000, 2)];
    
    // 慢操作警告
    if ($duration > 0.1) {
        $tags['slow'] = 'yes';
    }
    
    // 结果分析
    if (is_bool($returnValue)) {
        $tags['success'] = $returnValue ? 'yes' : 'no';
    } elseif (is_array($returnValue)) {
        $tags['result_count'] = count($returnValue);
    } elseif (is_int($returnValue)) {
        $tags['affected_rows'] = $returnValue;
    }
    
    return ['tags' => $tags];
});

// ===================
// 2. 用户代码白名单
// ===================

trace_set_callback_whitelist([
    // Controller层
    [
        'file_pattern' => '/var/www/app/Http/Controllers/*',
        'class_pattern' => 'App\\Http\\Controllers\\*'
    ],
    // Service层（排除简单方法）
    [
        'file_pattern' => '/var/www/app/Services/*',
        'class_pattern' => 'App\\Services\\*',
        'function_pattern' => [
            '*',
            '! get*',
            '! set*',
            '! is*',
            '! __*'
        ]
    ],
    // Repository层（只追踪复杂查询）
    [
        'file_pattern' => '/var/www/app/Repositories/*',
        'function_pattern' => ['*Query', '*Report', 'bulk*']
    ]
]);

// ===================
// 3. 扩展函数白名单
// ===================

trace_set_internal_whitelist([
    // MySQL/PDO
    [
        'module_pattern' => ['mysqli', 'pdo', 'pdo_mysql'],
        'function_pattern' => ['query', 'execute', 'exec', 'prepare']
    ],
    // Redis（排除高频操作）
    [
        'module_pattern' => 'redis',
        'class_pattern' => 'Redis',
        'function_pattern' => [
            '*',
            '! get',
            '! set',
            '! exists',
            '! ttl'
        ]
    ],
    // Curl
    [
        'module_pattern' => 'curl',
        'function_pattern' => ['curl_exec', 'curl_multi_exec']
    ]
]);

// ===================
// 4. 导出追踪数据
// ===================

// 在请求结束时导出
register_shutdown_function(function() {
    $spans = trace_get_spans();
    
    // 发送到追踪系统
    // sendToJaeger($spans);
    // sendToZipkin($spans);
    
    // 或保存到日志
    error_log(json_encode($spans));
});
```

---

## 🔍 常见问题

### Q1: 如何判断函数是用户代码还是扩展？

在回调中检查 `$callerFile`：
```php
$isExtension = !str_starts_with($callerFile ?? '', '/var/www/');
```

### Q2: 如何追踪特定的SQL查询？

```php
trace_set_internal_whitelist([
    ['module_pattern' => ['mysqli', 'pdo'], 'function_pattern' => 'query']
]);

trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    $tags = [];
    if (isset($args[0]) && is_string($args[0])) {
        $sql = $args[0];
        if (preg_match('/^\s*(SELECT|INSERT|UPDATE|DELETE)/i', $sql, $m)) {
            $tags['sql_type'] = $m[1];
            $tags['sql'] = substr($sql, 0, 200);
        }
    }
    return ['operation_name' => "DB::$func", 'tags' => $tags];
});
```

### Q3: parent_id为空怎么办？

启用调试日志查看：
```
[SPAN_CREATE] ⚠️ 创建无父级span: SomeFunction (current_span=0x0, root_span=0x123)
```

**常见原因：**
1. 回调返回 `null` 或 `false`，没有创建span
2. root_span未正确初始化
3. current_span被意外重置

**解决方案：**
- 确保回调总是返回有效的 `operation_name`
- 检查是否在CLI模式下（需要调用 `trace_reset()`）

### Q4: 如何在CLI模式下使用？

```php
// CLI模式需要手动初始化
trace_reset();  // 创建root span

// 设置回调和白名单...

// 执行你的代码

// 导出数据
$spans = trace_get_spans();
echo json_encode($spans, JSON_PRETTY_PRINT);
```

---

## 🎯 最佳实践

### 1. 按层次追踪

```php
// HTTP层：全部追踪
['file_pattern' => '/app/Controllers/*']

// 业务层：排除简单方法
['file_pattern' => '/app/Services/*', 'function_pattern' => ['*', '! get*', '! set*']]

// 数据层：只追踪查询
['module_pattern' => 'mysqli', 'function_pattern' => 'query']
```

### 2. 合理记录参数

```php
// 只记录关键参数
if ($func === 'login' && isset($args[0])) {
    $tags['username'] = $args[0];
}

// 限制字符串长度
if (isset($args[0]) && is_string($args[0])) {
    $tags['query'] = substr($args[0], 0, 200);
}
```

### 3. 性能监控

```php
trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    // 记录慢操作
    if ($duration > 0.1) {
        error_log("[SLOW] $spanId: {$duration}s");
    }
    
    return ['tags' => ['duration_ms' => round($duration * 1000, 2)]];
});
```

### 4. 错误追踪

```php
trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    $tags = [];
    
    if ($returnValue === false) {
        $tags['error'] = 'function_returned_false';
    }
    
    return ['tags' => $tags];
});
```

---

## 📊 性能考虑

### 开销分析

| 操作 | 开销 | 说明 |
|------|------|------|
| 白名单检查 | 极低 | 通配符匹配，早期退出 |
| 参数复制 | 中等 | 取决于参数大小 |
| 回调调用 | 中等 | 取决于回调逻辑复杂度 |
| Span创建 | 低 | 简单的内存分配 |

### 优化建议

1. **白名单越精确越好**
2. **避免追踪高频函数**（getter/setter、简单的Redis get/set）
3. **回调保持简单**（避免IO、数据库操作）
4. **限制记录的数据量**（截断大字符串、避免大对象）

---

## 🛠️ 高级功能

### Tags和Logs

```php
// 在enter回调中返回tags和logs
trace_set_callback('function_enter', function($func, $class, $callerFile, $callerLine, $parentSpanId, $args) {
    return [
        'operation_name' => "$class::$func",
        'tags' => [
            'user_id' => '123',
            'action' => 'login'
        ],
        'logs' => [
            ['level' => 'info', 'message' => 'Function started']
        ]
    ];
});

// 在exit回调中添加更多tags和logs
trace_set_callback('function_exit', function($spanId, $duration, $returnValue) {
    return [
        'tags' => [
            'duration_ms' => round($duration * 1000, 2),
            'success' => $returnValue !== false
        ],
        'logs' => [
            ['level' => 'info', 'message' => 'Function completed']
        ]
    ];
});

// 手动添加
trace_add_tag('custom_key', 'custom_value');
trace_add_log('debug', 'Custom log message');
```

### OpenTelemetry导出

```php
$spans = trace_get_spans();
// 返回标准的OpenTelemetry格式
// 可直接发送到Jaeger、Zipkin等追踪系统

// 示例输出：
[
    'traceId' => '507f1f77bcf86cd799439011',
    'serviceName' => 'php-app',
    'spans' => [
        [
            'spanId' => '507f191e810c19729de860ea',
            'parentSpanId' => null,
            'operationName' => 'http.request',
            'startTime' => 1234567890.123456,
            'endTime' => 1234567890.234567,
            'duration' => 0.111111,
            'tags' => [...],
            'logs' => [...]
        ],
        // ... more spans
    ]
]
```

---

## 📖 总结

### 核心对比

| 特性 | 用户函数 | 内部函数 |
|------|---------|---------|
| 设置函数 | `trace_set_callback_whitelist()` | `trace_set_internal_whitelist()` |
| 主要字段 | `file_pattern` | `module_pattern` |
| 匹配内容 | PHP文件路径 | 扩展模块名 |
| Hook点 | `zend_execute_ex` | `zend_execute_internal` |
| 示例 | `/var/www/app/UserController.php` | `mysqli`, `redis`, `curl` |

### 关键点

- ✅ **两个独立白名单** - 用户代码和扩展函数分开配置
- ✅ **调用方上下文** - 追踪调用发起的位置
- ✅ **完整参数和返回值** - 捕获输入输出
- ✅ **Tags动态合并** - enter和exit回调都可以添加tags
- ✅ **通配符支持** - 灵活的模式匹配
- ✅ **反向匹配** - 排除不需要的函数
- ✅ **FPM安全** - 请求级变量，避免进程复用污染
- ✅ **重入保护** - 避免回调中的函数调用导致死循环

---

## 🎉 使用场景

### 1. 性能分析
- 找出慢函数
- 分析调用链
- 检测内存泄漏

### 2. 调用链追踪
- 完整的请求链路
- 跨层调用关系
- 数据库和缓存访问模式

### 3. 问题排查
- 追踪异常调用
- 分析错误传播
- 调试复杂逻辑

### 4. 业务监控
- 关键业务指标
- 用户行为分析
- A/B测试数据收集

---

**PHP Trace扩展 - 让PHP应用的每一次调用都清晰可见！** 🚀
