/*
 * sandbox_core.h — Opaque mruby sandbox interface
 *
 * This header is safe to include from both mruby-only and CRuby-only
 * translation units. It does NOT include mruby.h or ruby.h.
 */

#ifndef SANDBOX_CORE_H
#define SANDBOX_CORE_H

#include <stddef.h>

/* Opaque handle */
typedef struct sandbox_state sandbox_state_t;

/* Result from an eval */
typedef struct {
    char *value;     /* inspected return value (NULL on error) */
    char *output;    /* captured puts/print/p output */
    char *error;     /* error message (NULL on success) */
} sandbox_result_t;

sandbox_state_t *sandbox_state_new(void);
void             sandbox_state_free(sandbox_state_t *state);
sandbox_result_t sandbox_state_eval(sandbox_state_t *state, const char *code);
void             sandbox_state_reset(sandbox_state_t *state);
void             sandbox_result_free(sandbox_result_t *result);

#endif /* SANDBOX_CORE_H */
