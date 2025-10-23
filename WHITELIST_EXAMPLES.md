# 白名单配置示例

## 基本用法

白名单是一个数组，每项包含三个可选字段：
- `file_pattern` - 文件路径模式（支持 `*` 通配符）
- `class_pattern` - 类名模式（支持 `*` 通配符）
- `function_pattern` - 函数名模式（支持 `*` 通配符）

**规则说明：**
- 如果某个字段没有设置，相当于 `*`（匹配所有）
- 多个规则之间是 **OR** 关系（符合任意一个即可）
- 每个规则内部的条件是 **AND** 关系（都要符合）

---

## 示例 1：跟踪特定目录的所有文件

```php
<?php
// 跟踪 /app/Controllers/ 目录下的所有函数
trace_set_callback_whitelist([
    ['file_pattern' => '/app/Controllers/*']
]);
```

---

## 示例 2：跟踪特定命名空间的类

```php
<?php
// 跟踪 App\Controller 命名空间下的所有类
trace_set_callback_whitelist([
    ['class_pattern' => 'App\\Controller\\*']
]);
```

---

## 示例 3：跟踪特定函数名

```php
<?php
// 跟踪所有以 handle 开头的函数
trace_set_callback_whitelist([
    ['function_pattern' => 'handle*']
]);
```

---

## 示例 4：组合条件（AND关系）

```php
<?php
// 跟踪 App\Controller 命名空间下，以 handle 开头的函数
trace_set_callback_whitelist([
    [
        'class_pattern' => 'App\\Controller\\*',
        'function_pattern' => 'handle*'
    ]
]);
```

---

## 示例 5：多个规则（OR关系）

```php
<?php
// 跟踪以下任意一个：
// 1. /app/Controllers/ 目录下的所有函数
// 2. /app/Services/ 目录下的所有函数
// 3. 所有以 process 开头的函数
trace_set_callback_whitelist([
    ['file_pattern' => '/app/Controllers/*'],
    ['file_pattern' => '/app/Services/*'],
    ['function_pattern' => 'process*']
]);
```

---

## 示例 6：复杂场景

```php
<?php
// 跟踪以下任意一个：
// 1. UserController 类的所有方法
// 2. /app/Services/ 目录下，以 create 或 update 开头的函数
// 3. 任何类的 execute 方法
trace_set_callback_whitelist([
    [
        'class_pattern' => '*UserController'
    ],
    [
        'file_pattern' => '/app/Services/*',
        'function_pattern' => 'create*'
    ],
    [
        'file_pattern' => '/app/Services/*',
        'function_pattern' => 'update*'
    ],
    [
        'function_pattern' => 'execute'
    ]
]);
```

---

## 示例 7：排除vendor目录

```php
<?php
// 跟踪所有非 vendor 目录的文件
// 注意：* 不能用于排除，需要使用多个规则
trace_set_callback_whitelist([
    ['file_pattern' => '/app/*'],
    ['file_pattern' => '/src/*'],
    ['file_pattern' => '/lib/*']
]);
```

---

## 示例 8：只跟踪特定文件的特定类

```php
<?php
// 只跟踪 UserController.php 文件中的 UserController 类
trace_set_callback_whitelist([
    [
        'file_pattern' => '*/UserController.php',
        'class_pattern' => '*UserController'
    ]
]);
```

---

## 通配符 `*` 的使用

- `*` 可以出现在模式的任何位置
- `*` 匹配零个或多个任意字符
- 支持多个 `*`

**示例：**
```php
'file_pattern' => '/app/*/Controller.php'     // 匹配 /app/User/Controller.php
'class_pattern' => 'App\\*\\Controller'       // 匹配 App\Admin\Controller
'function_pattern' => '*Action'               // 匹配 indexAction, createAction
'function_pattern' => 'get*Info'              // 匹配 getUserInfo, getOrderInfo
```

---

## 完整示例

```php
<?php

// 初始化trace
trace_set_callback('function_enter', function($func, $class, $file, $line, $parentSpanId, $argCount) {
    return [
        'operation_name' => $class ? "$class::$func" : $func,
        'tags' => [
            'function' => $func,
            'class' => $class,
            'file' => basename($file),
        ]
    ];
});

// 设置白名单：只跟踪业务代码
trace_set_callback_whitelist([
    // Controller层
    [
        'file_pattern' => '/app/Controllers/*',
        'class_pattern' => '*Controller'
    ],
    // Service层
    [
        'file_pattern' => '/app/Services/*',
        'class_pattern' => '*Service'
    ],
    // Repository层的查询方法
    [
        'file_pattern' => '/app/Repositories/*',
        'function_pattern' => 'find*'
    ],
    [
        'file_pattern' => '/app/Repositories/*',
        'function_pattern' => 'get*'
    ]
]);
```

---

## 性能建议

1. **越精确越好** - 尽量使用具体的模式，减少不必要的匹配
2. **优先文件过滤** - `file_pattern` 通常是最快的过滤条件
3. **避免过度通配** - 少用单独的 `*`，会匹配所有内容
4. **合理分层** - 按照应用分层设置规则，提高可维护性

---

## 注意事项

⚠️ **内部函数和trace_*函数会自动跳过**，无需在白名单中排除

⚠️ **如果没有设置白名单，将不会跟踪任何函数**

⚠️ **空数组 `[]` 等同于不跟踪任何函数**

