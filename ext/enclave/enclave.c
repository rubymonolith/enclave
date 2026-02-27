/*
 * enclave.c — Ruby C extension wrapper
 *
 * This file ONLY includes ruby.h, never mruby.h.
 * All mruby interaction goes through the opaque sandbox_core.h API.
 */

#include <ruby.h>
#include "sandbox_core.h"

/* Error class statics */
static VALUE cEnclaveError;
static VALUE cEnclaveTimeoutError;
static VALUE cEnclaveMemoryLimitError;

/* ------------------------------------------------------------------ */
/* sandbox_value_t <-> CRuby VALUE conversion                          */
/* ------------------------------------------------------------------ */

/* Convert sandbox_value_t -> CRuby VALUE */
static VALUE
sandbox_value_to_rb(const sandbox_value_t *val)
{
    switch (val->type) {
    case SANDBOX_VALUE_NIL:
        return Qnil;
    case SANDBOX_VALUE_TRUE:
        return Qtrue;
    case SANDBOX_VALUE_FALSE:
        return Qfalse;
    case SANDBOX_VALUE_INTEGER:
        return LL2NUM(val->as.i);
    case SANDBOX_VALUE_FLOAT:
        return DBL2NUM(val->as.f);
    case SANDBOX_VALUE_STRING:
        return rb_str_new(val->as.str.ptr, (long)val->as.str.len);
    case SANDBOX_VALUE_ARRAY: {
        VALUE ary = rb_ary_new_capa((long)val->as.arr.len);
        for (size_t i = 0; i < val->as.arr.len; i++) {
            rb_ary_push(ary, sandbox_value_to_rb(&val->as.arr.items[i]));
        }
        return ary;
    }
    case SANDBOX_VALUE_HASH: {
        VALUE hash = rb_hash_new();
        for (size_t i = 0; i < val->as.hash.len; i++) {
            rb_hash_aset(hash,
                sandbox_value_to_rb(&val->as.hash.keys[i]),
                sandbox_value_to_rb(&val->as.hash.vals[i]));
        }
        return hash;
    }
    }
    return Qnil;
}

/* Convert CRuby VALUE -> sandbox_value_t. Returns 0 on success, -1 on bad type. */
static int
rb_to_sandbox_value(VALUE v, sandbox_value_t *out, char *errbuf, size_t errbuf_size)
{
    memset(out, 0, sizeof(*out));

    if (NIL_P(v)) {
        out->type = SANDBOX_VALUE_NIL;
        return 0;
    }
    if (v == Qtrue) {
        out->type = SANDBOX_VALUE_TRUE;
        return 0;
    }
    if (v == Qfalse) {
        out->type = SANDBOX_VALUE_FALSE;
        return 0;
    }
    if (FIXNUM_P(v) || RB_TYPE_P(v, T_BIGNUM)) {
        out->type = SANDBOX_VALUE_INTEGER;
        out->as.i = (int64_t)NUM2LL(v);
        return 0;
    }
    if (RB_FLOAT_TYPE_P(v)) {
        out->type = SANDBOX_VALUE_FLOAT;
        out->as.f = NUM2DBL(v);
        return 0;
    }
    if (RB_TYPE_P(v, T_STRING)) {
        out->type = SANDBOX_VALUE_STRING;
        out->as.str.len = (size_t)RSTRING_LEN(v);
        out->as.str.ptr = malloc(out->as.str.len + 1);
        memcpy(out->as.str.ptr, RSTRING_PTR(v), out->as.str.len);
        out->as.str.ptr[out->as.str.len] = '\0';
        return 0;
    }
    if (RB_TYPE_P(v, T_SYMBOL)) {
        /* Symbol -> String */
        VALUE s = rb_sym2str(v);
        out->type = SANDBOX_VALUE_STRING;
        out->as.str.len = (size_t)RSTRING_LEN(s);
        out->as.str.ptr = malloc(out->as.str.len + 1);
        memcpy(out->as.str.ptr, RSTRING_PTR(s), out->as.str.len);
        out->as.str.ptr[out->as.str.len] = '\0';
        return 0;
    }
    if (RB_TYPE_P(v, T_ARRAY)) {
        long alen = RARRAY_LEN(v);
        out->type = SANDBOX_VALUE_ARRAY;
        out->as.arr.len = (size_t)alen;
        out->as.arr.items = calloc((size_t)alen, sizeof(sandbox_value_t));
        for (long i = 0; i < alen; i++) {
            if (rb_to_sandbox_value(rb_ary_entry(v, i), &out->as.arr.items[i],
                                    errbuf, errbuf_size) != 0) {
                for (long j = 0; j < i; j++) {
                    sandbox_value_free(&out->as.arr.items[j]);
                }
                free(out->as.arr.items);
                out->as.arr.items = NULL;
                return -1;
            }
        }
        return 0;
    }
    if (RB_TYPE_P(v, T_HASH)) {
        VALUE keys = rb_funcall(v, rb_intern("keys"), 0);
        long hlen = RARRAY_LEN(keys);
        out->type = SANDBOX_VALUE_HASH;
        out->as.hash.len = (size_t)hlen;
        out->as.hash.keys = calloc((size_t)hlen, sizeof(sandbox_value_t));
        out->as.hash.vals = calloc((size_t)hlen, sizeof(sandbox_value_t));
        for (long i = 0; i < hlen; i++) {
            VALUE k = rb_ary_entry(keys, i);
            VALUE val = rb_hash_aref(v, k);
            if (rb_to_sandbox_value(k, &out->as.hash.keys[i], errbuf, errbuf_size) != 0 ||
                rb_to_sandbox_value(val, &out->as.hash.vals[i], errbuf, errbuf_size) != 0) {
                for (long j = 0; j <= i; j++) {
                    sandbox_value_free(&out->as.hash.keys[j]);
                    sandbox_value_free(&out->as.hash.vals[j]);
                }
                free(out->as.hash.keys);
                free(out->as.hash.vals);
                out->as.hash.keys = NULL;
                out->as.hash.vals = NULL;
                return -1;
            }
        }
        return 0;
    }

    /* Unsupported type */
    VALUE cls = rb_class_name(rb_obj_class(v));
    snprintf(errbuf, errbuf_size, "TypeError: unsupported type for sandbox: %s",
             StringValueCStr(cls));
    return -1;
}

/* ------------------------------------------------------------------ */
/* CRuby callback: dispatches tool calls to @tool_context              */
/* ------------------------------------------------------------------ */

typedef struct {
    VALUE tool_context;  /* @tool_context object */
    VALUE method_name;   /* Symbol for the method */
    int   argc;
    VALUE *argv;
} cruby_call_args_t;

static VALUE
cruby_protected_call(VALUE arg)
{
    cruby_call_args_t *ca = (cruby_call_args_t *)arg;
    return rb_funcallv(ca->tool_context, SYM2ID(ca->method_name), ca->argc, ca->argv);
}

static sandbox_callback_result_t
sandbox_cruby_callback(const char *method_name,
                       const sandbox_value_t *args,
                       int argc,
                       void *userdata)
{
    sandbox_callback_result_t result;
    memset(&result, 0, sizeof(result));

    VALUE self = (VALUE)userdata;

    /* Convert sandbox args -> CRuby VALUEs */
    VALUE *rb_args = NULL;
    if (argc > 0) {
        rb_args = ALLOCA_N(VALUE, argc);
        for (int i = 0; i < argc; i++) {
            rb_args[i] = sandbox_value_to_rb(&args[i]);
        }
    }

    /* Call @tool_context.send(method_name, *args) via rb_protect */
    VALUE tool_context = rb_ivar_get(self, rb_intern("@tool_context"));
    VALUE method_sym = ID2SYM(rb_intern(method_name));

    cruby_call_args_t ca;
    ca.tool_context = tool_context;
    ca.method_name = method_sym;
    ca.argc = argc;
    ca.argv = rb_args;

    int state = 0;
    VALUE ret = rb_protect(cruby_protected_call, (VALUE)&ca, &state);

    if (state) {
        /* Exception was raised -- capture message */
        VALUE exc = rb_errinfo();
        rb_set_errinfo(Qnil);
        VALUE exc_str = rb_funcall(exc, rb_intern("inspect"), 0);
        const char *msg = StringValueCStr(exc_str);
        result.error = strdup(msg);
        return result;
    }

    /* Convert CRuby return -> sandbox_value_t */
    char errbuf[256];
    errbuf[0] = '\0';
    if (rb_to_sandbox_value(ret, &result.value, errbuf, sizeof(errbuf)) != 0) {
        result.error = strdup(errbuf);
        return result;
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* TypedData for Enclave                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    sandbox_state_t *state;
    int              closed;
} rb_enclave_t;

static void
rb_enclave_free(void *ptr)
{
    rb_enclave_t *sb = (rb_enclave_t *)ptr;
    if (sb) {
        if (sb->state) {
            sandbox_state_free(sb->state);
            sb->state = NULL;
        }
        free(sb);
    }
}

static size_t
rb_enclave_memsize(const void *ptr)
{
    return sizeof(rb_enclave_t);
}

static const rb_data_type_t enclave_data_type = {
    "Enclave",
    { NULL, rb_enclave_free, rb_enclave_memsize },
    NULL, NULL,
    RUBY_TYPED_FREE_IMMEDIATELY
};

static rb_enclave_t *
get_enclave(VALUE self)
{
    rb_enclave_t *sb;
    TypedData_Get_Struct(self, rb_enclave_t, &enclave_data_type, sb);
    if (sb->closed) {
        rb_raise(rb_eRuntimeError, "enclave is closed");
    }
    return sb;
}

/* ------------------------------------------------------------------ */
/* Enclave#initialize                                                  */
/* ------------------------------------------------------------------ */

static VALUE
enclave_alloc(VALUE klass)
{
    rb_enclave_t *sb = calloc(1, sizeof(rb_enclave_t));
    return TypedData_Wrap_Struct(klass, &enclave_data_type, sb);
}

static VALUE
enclave_initialize(VALUE self, VALUE rb_timeout, VALUE rb_memory_limit)
{
    rb_enclave_t *sb;
    TypedData_Get_Struct(self, rb_enclave_t, &enclave_data_type, sb);

    double timeout = NIL_P(rb_timeout) ? 0.0 : NUM2DBL(rb_timeout);
    size_t memory_limit = NIL_P(rb_memory_limit) ? 0 : (size_t)NUM2ULL(rb_memory_limit);

    sb->state = sandbox_state_new(timeout, memory_limit);
    if (!sb->state) {
        rb_raise(rb_eRuntimeError, "failed to initialize mruby enclave");
    }
    sb->closed = 0;

    /* Set up the callback so CRuby can handle tool calls */
    sandbox_state_set_callback(sb->state, sandbox_cruby_callback, (void *)self);

    return self;
}

/* ------------------------------------------------------------------ */
/* Enclave#_define_function                                            */
/* ------------------------------------------------------------------ */

static VALUE
enclave_define_function(VALUE self, VALUE rb_name)
{
    rb_enclave_t *sb = get_enclave(self);
    const char *name = StringValueCStr(rb_name);

    if (sandbox_state_define_function(sb->state, name) != 0) {
        rb_raise(rb_eRuntimeError, "too many tool functions (max %d)", 64);
    }

    return self;
}

/* ------------------------------------------------------------------ */
/* Enclave#_eval                                                       */
/* ------------------------------------------------------------------ */

static VALUE
enclave_eval(VALUE self, VALUE rb_code)
{
    rb_enclave_t *sb = get_enclave(self);
    const char *code = StringValueCStr(rb_code);

    sandbox_result_t result = sandbox_state_eval(sb->state, code);

    /* Check for resource limit errors — raise instead of returning in Result */
    if (result.error_kind == SANDBOX_ERROR_TIMEOUT) {
        const char *msg = result.error ? result.error : "execution timeout exceeded";
        VALUE exc_msg = rb_str_new_cstr(msg);
        sandbox_result_free(&result);
        rb_exc_raise(rb_exc_new_str(cEnclaveTimeoutError, exc_msg));
    }
    if (result.error_kind == SANDBOX_ERROR_MEMORY_LIMIT) {
        const char *msg = result.error ? result.error : "memory limit exceeded";
        VALUE exc_msg = rb_str_new_cstr(msg);
        sandbox_result_free(&result);
        rb_exc_raise(rb_exc_new_str(cEnclaveMemoryLimitError, exc_msg));
    }

    VALUE value = result.value ? rb_str_new_cstr(result.value) : Qnil;
    VALUE output = result.output ? rb_str_new_cstr(result.output) : rb_str_new_cstr("");
    VALUE error = result.error ? rb_str_new_cstr(result.error) : Qnil;

    sandbox_result_free(&result);

    return rb_ary_new_from_args(3, value, output, error);
}

/* ------------------------------------------------------------------ */
/* Enclave#reset!                                                      */
/* ------------------------------------------------------------------ */

static VALUE
enclave_reset(VALUE self)
{
    rb_enclave_t *sb = get_enclave(self);
    sandbox_state_reset(sb->state);
    return self;
}

/* ------------------------------------------------------------------ */
/* Enclave#close                                                       */
/* ------------------------------------------------------------------ */

static VALUE
enclave_close(VALUE self)
{
    rb_enclave_t *sb;
    TypedData_Get_Struct(self, rb_enclave_t, &enclave_data_type, sb);

    if (!sb->closed) {
        if (sb->state) {
            sandbox_state_free(sb->state);
            sb->state = NULL;
        }
        sb->closed = 1;
    }
    return Qnil;
}

static VALUE
enclave_closed_p(VALUE self)
{
    rb_enclave_t *sb;
    TypedData_Get_Struct(self, rb_enclave_t, &enclave_data_type, sb);
    return sb->closed ? Qtrue : Qfalse;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void
Init_enclave(void)
{
    VALUE cEnclave = rb_define_class("Enclave", rb_cObject);

    /* Error class hierarchy */
    cEnclaveError = rb_define_class_under(cEnclave, "Error", rb_eStandardError);
    cEnclaveTimeoutError = rb_define_class_under(cEnclave, "TimeoutError", cEnclaveError);
    cEnclaveMemoryLimitError = rb_define_class_under(cEnclave, "MemoryLimitError", cEnclaveError);

    rb_gc_register_mark_object(cEnclaveError);
    rb_gc_register_mark_object(cEnclaveTimeoutError);
    rb_gc_register_mark_object(cEnclaveMemoryLimitError);

    rb_define_alloc_func(cEnclave, enclave_alloc);
    rb_define_method(cEnclave, "_init",            enclave_initialize,      2);
    rb_define_method(cEnclave, "_eval",            enclave_eval,            1);
    rb_define_method(cEnclave, "_define_function", enclave_define_function, 1);
    rb_define_method(cEnclave, "reset!",           enclave_reset,           0);
    rb_define_method(cEnclave, "close",            enclave_close,           0);
    rb_define_method(cEnclave, "closed?",          enclave_closed_p,        0);
}
