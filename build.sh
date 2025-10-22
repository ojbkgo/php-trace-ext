#!/bin/bash

echo "🔧 编译PHP全链路跟踪扩展..."

# 清理之前的编译文件
make clean 2>/dev/null || true
phpize --clean 2>/dev/null || true

# 重新配置和编译
phpize
./configure --enable-trace
make

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ 编译成功!"
    echo ""
    echo "🧪 快速测试:"
    echo "php -d extension=./modules/trace.so -r \"echo 'Extension: ' . (extension_loaded('trace') ? 'OK' : 'FAILED') . \\\"\\\n\\\"; echo 'TraceID: ' . trace_get_trace_id() . \\\"\\\n\\\";\""
    echo ""
    echo "📋 完整测试:"
    echo "php -d extension=./modules/trace.so final_implementation.php"
    echo ""
    echo "⚙️ 安装到系统 (可选):"
    echo "sudo make install"
    echo "echo 'extension=trace.so' >> /path/to/php.ini"
else
    echo "❌ 编译失败，请检查错误信息"
    exit 1
fi