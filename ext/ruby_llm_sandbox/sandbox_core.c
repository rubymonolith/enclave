/*
 * sandbox_core.c — mruby sandbox implementation
 *
 * This file ONLY includes mruby headers, never ruby.h.
 * All interaction with CRuby goes through sandbox_core.h.
 */

#include "sandbox_core.h"

#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/string.h>
#include <mruby/proc.h>
#include <mruby/variable.h>
#include <mruby/error.h>
#include <mruby/array.h>
#include <mruby/irep.h>
#include <mruby/internal.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Output capture buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} output_buf_t;

static void
output_buf_init(output_buf_t *ob)
{
    ob->buf = NULL;
    ob->len = 0;
    ob->cap = 0;
}

static void
output_buf_free(output_buf_t *ob)
{
    if (ob->buf) {
        free(ob->buf);
        ob->buf = NULL;
    }
    ob->len = 0;
    ob->cap = 0;
}

static void
output_buf_reset(output_buf_t *ob)
{
    ob->len = 0;
    if (ob->buf) ob->buf[0] = '\0';
}

static void
output_buf_append(output_buf_t *ob, const char *str, size_t slen)
{
    if (slen == 0) return;
    size_t needed = ob->len + slen + 1;
    if (needed > ob->cap) {
        ob->cap = needed * 2;
        if (ob->cap < 256) ob->cap = 256;
        ob->buf = realloc(ob->buf, ob->cap);
    }
    memcpy(ob->buf + ob->len, str, slen);
    ob->len += slen;
    ob->buf[ob->len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Sandbox internal state                                              */
/* ------------------------------------------------------------------ */

struct sandbox_state {
    mrb_state    *mrb;
    mrb_ccontext *cxt;
    unsigned int  stack_keep;
    int           arena_idx;
    output_buf_t  output;
};

/* Key for storing output buffer pointer in mruby globals */
#define OUTPUT_BUF_KEY "$__sandbox_output_buf__"

static output_buf_t *
get_output_buf(mrb_state *mrb)
{
    mrb_value gv = mrb_gv_get(mrb, mrb_intern_cstr(mrb, OUTPUT_BUF_KEY));
    if (mrb_nil_p(gv)) return NULL;
    return (output_buf_t *)mrb_cptr(gv);
}

/* ------------------------------------------------------------------ */
/* mruby Kernel overrides for output capture                          */
/* ------------------------------------------------------------------ */

static mrb_value
sandbox_mrb_print(mrb_state *mrb, mrb_value self)
{
    mrb_int argc;
    mrb_value *argv;
    mrb_get_args(mrb, "*", &argv, &argc);

    output_buf_t *ob = get_output_buf(mrb);
    if (!ob) return mrb_nil_value();

    for (mrb_int i = 0; i < argc; i++) {
        mrb_value s = mrb_obj_as_string(mrb, argv[i]);
        output_buf_append(ob, RSTRING_PTR(s), RSTRING_LEN(s));
    }
    return mrb_nil_value();
}

static mrb_value
sandbox_mrb_puts(mrb_state *mrb, mrb_value self)
{
    mrb_int argc;
    mrb_value *argv;
    mrb_get_args(mrb, "*", &argv, &argc);

    output_buf_t *ob = get_output_buf(mrb);
    if (!ob) return mrb_nil_value();

    if (argc == 0) {
        output_buf_append(ob, "\n", 1);
    }
    else {
        for (mrb_int i = 0; i < argc; i++) {
            if (mrb_array_p(argv[i])) {
                mrb_int alen = RARRAY_LEN(argv[i]);
                mrb_value *aptr = RARRAY_PTR(argv[i]);
                for (mrb_int j = 0; j < alen; j++) {
                    mrb_value s = mrb_obj_as_string(mrb, aptr[j]);
                    output_buf_append(ob, RSTRING_PTR(s), RSTRING_LEN(s));
                    if (RSTRING_LEN(s) == 0 || RSTRING_PTR(s)[RSTRING_LEN(s)-1] != '\n') {
                        output_buf_append(ob, "\n", 1);
                    }
                }
            }
            else {
                mrb_value s = mrb_obj_as_string(mrb, argv[i]);
                output_buf_append(ob, RSTRING_PTR(s), RSTRING_LEN(s));
                if (RSTRING_LEN(s) == 0 || RSTRING_PTR(s)[RSTRING_LEN(s)-1] != '\n') {
                    output_buf_append(ob, "\n", 1);
                }
            }
        }
    }
    return mrb_nil_value();
}

static mrb_value
sandbox_mrb_p(mrb_state *mrb, mrb_value self)
{
    mrb_int argc;
    mrb_value *argv;
    mrb_get_args(mrb, "*", &argv, &argc);

    output_buf_t *ob = get_output_buf(mrb);
    if (!ob) return mrb_nil_value();

    for (mrb_int i = 0; i < argc; i++) {
        mrb_value s = mrb_funcall_argv(mrb, argv[i],
                        mrb_intern_lit(mrb, "inspect"), 0, NULL);
        if (mrb_string_p(s)) {
            output_buf_append(ob, RSTRING_PTR(s), RSTRING_LEN(s));
        }
        output_buf_append(ob, "\n", 1);
    }

    if (argc == 0) return mrb_nil_value();
    if (argc == 1) return argv[0];
    return mrb_ary_new_from_values(mrb, argc, argv);
}

/* ------------------------------------------------------------------ */
/* Internal: initialize an mrb_state with sandbox settings            */
/* ------------------------------------------------------------------ */

static void
sandbox_setup_mrb(sandbox_state_t *state)
{
    /* Store output buffer pointer in mruby global */
    mrb_gv_set(state->mrb, mrb_intern_cstr(state->mrb, OUTPUT_BUF_KEY),
               mrb_cptr_value(state->mrb, &state->output));

    /* Override Kernel#print, define Kernel#puts, override Kernel#p */
    struct RClass *kernel = state->mrb->kernel_module;
    mrb_define_method(state->mrb, kernel, "print", sandbox_mrb_print, MRB_ARGS_ANY());
    mrb_define_method(state->mrb, kernel, "puts",  sandbox_mrb_puts,  MRB_ARGS_ANY());
    mrb_define_method(state->mrb, kernel, "p",     sandbox_mrb_p,     MRB_ARGS_ANY());

    /* Initialize _ variable (like mirb) */
    struct mrb_parser_state *parser = mrb_parse_string(state->mrb, "_=nil", state->cxt);
    if (parser) {
        struct RProc *proc = mrb_generate_code(state->mrb, parser);
        if (proc) {
            mrb_vm_run(state->mrb, proc, mrb_top_self(state->mrb), 0);
            state->stack_keep = proc->body.irep->nlocals;
        }
        mrb_parser_free(parser);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

sandbox_state_t *
sandbox_state_new(void)
{
    sandbox_state_t *state = calloc(1, sizeof(sandbox_state_t));
    if (!state) return NULL;

    state->mrb = mrb_open();
    if (!state->mrb || state->mrb->exc) {
        free(state);
        return NULL;
    }

    state->cxt = mrb_ccontext_new(state->mrb);
    state->cxt->capture_errors = TRUE;
    mrb_ccontext_filename(state->mrb, state->cxt, "(sandbox)");

    state->stack_keep = 0;
    state->arena_idx = mrb_gc_arena_save(state->mrb);

    output_buf_init(&state->output);
    sandbox_setup_mrb(state);

    return state;
}

void
sandbox_state_free(sandbox_state_t *state)
{
    if (!state) return;
    if (state->cxt && state->mrb) {
        mrb_ccontext_free(state->mrb, state->cxt);
    }
    if (state->mrb) {
        mrb_close(state->mrb);
    }
    output_buf_free(&state->output);
    free(state);
}

static char *
strdup_safe(const char *s, size_t len)
{
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

sandbox_result_t
sandbox_state_eval(sandbox_state_t *state, const char *code)
{
    sandbox_result_t result = { NULL, NULL, NULL };

    output_buf_reset(&state->output);

    /* Parse */
    struct mrb_parser_state *parser = mrb_parser_new(state->mrb);
    if (!parser) {
        result.error = strdup_safe("parser allocation failed", 24);
        result.output = strdup_safe("", 0);
        return result;
    }

    parser->s = code;
    parser->send = code + strlen(code);
    parser->lineno = state->cxt->lineno;
    mrb_parser_parse(parser, state->cxt);

    /* Syntax error? */
    if (parser->nerr > 0) {
        char errbuf[1024];
        snprintf(errbuf, sizeof(errbuf), "SyntaxError: %s (line %d)",
                 parser->error_buffer[0].message,
                 parser->error_buffer[0].lineno - state->cxt->lineno + 1);
        mrb_parser_free(parser);

        result.error = strdup_safe(errbuf, strlen(errbuf));
        result.output = state->output.len > 0
            ? strdup_safe(state->output.buf, state->output.len)
            : strdup_safe("", 0);
        return result;
    }

    /* Generate bytecode */
    struct RProc *proc = mrb_generate_code(state->mrb, parser);
    mrb_parser_free(parser);

    if (!proc) {
        result.error = strdup_safe("code generation failed", 22);
        result.output = state->output.len > 0
            ? strdup_safe(state->output.buf, state->output.len)
            : strdup_safe("", 0);
        return result;
    }

    /* Adjust environment stack for local variable persistence (mirb pattern) */
    if (state->mrb->c->cibase->u.env) {
        struct REnv *e = mrb_vm_ci_env(state->mrb->c->cibase);
        if (e && MRB_ENV_LEN(e) < proc->body.irep->nlocals) {
            MRB_ENV_SET_LEN(e, proc->body.irep->nlocals);
        }
    }

    /* Execute */
    mrb_value mrb_result = mrb_vm_run(state->mrb, proc,
                                       mrb_top_self(state->mrb),
                                       state->stack_keep);
    state->stack_keep = proc->body.irep->nlocals;

    /* Collect output */
    result.output = state->output.len > 0
        ? strdup_safe(state->output.buf, state->output.len)
        : strdup_safe("", 0);

    /* Check for exception */
    if (state->mrb->exc) {
        mrb_value exc = mrb_obj_value(state->mrb->exc);
        mrb_value exc_str = mrb_funcall_argv(state->mrb, exc,
                              mrb_intern_lit(state->mrb, "inspect"), 0, NULL);
        if (mrb_string_p(exc_str)) {
            result.error = strdup_safe(RSTRING_PTR(exc_str), RSTRING_LEN(exc_str));
        }
        else {
            result.error = strdup_safe("unknown error", 13);
        }
        state->mrb->exc = NULL;
        mrb_gc_arena_restore(state->mrb, state->arena_idx);
        state->cxt->lineno++;
        return result;
    }

    /* Get inspect of result value */
    mrb_value result_str = mrb_funcall_argv(state->mrb, mrb_result,
                             mrb_intern_lit(state->mrb, "inspect"), 0, NULL);
    if (mrb_string_p(result_str)) {
        result.value = strdup_safe(RSTRING_PTR(result_str), RSTRING_LEN(result_str));
    }
    else {
        result.value = strdup_safe("(unprintable)", 13);
    }

    /* Store result in _ (like mirb) */
    if (state->mrb->c->ci->stack) {
        *(state->mrb->c->ci->stack + 1) = mrb_result;
    }

    mrb_gc_arena_restore(state->mrb, state->arena_idx);
    state->cxt->lineno++;

    return result;
}

void
sandbox_state_reset(sandbox_state_t *state)
{
    if (!state) return;

    /* Tear down */
    if (state->cxt) {
        mrb_ccontext_free(state->mrb, state->cxt);
        state->cxt = NULL;
    }
    if (state->mrb) {
        mrb_close(state->mrb);
        state->mrb = NULL;
    }
    output_buf_reset(&state->output);

    /* Recreate */
    state->mrb = mrb_open();
    if (!state->mrb) return;

    state->cxt = mrb_ccontext_new(state->mrb);
    state->cxt->capture_errors = TRUE;
    mrb_ccontext_filename(state->mrb, state->cxt, "(sandbox)");
    state->stack_keep = 0;
    state->arena_idx = mrb_gc_arena_save(state->mrb);

    sandbox_setup_mrb(state);
}

void
sandbox_result_free(sandbox_result_t *result)
{
    if (result->value) { free(result->value); result->value = NULL; }
    if (result->output) { free(result->output); result->output = NULL; }
    if (result->error) { free(result->error); result->error = NULL; }
}
