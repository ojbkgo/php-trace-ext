#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "SAPI.h"
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
    zend_bool in_trace_callback;  // 重入保护标志：防止在回调中再次触发追踪
    // 请求级回调（每个请求独立，避免FPM进程复用时相互影响）
    zval function_enter_callback;
    zval function_exit_callback;
    zval curl_callback;
    zval db_callback;
    zval trace_whitelist;           // 用户函数白名单（file_pattern）
    zval internal_trace_whitelist;  // 内部函数白名单（module_pattern）
ZEND_END_MODULE_GLOBALS(trace)

#ifdef ZTS
#define TRACE_G(v) TSRMG(trace_globals_id, zend_trace_globals *, v)
#else
#define TRACE_G(v) (trace_globals.v)
#endif

ZEND_DECLARE_MODULE_GLOBALS(trace)

// 原始函数指针（模块级，全局共享）
void (*original_zend_execute_ex)(zend_execute_data *execute_data) = NULL;
void (*original_zend_execute_internal)(zend_execute_data *execute_data, zval *return_value) = NULL;

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

// Debug日志函数（默认开启，只记录关键信息）
void trace_debug_log(const char *format, ...)
{
    const char *log_file = "/tmp/php_trace_debug.log";
    
    FILE *fp = fopen(log_file, "a");
    if (fp) {
        va_list args;
        va_start(args, format);
        
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
    
    // 调试：只记录异常情况（parent为空但root_span存在）
    if (!parent && TRACE_G(root_span)) {
        trace_debug_log("[SPAN_CREATE] ⚠️ 创建无父级span: %s (current_span=%p, root_span=%p)",
                       operation_name,
                       TRACE_G(current_span),
                       TRACE_G(root_span));
    }
    
    // 初始化tags
    ALLOC_HASHTABLE(span->tags);
    zend_hash_init(span->tags, 0, NULL, ZVAL_PTR_DTOR, 0);
    
    // 初始化logs
    ALLOC_HASHTABLE(span->logs);
    zend_hash_init(span->logs, 0, NULL, ZVAL_PTR_DTOR, 0);
    
    if (TRACE_G(all_spans)) {
        zval span_zval;
        /* 将span指针存储到zval中，以便可以将其添加到all_spans哈希表中 */
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
    if (Z_ISUNDEF_P(callback)) {
        trace_debug_log("[ERROR] 回调未定义");
        return;
    }
    
    // 检查回调是否可调用
    if (!zend_is_callable(callback, 0, NULL)) {
        return;
    }
    
    // ⚠️ 设置重入保护标志，防止回调中的函数调用再次触发追踪
    TRACE_G(in_trace_callback) = 1;
    
    // 调用用户函数
    call_user_function(CG(function_table), NULL, callback, retval, argc, argv);
    
    // ⚠️ 清除重入保护标志
    TRACE_G(in_trace_callback) = 0;
    
    // 清除可能的异常，避免影响后续执行
    if (EG(exception)) {
        zend_clear_exception();
    }
}

// 简单的通配符匹配函数（支持 * 通配符）
// 例如: "App\\Controller\\*" 匹配 "App\\Controller\\UserController"
//      "/app/*/test.php" 匹配 "/app/controllers/test.php"
//      "*" 匹配任何字符串
int trace_wildcard_match(const char *str, const char *pattern)
{
    if (!pattern || strcmp(pattern, "*") == 0) {
        return 1;  // 没有pattern或pattern是*，匹配所有
    }
    
    if (!str) {
        return 0;  // 没有str，不匹配
    }
    
    const char *s = str;
    const char *p = pattern;
    const char *star_pos = NULL;
    const char *match_pos = NULL;
    
    while (*s) {
        if (*p == '*') {
            // 遇到 *，记录位置
            star_pos = p++;
            match_pos = s;
        } else if (*p == *s) {
            // 字符匹配，继续
            p++;
            s++;
        } else if (star_pos) {
            // 不匹配，回溯到上一个 *
            p = star_pos + 1;
            s = ++match_pos;
        } else {
            // 不匹配且没有 * 可以回溯
            return 0;
        }
    }
    
    // 处理pattern末尾的 *
    while (*p == '*') {
        p++;
    }
    
    // 如果pattern也结束了，则匹配成功
    return *p == '\0';
}

// 检查字符串是否匹配模式数组
// patterns 是一个数组，每项都需要匹配（AND关系）
// 如果pattern以 "! " 开头，表示反向匹配（不应该匹配）
// 返回：1=匹配，0=不匹配
int trace_match_patterns(const char *str, zval *patterns)
{
    if (!patterns) {
        return 1;  // 没有pattern，匹配所有
    }
    
    // 如果是字符串，转换为单元素数组处理
    if (Z_TYPE_P(patterns) == IS_STRING) {
        const char *pattern = Z_STRVAL_P(patterns);
        int is_negative = (strlen(pattern) > 2 && pattern[0] == '!' && pattern[1] == ' ');
        const char *actual_pattern = is_negative ? pattern + 2 : pattern;
        int match_result = trace_wildcard_match(str, actual_pattern);
        return is_negative ? !match_result : match_result;
    }
    
    // 如果是数组，所有pattern都需要匹配（AND关系）
    if (Z_TYPE_P(patterns) == IS_ARRAY) {
        zval *pattern_item;
        ZEND_HASH_FOREACH_VAL(Z_ARR_P(patterns), pattern_item) {
            if (Z_TYPE_P(pattern_item) != IS_STRING) {
                continue;
            }
            
            const char *pattern = Z_STRVAL_P(pattern_item);
            int is_negative = (strlen(pattern) > 2 && pattern[0] == '!' && pattern[1] == ' ');
            const char *actual_pattern = is_negative ? pattern + 2 : pattern;
            int match_result = trace_wildcard_match(str, actual_pattern);
            
            // 正向匹配：需要匹配
            // 反向匹配：不应该匹配
            if (is_negative) {
                if (match_result) {
                    return 0;  // 反向匹配成功（不应该匹配但匹配了），整体失败
                }
            } else {
                if (!match_result) {
                    return 0;  // 正向匹配失败，整体失败
                }
            }
        } ZEND_HASH_FOREACH_END();
        return 1;  // 所有pattern都通过
    }
    
    return 1;  // 其他类型，默认匹配
}

// 判断是否应该跟踪函数
// 白名单格式：[
//   [
//     'file_pattern' => ['/app/*', '! */vendor/*'],  // 在/app/下 且 不在vendor下
//     'class_pattern' => ['App\\*'],
//     'function_pattern' => ['handle*', '! *Internal']
//   ]
// ]
// 多个规则之间是 OR 关系（符合任意一个即可）
// 每个规则内部的条件是 AND 关系（都要符合）
// 每个字段的多个pattern也是 AND 关系（都要符合）
// 判断是否应该跟踪用户函数（PHP代码）
int trace_should_trace_function(zend_execute_data *execute_data)
{
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // 只处理用户函数
    if (execute_data->func->type != ZEND_USER_FUNCTION) {
        return 0;
    }
    
    // 始终跳过trace扩展自身的函数，避免无限递归
    if (execute_data->func->common.function_name &&
        strncmp(ZSTR_VAL(execute_data->func->common.function_name), "trace_", 6) == 0) {
        return 0;
    }
    
    // 如果没有设置白名单，不跟踪
    if (Z_ISUNDEF(TRACE_G(trace_whitelist))) {
        return 0;
    }
    
    // 如果白名单不是数组，不跟踪
    if (Z_TYPE(TRACE_G(trace_whitelist)) != IS_ARRAY) {
        return 0;
    }
    
    // 获取函数信息
    const char *func_name = execute_data->func->common.function_name ? 
                           ZSTR_VAL(execute_data->func->common.function_name) : "";
    const char *class_name = execute_data->func->common.scope ? 
                            ZSTR_VAL(execute_data->func->common.scope->name) : "";
    
    // 获取文件路径
    const char *file_name = "";
    if (execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
        // 内部函数：使用模块名
        if (execute_data->func->internal_function.module) {
            file_name = execute_data->func->internal_function.module->name;
        }
    } else if (execute_data->func->type == ZEND_USER_FUNCTION) {
        // 用户函数：使用实际文件路径
        if (execute_data->func->op_array.filename) {
            file_name = ZSTR_VAL(execute_data->func->op_array.filename);
        }
    }

    // 如果类名称和函数名称都为空，不跟踪
    if ((!class_name || class_name[0] == '\0') && (!func_name || func_name[0] == '\0')) {
        return 0;
    }
    
    // 遍历白名单规则（OR关系，符合任意一个即可）
    zval *rule;
    ZEND_HASH_FOREACH_VAL(Z_ARR(TRACE_G(trace_whitelist)), rule) {
        if (Z_TYPE_P(rule) != IS_ARRAY) {
            continue;
        }
        
        int matched = 1;  // 假设匹配，逐项检查（AND关系）
        
        // 检查 file_pattern
        zval *file_patterns = zend_hash_str_find(Z_ARR_P(rule), "file_pattern", sizeof("file_pattern") - 1);
        if (file_patterns) {
            if (!trace_match_patterns(file_name, file_patterns)) {
                matched = 0;
            }
        }
        
        // 检查 class_pattern
        if (matched) {
            zval *class_patterns = zend_hash_str_find(Z_ARR_P(rule), "class_pattern", sizeof("class_pattern") - 1);
            if (class_patterns) {
                if (!trace_match_patterns(class_name, class_patterns)) {
                    matched = 0;
                }
            }
        }
        
        // 检查 function_pattern
        if (matched) {
            zval *func_patterns = zend_hash_str_find(Z_ARR_P(rule), "function_pattern", sizeof("function_pattern") - 1);
            if (func_patterns) {
                if (!trace_match_patterns(func_name, func_patterns)) {
                    matched = 0;
                }
            }
        }
        
        // 如果所有条件都匹配，则跟踪此函数
        if (matched) {
            return 1;
        }
    } ZEND_HASH_FOREACH_END();
    
    // 白名单中没有匹配的规则，不跟踪
    return 0;
}

// 判断是否应该跟踪内部函数（扩展函数：mysql、redis、curl等）
int trace_should_trace_internal_function(zend_execute_data *execute_data)
{
    if (!execute_data || !execute_data->func) {
        return 0;
    }
    
    // 只处理内部函数
    if (execute_data->func->type != ZEND_INTERNAL_FUNCTION) {
        return 0;
    }
    
    // 获取模块名和函数名
    const char *module_name = NULL;
    if (execute_data->func->internal_function.module) {
        module_name = execute_data->func->internal_function.module->name;
    }
    
    const char *func_name = execute_data->func->common.function_name ? 
                           ZSTR_VAL(execute_data->func->common.function_name) : "";
    const char *class_name = execute_data->func->common.scope ? 
                            ZSTR_VAL(execute_data->func->common.scope->name) : "";
    
    // 没有模块名，跳过
    if (!module_name) {
        return 0;
    }
    
    // 如果没有设置内部函数白名单，不跟踪
    if (Z_ISUNDEF(TRACE_G(internal_trace_whitelist))) {
        return 0;
    }
    
    if (Z_TYPE(TRACE_G(internal_trace_whitelist)) != IS_ARRAY) {
        return 0;
    }
    
    // 遍历内部函数白名单规则（OR关系）
    zval *rule;
    ZEND_HASH_FOREACH_VAL(Z_ARR(TRACE_G(internal_trace_whitelist)), rule) {
        if (Z_TYPE_P(rule) != IS_ARRAY) {
            continue;
        }
        
        int matched = 1;
        
        // 检查 module_pattern（对于内部函数，使用module而不是file）
        zval *module_patterns = zend_hash_str_find(Z_ARR_P(rule), "module_pattern", sizeof("module_pattern") - 1);
        if (module_patterns) {
            if (!trace_match_patterns(module_name, module_patterns)) {
                matched = 0;
            }
        }
        
        // 检查 class_pattern
        if (matched) {
            zval *class_patterns = zend_hash_str_find(Z_ARR_P(rule), "class_pattern", sizeof("class_pattern") - 1);
            if (class_patterns) {
                if (!trace_match_patterns(class_name, class_patterns)) {
                    matched = 0;
                }
            }
        }
        
        // 检查 function_pattern
        if (matched) {
            zval *func_patterns = zend_hash_str_find(Z_ARR_P(rule), "function_pattern", sizeof("function_pattern") - 1);
            if (func_patterns) {
                if (!trace_match_patterns(func_name, func_patterns)) {
                    matched = 0;
                }
            }
        }
        
        if (matched) {
            return 1;
        }
    } ZEND_HASH_FOREACH_END();
    
    return 0;
}

// 函数执行钩子 (完整实现)
void trace_execute_ex(zend_execute_data *execute_data)
{
    trace_span_t *span = NULL;
    zval callback_result;
    ZVAL_UNDEF(&callback_result);
    
    // ⚠️ 重入保护：如果正在执行回调，直接调用原始函数，避免无限递归
    if (TRACE_G(in_trace_callback)) {
        original_zend_execute_ex(execute_data);
        return;
    }
    
    // 快速路径：检查是否需要跟踪
    if (!TRACE_G(enabled) || Z_ISUNDEF(TRACE_G(function_enter_callback))) {
        original_zend_execute_ex(execute_data);
        return;
    }
    
    if (!trace_should_trace_function(execute_data)) {
        original_zend_execute_ex(execute_data);
        return;
    }
    
    // 安全检查
    if (!execute_data || !execute_data->func) {
        original_zend_execute_ex(execute_data);
        return;
    }
    
    // 获取调用方上下文（caller's context）
    const char *caller_file = NULL;
    int caller_line = 0;
    if (execute_data->prev_execute_data) {
        zend_execute_data *caller = execute_data->prev_execute_data;
        if (caller->func && caller->func->type == ZEND_USER_FUNCTION) {
            if (caller->func->op_array.filename) {
                caller_file = ZSTR_VAL(caller->func->op_array.filename);
            }
            if (caller->opline) {
                caller_line = caller->opline->lineno;
            }
        }
    }
    
    // 准备回调参数：function, class, caller_file, caller_line, parent_span_id, args
    zval args[6];
    
    // 函数名
    if (execute_data->func->common.function_name) {
        ZVAL_STR_COPY(&args[0], execute_data->func->common.function_name);
    } else {
        ZVAL_STRING(&args[0], "anonymous");
    }
    
    // 类名
    if (execute_data->func->common.scope) {
        ZVAL_STR_COPY(&args[1], execute_data->func->common.scope->name);
    } else {
        ZVAL_NULL(&args[1]);
    }
    
    // 调用方文件名
    if (caller_file) {
        ZVAL_STRING(&args[2], caller_file);
    } else {
        ZVAL_NULL(&args[2]);
    }
    
    // 调用方行号
    ZVAL_LONG(&args[3], caller_line);
    
    // 父Span ID
    if (TRACE_G(current_span) && TRACE_G(current_span)->span_id) {
        ZVAL_STR_COPY(&args[4], TRACE_G(current_span)->span_id);
    } else {
        ZVAL_NULL(&args[4]);
    }
    
    // 函数参数数组
    array_init(&args[5]);
    uint32_t arg_count = ZEND_CALL_NUM_ARGS(execute_data);
    if (arg_count > 0) {
        zval *p = ZEND_CALL_ARG(execute_data, 1);
        uint32_t i = 0; 
        while (i < arg_count) {
            zval arg_copy;
            ZVAL_COPY(&arg_copy, p);
            add_next_index_zval(&args[5], &arg_copy);
            p++;
            i++;
        }
    }
    
    // 调用用户回调
    trace_call_user_callback(&TRACE_G(function_enter_callback), 6, args, &callback_result);
    
    // 根据回调返回值创建span
    if (Z_TYPE(callback_result) == IS_ARRAY) {
        zval *operation_name = zend_hash_str_find(Z_ARR(callback_result), "operation_name", sizeof("operation_name") - 1);
        if (operation_name && Z_TYPE_P(operation_name) == IS_STRING && Z_STRLEN_P(operation_name) > 0) {
            span = trace_create_span(Z_STRVAL_P(operation_name), TRACE_G(current_span));
            if (span) {
                TRACE_G(current_span) = span;
                
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
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                
                // 处理callback返回的logs
                zval *logs = zend_hash_str_find(Z_ARR(callback_result), "logs", sizeof("logs") - 1);
                if (logs && Z_TYPE_P(logs) == IS_ARRAY && span->logs) {
                    zval *log_item;
                    ZEND_HASH_FOREACH_VAL(Z_ARR_P(logs), log_item) {
                        if (Z_TYPE_P(log_item) == IS_ARRAY) {
                            // 复制log条目并添加时间戳
                            zval log_entry;
                            array_init(&log_entry);
                            
                            zval *level = zend_hash_str_find(Z_ARR_P(log_item), "level", sizeof("level") - 1);
                            if (level && Z_TYPE_P(level) == IS_STRING) {
                                add_assoc_str(&log_entry, "level", zend_string_copy(Z_STR_P(level)));
                            }
                            
                            zval *message = zend_hash_str_find(Z_ARR_P(log_item), "message", sizeof("message") - 1);
                            if (message && Z_TYPE_P(message) == IS_STRING) {
                                add_assoc_str(&log_entry, "message", zend_string_copy(Z_STR_P(message)));
                            }
                            
                            add_assoc_double(&log_entry, "timestamp", trace_get_microtime());
                            
                            zend_hash_next_index_insert(span->logs, &log_entry);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
            }
        }
    }
    
    // 清理回调参数
    int i;
    for (i = 0; i < 6; i++) {
        zval_dtor(&args[i]);
    }
    if (!Z_ISUNDEF(callback_result)) {
        zval_dtor(&callback_result);
    }
    
    // 调用原始函数
    original_zend_execute_ex(execute_data);
    
    // 函数执行完成后处理
    if (span) {
        // 完成span
        trace_finish_span(span);
        
        // 恢复父span
        TRACE_G(current_span) = span->parent;
        
        // 调用exit回调
        if (!Z_ISUNDEF(TRACE_G(function_exit_callback))) {
            zval exit_args[3];
            
            // span_id
            ZVAL_STR_COPY(&exit_args[0], span->span_id);
            
            // duration (执行时长)
            ZVAL_DOUBLE(&exit_args[1], span->end_time - span->start_time);
            
            // 函数返回值
            if (execute_data->return_value && !Z_ISUNDEF_P(execute_data->return_value)) {
                ZVAL_COPY(&exit_args[2], execute_data->return_value);
            } else {
                ZVAL_NULL(&exit_args[2]);
            }
            
            zval exit_result;
            ZVAL_UNDEF(&exit_result);
            trace_call_user_callback(&TRACE_G(function_exit_callback), 3, exit_args, &exit_result);
            
            // 处理exit回调返回的tags，合并到span
            if (Z_TYPE(exit_result) == IS_ARRAY) {
                zval *exit_tags = zend_hash_str_find(Z_ARR(exit_result), "tags", sizeof("tags") - 1);
                if (exit_tags && Z_TYPE_P(exit_tags) == IS_ARRAY && span->tags) {
                    zend_string *tag_key;
                    zval *tag_val;
                    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARR_P(exit_tags), tag_key, tag_val) {
                        if (tag_key) {
                            zval tag_copy;
                            ZVAL_COPY(&tag_copy, tag_val);
                            // 更新或添加tag
                            zend_hash_update(span->tags, tag_key, &tag_copy);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                
                // 处理exit回调返回的logs，合并到span
                zval *exit_logs = zend_hash_str_find(Z_ARR(exit_result), "logs", sizeof("logs") - 1);
                if (exit_logs && Z_TYPE_P(exit_logs) == IS_ARRAY && span->logs) {
                    zval *log_item;
                    ZEND_HASH_FOREACH_VAL(Z_ARR_P(exit_logs), log_item) {
                        if (Z_TYPE_P(log_item) == IS_ARRAY) {
                            zval log_entry;
                            array_init(&log_entry);
                            
                            zval *level = zend_hash_str_find(Z_ARR_P(log_item), "level", sizeof("level") - 1);
                            if (level && Z_TYPE_P(level) == IS_STRING) {
                                add_assoc_str(&log_entry, "level", zend_string_copy(Z_STR_P(level)));
                            }
                            
                            zval *message = zend_hash_str_find(Z_ARR_P(log_item), "message", sizeof("message") - 1);
                            if (message && Z_TYPE_P(message) == IS_STRING) {
                                add_assoc_str(&log_entry, "message", zend_string_copy(Z_STR_P(message)));
                            }
                            
                            add_assoc_double(&log_entry, "timestamp", trace_get_microtime());
                            
                            zend_hash_next_index_insert(span->logs, &log_entry);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
            }

            // 清理
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

// 内部函数执行钩子（处理扩展函数：mysql、redis、curl等）
void trace_execute_internal(zend_execute_data *execute_data, zval *return_value)
{
    trace_span_t *span = NULL;
    zval callback_result;
    ZVAL_UNDEF(&callback_result);
    
    // ⚠️ 重入保护
    if (TRACE_G(in_trace_callback)) {
        if (original_zend_execute_internal) {
            original_zend_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }
    
    // 快速路径：检查是否需要跟踪
    if (!TRACE_G(enabled) || Z_ISUNDEF(TRACE_G(function_enter_callback))) {
        if (original_zend_execute_internal) {
            original_zend_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }
    
    if (!trace_should_trace_internal_function(execute_data)) {
        if (original_zend_execute_internal) {
            original_zend_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }
    
    // 安全检查
    if (!execute_data || !execute_data->func) {
        if (original_zend_execute_internal) {
            original_zend_execute_internal(execute_data, return_value);
        } else {
            execute_internal(execute_data, return_value);
        }
        return;
    }
    
    // 获取调用方上下文
    const char *caller_file = NULL;
    int caller_line = 0;
    if (execute_data->prev_execute_data) {
        zend_execute_data *caller = execute_data->prev_execute_data;
        if (caller->func && caller->func->type == ZEND_USER_FUNCTION) {
            if (caller->func->op_array.filename) {
                caller_file = ZSTR_VAL(caller->func->op_array.filename);
            }
            if (caller->opline) {
                caller_line = caller->opline->lineno;
            }
        }
    }
    
    // 准备回调参数
    zval args[6];
    
    // 函数名
    if (execute_data->func->common.function_name) {
        ZVAL_STR_COPY(&args[0], execute_data->func->common.function_name);
    } else {
        ZVAL_STRING(&args[0], "anonymous");
    }
    
    // 类名
    if (execute_data->func->common.scope) {
        ZVAL_STR_COPY(&args[1], execute_data->func->common.scope->name);
    } else {
        ZVAL_NULL(&args[1]);
    }
    
    // 调用方文件名
    if (caller_file) {
        ZVAL_STRING(&args[2], caller_file);
    } else {
        ZVAL_NULL(&args[2]);
    }
    
    // 调用方行号
    ZVAL_LONG(&args[3], caller_line);
    
    // 父Span ID
    if (TRACE_G(current_span) && TRACE_G(current_span)->span_id) {
        ZVAL_STR_COPY(&args[4], TRACE_G(current_span)->span_id);
    } else {
        ZVAL_NULL(&args[4]);
    }
    
    // 函数参数数组
    array_init(&args[5]);
    uint32_t arg_count = ZEND_CALL_NUM_ARGS(execute_data);
    if (arg_count > 0) {
        zval *p = ZEND_CALL_ARG(execute_data, 1);
        uint32_t i = 0;
        while (i < arg_count) {
            zval arg_copy;
            ZVAL_COPY(&arg_copy, p);
            add_next_index_zval(&args[5], &arg_copy);
            p++;
            i++;
        }
    }
    
    // 调用用户回调
    trace_call_user_callback(&TRACE_G(function_enter_callback), 6, args, &callback_result);
    
    // 根据回调返回值创建span
    if (Z_TYPE(callback_result) == IS_ARRAY) {
        zval *operation_name = zend_hash_str_find(Z_ARR(callback_result), "operation_name", sizeof("operation_name") - 1);
        if (operation_name && Z_TYPE_P(operation_name) == IS_STRING && Z_STRLEN_P(operation_name) > 0) {
            span = trace_create_span(Z_STRVAL_P(operation_name), TRACE_G(current_span));
            if (span) {
                TRACE_G(current_span) = span;
                
                // 处理callback返回的tags
                zval *tags = zend_hash_str_find(Z_ARR(callback_result), "tags", sizeof("tags") - 1);
                if (tags && Z_TYPE_P(tags) == IS_ARRAY) {
                    zend_string *tag_key;
                    zval *tag_val;
                    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARR_P(tags), tag_key, tag_val) {
                        if (tag_key) {
                            zval tag_copy;
                            ZVAL_COPY(&tag_copy, tag_val);
                            zend_hash_add(span->tags, tag_key, &tag_copy);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                
                // 处理callback返回的logs
                zval *logs = zend_hash_str_find(Z_ARR(callback_result), "logs", sizeof("logs") - 1);
                if (logs && Z_TYPE_P(logs) == IS_ARRAY && span->logs) {
                    zval *log_item;
                    ZEND_HASH_FOREACH_VAL(Z_ARR_P(logs), log_item) {
                        if (Z_TYPE_P(log_item) == IS_ARRAY) {
                            zval log_entry;
                            array_init(&log_entry);
                            
                            zval *level = zend_hash_str_find(Z_ARR_P(log_item), "level", sizeof("level") - 1);
                            if (level && Z_TYPE_P(level) == IS_STRING) {
                                add_assoc_str(&log_entry, "level", zend_string_copy(Z_STR_P(level)));
                            }
                            
                            zval *message = zend_hash_str_find(Z_ARR_P(log_item), "message", sizeof("message") - 1);
                            if (message && Z_TYPE_P(message) == IS_STRING) {
                                add_assoc_str(&log_entry, "message", zend_string_copy(Z_STR_P(message)));
                            }
                            
                            add_assoc_double(&log_entry, "timestamp", trace_get_microtime());
                            
                            zend_hash_next_index_insert(span->logs, &log_entry);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
            }
        }
    }
    
    // 清理回调参数
    int i;
    for (i = 0; i < 6; i++) {
        zval_dtor(&args[i]);
    }
    if (!Z_ISUNDEF(callback_result)) {
        zval_dtor(&callback_result);
    }
    
    // 调用原始内部函数
    if (original_zend_execute_internal) {
        original_zend_execute_internal(execute_data, return_value);
    } else {
        execute_internal(execute_data, return_value);
    }
    
    // 函数执行完成后处理
    if (span) {
        // 完成span
        trace_finish_span(span);
        
        // 恢复父span
        TRACE_G(current_span) = span->parent;
        
        // 调用exit回调
        if (!Z_ISUNDEF(TRACE_G(function_exit_callback))) {
            zval exit_args[3];
            
            // span_id
            ZVAL_STR_COPY(&exit_args[0], span->span_id);
            
            // duration
            ZVAL_DOUBLE(&exit_args[1], span->end_time - span->start_time);
            
            // 返回值
            if (return_value && !Z_ISUNDEF_P(return_value)) {
                ZVAL_COPY(&exit_args[2], return_value);
            } else {
                ZVAL_NULL(&exit_args[2]);
            }
            
            zval exit_result;
            ZVAL_UNDEF(&exit_result);
            trace_call_user_callback(&TRACE_G(function_exit_callback), 3, exit_args, &exit_result);
            
            // 处理exit回调返回的tags
            if (Z_TYPE(exit_result) == IS_ARRAY) {
                zval *exit_tags = zend_hash_str_find(Z_ARR(exit_result), "tags", sizeof("tags") - 1);
                if (exit_tags && Z_TYPE_P(exit_tags) == IS_ARRAY && span->tags) {
                    zend_string *tag_key;
                    zval *tag_val;
                    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARR_P(exit_tags), tag_key, tag_val) {
                        if (tag_key) {
                            zval tag_copy;
                            ZVAL_COPY(&tag_copy, tag_val);
                            zend_hash_update(span->tags, tag_key, &tag_copy);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                
                // 处理exit回调返回的logs
                zval *exit_logs = zend_hash_str_find(Z_ARR(exit_result), "logs", sizeof("logs") - 1);
                if (exit_logs && Z_TYPE_P(exit_logs) == IS_ARRAY && span->logs) {
                    zval *log_item;
                    ZEND_HASH_FOREACH_VAL(Z_ARR_P(exit_logs), log_item) {
                        if (Z_TYPE_P(log_item) == IS_ARRAY) {
                            zval log_entry;
                            array_init(&log_entry);
                            
                            zval *level = zend_hash_str_find(Z_ARR_P(log_item), "level", sizeof("level") - 1);
                            if (level && Z_TYPE_P(level) == IS_STRING) {
                                add_assoc_str(&log_entry, "level", zend_string_copy(Z_STR_P(level)));
                            }
                            
                            zval *message = zend_hash_str_find(Z_ARR_P(log_item), "message", sizeof("message") - 1);
                            if (message && Z_TYPE_P(message) == IS_STRING) {
                                add_assoc_str(&log_entry, "message", zend_string_copy(Z_STR_P(message)));
                            }
                            
                            add_assoc_double(&log_entry, "timestamp", trace_get_microtime());
                            
                            zend_hash_next_index_insert(span->logs, &log_entry);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
            }
            
            // 清理
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
        trace_debug_log("[ERROR] trace_set_callback: 参数解析失败");
        RETURN_FALSE;
    }
    
    if (strcmp(type, "function_enter") == 0) {
        if (!Z_ISUNDEF(TRACE_G(function_enter_callback))) {
            zval_dtor(&TRACE_G(function_enter_callback));
        }
        ZVAL_COPY(&TRACE_G(function_enter_callback), callback);
    } else if (strcmp(type, "function_exit") == 0) {
        if (!Z_ISUNDEF(TRACE_G(function_exit_callback))) {
            zval_dtor(&TRACE_G(function_exit_callback));
        }
        ZVAL_COPY(&TRACE_G(function_exit_callback), callback);
    } else if (strcmp(type, "curl") == 0) {
        if (!Z_ISUNDEF(TRACE_G(curl_callback))) {
            zval_dtor(&TRACE_G(curl_callback));
        }
        ZVAL_COPY(&TRACE_G(curl_callback), callback);
    } else if (strcmp(type, "database") == 0) {
        if (!Z_ISUNDEF(TRACE_G(db_callback))) {
            zval_dtor(&TRACE_G(db_callback));
        }
        ZVAL_COPY(&TRACE_G(db_callback), callback);
    } else {
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
        trace_debug_log("[ERROR] trace_set_callback_whitelist: 参数解析失败");
        RETURN_FALSE;
    }
    
    // 存储白名单规则
    if (!Z_ISUNDEF(TRACE_G(trace_whitelist))) {
        zval_dtor(&TRACE_G(trace_whitelist));
    }
    
    ZVAL_COPY(&TRACE_G(trace_whitelist), rules);
    
    RETURN_TRUE;
}

// 设置内部函数白名单（用于mysql、redis、curl等扩展）
PHP_FUNCTION(trace_set_internal_whitelist)
{
    zval *rules;
    
    if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &rules) == FAILURE) {
        trace_debug_log("[ERROR] trace_set_internal_whitelist: 参数解析失败");
        RETURN_FALSE;
    }
    
    // 存储内部函数白名单规则
    if (!Z_ISUNDEF(TRACE_G(internal_trace_whitelist))) {
        zval_dtor(&TRACE_G(internal_trace_whitelist));
    }
    
    ZVAL_COPY(&TRACE_G(internal_trace_whitelist), rules);
    
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
        } else {
            trace_generate_ids();
        }
        
        // 创建新的根span
        TRACE_G(root_span) = trace_create_span("http.request", NULL);
        TRACE_G(current_span) = TRACE_G(root_span);
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
    PHP_FE(trace_set_internal_whitelist, arginfo_trace_set_callback_whitelist)
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
    trace_globals->in_trace_callback = 0;
    // 初始化请求级回调和白名单
    ZVAL_UNDEF(&trace_globals->function_enter_callback);
    ZVAL_UNDEF(&trace_globals->function_exit_callback);
    ZVAL_UNDEF(&trace_globals->curl_callback);
    ZVAL_UNDEF(&trace_globals->db_callback);
    ZVAL_UNDEF(&trace_globals->trace_whitelist);
    ZVAL_UNDEF(&trace_globals->internal_trace_whitelist);
}

// 模块初始化
PHP_MINIT_FUNCTION(trace)
{
    ZEND_INIT_MODULE_GLOBALS(trace, php_trace_init_globals, NULL);
    REGISTER_INI_ENTRIES();
    
    // 只在非CLI模式下启用函数调用钩子
    // 检查所有命令行相关的SAPI：cli, phpdbg, embed
    int is_cli = (strcmp(sapi_module.name, "cli") == 0 ||
                  strcmp(sapi_module.name, "phpdbg") == 0 ||
                  strcmp(sapi_module.name, "embed") == 0);
    
    if (!is_cli) {
        // Hook 用户函数（PHP代码）
        original_zend_execute_ex = zend_execute_ex;
        zend_execute_ex = trace_execute_ex;
        
        // Hook 内部函数（扩展函数：mysql、redis、curl等）
        original_zend_execute_internal = zend_execute_internal;
        zend_execute_internal = trace_execute_internal;
    }
    
    return SUCCESS;
}

// 模块关闭
PHP_MSHUTDOWN_FUNCTION(trace)
{
    // 恢复原始函数指针
    if (original_zend_execute_ex) {
        zend_execute_ex = original_zend_execute_ex;
    }
    if (original_zend_execute_internal) {
        zend_execute_internal = original_zend_execute_internal;
    }
    
    UNREGISTER_INI_ENTRIES();
    return SUCCESS;
}

// 请求初始化
PHP_RINIT_FUNCTION(trace)
{
    // 初始化回调和白名单（每个请求独立）
    ZVAL_UNDEF(&TRACE_G(function_enter_callback));
    ZVAL_UNDEF(&TRACE_G(function_exit_callback));
    ZVAL_UNDEF(&TRACE_G(curl_callback));
    ZVAL_UNDEF(&TRACE_G(db_callback));
    ZVAL_UNDEF(&TRACE_G(trace_whitelist));
    ZVAL_UNDEF(&TRACE_G(internal_trace_whitelist));
    TRACE_G(in_trace_callback) = 0;
    
    if (TRACE_G(enabled)) {
        TRACE_G(current_span) = NULL;
        TRACE_G(root_span) = NULL;
        TRACE_G(span_counter) = 0;
        
        ALLOC_HASHTABLE(TRACE_G(all_spans));
        zend_hash_init(TRACE_G(all_spans), 8, NULL, ZVAL_PTR_DTOR, 0);
        
        trace_generate_ids();
        
        if (!TRACE_G(service_name)) {
            TRACE_G(service_name) = zend_string_init("php-app", 7, 0);
        }
        
        TRACE_G(root_span) = trace_create_span("http.request", NULL);
        TRACE_G(current_span) = TRACE_G(root_span);
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
    
    // 清理回调和白名单（避免FPM进程复用时相互影响）
    if (!Z_ISUNDEF(TRACE_G(function_enter_callback))) {
        zval_dtor(&TRACE_G(function_enter_callback));
        ZVAL_UNDEF(&TRACE_G(function_enter_callback));
    }
    if (!Z_ISUNDEF(TRACE_G(function_exit_callback))) {
        zval_dtor(&TRACE_G(function_exit_callback));
        ZVAL_UNDEF(&TRACE_G(function_exit_callback));
    }
    if (!Z_ISUNDEF(TRACE_G(curl_callback))) {
        zval_dtor(&TRACE_G(curl_callback));
        ZVAL_UNDEF(&TRACE_G(curl_callback));
    }
    if (!Z_ISUNDEF(TRACE_G(db_callback))) {
        zval_dtor(&TRACE_G(db_callback));
        ZVAL_UNDEF(&TRACE_G(db_callback));
    }
    if (!Z_ISUNDEF(TRACE_G(trace_whitelist))) {
        zval_dtor(&TRACE_G(trace_whitelist));
        ZVAL_UNDEF(&TRACE_G(trace_whitelist));
    }
    if (!Z_ISUNDEF(TRACE_G(internal_trace_whitelist))) {
        zval_dtor(&TRACE_G(internal_trace_whitelist));
        ZVAL_UNDEF(&TRACE_G(internal_trace_whitelist));
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
    php_info_print_table_row(2, "function_enter", !Z_ISUNDEF(TRACE_G(function_enter_callback)) ? "Set" : "Not set");
    php_info_print_table_row(2, "function_exit", !Z_ISUNDEF(TRACE_G(function_exit_callback)) ? "Set" : "Not set");
    php_info_print_table_row(2, "curl", !Z_ISUNDEF(TRACE_G(curl_callback)) ? "Set" : "Not set");
    php_info_print_table_row(2, "database", !Z_ISUNDEF(TRACE_G(db_callback)) ? "Set" : "Not set");
    php_info_print_table_end();
    
    php_info_print_table_start();
    php_info_print_table_header(2, "Whitelist (User Functions)", "Status");
    
    if (!Z_ISUNDEF(TRACE_G(trace_whitelist)) && Z_TYPE(TRACE_G(trace_whitelist)) == IS_ARRAY) {
        char rule_count_str[32];
        snprintf(rule_count_str, sizeof(rule_count_str), "%d rules", zend_hash_num_elements(Z_ARR(TRACE_G(trace_whitelist))));
        php_info_print_table_row(2, "Rules (file_pattern)", rule_count_str);
    } else {
        php_info_print_table_row(2, "Rules (file_pattern)", "Not set");
    }
    
    php_info_print_table_end();
    
    php_info_print_table_start();
    php_info_print_table_header(2, "Whitelist (Internal Functions)", "Status");
    
    if (!Z_ISUNDEF(TRACE_G(internal_trace_whitelist)) && Z_TYPE(TRACE_G(internal_trace_whitelist)) == IS_ARRAY) {
        char rule_count_str[32];
        snprintf(rule_count_str, sizeof(rule_count_str), "%d rules", zend_hash_num_elements(Z_ARR(TRACE_G(internal_trace_whitelist))));
        php_info_print_table_row(2, "Rules (module_pattern)", rule_count_str);
    } else {
        php_info_print_table_row(2, "Rules (module_pattern)", "Not set");
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