#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define _GNU_SOURCE
#include "search.h"
#undef _GNU_SOURCE
#include "script_disass.h"
#include "script_gram.tab.h"
#include "script_lex.yy.h"
#include "script_parse_ctx.h"

/* Needed for script name lookup */
void init_script_handlers();
extern struct script_cmd_handler script_handlers[SCRIPT_NOPS];

/* Never freed */
static struct hsearch_data handlers_htab;

static bool init_handlers_htab() {
    static bool ok;
    if (ok)
        return true;

    if (hcreate_r(SCRIPT_NOPS, &handlers_htab) == 0) {
        perror("hcreate");
        return false;
    }
    for (size_t i = 0; i < SCRIPT_NOPS; i++) {
        if (!script_handlers[i].name)
            continue;

        ENTRY query = {.key = (void*)script_handlers[i].name, .data = (void*)i};
        ENTRY* entry;
        if (hsearch_r(query, ENTER, &entry, &handlers_htab) == 0) {
            perror("hsearch");
            hdestroy_r(&handlers_htab);
            return false;
        }
    }

    ok = true;
    return true;
}

bool script_parse_ctx_init(struct script_parse_ctx* ctx, const char* script) {
    ctx->ndiags = 0;
    ctx->nstmts = 0;
    ctx->script = script;
    ctx->filename = NULL;
    init_script_handlers();
    return init_handlers_htab();
}

bool script_parse_ctx_parse(struct script_parse_ctx* ctx) {
    yyscan_t scanner;

    if (script_lex_init_extra(ctx, &scanner))
        return false;
    YY_BUFFER_STATE st = script__scan_string(ctx->script, scanner);
    bool ret = script_parse(ctx, scanner) == 0 && ctx->ndiags == 0;

    script__delete_buffer(st, scanner);
    script_lex_destroy(scanner);

    return ret;
}

bool script_parse_ctx_add_diag(struct script_parse_ctx* ctx, const struct script_diag* diag) {
    if (ctx->ndiags >= SCRIPT_PARSE_DIAGS_SZ)
        return false;
    ctx->diags[ctx->ndiags++] = *diag;
    return true;
}

bool script_arg_list_add_arg(struct script_arg_list* args, const struct script_arg* arg) {
    if (args->nargs >= SCRIPT_PARSE_CTX_ARGS_SZ)
        return false;
    args->args[args->nargs++] = *arg;
    return true;
}

bool script_ctx_add_stmt(struct script_parse_ctx* ctx, const struct script_stmt* stmt) {
    return script_ctx_insert_next_stmt(ctx, stmt, NULL);
}

bool script_ctx_insert_next_stmt(struct script_parse_ctx* ctx, const struct script_stmt* stmt,
    struct script_stmt* prev) {
    if (ctx->nstmts >= SCRIPT_PARSE_CTX_STMTS_SZ)
        return false;

    /* If prev is not passed, we need to discover it in order */
    if (!prev && ctx->nstmts > 0) {
        prev = &ctx->stmts[0];
        while (prev->next)
            prev = prev->next;
    }

    struct script_stmt* dst = &ctx->stmts[ctx->nstmts++];
    *dst = *stmt;

    struct script_stmt* next = NULL;

    if (prev) {
        next = prev->next;
        prev->next = dst;
    }

    dst->prev = prev;
    dst->next = next;
    if (next)
        next->prev = dst;
    return true;
}

bool script_op_idx(const char* name, size_t* dst) {
    assert(name);
    if (!strncmp(name, "OP_", 3) && strlen(name) > 3) {
        size_t idx = strtoumax(&name[3], NULL, 0);
        if (!script_op_idx_chk(idx))
            return false;
        *dst = idx;
        return true;
    }

    ENTRY query = {.key = (char*)name, .data = NULL};
    ENTRY* res;
    if (hsearch_r(query, FIND, &res, &handlers_htab) == 0)
        return false;
    *dst = (size_t)res->data;
    return true;
}

bool script_op_idx_chk(size_t idx) {
    return idx < SCRIPT_NOPS;
}

void script_arg_free(const struct script_arg* arg) {
    if (arg->type == ARG_TY_STR || arg->type == ARG_TY_LABEL)
        free((void*)arg->str);
    else if (arg->type == ARG_TY_NUMBERED_STR)
        free((void*)arg->numbered_str.str);
}

void script_stmt_free(struct script_stmt* stmt, bool inorder) {
    assert(stmt);

    if (inorder) {
        struct script_stmt* prev = stmt->prev, * next = stmt->next;
        if (prev)
            prev->next = next;
        if (next)
            next->prev = prev;
    }

    if (stmt->label)
        free((void*)stmt->label);
    if (stmt->ty == STMT_TY_OP)
        for (int i = 0; i < stmt->op.args.nargs; i++)
            script_arg_free((void*)&stmt->op.args.args[i]);
}

void script_parse_ctx_free(struct script_parse_ctx* ctx) {
    for (size_t i = 0; i < ctx->nstmts; i++)
        script_stmt_free(&ctx->stmts[i], false);
}
