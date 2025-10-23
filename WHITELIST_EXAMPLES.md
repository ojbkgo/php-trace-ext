# 白名单配置示例（增强版）

## 基本用法

白名单是一个数组，每项包含三个可选字段：
- `file_pattern` - 文件路径模式（字符串或数组）
- `class_pattern` - 类名模式（字符串或数组）
- `function_pattern` - 函数名模式（字符串或数组）

**规则说明：**
- 每个字段可以是**字符串**或**字符串数组**
- 如果是数组，数组中每项都是 **AND** 关系（都要符合）
- 多个规则之间是 **OR** 关系（符合任意一个即可）
- 每个规则内部的字段是 **AND** 关系（都要符合）
- 支持通配符 `*`
- 支持反向匹配：以 `! ` 开头（感叹号+空格）表示"不匹配"

---

## 示例 1：简单模式（字符串）

```php
<?php
// 跟踪 /app/Controllers/ 目录下的所有函数
trace_set_callback_whitelist([
    ['file_pattern' => '/app/Controllers/*']
]);
```

---

## 示例 2：排除特定目录（反向匹配）

```php
<?php
// 跟踪 /app/ 下但排除 vendor 目录
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/app/*',           // 必须在 /app/ 下
            '! */vendor/*'      // 但不在 vendor 下
        ]
    ]
]);
```

---

## 示例 3：多条件组合（AND关系）

```php
<?php
// 跟踪 Controller 类中以 handle 开头的方法
trace_set_callback_whitelist([
    [
        'class_pattern' => ['*Controller'],
        'function_pattern' => ['handle*']
    ]
]);
```

---

## 示例 4：复杂排除逻辑

```php
<?php
// 跟踪业务代码，但排除测试和内部方法
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/app/*',              // 在 /app/ 下
            '! *Test.php',         // 排除测试文件
            '! */vendor/*'         // 排除vendor
        ],
        'class_pattern' => [
            'App\\*'               // App命名空间
        ],
        'function_pattern' => [
            '*',                   // 任意函数
            '! _*',                // 排除下划线开头（私有方法）
            '! *Internal'          // 排除Internal结尾
        ]
    ]
]);
```

---

## 示例 5：多规则（OR关系）

```php
<?php
// 跟踪以下任意一个：
// 1. Controller 层
// 2. Service 层  
// 3. Repository 的查询方法
trace_set_callback_whitelist([
    // 规则1：Controller
    [
        'file_pattern' => '/app/Controllers/*',
        'class_pattern' => '*Controller'
    ],
    // 规则2：Service
    [
        'file_pattern' => '/app/Services/*',
        'class_pattern' => '*Service'
    ],
    // 规则3：Repository查询
    [
        'file_pattern' => '/app/Repositories/*',
        'function_pattern' => ['find*', 'get*', 'search*']
    ]
]);
```

---

## 示例 6：排除第三方库

```php
<?php
// 只跟踪自己的代码，排除所有第三方库
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/var/www/*',          // 项目根目录
            '! */vendor/*',        // 排除composer包
            '! */node_modules/*',  // 排除npm包
            '! */cache/*'          // 排除缓存
        ]
    ]
]);
```

---

## 示例 7：特定命名空间，排除特定方法

```php
<?php
// 跟踪 App\Http 命名空间，但排除getter/setter
trace_set_callback_whitelist([
    [
        'class_pattern' => 'App\\Http\\*',
        'function_pattern' => [
            '*',              // 所有方法
            '! get*',         // 排除getter
            '! set*',         // 排除setter
            '! __*'           // 排除魔术方法
        ]
    ]
]);
```

---

## 示例 8：按文件类型过滤

```php
<?php
// 只跟踪 Controller 和 Service 文件
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/app/*',
            '*Controller.php',
            '*Service.php'
        ]
    ]
]);

// 或者分开写（OR关系）
trace_set_callback_whitelist([
    ['file_pattern' => '/app/*Controller.php'],
    ['file_pattern' => '/app/*Service.php']
]);
```

---

## 示例 9：白名单 + 黑名单组合

```php
<?php
// 跟踪所有业务代码，但排除特定模块
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/app/*',
            '! */vendor/*',
            '! */Debug/*',         // 排除调试模块
            '! */Legacy/*'         // 排除遗留代码
        ],
        'class_pattern' => [
            'App\\*',
            '! *Debug*',
            '! *Test*'
        ]
    ]
]);
```

---

## 示例 10：按功能分层跟踪

```php
<?php
// 精确控制各层的跟踪
trace_set_callback_whitelist([
    // HTTP层：所有请求处理
    [
        'file_pattern' => '/app/Http/*',
        'class_pattern' => 'App\\Http\\Controllers\\*'
    ],
    // 业务层：排除简单的get/set
    [
        'file_pattern' => '/app/Services/*',
        'function_pattern' => [
            '*',
            '! get*',
            '! set*',
            '! is*'
        ]
    ],
    // 数据层：只跟踪复杂查询
    [
        'file_pattern' => '/app/Repositories/*',
        'function_pattern' => [
            '*',
            '! find',              // 排除简单的find
            '! findBy*',           // 排除简单的findBy
            '! save',              // 排除save
            '! delete'             // 排除delete
        ]
    ]
]);
```

---

## 反向匹配详解

### 语法
```php
'! pattern'  // 感叹号 + 空格 + 模式
```

### 工作原理
- **正向匹配**：`/app/*` - 必须匹配这个模式
- **反向匹配**：`! */vendor/*` - 不能匹配这个模式

### 组合使用
```php
[
    'file_pattern' => [
        '/app/*',           // ✅ 必须匹配：在 /app/ 下
        '! */Test.php',     // ❌ 不能匹配：不是测试文件
        '! */vendor/*'      // ❌ 不能匹配：不在 vendor 下
    ]
]
```

只有**同时满足所有条件**才会跟踪。

---

## 通配符 `*` 的使用

| 模式 | 匹配 | 不匹配 |
|------|------|--------|
| `App\\*` | `App\\User`, `App\\Controller\\User` | `Root\\App` |
| `*Controller` | `UserController`, `App\\UserController` | `ControllerBase` |
| `handle*` | `handle`, `handleRequest` | `myHandle` |
| `/app/*/test.php` | `/app/admin/test.php` | `/app/test.php` |
| `*` | 任何内容 | - |

---

## 完整实战示例

```php
<?php

// 设置回调
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

// 设置白名单：只跟踪核心业务代码
trace_set_callback_whitelist([
    // HTTP层
    [
        'file_pattern' => [
            '/var/www/app/Http/*',
            '! */Middleware/*'        // 排除中间件
        ],
        'class_pattern' => [
            'App\\Http\\Controllers\\*',
            '! *AbstractController'   // 排除抽象类
        ]
    ],
    
    // 业务逻辑层
    [
        'file_pattern' => [
            '/var/www/app/Services/*',
            '! */vendor/*'
        ],
        'class_pattern' => 'App\\Services\\*',
        'function_pattern' => [
            '*',
            '! get*',                 // 排除getter
            '! set*',                 // 排除setter
            '! __*'                   // 排除魔术方法
        ]
    ],
    
    // 数据访问层：只跟踪复杂操作
    [
        'file_pattern' => '/var/www/app/Repositories/*',
        'function_pattern' => [
            '*Query',                 // 复杂查询
            '*Report',                // 报表
            'bulk*'                   // 批量操作
        ]
    ]
]);
```

---

## 性能优化建议

### ✅ 推荐做法

1. **越具体越好**
```php
// 好：明确的路径
['file_pattern' => '/app/Controllers/UserController.php']

// 差：过于宽泛
['file_pattern' => '*']
```

2. **优先使用 file_pattern**
```php
// 好：文件过滤最快
['file_pattern' => '/app/Controllers/*']

// 可以：但比文件过滤慢
['class_pattern' => 'App\\Controllers\\*']
```

3. **合理使用反向匹配**
```php
// 好：先正向过滤，再排除
[
    'file_pattern' => [
        '/app/*',
        '! */vendor/*'
    ]
]

// 差：全部反向，效率低
[
    'file_pattern' => [
        '! */a/*',
        '! */b/*',
        '! */c/*'
    ]
]
```

### ⚠️ 注意事项

- 每个函数调用都会执行过滤检查，保持白名单简洁
- 数组越长，检查越慢，合理组织规则
- 避免过于复杂的嵌套模式

---

## 调试技巧

### 测试白名单
```php
<?php
// 临时设置宽松的规则，看看是否生效
trace_set_callback_whitelist([
    ['file_pattern' => '*']  // 跟踪所有，用于调试
]);
```

### 逐步收窄
```php
<?php
// 第一步：跟踪整个目录
trace_set_callback_whitelist([
    ['file_pattern' => '/app/*']
]);

// 第二步：排除不需要的
trace_set_callback_whitelist([
    [
        'file_pattern' => [
            '/app/*',
            '! */vendor/*'
        ]
    ]
]);

// 第三步：进一步精确
trace_set_callback_whitelist([
    [
        'file_pattern' => ['/app/*', '! */vendor/*'],
        'class_pattern' => 'App\\*'
    ]
]);
```

---

## 总结

| 特性 | 说明 | 示例 |
|------|------|------|
| 字符串模式 | 单个匹配条件 | `'file_pattern' => '/app/*'` |
| 数组模式 | 多个AND条件 | `'file_pattern' => ['/app/*', '! */vendor/*']` |
| 通配符 | `*` 匹配任意字符 | `'class_pattern' => 'App\\*'` |
| 反向匹配 | `! ` 开头表示排除 | `'! */Test.php'` |
| 规则OR | 多个规则符合一个即可 | `[{rule1}, {rule2}]` |
| 字段AND | 规则内字段都要符合 | `{file: x, class: y}` |

**记住：灵活组合这些特性，可以实现非常精确的追踪控制！** 🎯
