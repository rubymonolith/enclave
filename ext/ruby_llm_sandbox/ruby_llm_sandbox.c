/*
 * ruby_llm_sandbox.c — Ruby C extension wrapper
 *
 * This file ONLY includes ruby.h, never mruby.h.
 * All mruby interaction goes through the opaque sandbox_core.h API.
 */

#include <ruby.h>
#include "sandbox_core.h"

/* ------------------------------------------------------------------ */
/* TypedData for Sandbox                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    sandbox_state_t *state;
    int              closed;
} rb_sandbox_t;

static void
rb_sandbox_free(void *ptr)
{
    rb_sandbox_t *sb = (rb_sandbox_t *)ptr;
    if (sb) {
        if (sb->state) {
            sandbox_state_free(sb->state);
            sb->state = NULL;
        }
        free(sb);
    }
}

static size_t
rb_sandbox_memsize(const void *ptr)
{
    return sizeof(rb_sandbox_t);
}

static const rb_data_type_t sandbox_data_type = {
    "Ruby::LLM::Sandbox",
    { NULL, rb_sandbox_free, rb_sandbox_memsize },
    NULL, NULL,
    RUBY_TYPED_FREE_IMMEDIATELY
};

static rb_sandbox_t *
get_sandbox(VALUE self)
{
    rb_sandbox_t *sb;
    TypedData_Get_Struct(self, rb_sandbox_t, &sandbox_data_type, sb);
    if (sb->closed) {
        rb_raise(rb_eRuntimeError, "sandbox is closed");
    }
    return sb;
}

/* ------------------------------------------------------------------ */
/* Sandbox#initialize                                                  */
/* ------------------------------------------------------------------ */

static VALUE
sandbox_alloc(VALUE klass)
{
    rb_sandbox_t *sb = calloc(1, sizeof(rb_sandbox_t));
    return TypedData_Wrap_Struct(klass, &sandbox_data_type, sb);
}

static VALUE
sandbox_initialize(VALUE self)
{
    rb_sandbox_t *sb;
    TypedData_Get_Struct(self, rb_sandbox_t, &sandbox_data_type, sb);

    sb->state = sandbox_state_new();
    if (!sb->state) {
        rb_raise(rb_eRuntimeError, "failed to initialize mruby sandbox");
    }
    sb->closed = 0;

    return self;
}

/* ------------------------------------------------------------------ */
/* Sandbox#_eval                                                       */
/* ------------------------------------------------------------------ */

static VALUE
sandbox_eval(VALUE self, VALUE rb_code)
{
    rb_sandbox_t *sb = get_sandbox(self);
    const char *code = StringValueCStr(rb_code);

    sandbox_result_t result = sandbox_state_eval(sb->state, code);

    VALUE value = result.value ? rb_str_new_cstr(result.value) : Qnil;
    VALUE output = result.output ? rb_str_new_cstr(result.output) : rb_str_new_cstr("");
    VALUE error = result.error ? rb_str_new_cstr(result.error) : Qnil;

    sandbox_result_free(&result);

    return rb_ary_new_from_args(3, value, output, error);
}

/* ------------------------------------------------------------------ */
/* Sandbox#reset!                                                      */
/* ------------------------------------------------------------------ */

static VALUE
sandbox_reset(VALUE self)
{
    rb_sandbox_t *sb = get_sandbox(self);
    sandbox_state_reset(sb->state);
    return self;
}

/* ------------------------------------------------------------------ */
/* Sandbox#close                                                       */
/* ------------------------------------------------------------------ */

static VALUE
sandbox_close(VALUE self)
{
    rb_sandbox_t *sb;
    TypedData_Get_Struct(self, rb_sandbox_t, &sandbox_data_type, sb);

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
sandbox_closed_p(VALUE self)
{
    rb_sandbox_t *sb;
    TypedData_Get_Struct(self, rb_sandbox_t, &sandbox_data_type, sb);
    return sb->closed ? Qtrue : Qfalse;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

void
Init_ruby_llm_sandbox(void)
{
    VALUE mRuby = rb_define_module("Ruby");
    VALUE mLLM  = rb_define_module_under(mRuby, "LLM");
    VALUE cSandbox = rb_define_class_under(mLLM, "Sandbox", rb_cObject);

    rb_define_alloc_func(cSandbox, sandbox_alloc);
    rb_define_method(cSandbox, "initialize", sandbox_initialize, 0);
    rb_define_method(cSandbox, "_eval",      sandbox_eval,       1);
    rb_define_method(cSandbox, "reset!",     sandbox_reset,      0);
    rb_define_method(cSandbox, "close",      sandbox_close,      0);
    rb_define_method(cSandbox, "closed?",    sandbox_closed_p,   0);
}
