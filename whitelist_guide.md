# 白名单规则完全指南

## 支持的15种匹配规则

### 函数名规则（5种）

```php
// 1. 精确匹配
['function' => 'user_login']
// 只匹配: user_login

// 2. 前缀匹配
['function_prefix' => 'api_']
// 匹配: api_get, api_post, api_delete等

// 3. 后缀匹配
['function_suffix' => '_handler']
// 匹配: login_handler, logout_handler等

// 4. 包含匹配
['function_contains' => 'process']
// 匹配: process_data, data_process, pre_process等

// 5. 不包含匹配
['function_not_contains' => 'test']
// 排除: test_function, function_test, test等
```

### 类名规则（4种）

```php
// 1. 精确匹配
['class' => 'UserService']
// 只匹配: UserService

// 2. 前缀匹配（命名空间）
['class_prefix' => 'App\\Controller\\']
// 匹配: App\Controller\UserController, App\Controller\OrderController等

// 3. 后缀匹配
['class_suffix' => 'Controller']
// 匹配: UserController, OrderController等

// 4. 包含匹配
['class_contains' => 'Service']
// 匹配: UserService, OrderService, ServiceProvider等
```

### 文件路径规则（5种）

```php
// 1. 精确匹配
['file' => '/var/www/app/user.php']
// 只匹配: /var/www/app/user.php

// 2. 前缀匹配（目录）
['file_prefix' => '/var/www/app/controllers/']
// 匹配: 该目录下所有文件

// 3. 后缀匹配（文件扩展名）
['file_suffix' => '.controller.php']
// 匹配: user.controller.php, order.controller.php等

// 4. 包含匹配
['file_contains' => '/services/']
// 匹配: 路径中包含/services/的所有文件

// 5. 不包含匹配
['file_not_contains' => '/vendor/']
// 排除: vendor目录下的所有文件
```

## 规则组合

### AND关系（同一规则内）

```php
// 只跟踪: UserService类的handle开头方法，且不在vendor目录
trace_set_callback_whitelist([
    [
        'class' => 'UserService',
        'function_prefix' => 'handle',
        'file_not_contains' => '/vendor/'
    ]
]);
```

所有条件必须**同时满足**。

### OR关系（多个规则）

```php
// 跟踪: UserService类 或 api_开头的函数 或 /controllers/目录
trace_set_callback_whitelist([
    ['class' => 'UserService'],
    ['function_prefix' => 'api_'],
    ['file_contains' => '/controllers/']
]);
```

满足**任一规则**即跟踪。

## 实际场景示例

### 场景1: 只跟踪业务代码，排除框架和vendor

```php
trace_set_callback_whitelist([
    [
        'file_prefix' => '/var/www/app/',
        'file_not_contains' => '/vendor/',
        'file_not_contains' => '/framework/'
    ]
]);
```

### 场景2: 只跟踪API接口

```php
trace_set_callback_whitelist([
    ['function_prefix' => 'api_'],
    ['class_suffix' => 'ApiController'],
    ['file_contains' => '/api/']
]);
```

### 场景3: 跟踪所有Service层

```php
trace_set_callback_whitelist([
    ['class_suffix' => 'Service'],
    ['file_contains' => '/services/']
]);
```

### 场景4: 排除测试代码

```php
trace_set_callback_whitelist([
    [
        'file_prefix' => '/app/',
        'file_not_contains' => '/tests/',
        'function_not_contains' => 'test_'
    ]
]);
```

### 场景5: 只跟踪关键业务流程

```php
trace_set_callback_whitelist([
    ['function' => 'user_register'],
    ['function' => 'user_login'],
    ['function' => 'create_order'],
    ['function' => 'process_payment'],
    ['class' => 'PaymentGateway'],
]);
```

## 性能优化建议

1. **使用前缀匹配而不是包含匹配** - 前缀匹配更快
2. **优先使用file规则** - 文件级别过滤效率最高
3. **避免过于宽泛的规则** - 会导致跟踪过多函数
4. **使用not_contains排除不需要的部分** - 精确控制范围

## 默认行为

- **不设置白名单** = 跟踪所有用户函数
- **设置空数组** = 不跟踪任何函数
- **内部函数** = 始终不跟踪
- **trace_*函数** = 始终不跟踪（避免递归）
