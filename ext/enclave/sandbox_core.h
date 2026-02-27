/*
 * sandbox_core.h — Opaque mruby sandbox interface
 *
 * This header is safe to include from both mruby-only and CRuby-only
 * translation units. It does NOT include mruby.h or ruby.h.
 */

#ifndef SANDBOX_CORE_H
#define SANDBOX_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Opaque handle */
typedef struct sandbox_state sandbox_state_t;

/* Result from an eval */
typedef struct {
    char *value;     /* inspected return value (NULL on error) */
    char *output;    /* captured puts/print/p output */
    char *error;     /* error message (NULL on success) */
} sandbox_result_t;

/* ------------------------------------------------------------------ */
/* Intermediate value type for the serialization boundary              */
/* ------------------------------------------------------------------ */

typedef enum {
    SANDBOX_VALUE_NIL,
    SANDBOX_VALUE_TRUE,
    SANDBOX_VALUE_FALSE,
    SANDBOX_VALUE_INTEGER,
    SANDBOX_VALUE_FLOAT,
    SANDBOX_VALUE_STRING,
    SANDBOX_VALUE_ARRAY,
    SANDBOX_VALUE_HASH
} sandbox_value_type_t;

typedef struct sandbox_value sandbox_value_t;

struct sandbox_value {
    sandbox_value_type_t type;
    union {
        int64_t  i;
        double   f;
        struct { char *ptr; size_t len; }                          str;
        struct { sandbox_value_t *items; size_t len; }             arr;
        struct { sandbox_value_t *keys; sandbox_value_t *vals; size_t len; } hash;
    } as;
};

/* Result from a tool callback */
typedef struct {
    sandbox_value_t value;  /* return value (valid when error is NULL) */
    char           *error;  /* error message (NULL on success, caller frees) */
} sandbox_callback_result_t;

/* Callback function pointer: called from mruby side, dispatches to CRuby */
typedef sandbox_callback_result_t (*sandbox_callback_func_t)(
    const char         *method_name,
    const sandbox_value_t *args,
    int                 argc,
    void               *userdata
);

/* Free a sandbox_value_t (recursive for arrays/hashes) */
void sandbox_value_free(sandbox_value_t *val);

/* Set the callback used to dispatch tool calls */
void sandbox_state_set_callback(sandbox_state_t *state,
                                sandbox_callback_func_t callback,
                                void *userdata);

/* Register a function name in the mruby sandbox (uses the trampoline) */
int sandbox_state_define_function(sandbox_state_t *state, const char *name);

/* ------------------------------------------------------------------ */
/* Core API                                                            */
/* ------------------------------------------------------------------ */

sandbox_state_t *sandbox_state_new(void);
void             sandbox_state_free(sandbox_state_t *state);
sandbox_result_t sandbox_state_eval(sandbox_state_t *state, const char *code);
void             sandbox_state_reset(sandbox_state_t *state);
void             sandbox_result_free(sandbox_result_t *result);

#endif /* SANDBOX_CORE_H */
