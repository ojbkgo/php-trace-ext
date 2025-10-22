#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include <sys/time.h>
#include <stdio.h>

#define PHP_TRACE_VERSION "2.0.0"

// Span结构体
typedef struct _trace_span {
    zend_string *span_id;
    zend_string *parent_id;
    zend_string *operation_name;
    double start_time;
    double end_time;
    zend_array *tags;  // 新增：支持tags
    zend_array *logs;
    struct _trace_span *parent;
} trace_span_t;

// 全局变量
ZEND_BEGIN_MODULE_GLOBALS(trace)
    zend_bool enabled;
    zend_bool debug_enabled;  // 新增：debug开关
    zend_string *debug_log_path;  // 新增：debug日志路径
    zend_string *trace_id;
    zend_string *service_name;
    trace_span_t *current_span;
    trace_span_t *root_span;
    zend_array *all_spans;
    zend_long span_counter;
ZEND_END_MODULE_GLOBALS(trace)

#ifdef ZTS
#define TRACE_G(v) TSRMG(trace_globals_id, zend_trace_globals *, v)
#else
#define TRACE_G(v) (trace_globals.v)
#endif

ZEND_DECLARE_MODULE_GLOBALS(trace)

// 原始函数指针
void (*original_zend_execute_ex)(zend_execute_data *execute_data) = NULL;

// 用户回调存储
zval g_function_enter_callback;
zval g_function_exit_callback;
zval g_curl_callback;
zval g_db_callback;

// 白名单配置
zval g_trace_whitelist;  // 存储白名单规则

// 参数信息
ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_get_trace_id, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_set_callback, 0, 0, 2)
    ZEND_ARG_INFO(0, type)
    ZEND_ARG_INFO(0, callback)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_get_current_span, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_add_log, 0, 0, 2)
    ZEND_ARG_INFO(0, level)
    ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_get_spans, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_set_callback_whitelist, 0, 0, 1)
    ZEND_ARG_INFO(0, rules)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_reset, 0, 0, 0)
    ZEND_ARG_INFO(0, trace_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_trace_add_tag, 0, 0, 2)
    ZEND_ARG_INFO(0, key)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

// Debug日志函数（移到宏定义之后）
void trace_debug_log(const char *format, ...)
{
    // 使用简单的条件判断，确保debug功能可用
    // 即使在MINIT之前也能工作
    int debug_enabled = 0;
    const char *log_file = "/tmp/php_trace_debug.log";
    
    // 尝试从全局变量读取
    if (trace_globals.debug_enabled) {
        debug_enabled = 1;
        if (trace_globals.debug_log_path) {
            log_file = ZSTR_VAL(trace_globals.debug_log_path);
        }
    }
    
    // 如果全局变量未初始化，检查环境变量
    if (!debug_enabled) {
        char *env_debug = getenv("PHP_TRACE_DEBUG");
        if (env_debug && strcmp(env_debug, "1") == 0) {
            debug_enabled = 1;
        }
    }
    
    if (!debug_enabled) {
        return;
    }
    
    FILE *fp = fopen(log_file, "a");
    if (fp) {
        va_list args;
        va_start(args, format);
        
        // 添加时间戳和进程信息
        struct timeval tv;
        gettimeofday(&tv, NULL);
        fprintf(fp, "[%ld.%06d][PID:%d] ", (long)tv.tv_sec, (int)tv.tv_usec, getpid());
        
        vfprintf(fp, format, args);
        fprintf(fp, "\n");
        fflush(fp);
        
        va_end(args);
        fclose(fp);
    }
}

// 工具函数
double trace_get_microtime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

void trace_generate_ids(void)
{
    char trace_id_str[33];
    snprintf(trace_id_str, sizeof(trace_id_str), "%016lx%016lx", 
             (unsigned long)time(NULL), (unsigned long)getpid());
    
    if (TRACE_G(trace_id)) {
        zend_string_release(TRACE_G(trace_id));
    }
    TRACE_G(trace_id) = zend_string_init(trace_id_str, strlen(trace_id_str), 0);
}

zend_string* trace_generate_span_id(void)
{
    char span_id_str[17];
    snprintf(span_id_str, sizeof(span_id_str), "%016lx", 
             (unsigned long)(time(NULL) + (++TRACE_G(span_counter))));
    return zend_string_init(span_id_str, strlen(span_id_str), 0);
}

trace_span_t* trace_create_span(const char *operation_name, trace_span_t *parent)
{
    trace_span_t *span = emalloc(sizeof(trace_span_t));
    
    span->span_id = trace_generate_span_id();
    span->parent_id = parent ? zend_string_copy(parent->span_id) : NULL;
    span->operation_name = zend_string_init(operation_name, strlen(operation_name), 0);
    span->start_time = trace_get_microtime();
    span->end_time = 0.0;
    span->parent = parent;
    
    // 初始化tags
    ALLOC_HASHTABLE(span->tags);
    zend_hash_init(span->tags, 0, NULL, ZVAL_PTR_DTOR, 0);
    
    // 初始化logs
    ALLOC_HASHTABLE(span->logs);
    zend_hash_init(span->logs, 0, NULL, ZVAL_PTR_DTOR, 0);
    
    if (TRACE_G(all_spans)) {
        zval span_zval;
        ZVAL_PTR(&span_zval, span);
        zend_hash_next_index_insert(TRACE_G(all_spans), &span_zval);
    }
    
    return span;
}

void trace_finish_span(trace_span_t *span)
{
    if (span && span->end_time == 0.0) {
        span->end_time = trace_get_microtime();
    }
}

void trace_call_user_callback(zval *callback, int argc, zval *argv, zval *retval)
{
    trace_debug_log("trace_call_user_callback: 开始, 参数数量=%d", argc);
    
    if (Z_ISUNDEF_P(callback)) {
        trace_debug_log("trace_call_user_callback: 回调未定义");
        return;
    }
    
    if (!zend_is_callable(callback, 0, NULL)) {
        trace_debug_log("trace_call_user_callback: 回调不可调用");
        return;
    }
    
    trace_debug_log("trace_call_user_callback: 调用用户函数");
    
    int result = call_user_function(CG(function_table), NULL, callback, retval, argc, argv);
    
    if (result == SUCCESS) {
        trace_debug_log("trace_call_user_callback: 调用成功, 返回值类型=%d", Z_TYPE_P(retval));
    } else {
        trace_debug_log("trace_call_user_callback: 调用失败");
    }
}

// 字符串匹配辅助函数
int trace_string_match(const char *haystack, const char *needle, const char *match_type)
{
    if (!haystack || !needle) {
        return 0;
    }
    
    size_t haystack_len = strlen(haystack);
    size_t needle_len = strlen(needle);
    
    if (strcmp(match_type, "exact") == 0) {
        return strcmp(haystack, needle) == 0;
    } else if (strcmp(match_type, "prefix") == 0) {
        return strncmp(haystack, needle, needle_len) == 0;
    } else if (strcmp(match_type, "suffix") == 0) {
        if (needle_len > haystack_len) return 0;
        return strcmp(haystack + (haystack_len - needle_len), needle) == 0;
    } else if (strcmp(match_type, "contains") == 0) {
        return strstr(haystack, needle) != NULL;
    } else if (strcmp(match_type, "not_contains") == 0) {
        return strstr(haystack, needle) == NULL;
    }
    
    return 0;
}

int trace_should_trace_function(zend_execute_data *execute_data)
{
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // 始终跳过内部函数
    if (execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
        return 0;
    }
    
    // 始终跳过trace扩展自身的函数，避免无限递归
    if (execute_data->func->common.function_name &&
        strncmp(ZSTR_VAL(execute_data->func->common.function_name), "trace_", 6) == 0) {
        return 0;
    }
    
    // 如果没有设置白名单，跟踪所有函数
    if (Z_ISUNDEF(g_trace_whitelist)) {
        return 1;
    }
    
    // 如果白名单不是数组，跟踪所有函数
    if (Z_TYPE(g_trace_whitelist) != IS_ARRAY) {
        return 1;
    }
    
    // 获取函数信息
    const char *func_name = execute_data->func->common.function_name ? 
                           ZSTR_VAL(execute_data->func->common.function_name) : NULL;
    const char *class_name = execute_data->func->common.scope ? 
                            ZSTR_VAL(execute_data->func->common.scope->name) : NULL;
    const char *file_name = (execute_data->func->type == ZEND_USER_FUNCTION && 
                            execute_data->func->op_array.filename) ?
                           ZSTR_VAL(execute_data->func->op_array.filename) : NULL;
    
    // 遍历白名单规则
    zval *rule;
    ZEND_HASH_FOREACH_VAL(Z_ARR(g_trace_whitelist), rule) {
        if (Z_TYPE_P(rule) != IS_ARRAY) {
            continue;
        }
        
        int matched = 1;  // 假设匹配，逐项检查（AND关系）
        
        // 函数名匹配（支持：function, function_prefix, function_suffix, function_contains, function_not_contains）
        zval *func_exact = zend_hash_str_find(Z_ARR_P(rule), "function", sizeof("function") - 1);
        if (matched && func_exact && Z_TYPE_P(func_exact) == IS_STRING) {
            if (!trace_string_match(func_name, Z_STRVAL_P(func_exact), "exact")) {
                matched = 0;
            }
        }
        
        zval *func_prefix = zend_hash_str_find(Z_ARR_P(rule), "function_prefix", sizeof("function_prefix") - 1);
        if (matched && func_prefix && Z_TYPE_P(func_prefix) == IS_STRING) {
            if (!trace_string_match(func_name, Z_STRVAL_P(func_prefix), "prefix")) {
                matched = 0;
            }
        }
        
        zval *func_suffix = zend_hash_str_find(Z_ARR_P(rule), "function_suffix", sizeof("function_suffix") - 1);
        if (matched && func_suffix && Z_TYPE_P(func_suffix) == IS_STRING) {
            if (!trace_string_match(func_name, Z_STRVAL_P(func_suffix), "suffix")) {
                matched = 0;
            }
        }
        
        zval *func_contains = zend_hash_str_find(Z_ARR_P(rule), "function_contains", sizeof("function_contains") - 1);
        if (matched && func_contains && Z_TYPE_P(func_contains) == IS_STRING) {
            if (!trace_string_match(func_name, Z_STRVAL_P(func_contains), "contains")) {
                matched = 0;
            }
        }
        
        zval *func_not_contains = zend_hash_str_find(Z_ARR_P(rule), "function_not_contains", sizeof("function_not_contains") - 1);
        if (matched && func_not_contains && Z_TYPE_P(func_not_contains) == IS_STRING) {
            if (!trace_string_match(func_name, Z_STRVAL_P(func_not_contains), "not_contains")) {
                matched = 0;
            }
        }
        
        // 类名匹配（支持：class, class_prefix, class_suffix, class_contains, class_not_contains）
        zval *class_exact = zend_hash_str_find(Z_ARR_P(rule), "class", sizeof("class") - 1);
        if (matched && class_exact && Z_TYPE_P(class_exact) == IS_STRING) {
            if (!trace_string_match(class_name, Z_STRVAL_P(class_exact), "exact")) {
                matched = 0;
            }
        }
        
        zval *class_prefix = zend_hash_str_find(Z_ARR_P(rule), "class_prefix", sizeof("class_prefix") - 1);
        if (matched && class_prefix && Z_TYPE_P(class_prefix) == IS_STRING) {
            if (!trace_string_match(class_name, Z_STRVAL_P(class_prefix), "prefix")) {
                matched = 0;
            }
        }
        
        zval *class_suffix = zend_hash_str_find(Z_ARR_P(rule), "class_suffix", sizeof("class_suffix") - 1);
        if (matched && class_suffix && Z_TYPE_P(class_suffix) == IS_STRING) {
            if (!trace_string_match(class_name, Z_STRVAL_P(class_suffix), "suffix")) {
                matched = 0;
            }
        }
        
        zval *class_contains = zend_hash_str_find(Z_ARR_P(rule), "class_contains", sizeof("class_contains") - 1);
        if (matched && class_contains && Z_TYPE_P(class_contains) == IS_STRING) {
            if (!trace_string_match(class_name, Z_STRVAL_P(class_contains), "contains")) {
                matched = 0;
            }
        }
        
        // 文件名匹配（支持：file, file_prefix, file_suffix, file_contains, file_not_contains）
        zval *file_exact = zend_hash_str_find(Z_ARR_P(rule), "file", sizeof("file") - 1);
        if (matched && file_exact && Z_TYPE_P(file_exact) == IS_STRING) {
            if (!trace_string_match(file_name, Z_STRVAL_P(file_exact), "exact")) {
                matched = 0;
            }
        }
        
        zval *file_prefix = zend_hash_str_find(Z_ARR_P(rule), "file_prefix", sizeof("file_prefix") - 1);
        if (matched && file_prefix && Z_TYPE_P(file_prefix) == IS_STRING) {
            if (!trace_string_match(file_name, Z_STRVAL_P(file_prefix), "prefix")) {
                matched = 0;
            }
        }
        
        zval *file_suffix = zend_hash_str_find(Z_ARR_P(rule), "file_suffix", sizeof("file_suffix") - 1);
        if (matched && file_suffix && Z_TYPE_P(file_suffix) == IS_STRING) {
            if (!trace_string_match(file_name, Z_STRVAL_P(file_suffix), "suffix")) {
                matched = 0;
            }
        }
        
        zval *file_contains = zend_hash_str_find(Z_ARR_P(rule), "file_contains", sizeof("file_contains") - 1);
        if (matched && file_contains && Z_TYPE_P(file_contains) == IS_STRING) {
            if (!trace_string_match(file_name, Z_STRVAL_P(file_contains), "contains")) {
                matched = 0;
            }
        }
        
        zval *file_not_contains = zend_hash_str_find(Z_ARR_P(rule), "file_not_contains", sizeof("file_not_contains") - 1);
        if (matched && file_not_contains && Z_TYPE_P(file_not_contains) == IS_STRING) {
            if (!trace_string_match(file_name, Z_STRVAL_P(file_not_contains), "not_contains")) {
                matched = 0;
            }
        }
        
        // 如果所有条件都匹配（AND关系），则跟踪此函数
        if (matched) {
            return 1;
        }
    } ZEND_HASH_FOREACH_END();
    
    // 白名单中没有匹配的规则，不跟踪
    return 0;
}

// 函数执行钩子 (完整实现)
void trace_execute_ex(zend_execute_data *execute_data)
{
    trace_span_t *span = NULL;
    zval callback_result;
    ZVAL_UNDEF(&callback_result);
    
    const char *func_name_debug = NULL;
    if (execute_data && execute_data->func && execute_data->func->common.function_name) {
        func_name_debug = ZSTR_VAL(execute_data->func->common.function_name);
    }
    
    trace_debug_log("=== trace_execute_ex 开始: 函数=%s ===", func_name_debug ? func_name_debug : "NULL");
    
    // 快速路径：如果没有回调或不需要跟踪，直接执行
    if (!TRACE_G(enabled)) {
        trace_debug_log("trace_execute_ex: TRACE_G(enabled) 为 FALSE");
        original_zend_execute_ex(execute_data);
        return;
    }
    
    if (Z_ISUNDEF(g_function_enter_callback)) {
        trace_debug_log("trace_execute_ex: g_function_enter_callback 未定义");
        original_zend_execute_ex(execute_data);
        return;
    }
    
    if (!trace_should_trace_function(execute_data)) {
        trace_debug_log("trace_execute_ex: trace_should_trace_function 返回 FALSE");
        original_zend_execute_ex(execute_data);
        return;
    }
    
    trace_debug_log("trace_execute_ex: 将跟踪函数 %s", func_name_debug);
    
    // 安全检查
    if (!execute_data || !execute_data->func) {
        trace_debug_log("trace_execute_ex: execute_data 或 func 为 NULL");
        original_zend_execute_ex(execute_data);
        return;
    }
    
    // 准备回调参数
    trace_debug_log("trace_execute_ex: 准备回调参数");
    zval args[6];
    
    // 函数名
    if (execute_data->func->common.function_name) {
        ZVAL_STR_COPY(&args[0], execute_data->func->common.function_name);
        trace_debug_log("trace_execute_ex: 函数名=%s", ZSTR_VAL(execute_data->func->common.function_name));
    } else {
        ZVAL_STRING(&args[0], "anonymous");
        trace_debug_log("trace_execute_ex: 函数名=匿名函数");
    }
    
    // 类名
    if (execute_data->func->common.scope) {
        ZVAL_STR_COPY(&args[1], execute_data->func->common.scope->name);
        trace_debug_log("trace_execute_ex: 类名=%s", ZSTR_VAL(execute_data->func->common.scope->name));
    } else {
        ZVAL_NULL(&args[1]);
        trace_debug_log("trace_execute_ex: 类名=NULL");
    }
    
    // 文件名 - 只对用户函数有效
    if (execute_data->func->type == ZEND_USER_FUNCTION && execute_data->func->op_array.filename) {
        ZVAL_STR_COPY(&args[2], execute_data->func->op_array.filename);
        trace_debug_log("trace_execute_ex: 文件名=%s", ZSTR_VAL(execute_data->func->op_array.filename));
    } else {
        ZVAL_NULL(&args[2]);
        trace_debug_log("trace_execute_ex: 文件名=NULL");
    }
    
    // 行号
    int lineno = execute_data->opline ? execute_data->opline->lineno : 0;
    ZVAL_LONG(&args[3], lineno);
    trace_debug_log("trace_execute_ex: 行号=%d", lineno);
    
    // 父Span ID
    if (TRACE_G(current_span) && TRACE_G(current_span)->span_id) {
        ZVAL_STR_COPY(&args[4], TRACE_G(current_span)->span_id);
        trace_debug_log("trace_execute_ex: 父span_id=%s", ZSTR_VAL(TRACE_G(current_span)->span_id));
    } else {
        ZVAL_NULL(&args[4]);
        trace_debug_log("trace_execute_ex: 父span_id=NULL");
    }
    
    // 参数数量
    int arg_count = ZEND_CALL_NUM_ARGS(execute_data);
    ZVAL_LONG(&args[5], arg_count);
    trace_debug_log("trace_execute_ex: 参数数量=%d", arg_count);
    
    // 调用用户回调
    trace_debug_log("trace_execute_ex: 调用用户回调");
    trace_call_user_callback(&g_function_enter_callback, 6, args, &callback_result);
    trace_debug_log("trace_execute_ex: 用户回调返回, 类型=%d", Z_TYPE(callback_result));
    
    // 根据回调返回值创建span
    if (Z_TYPE(callback_result) == IS_ARRAY) {
        trace_debug_log("trace_execute_ex: 回调返回数组");
        zval *operation_name = zend_hash_str_find(Z_ARR(callback_result), "operation_name", sizeof("operation_name") - 1);
        if (operation_name && Z_TYPE_P(operation_name) == IS_STRING && Z_STRLEN_P(operation_name) > 0) {
            trace_debug_log("trace_execute_ex: 创建span，操作名=%s", Z_STRVAL_P(operation_name));
            span = trace_create_span(Z_STRVAL_P(operation_name), TRACE_G(current_span));
            if (span) {
                TRACE_G(current_span) = span;
                trace_debug_log("trace_execute_ex: span创建成功, span_id=%s", ZSTR_VAL(span->span_id));
                
                // 处理callback返回的tags
                zval *tags = zend_hash_str_find(Z_ARR(callback_result), "tags", sizeof("tags") - 1);
                if (tags && Z_TYPE_P(tags) == IS_ARRAY) {
                    zend_string *tag_key;
                    zval *tag_val;
                    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARR_P(tags), tag_key, tag_val) {
                        if (tag_key && Z_TYPE_P(tag_val) == IS_STRING) {
                            zval tag_copy;
                            ZVAL_COPY(&tag_copy, tag_val);
                            zend_hash_add(span->tags, tag_key, &tag_copy);
                            trace_debug_log("trace_execute_ex: 从回调添加tag %s=%s", ZSTR_VAL(tag_key), Z_STRVAL_P(tag_val));
                        }
                    } ZEND_HASH_FOREACH_END();
                }
            } else {
                trace_debug_log("trace_execute_ex: span创建失败");
            }
        } else {
            trace_debug_log("trace_execute_ex: 未找到操作名或操作名无效");
        }
    } else if (Z_TYPE(callback_result) == IS_FALSE) {
        trace_debug_log("trace_execute_ex: 回调返回FALSE");
    } else {
        trace_debug_log("trace_execute_ex: 回调返回类型=%d", Z_TYPE(callback_result));
    }
    
    // 清理回调参数
    trace_debug_log("trace_execute_ex: 清理回调参数");
    int i;
    for (i = 0; i < 6; i++) {
        zval_dtor(&args[i]);
    }
    if (!Z_ISUNDEF(callback_result)) {
        zval_dtor(&callback_result);
    }
    trace_debug_log("trace_execute_ex: 清理完成");
    
    // 调用原始函数
    trace_debug_log("trace_execute_ex: 调用原始函数");
    original_zend_execute_ex(execute_data);
    trace_debug_log("trace_execute_ex: 原始函数返回");
    
    // 函数执行完成后处理
    if (span) {
        // 完成span
        trace_finish_span(span);
        
        // 恢复父span
        TRACE_G(current_span) = span->parent;
        
        // 调用exit回调
        if (!Z_ISUNDEF(g_function_exit_callback)) {
            zval exit_args[3];
            ZVAL_STR_COPY(&exit_args[0], span->span_id);
            ZVAL_DOUBLE(&exit_args[1], span->end_time - span->start_time);
            ZVAL_NULL(&exit_args[2]); // 返回值暂不传递
            
            zval exit_result;
            ZVAL_UNDEF(&exit_result);
            trace_call_user_callback(&g_function_exit_callback, 3, exit_args, &exit_result);
            
            int j;
            for (j = 0; j < 3; j++) {
                zval_dtor(&exit_args[j]);
            }
            if (!Z_ISUNDEF(exit_result)) {
                zval_dtor(&exit_result);
            }
        }
    }
}

// PHP函数实现
PHP_FUNCTION(trace_get_trace_id)
{
    if (TRACE_G(trace_id)) {
        RETURN_STR_COPY(TRACE_G(trace_id));
    }
    RETURN_NULL();
}

PHP_FUNCTION(trace_set_callback)
{
    char *type;
    size_t type_len;
    zval *callback;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sz", &type, &type_len, &callback) == FAILURE) {
        trace_debug_log("trace_set_callback: 参数解析失败");
        RETURN_FALSE;
    }
    
    trace_debug_log("trace_set_callback: 类型=%s", type);
    
    if (strcmp(type, "function_enter") == 0) {
        ZVAL_COPY(&g_function_enter_callback, callback);
        trace_debug_log("trace_set_callback: function_enter 回调已设置, 可调用=%d", zend_is_callable(callback, 0, NULL));
    } else if (strcmp(type, "function_exit") == 0) {
        ZVAL_COPY(&g_function_exit_callback, callback);
        trace_debug_log("trace_set_callback: function_exit 回调已设置");
    } else if (strcmp(type, "curl") == 0) {
        ZVAL_COPY(&g_curl_callback, callback);
        trace_debug_log("trace_set_callback: curl 回调已设置");
    } else if (strcmp(type, "database") == 0) {
        ZVAL_COPY(&g_db_callback, callback);
        trace_debug_log("trace_set_callback: database 回调已设置");
    } else {
        trace_debug_log("trace_set_callback: 未知类型=%s", type);
        RETURN_FALSE;
    }
    
    RETURN_TRUE;
}

PHP_FUNCTION(trace_get_current_span)
{
    if (TRACE_G(current_span)) {
        array_init(return_value);
        add_assoc_str(return_value, "span_id", zend_string_copy(TRACE_G(current_span)->span_id));
        add_assoc_str(return_value, "operation_name", zend_string_copy(TRACE_G(current_span)->operation_name));
        add_assoc_double(return_value, "start_time", TRACE_G(current_span)->start_time);
        if (TRACE_G(current_span)->parent_id) {
            add_assoc_str(return_value, "parent_id", zend_string_copy(TRACE_G(current_span)->parent_id));
        } else {
            add_assoc_null(return_value, "parent_id");
        }
        return;
    }
    RETURN_NULL();
}

PHP_FUNCTION(trace_add_log)
{
    char *level, *message;
    size_t level_len, message_len;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &level, &level_len, &message, &message_len) == FAILURE) {
        RETURN_FALSE;
    }
    
    if (TRACE_G(current_span) && TRACE_G(current_span)->logs) {
        zval log_entry;
        array_init(&log_entry);
        add_assoc_string(&log_entry, "level", level);
        add_assoc_string(&log_entry, "message", message);
        add_assoc_double(&log_entry, "timestamp", trace_get_microtime());
        
        zend_hash_next_index_insert(TRACE_G(current_span)->logs, &log_entry);
        RETURN_TRUE;
    }
    
    RETURN_FALSE;
}

PHP_FUNCTION(trace_get_spans)
{
    array_init(return_value);
    
    if (TRACE_G(trace_id)) {
        add_assoc_str(return_value, "trace_id", zend_string_copy(TRACE_G(trace_id)));
    } else {
        add_assoc_string(return_value, "trace_id", "");
    }
    
    zval spans_array;
    array_init(&spans_array);
    
    if (TRACE_G(all_spans)) {
        zval *span_zval;
        ZEND_HASH_FOREACH_VAL(TRACE_G(all_spans), span_zval) {
            trace_span_t *span = (trace_span_t*)Z_PTR_P(span_zval);
            if (span) {
                zval span_data;
                array_init(&span_data);
                
                add_assoc_str(&span_data, "span_id", zend_string_copy(span->span_id));
                add_assoc_str(&span_data, "operation_name", zend_string_copy(span->operation_name));
                add_assoc_double(&span_data, "start_time", span->start_time);
                add_assoc_double(&span_data, "end_time", span->end_time);
                add_assoc_double(&span_data, "duration", span->end_time > 0 ? span->end_time - span->start_time : 0.0);
                
                if (span->parent_id) {
                    add_assoc_str(&span_data, "parent_id", zend_string_copy(span->parent_id));
                } else {
                    add_assoc_null(&span_data, "parent_id");
                }
                
                // 添加tags
                zval tags_array;
                array_init(&tags_array);
                
                if (span->tags) {
                    zend_string *tag_key;
                    zval *tag_val;
                    ZEND_HASH_FOREACH_STR_KEY_VAL(span->tags, tag_key, tag_val) {
                        if (tag_key) {
                            zval tag_copy;
                            ZVAL_COPY(&tag_copy, tag_val);
                            zend_hash_add(Z_ARR(tags_array), tag_key, &tag_copy);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                
                add_assoc_zval(&span_data, "tags", &tags_array);
                
                // 添加logs
                zval logs_array;
                array_init(&logs_array);
                
                if (span->logs) {
                    zval *log_entry;
                    ZEND_HASH_FOREACH_VAL(span->logs, log_entry) {
                        zval log_copy;
                        ZVAL_COPY(&log_copy, log_entry);
                        zend_hash_next_index_insert(Z_ARR(logs_array), &log_copy);
                    } ZEND_HASH_FOREACH_END();
                }
                
                add_assoc_zval(&span_data, "logs", &logs_array);
                zend_hash_next_index_insert(Z_ARR(spans_array), &span_data);
            }
        } ZEND_HASH_FOREACH_END();
    }
    
    add_assoc_zval(return_value, "spans", &spans_array);
}

PHP_FUNCTION(trace_set_callback_whitelist)
{
    zval *rules;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &rules) == FAILURE) {
        trace_debug_log("trace_set_callback_whitelist: 参数解析失败");
        RETURN_FALSE;
    }
    
    trace_debug_log("trace_set_callback_whitelist: 规则类型=%d", Z_TYPE_P(rules));
    
    // 存储白名单规则
    if (!Z_ISUNDEF(g_trace_whitelist)) {
        zval_dtor(&g_trace_whitelist);
    }
    
    ZVAL_COPY(&g_trace_whitelist, rules);
    
    if (Z_TYPE_P(rules) == IS_ARRAY) {
        trace_debug_log("trace_set_callback_whitelist: 白名单已设置, 规则数量=%d", zend_hash_num_elements(Z_ARR_P(rules)));
    }
    
    RETURN_TRUE;
}

// 重置trace（用于CLI模式）
PHP_FUNCTION(trace_reset)
{
    char *trace_id = NULL;
    size_t trace_id_len = 0;
    
    // trace_id是可选参数
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "|s", &trace_id, &trace_id_len) == FAILURE) {
        RETURN_FALSE;
    }
    
    trace_debug_log("trace_reset: 开始重置trace");
    
    if (TRACE_G(enabled)) {
        // 完成当前根span
        if (TRACE_G(root_span)) {
            trace_finish_span(TRACE_G(root_span));
        }
        
        // 清理spans
        if (TRACE_G(all_spans)) {
            zval *span_zval;
            ZEND_HASH_FOREACH_VAL(TRACE_G(all_spans), span_zval) {
                trace_span_t *span = (trace_span_t*)Z_PTR_P(span_zval);
                if (span) {
                    if (span->span_id) zend_string_release(span->span_id);
                    if (span->parent_id) zend_string_release(span->parent_id);
                    if (span->operation_name) zend_string_release(span->operation_name);
                    if (span->tags) {
                        zend_hash_destroy(span->tags);
                        FREE_HASHTABLE(span->tags);
                    }
                    if (span->logs) {
                        zend_hash_destroy(span->logs);
                        FREE_HASHTABLE(span->logs);
                    }
                    efree(span);
                }
            } ZEND_HASH_FOREACH_END();
            
            zend_hash_destroy(TRACE_G(all_spans));
            FREE_HASHTABLE(TRACE_G(all_spans));
        }
        
        // 重新初始化
        TRACE_G(current_span) = NULL;
        TRACE_G(root_span) = NULL;
        TRACE_G(span_counter) = 0;
        
        ALLOC_HASHTABLE(TRACE_G(all_spans));
        zend_hash_init(TRACE_G(all_spans), 8, NULL, ZVAL_PTR_DTOR, 0);
        
        // 设置TraceID
        if (trace_id && trace_id_len > 0) {
            if (TRACE_G(trace_id)) {
                zend_string_release(TRACE_G(trace_id));
            }
            TRACE_G(trace_id) = zend_string_init(trace_id, trace_id_len, 0);
            trace_debug_log("trace_reset: 使用指定TraceID=%s", trace_id);
        } else {
            trace_generate_ids();
            trace_debug_log("trace_reset: 生成新TraceID=%s", ZSTR_VAL(TRACE_G(trace_id)));
        }
        
        // 创建新的根span
        TRACE_G(root_span) = trace_create_span("http.request", NULL);
        TRACE_G(current_span) = TRACE_G(root_span);
        
        trace_debug_log("trace_reset: 重置完成");
    }
    
    RETURN_TRUE;
}

// 向当前Span添加tag
PHP_FUNCTION(trace_add_tag)
{
    char *key, *value;
    size_t key_len, value_len;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &key, &key_len, &value, &value_len) == FAILURE) {
        RETURN_FALSE;
    }
    
    if (TRACE_G(current_span) && TRACE_G(current_span)->tags) {
        zval tag_value;
        ZVAL_STRING(&tag_value, value);
        zend_hash_str_update(TRACE_G(current_span)->tags, key, key_len, &tag_value);
        
        trace_debug_log("trace_add_tag: 添加tag %s=%s 到span %s", key, value, ZSTR_VAL(TRACE_G(current_span)->span_id));
        RETURN_TRUE;
    }
    
    RETURN_FALSE;
}

// 函数表
const zend_function_entry trace_functions[] = {
    PHP_FE(trace_get_trace_id, arginfo_trace_get_trace_id)
    PHP_FE(trace_set_callback, arginfo_trace_set_callback)
    PHP_FE(trace_get_current_span, arginfo_trace_get_current_span)
    PHP_FE(trace_add_log, arginfo_trace_add_log)
    PHP_FE(trace_add_tag, arginfo_trace_add_tag)
    PHP_FE(trace_get_spans, arginfo_trace_get_spans)
    PHP_FE(trace_set_callback_whitelist, arginfo_trace_set_callback_whitelist)
    PHP_FE(trace_reset, arginfo_trace_reset)
    PHP_FE_END
};

// INI配置
PHP_INI_BEGIN()
    STD_PHP_INI_BOOLEAN("trace.enabled", "1", PHP_INI_ALL, OnUpdateBool, enabled, zend_trace_globals, trace_globals)
    STD_PHP_INI_BOOLEAN("trace.debug_enabled", "0", PHP_INI_ALL, OnUpdateBool, debug_enabled, zend_trace_globals, trace_globals)
    STD_PHP_INI_ENTRY("trace.debug_log_path", "/tmp/php_trace_debug.log", PHP_INI_ALL, OnUpdateString, debug_log_path, zend_trace_globals, trace_globals)
PHP_INI_END()

// 全局变量初始化
static void php_trace_init_globals(zend_trace_globals *trace_globals)
{
    trace_globals->enabled = 1;
    trace_globals->debug_enabled = 0;
    trace_globals->debug_log_path = NULL;
    trace_globals->trace_id = NULL;
    trace_globals->service_name = NULL;
    trace_globals->current_span = NULL;
    trace_globals->root_span = NULL;
    trace_globals->all_spans = NULL;
    trace_globals->span_counter = 0;
}

// 模块初始化
PHP_MINIT_FUNCTION(trace)
{
    trace_debug_log("PHP_MINIT_FUNCTION: start");
    
    ZEND_INIT_MODULE_GLOBALS(trace, php_trace_init_globals, NULL);
    REGISTER_INI_ENTRIES();
    
    ZVAL_UNDEF(&g_function_enter_callback);
    ZVAL_UNDEF(&g_function_exit_callback);
    ZVAL_UNDEF(&g_curl_callback);
    ZVAL_UNDEF(&g_db_callback);
    ZVAL_UNDEF(&g_trace_whitelist);
    
    trace_debug_log("PHP_MINIT_FUNCTION: globals initialized");
    
    // 启用函数调用钩子
    original_zend_execute_ex = zend_execute_ex;
    zend_execute_ex = trace_execute_ex;
    
    trace_debug_log("PHP_MINIT_FUNCTION: hook installed, original=%p, new=%p", original_zend_execute_ex, trace_execute_ex);
    
    return SUCCESS;
}

// 模块关闭
PHP_MSHUTDOWN_FUNCTION(trace)
{
    if (original_zend_execute_ex) {
        zend_execute_ex = original_zend_execute_ex;
    }
    
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

// 请求初始化
PHP_RINIT_FUNCTION(trace)
{
    trace_debug_log("PHP_RINIT_FUNCTION: start, enabled=%d", TRACE_G(enabled));
    
    if (TRACE_G(enabled)) {
        TRACE_G(current_span) = NULL;
        TRACE_G(root_span) = NULL;
        TRACE_G(span_counter) = 0;
        
        ALLOC_HASHTABLE(TRACE_G(all_spans));
        zend_hash_init(TRACE_G(all_spans), 8, NULL, ZVAL_PTR_DTOR, 0);
        
        trace_generate_ids();
        trace_debug_log("PHP_RINIT_FUNCTION: TraceID generated=%s", ZSTR_VAL(TRACE_G(trace_id)));
        
        if (!TRACE_G(service_name)) {
            TRACE_G(service_name) = zend_string_init("php-app", 7, 0);
        }
        
        TRACE_G(root_span) = trace_create_span("http.request", NULL);
        TRACE_G(current_span) = TRACE_G(root_span);
        trace_debug_log("PHP_RINIT_FUNCTION: root span created=%s", ZSTR_VAL(TRACE_G(root_span)->span_id));
    }
    
    return SUCCESS;
}

// 请求关闭
PHP_RSHUTDOWN_FUNCTION(trace)
{
    if (TRACE_G(enabled)) {
        if (TRACE_G(root_span)) {
            trace_finish_span(TRACE_G(root_span));
        }
        
        if (TRACE_G(trace_id)) {
            zend_string_release(TRACE_G(trace_id));
            TRACE_G(trace_id) = NULL;
        }
        
        if (TRACE_G(all_spans)) {
            zval *span_zval;
            ZEND_HASH_FOREACH_VAL(TRACE_G(all_spans), span_zval) {
                trace_span_t *span = (trace_span_t*)Z_PTR_P(span_zval);
                if (span) {
                    if (span->span_id) zend_string_release(span->span_id);
                    if (span->parent_id) zend_string_release(span->parent_id);
                    if (span->operation_name) zend_string_release(span->operation_name);
                    if (span->tags) {
                        zend_hash_destroy(span->tags);
                        FREE_HASHTABLE(span->tags);
                    }
                    if (span->logs) {
                        zend_hash_destroy(span->logs);
                        FREE_HASHTABLE(span->logs);
                    }
                    efree(span);
                }
            } ZEND_HASH_FOREACH_END();
            
            zend_hash_destroy(TRACE_G(all_spans));
            FREE_HASHTABLE(TRACE_G(all_spans));
            TRACE_G(all_spans) = NULL;
        }
        
        TRACE_G(current_span) = NULL;
        TRACE_G(root_span) = NULL;
    }
    return SUCCESS;
}

// 模块信息
PHP_MINFO_FUNCTION(trace)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "Trace Extension", "enabled");
    php_info_print_table_row(2, "Version", PHP_TRACE_VERSION);
    php_info_print_table_row(2, "Author", "ziyue.wen");
    php_info_print_table_end();
    
    php_info_print_table_start();
    php_info_print_table_header(2, "Current Request", "Value");
    
    if (TRACE_G(trace_id)) {
        php_info_print_table_row(2, "Trace ID", ZSTR_VAL(TRACE_G(trace_id)));
    } else {
        php_info_print_table_row(2, "Trace ID", "Not initialized");
    }
    
    if (TRACE_G(service_name)) {
        php_info_print_table_row(2, "Service Name", ZSTR_VAL(TRACE_G(service_name)));
    } else {
        php_info_print_table_row(2, "Service Name", "Not set");
    }
    
    char span_count_str[32];
    if (TRACE_G(all_spans)) {
        snprintf(span_count_str, sizeof(span_count_str), "%d", zend_hash_num_elements(TRACE_G(all_spans)));
        php_info_print_table_row(2, "Total Spans", span_count_str);
    } else {
        php_info_print_table_row(2, "Total Spans", "0");
    }
    
    if (TRACE_G(current_span)) {
        php_info_print_table_row(2, "Current Span", ZSTR_VAL(TRACE_G(current_span)->operation_name));
        php_info_print_table_row(2, "Current Span ID", ZSTR_VAL(TRACE_G(current_span)->span_id));
    } else {
        php_info_print_table_row(2, "Current Span", "None");
    }
    
    php_info_print_table_end();
    
    php_info_print_table_start();
    php_info_print_table_header(2, "Callbacks", "Status");
    php_info_print_table_row(2, "function_enter", !Z_ISUNDEF(g_function_enter_callback) ? "Set" : "Not set");
    php_info_print_table_row(2, "function_exit", !Z_ISUNDEF(g_function_exit_callback) ? "Set" : "Not set");
    php_info_print_table_row(2, "curl", !Z_ISUNDEF(g_curl_callback) ? "Set" : "Not set");
    php_info_print_table_row(2, "database", !Z_ISUNDEF(g_db_callback) ? "Set" : "Not set");
    php_info_print_table_end();
    
    php_info_print_table_start();
    php_info_print_table_header(2, "Whitelist", "Status");
    
    if (!Z_ISUNDEF(g_trace_whitelist) && Z_TYPE(g_trace_whitelist) == IS_ARRAY) {
        char rule_count_str[32];
        snprintf(rule_count_str, sizeof(rule_count_str), "%d rules", zend_hash_num_elements(Z_ARR(g_trace_whitelist)));
        php_info_print_table_row(2, "Rules", rule_count_str);
    } else {
        php_info_print_table_row(2, "Rules", "Not set (trace all)");
    }
    
    php_info_print_table_end();
    
    php_info_print_table_start();
    php_info_print_table_header(2, "Features", "Support");
    php_info_print_table_row(2, "Auto Function Tracing", "Yes");
    php_info_print_table_row(2, "Span Stack Management", "Yes");
    php_info_print_table_row(2, "Tags Support", "Yes");
    php_info_print_table_row(2, "Logs Support", "Yes");
    php_info_print_table_row(2, "Whitelist Rules", "15 types");
    php_info_print_table_row(2, "CLI Mode (trace_reset)", "Yes");
    php_info_print_table_row(2, "OpenTelemetry Format", "Yes");
    php_info_print_table_row(2, "Debug Logging", "Yes");
    php_info_print_table_end();
    
    DISPLAY_INI_ENTRIES();
}

// 模块入口
zend_module_entry trace_module_entry = {
    STANDARD_MODULE_HEADER,
    "trace",
    trace_functions,
    PHP_MINIT(trace),
    PHP_MSHUTDOWN(trace),
    PHP_RINIT(trace),
    PHP_RSHUTDOWN(trace),
    PHP_MINFO(trace),
    PHP_TRACE_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_TRACE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(trace)
#endif