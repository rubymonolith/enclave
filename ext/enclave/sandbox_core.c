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
#include <mruby/error.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/irep.h>
#include <mruby/internal.h>
#include <mruby/class.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Memory tracking allocator                                           */
/* ------------------------------------------------------------------ */

/* Header prepended to every allocation for size tracking.
 * Aligned to max_align_t so the payload stays properly aligned. */
#define MEM_HEADER_SIZE \
    ((sizeof(size_t) + _Alignof(max_align_t) - 1) & ~(_Alignof(max_align_t) - 1))

typedef struct {
    size_t current;    /* current total bytes allocated */
    size_t limit;      /* 0 = unlimited */
    int    exceeded;   /* flag: set when limit was hit */
} mem_tracker_t;

static __thread mem_tracker_t *tl_mem_tracker = NULL;

static mem_tracker_t *
mem_tracker_activate(mem_tracker_t *tracker)
{
    mem_tracker_t *prev = tl_mem_tracker;
    tl_mem_tracker = tracker;
    return prev;
}

static void
mem_tracker_restore(mem_tracker_t *prev)
{
    tl_mem_tracker = prev;
}

/* Override mrb_basic_alloc_func from mruby's allocf.c.
 * Our object file is linked before libmruby.a, so this definition wins.
 * ALWAYS prepends a size_t header for tracking. The tracker (when active)
 * provides limit enforcement; headers are prepended regardless. */
void *
mrb_basic_alloc_func(void *ptr, size_t size)
{
    mem_tracker_t *tracker = tl_mem_tracker;

    /* Free */
    if (size == 0) {
        if (ptr) {
            char *hdr = (char *)ptr - MEM_HEADER_SIZE;
            size_t old_size = *(size_t *)hdr;
            if (tracker) tracker->current -= old_size;
            free(hdr);
        }
        return NULL;
    }

    /* Malloc */
    if (ptr == NULL) {
        if (tracker && tracker->limit > 0 &&
            (tracker->current + size) > tracker->limit) {
            tracker->exceeded = 1;
            return NULL; /* mruby will GC and retry, then raise NoMemoryError */
        }
        size_t total = MEM_HEADER_SIZE + size;
        char *block = (char *)malloc(total);
        if (!block) return NULL;
        *(size_t *)block = size;
        if (tracker) tracker->current += size;
        return block + MEM_HEADER_SIZE;
    }

    /* Realloc */
    {
        char *old_hdr = (char *)ptr - MEM_HEADER_SIZE;
        size_t old_size = *(size_t *)old_hdr;
        if (tracker && tracker->limit > 0 &&
            (tracker->current - old_size + size) > tracker->limit) {
            tracker->exceeded = 1;
            return NULL;
        }
        size_t total = MEM_HEADER_SIZE + size;
        char *new_block = (char *)realloc(old_hdr, total);
        if (!new_block) return NULL;
        if (tracker) tracker->current = tracker->current - old_size + size;
        *(size_t *)new_block = size;
        return new_block + MEM_HEADER_SIZE;
    }
}

/* ------------------------------------------------------------------ */
/* Timeout state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    struct timespec deadline;
    int             expired;
    unsigned int    check_counter;
} timeout_state_t;

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

#define SANDBOX_MAX_FUNCTIONS 64

struct sandbox_state {
    mrb_state    *mrb;
    mrb_ccontext *cxt;
    unsigned int  stack_keep;
    int           arena_idx;
    output_buf_t  output;

    /* Tool callback */
    sandbox_callback_func_t callback;
    void                   *callback_userdata;

    /* Registered function names (survive reset) */
    char *func_names[SANDBOX_MAX_FUNCTIONS];
    int   func_count;

    /* Resource limits */
    double          timeout_seconds;   /* 0 = unlimited */
    size_t          memory_limit;      /* 0 = unlimited */
    mem_tracker_t   mem_tracker;
    timeout_state_t timeout_state;
};

/* ------------------------------------------------------------------ */
/* Code fetch hook for timeout                                         */
/* ------------------------------------------------------------------ */

#define TIMEOUT_CHECK_INTERVAL 1024

static void
sandbox_code_fetch_hook(struct mrb_state *mrb, const struct mrb_irep *irep,
                        const mrb_code *pc, mrb_value *regs)
{
    sandbox_state_t *state = (sandbox_state_t *)mrb->ud;
    if (!state) return;

    timeout_state_t *ts = &state->timeout_state;
    if (ts->expired) return; /* already raised, avoid re-entry */

    /* Only check clock every N instructions */
    ts->check_counter++;
    if (ts->check_counter < TIMEOUT_CHECK_INTERVAL) return;
    ts->check_counter = 0;

    /* Check if deadline is set (zero means no timeout) */
    if (ts->deadline.tv_sec == 0 && ts->deadline.tv_nsec == 0) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (now.tv_sec > ts->deadline.tv_sec ||
        (now.tv_sec == ts->deadline.tv_sec && now.tv_nsec >= ts->deadline.tv_nsec)) {
        ts->expired = 1;
        mrb_raise(mrb, mrb_class_get(mrb, "RuntimeError"), "execution timeout exceeded");
    }
}

/* ------------------------------------------------------------------ */
/* sandbox_value_t helpers                                             */
/* ------------------------------------------------------------------ */

void
sandbox_value_free(sandbox_value_t *val)
{
    if (!val) return;
    switch (val->type) {
    case SANDBOX_VALUE_STRING:
        if (val->as.str.ptr) { free(val->as.str.ptr); val->as.str.ptr = NULL; }
        break;
    case SANDBOX_VALUE_ARRAY:
        for (size_t i = 0; i < val->as.arr.len; i++) {
            sandbox_value_free(&val->as.arr.items[i]);
        }
        free(val->as.arr.items);
        val->as.arr.items = NULL;
        break;
    case SANDBOX_VALUE_HASH:
        for (size_t i = 0; i < val->as.hash.len; i++) {
            sandbox_value_free(&val->as.hash.keys[i]);
            sandbox_value_free(&val->as.hash.vals[i]);
        }
        free(val->as.hash.keys);
        free(val->as.hash.vals);
        val->as.hash.keys = NULL;
        val->as.hash.vals = NULL;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* mruby → sandbox_value_t conversion                                 */
/* ------------------------------------------------------------------ */

/* Returns 0 on success, -1 on unsupported type (sets errbuf) */
static int
mrb_to_sandbox_value(mrb_state *mrb, mrb_value v, sandbox_value_t *out, char *errbuf, size_t errbuf_size)
{
    memset(out, 0, sizeof(*out));

    if (mrb_nil_p(v)) {
        out->type = SANDBOX_VALUE_NIL;
        return 0;
    }
    if (mrb_true_p(v)) {
        out->type = SANDBOX_VALUE_TRUE;
        return 0;
    }
    if (mrb_false_p(v)) {
        out->type = SANDBOX_VALUE_FALSE;
        return 0;
    }
    if (mrb_integer_p(v)) {
        out->type = SANDBOX_VALUE_INTEGER;
        out->as.i = (int64_t)mrb_integer(v);
        return 0;
    }
    if (mrb_float_p(v)) {
        out->type = SANDBOX_VALUE_FLOAT;
        out->as.f = mrb_float(v);
        return 0;
    }
    if (mrb_string_p(v)) {
        out->type = SANDBOX_VALUE_STRING;
        out->as.str.len = (size_t)RSTRING_LEN(v);
        out->as.str.ptr = malloc(out->as.str.len + 1);
        memcpy(out->as.str.ptr, RSTRING_PTR(v), out->as.str.len);
        out->as.str.ptr[out->as.str.len] = '\0';
        return 0;
    }
    if (mrb_symbol_p(v)) {
        /* Symbol → String */
        out->type = SANDBOX_VALUE_STRING;
        mrb_int slen;
        const char *sname = mrb_sym_name_len(mrb, mrb_symbol(v), &slen);
        out->as.str.len = (size_t)slen;
        out->as.str.ptr = malloc(out->as.str.len + 1);
        memcpy(out->as.str.ptr, sname, out->as.str.len);
        out->as.str.ptr[out->as.str.len] = '\0';
        return 0;
    }
    if (mrb_array_p(v)) {
        mrb_int alen = RARRAY_LEN(v);
        out->type = SANDBOX_VALUE_ARRAY;
        out->as.arr.len = (size_t)alen;
        out->as.arr.items = calloc((size_t)alen, sizeof(sandbox_value_t));
        for (mrb_int i = 0; i < alen; i++) {
            if (mrb_to_sandbox_value(mrb, mrb_ary_entry(v, i),
                                     &out->as.arr.items[i], errbuf, errbuf_size) != 0) {
                /* Clean up already-converted items */
                for (mrb_int j = 0; j < i; j++) {
                    sandbox_value_free(&out->as.arr.items[j]);
                }
                free(out->as.arr.items);
                out->as.arr.items = NULL;
                return -1;
            }
        }
        return 0;
    }
    if (mrb_hash_p(v)) {
        mrb_value keys = mrb_hash_keys(mrb, v);
        mrb_int hlen = RARRAY_LEN(keys);
        out->type = SANDBOX_VALUE_HASH;
        out->as.hash.len = (size_t)hlen;
        out->as.hash.keys = calloc((size_t)hlen, sizeof(sandbox_value_t));
        out->as.hash.vals = calloc((size_t)hlen, sizeof(sandbox_value_t));
        for (mrb_int i = 0; i < hlen; i++) {
            mrb_value k = mrb_ary_entry(keys, i);
            mrb_value val = mrb_hash_get(mrb, v, k);
            if (mrb_to_sandbox_value(mrb, k, &out->as.hash.keys[i], errbuf, errbuf_size) != 0 ||
                mrb_to_sandbox_value(mrb, val, &out->as.hash.vals[i], errbuf, errbuf_size) != 0) {
                for (mrb_int j = 0; j <= i; j++) {
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
    mrb_value cls_name = mrb_obj_as_string(mrb, mrb_funcall_argv(mrb, mrb_obj_value(mrb_obj_class(mrb, v)),
                           mrb_intern_lit(mrb, "name"), 0, NULL));
    snprintf(errbuf, errbuf_size, "TypeError: unsupported type for sandbox: %s",
             mrb_string_p(cls_name) ? RSTRING_PTR(cls_name) : "unknown");
    return -1;
}

/* ------------------------------------------------------------------ */
/* sandbox_value_t → mruby conversion                                 */
/* ------------------------------------------------------------------ */

static mrb_value
sandbox_value_to_mrb(mrb_state *mrb, const sandbox_value_t *val)
{
    switch (val->type) {
    case SANDBOX_VALUE_NIL:
        return mrb_nil_value();
    case SANDBOX_VALUE_TRUE:
        return mrb_true_value();
    case SANDBOX_VALUE_FALSE:
        return mrb_false_value();
    case SANDBOX_VALUE_INTEGER:
        return mrb_int_value(mrb, (mrb_int)val->as.i);
    case SANDBOX_VALUE_FLOAT:
        return mrb_float_value(mrb, (mrb_float)val->as.f);
    case SANDBOX_VALUE_STRING:
        return mrb_str_new(mrb, val->as.str.ptr, (mrb_int)val->as.str.len);
    case SANDBOX_VALUE_ARRAY: {
        mrb_value ary = mrb_ary_new_capa(mrb, (mrb_int)val->as.arr.len);
        for (size_t i = 0; i < val->as.arr.len; i++) {
            mrb_ary_push(mrb, ary, sandbox_value_to_mrb(mrb, &val->as.arr.items[i]));
        }
        return ary;
    }
    case SANDBOX_VALUE_HASH: {
        mrb_value hash = mrb_hash_new_capa(mrb, (mrb_int)val->as.hash.len);
        for (size_t i = 0; i < val->as.hash.len; i++) {
            mrb_hash_set(mrb, hash,
                sandbox_value_to_mrb(mrb, &val->as.hash.keys[i]),
                sandbox_value_to_mrb(mrb, &val->as.hash.vals[i]));
        }
        return hash;
    }
    }
    return mrb_nil_value();
}

/* ------------------------------------------------------------------ */
/* Trampoline: single C function for all registered tool functions     */
/* ------------------------------------------------------------------ */

static sandbox_state_t *
get_sandbox_state(mrb_state *mrb)
{
    return (sandbox_state_t *)mrb->ud;
}

static mrb_value
sandbox_function_trampoline(mrb_state *mrb, mrb_value self)
{
    sandbox_state_t *state = get_sandbox_state(mrb);
    if (!state || !state->callback) {
        mrb_raise(mrb, mrb_class_get(mrb, "RuntimeError"), "no tool callback registered");
        return mrb_nil_value();
    }

    /* Get the method name from the call info */
    const char *method_name = mrb_sym_name(mrb, mrb->c->ci->mid);

    /* Get args */
    mrb_int argc;
    mrb_value *argv;
    mrb_get_args(mrb, "*", &argv, &argc);

    /* Convert mruby args → sandbox_value_t[] */
    sandbox_value_t *sargs = NULL;
    char errbuf[256];
    errbuf[0] = '\0';

    if (argc > 0) {
        sargs = calloc((size_t)argc, sizeof(sandbox_value_t));
        for (mrb_int i = 0; i < argc; i++) {
            if (mrb_to_sandbox_value(mrb, argv[i], &sargs[i], errbuf, sizeof(errbuf)) != 0) {
                /* Clean up already-converted args */
                for (mrb_int j = 0; j < i; j++) {
                    sandbox_value_free(&sargs[j]);
                }
                free(sargs);
                mrb_raise(mrb, mrb_class_get(mrb, "TypeError"), errbuf);
                return mrb_nil_value();
            }
        }
    }

    /* Call the CRuby callback */
    sandbox_callback_result_t cb_result = state->callback(
        method_name, sargs, (int)argc, state->callback_userdata);

    /* Free the converted args */
    for (mrb_int i = 0; i < argc; i++) {
        sandbox_value_free(&sargs[i]);
    }
    free(sargs);

    /* Check for error from callback */
    if (cb_result.error) {
        char *err_copy = strdup(cb_result.error);
        free(cb_result.error);
        sandbox_value_free(&cb_result.value);
        mrb_raise(mrb, mrb_class_get(mrb, "RuntimeError"), err_copy);
        free(err_copy);
        return mrb_nil_value();
    }

    /* Convert result back to mruby */
    mrb_value ret = sandbox_value_to_mrb(mrb, &cb_result.value);
    sandbox_value_free(&cb_result.value);

    return ret;
}

/* ------------------------------------------------------------------ */
/* Register functions in mruby                                        */
/* ------------------------------------------------------------------ */

static void
register_functions_in_mrb(sandbox_state_t *state)
{
    struct RClass *kernel = state->mrb->kernel_module;
    for (int i = 0; i < state->func_count; i++) {
        mrb_define_method(state->mrb, kernel, state->func_names[i],
                          sandbox_function_trampoline, MRB_ARGS_ANY());
    }
}

static output_buf_t *
get_output_buf(mrb_state *mrb)
{
    sandbox_state_t *state = (sandbox_state_t *)mrb->ud;
    if (!state) return NULL;
    return &state->output;
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
    /* Override Kernel#print, define Kernel#puts, override Kernel#p */
    struct RClass *kernel = state->mrb->kernel_module;
    mrb_define_method(state->mrb, kernel, "print", sandbox_mrb_print, MRB_ARGS_ANY());
    mrb_define_method(state->mrb, kernel, "puts",  sandbox_mrb_puts,  MRB_ARGS_ANY());
    mrb_define_method(state->mrb, kernel, "p",     sandbox_mrb_p,     MRB_ARGS_ANY());

    /* Re-register tool functions (survives reset) */
    register_functions_in_mrb(state);

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
sandbox_state_new(double timeout, size_t memory_limit)
{
    sandbox_state_t *state = calloc(1, sizeof(sandbox_state_t));
    if (!state) return NULL;

    state->timeout_seconds = timeout;
    state->memory_limit = memory_limit;

    /* Activate tracker with limit=0 (unlimited) during init so all
     * allocations get the size header prepended. */
    state->mem_tracker.current = 0;
    state->mem_tracker.limit = 0;
    state->mem_tracker.exceeded = 0;
    mem_tracker_t *prev = mem_tracker_activate(&state->mem_tracker);

    state->mrb = mrb_open();

    if (!state->mrb || state->mrb->exc) {
        mem_tracker_restore(prev);
        free(state);
        return NULL;
    }

    /* Store sandbox_state in mrb->ud for the code_fetch_hook */
    state->mrb->ud = state;

    state->cxt = mrb_ccontext_new(state->mrb);
    state->cxt->capture_errors = TRUE;
    mrb_ccontext_filename(state->mrb, state->cxt, "(sandbox)");

    state->stack_keep = 0;
    state->arena_idx = mrb_gc_arena_save(state->mrb);

    output_buf_init(&state->output);
    sandbox_setup_mrb(state);

    mem_tracker_restore(prev);

    return state;
}

void
sandbox_state_free(sandbox_state_t *state)
{
    if (!state) return;

    /* Activate tracker around mrb_close so frees go through our allocator */
    state->mem_tracker.limit = 0; /* unlimited during teardown */
    mem_tracker_t *prev = mem_tracker_activate(&state->mem_tracker);

    if (state->cxt && state->mrb) {
        mrb_ccontext_free(state->mrb, state->cxt);
    }
    if (state->mrb) {
        mrb_close(state->mrb);
    }

    mem_tracker_restore(prev);

    output_buf_free(&state->output);
    for (int i = 0; i < state->func_count; i++) {
        free(state->func_names[i]);
    }
    free(state);
}

/* ------------------------------------------------------------------ */
/* Limit orchestration helpers                                         */
/* ------------------------------------------------------------------ */

/* Activate limits before eval. Returns prev tracker for restore. */
static mem_tracker_t *
sandbox_limits_begin(sandbox_state_t *state)
{
    state->mem_tracker.exceeded = 0;
    state->mem_tracker.limit = state->memory_limit;
    mem_tracker_t *prev = mem_tracker_activate(&state->mem_tracker);

    state->timeout_state.expired = 0;
    state->timeout_state.check_counter = 0;
    if (state->timeout_seconds > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double int_part;
        double frac = modf(state->timeout_seconds, &int_part);
        state->timeout_state.deadline.tv_sec = now.tv_sec + (time_t)int_part;
        state->timeout_state.deadline.tv_nsec = now.tv_nsec + (long)(frac * 1e9);
        if (state->timeout_state.deadline.tv_nsec >= 1000000000L) {
            state->timeout_state.deadline.tv_sec++;
            state->timeout_state.deadline.tv_nsec -= 1000000000L;
        }
        state->mrb->code_fetch_hook = sandbox_code_fetch_hook;
    } else {
        state->timeout_state.deadline.tv_sec = 0;
        state->timeout_state.deadline.tv_nsec = 0;
        state->mrb->code_fetch_hook = NULL;
    }

    return prev;
}

/* Stop enforcing limits but keep tracker active for post-exec mruby calls. */
static void
sandbox_limits_end(sandbox_state_t *state)
{
    state->mrb->code_fetch_hook = NULL;
    state->mem_tracker.limit = 0;
}

/* Classify error from flags, not string matching. */
static sandbox_error_kind_t
sandbox_classify_error(sandbox_state_t *state)
{
    if (state->timeout_state.expired) {
        return SANDBOX_ERROR_TIMEOUT;
    } else if (state->mem_tracker.exceeded) {
        return SANDBOX_ERROR_MEMORY_LIMIT;
    }
    return SANDBOX_ERROR_RUNTIME;
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
    sandbox_result_t result = { NULL, NULL, NULL, SANDBOX_ERROR_NONE };

    output_buf_reset(&state->output);

    mem_tracker_t *prev = sandbox_limits_begin(state);

    /* Parse */
    struct mrb_parser_state *parser = mrb_parser_new(state->mrb);
    if (!parser) {
        mem_tracker_restore(prev);
        result.error = strdup_safe("parser allocation failed", 24);
        result.error_kind = SANDBOX_ERROR_RUNTIME;
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
        mem_tracker_restore(prev);

        result.error = strdup_safe(errbuf, strlen(errbuf));
        result.error_kind = SANDBOX_ERROR_RUNTIME;
        result.output = state->output.len > 0
            ? strdup_safe(state->output.buf, state->output.len)
            : strdup_safe("", 0);
        return result;
    }

    /* Generate bytecode */
    struct RProc *proc = mrb_generate_code(state->mrb, parser);
    mrb_parser_free(parser);

    if (!proc) {
        mem_tracker_restore(prev);
        result.error = strdup_safe("code generation failed", 22);
        result.error_kind = SANDBOX_ERROR_RUNTIME;
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

    sandbox_limits_end(state);

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

        result.error_kind = sandbox_classify_error(state);

        state->mrb->exc = NULL;
        mrb_gc_arena_restore(state->mrb, state->arena_idx);
        state->cxt->lineno++;
        mem_tracker_restore(prev);
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
    mem_tracker_restore(prev);

    return result;
}

void
sandbox_state_reset(sandbox_state_t *state)
{
    if (!state) return;

    /* Activate tracker (unlimited) during teardown and recreate */
    state->mem_tracker.limit = 0;
    mem_tracker_t *prev = mem_tracker_activate(&state->mem_tracker);

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

    /* Recreate with tracked allocator (limit=0 during init) */
    state->mem_tracker.current = 0;
    state->mem_tracker.exceeded = 0;

    state->mrb = mrb_open();

    if (!state->mrb) {
        mem_tracker_restore(prev);
        return;
    }

    state->mrb->ud = state;

    state->cxt = mrb_ccontext_new(state->mrb);
    state->cxt->capture_errors = TRUE;
    mrb_ccontext_filename(state->mrb, state->cxt, "(sandbox)");
    state->stack_keep = 0;
    state->arena_idx = mrb_gc_arena_save(state->mrb);

    sandbox_setup_mrb(state);

    mem_tracker_restore(prev);
}

void
sandbox_result_free(sandbox_result_t *result)
{
    if (result->value) { free(result->value); result->value = NULL; }
    if (result->output) { free(result->output); result->output = NULL; }
    if (result->error) { free(result->error); result->error = NULL; }
}

/* ------------------------------------------------------------------ */
/* Tool callback API                                                   */
/* ------------------------------------------------------------------ */

void
sandbox_state_set_callback(sandbox_state_t *state,
                           sandbox_callback_func_t callback,
                           void *userdata)
{
    state->callback = callback;
    state->callback_userdata = userdata;
}

int
sandbox_state_define_function(sandbox_state_t *state, const char *name)
{
    if (state->func_count >= SANDBOX_MAX_FUNCTIONS) return -1;

    state->func_names[state->func_count] = strdup(name);
    state->func_count++;

    /* Register in the current mruby state */
    struct RClass *kernel = state->mrb->kernel_module;
    mrb_define_method(state->mrb, kernel, name,
                      sandbox_function_trampoline, MRB_ARGS_ANY());
    return 0;
}

