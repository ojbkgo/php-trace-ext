<?php

echo "=== PHP全链路跟踪扩展 - 完整功能测试 ===\n\n";

if (!extension_loaded('trace')) {
    die("错误: trace扩展未加载\n");
}

echo "1. TraceID: " . trace_get_trace_id() . "\n\n";

echo "2. 设置白名单:\n";
trace_set_callback_whitelist([
    ['class_contains' => 'OrderProcessor'],  // 注意：需要是数组的数组
]);
echo "   白名单设置完成\n\n";

echo "3. 设置回调（支持返回tags）:\n";
trace_set_callback('function_enter', function($func, $class, $file, $line, $parent, $args) {
    echo "  → {$func}" . ($class ? "::{$class}" : "") . "\n";
    
    // 返回operation_name和tags
    return [
        'operation_name' => $func,
        'tags' => [
            'function_name' => $func,
            'class_name' => $class ?: '',
            'file' => basename($file ?: ''),
            'line' => (string)$line
        ]
    ];
});

trace_set_callback('function_exit', function($span_id, $duration, $rv) {
    echo "  ← 耗时: " . number_format($duration * 1000, 2) . "ms\n";
});
echo "   回调设置完成\n\n";

echo "4. 类方法调用测试:\n";

class OrderProcessor {
    private $orderId;
    private $items = [];
    
    public function __construct($orderId) {
        $this->orderId = $orderId;
        trace_add_tag('order_id', $orderId);
        trace_add_log('info', '订单处理器初始化');
    }
    
    public function addItem($name, $price, $quantity) {
        $this->items[] = [
            'name' => $name,
            'price' => $price,
            'quantity' => $quantity
        ];
        
        trace_add_tag('item_count', count($this->items));
        trace_add_log('debug', "添加商品: {$name}");
        
        return $this;
    }
    
    public function calculateTotal() {
        trace_add_log('info', '计算订单总价');
        $total = 0;
        
        foreach ($this->items as $item) {
            $itemTotal = $this->calculateItemPrice($item);
            $total += $itemTotal;
        }
        
        trace_add_tag('total_amount', $total);
        return $total;
    }
    
    private function calculateItemPrice($item) {
        trace_add_log('debug', "计算商品价格: {$item['name']}");
        $discount = $this->applyDiscount($item);
        return $item['price'] * $item['quantity'] * (1 - $discount);
    }
    
    private function applyDiscount($item) {
        trace_add_tag('discount_applied', 'true');
        
        // 模拟一些处理时间
        usleep(10000);
        
        // 根据数量提供折扣
        if ($item['quantity'] >= 10) {
            trace_add_log('info', "大量购买折扣: {$item['name']}");
            return 0.15; // 15% 折扣
        } elseif ($item['quantity'] >= 5) {
            trace_add_log('info', "中量购买折扣: {$item['name']}");
            return 0.10; // 10% 折扣
        } else {
            trace_add_log('info', "无折扣: {$item['name']}");
            return 0;
        }
    }
    
    public function processOrder() {
        trace_add_log('info', "开始处理订单: {$this->orderId}");
        
        $total = $this->calculateTotal();
        $tax = $this->calculateTax($total);
        $finalTotal = $total + $tax;
        
        $result = $this->finalizeOrder($finalTotal);
        
        trace_add_tag('final_total', $finalTotal);
        trace_add_log('info', "订单处理完成: {$this->orderId}");
        
        return $result;
    }
    
    private function calculateTax($amount) {
        trace_add_log('debug', "计算税费");
        usleep(5000);
        $taxRate = 0.08; // 8% 税率
        return $amount * $taxRate;
    }
    
    private function finalizeOrder($total) {
        trace_add_log('info', "完成订单");
        usleep(15000);
        return [
            'order_id' => $this->orderId,
            'items' => count($this->items),
            'total' => $total,
            'status' => 'completed'
        ];
    }
}

// 创建订单并处理
$processor = new OrderProcessor('ORD-12345');
$processor->addItem('笔记本电脑', 5000, 1)
          ->addItem('鼠标', 100, 5)
          ->addItem('键盘', 200, 3);

$result = $processor->processOrder();
echo "  订单结果: " . json_encode($result, JSON_UNESCAPED_UNICODE) . "\n\n";

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
