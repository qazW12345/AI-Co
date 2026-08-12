/* bootstrap/src/sema/fn_main_test.c
 *
 * WP-M0-13d2 entry-main and reserved-name tests: the entry-point
 * contract of spec sec. 15.3 (AIC-E0418, DIAGNOSTIC-CONTRACT sec.
 * 11.5) and the reserved built-in name rule of spec sec. 4.5.
 *
 * The tests run the full pipeline through load -> lex -> parse ->
 * name_resolve -> completeness -> layout -> convert -> optype ->
 * const_eval_check (12a) -> rec_fail_emit (12b2) -> expr_core_check
 * (13b1) -> expr_ops_check (13b2) -> stmt_core_check (13c1) ->
 * stmt_reach_check (13c2) -> fn_core_check (13d1) -> fn_main_check
 * (13d2) so the boundary tests can prove fn_main emits only its own
 * record:
 *   - AIC-E0412/E0413/E0414/E0420 (13c1), AIC-E0416/E0417 (13c2),
 *     AIC-E0415 (13d1), and AIC-N0207/N0208/N0209 (name) are never
 *     produced here;
 *   - reserved built-in names (sec. 4.5) never reach this stage:
 *     `cast`/`wrap`/`len`/`ptr` are lexer keywords, so a declaration
 *     spelling one of them is rejected by the parser with AIC-S0101,
 *     and `rt`/`rt.*` module names are rejected by the name phase
 *     (AIC-N0207/N0208/N0209); fn_main never emits a record for them.
 *
 * Corpus anchor note: tests/negative/cases/derived-semantic-main-
 * missing/input.ai is embedded verbatim below (test_e0418_missing)
 * and the record is asserted against the committed expected.json
 * (message "entry 'main' signature invalid or missing"; primary span
 * = the `module main;` declaration, line 1 cols 1..13, offsets
 * 0..12). The corpus directory itself is WP-M0-03's area and is not
 * modified here.
 *
 * Build (from the repository root; MSVC example):
 *   STAGE0_OUT_DIR='bootstrap\\stage0\\msvc-sema-d2' \
 *     ./bootstrap/build/build-stage0-msvc.cmd \
 *     bootstrap/src/sema/fn_main_test.c \
 *     bootstrap/src/sema/fn_main.c \
 *     bootstrap/src/sema/fn_core.c \
 *     bootstrap/src/sema/stmt_reach.c \
 *     bootstrap/src/sema/stmt_core.c \
 *     bootstrap/src/sema/expr_ops.c \
 *     bootstrap/src/sema/expr_core.c \
 *     bootstrap/src/const/eval_fail_rec.c \
 *     bootstrap/src/const/eval_fail_arith.c \
 *     bootstrap/src/const/eval_core.c \
 *     bootstrap/src/types/optype.c bootstrap/src/types/convert.c \
 *     bootstrap/src/types/layout.c bootstrap/src/types/type_identity.c \
 *     bootstrap/src/types/type_tables.c \
 *     bootstrap/src/name/name.c bootstrap/src/ast/ast.c \
 *     bootstrap/src/parse/parse.c bootstrap/src/lex/lex.c \
 *     bootstrap/src/load/load.c bootstrap/src/diag/diag.c \
 *     bootstrap/src/diag/diag_codes.c bootstrap/src/diag/diag_emit.c
 *   ./bootstrap/stage0/msvc-sema-d2/fn_main_test.exe
 * (repeat with build-stage0-clang.cmd / bootstrap\\stage0\\clang-sema-d2)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "fn_main.h"
#include "fn_core.h"
#include "stmt_reach.h"
#include "stmt_core.h"
#include "expr_core.h"
#include "expr_ops.h"
#include "../const/eval_fail_rec.h"

#include "../parse/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Full pipeline: load -> lex -> parse -> name_resolve -> completeness ->
 * layout -> convert -> optype -> const_eval_check (12a) ->
 * rec_fail_emit (12b2) -> expr_core_check (13b1) -> expr_ops_check (13b2)
 * -> stmt_core_check (13c1) -> stmt_reach_check (13c2) -> fn_core_check
 * (13d1) -> fn_main_check (13d2).
 * ------------------------------------------------------------------------- */

typedef struct Pipeline {
    LoadStatus ld;
    LexStatus lx;
    ParseStatus ps;
    LoadSource *src;
    LexToken *toks;
    size_t tn;
    AstNode *program;
    NameResult *result;
    DiagRecord **recs;      /* load/lex/parse/name-phase records */
    size_t rn;
    NameStatus st;
    DiagRecord **trecs;     /* completeness records */
    size_t trn;
    TypeCheckStatus tst;
    LayoutBuild *build;
    DiagRecord **lrecs;     /* layout records */
    size_t lrn;
    LayoutStatus lst;
    DiagRecord **crecs;     /* convert records */
    size_t crn;
    ConvertStatus cst;
    DiagRecord **orecs;     /* optype records */
    size_t orn;
    OptypeStatus ost;
    DiagRecord **erecs;     /* 12a const records */
    size_t ern;
    EvalFailureSite *efails;
    size_t efailn;
    ConstEvalStatus esc;
    DiagRecord **rrecs;     /* 12b2 records */
    size_t rrn;
    RecFailStatus rsc;
    DiagRecord **xrecs;     /* 13b1 records */
    size_t xrn;
    EvalFailureSite *xfails;
    size_t xfailn;
    ExprCoreStatus xsc;
    DiagRecord **opsrecs;   /* 13b2 records */
    size_t opsrn;
    ExprOpsStatus osc;
    DiagRecord **srecs;     /* 13c1 records (AIC-E0412..E0414/E0420) */
    size_t srn;
    StmtCoreStatus ssc;
    DiagRecord **rrecs2;    /* 13c2 records (AIC-E0416/E0417) */
    size_t rrn2;
    StmtReachStatus rsc2;
    DiagRecord **frecs;     /* 13d1 records (AIC-E0415) */
    size_t frn;
    FnCoreStatus fsc;
    DiagRecord **mrecs;     /* 13d2 records (AIC-E0418) */
    size_t mrn;
    FnMainStatus msc;
} Pipeline;

static void pipeline_run_mem(Pipeline *p, const char *src_text)
{
    memset(p, 0, sizeof(*p));
    p->ld = load_source_from_bytes("input.ai", (const uint8_t *)src_text,
                                   strlen(src_text), &p->src, &p->recs, &p->rn);
    CHECK(p->ld == LOAD_OK);
    if (p->ld != LOAD_OK) return;
    p->lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    CHECK(p->lx == LEX_OK);
    if (p->lx != LEX_OK) return;
    p->ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
    CHECK(p->ps == PARSE_OK);
    if (p->ps != PARSE_OK) return;
    p->st = name_resolve(".", "main", "input.ai", p->src, p->program,
                         &p->result, &p->recs, &p->rn);
    if (p->st != NAME_OK) return;
    p->tst = types_check_completeness(p->result, &p->trecs, &p->trn);
    if (p->tst != TYPE_CHECK_OK) return;
    p->lst = types_layout_build(p->result, &p->build, &p->lrecs, &p->lrn);
    if (p->lst != LAYOUT_OK && p->lst != LAYOUT_DIAG_ERROR &&
        p->lst != LAYOUT_UNEVALUABLE) return;
    p->cst = types_convert_check(p->result, &p->crecs, &p->crn);
    if (p->cst == CONVERT_DIAG_ERROR) return;
    p->ost = types_optype_check(p->result, &p->orecs, &p->orn);
    if (p->ost == OPTYPE_DIAG_ERROR) return;
    p->esc = const_eval_check(p->result, p->build, &p->erecs, &p->ern,
                              &p->efails, &p->efailn);
    p->rsc = rec_fail_emit(p->result, p->build, &p->rrecs, &p->rrn);
    p->xsc = expr_core_check(p->result, p->build, &p->xrecs, &p->xrn,
                             &p->xfails, &p->xfailn);
    p->osc = expr_ops_check(p->result, p->build, &p->opsrecs, &p->opsrn);
    p->ssc = stmt_core_check(p->result, p->build, &p->srecs, &p->srn);
    p->rsc2 = stmt_reach_check(p->result, p->build, &p->rrecs2, &p->rrn2);
    p->fsc = fn_core_check(p->result, p->build, &p->frecs, &p->frn);
    p->msc = fn_main_check(p->result, p->build, &p->mrecs, &p->mrn);
}

/* Load/lex/parse only, without CHECKing success: used for inputs the
 * parser must reject (reserved built-in names are lexer keywords, so
 * `fn cast()` etc. never parse). The standard runner's internal
 * CHECKs would otherwise fail on these intentionally-invalid inputs. */
static void pipeline_run_mem_parse_fail(Pipeline *p, const char *src_text)
{
    memset(p, 0, sizeof(*p));
    p->ld = load_source_from_bytes("input.ai", (const uint8_t *)src_text,
                                   strlen(src_text), &p->src, &p->recs, &p->rn);
    if (p->ld != LOAD_OK) return;
    p->lx = lex_tokenize(p->src, &p->toks, &p->tn, &p->recs, &p->rn);
    if (p->lx != LEX_OK) return;
    p->ps = parse_program(p->toks, p->tn, &p->program, &p->recs, &p->rn);
}

static void pipeline_free(Pipeline *p)
{
    name_result_free(p->result);
    name_records_free(p->recs, p->rn);
    types_records_free(p->trecs, p->trn);
    layout_build_free(p->build);
    types_records_free(p->lrecs, p->lrn);
    types_records_free(p->crecs, p->crn);
    types_records_free(p->orecs, p->orn);
    types_records_free(p->erecs, p->ern);
    free(p->efails);
    types_records_free(p->rrecs, p->rrn);
    types_records_free(p->xrecs, p->xrn);
    free(p->xfails);
    types_records_free(p->opsrecs, p->opsrn);
    types_records_free(p->srecs, p->srn);
    types_records_free(p->rrecs2, p->rrn2);
    types_records_free(p->frecs, p->frn);
    types_records_free(p->mrecs, p->mrn);
    ast_node_free(p->program);
    lex_tokens_free(p->toks, p->tn);
    load_source_free(p->src);
    memset(p, 0, sizeof(*p));
}

/* Check a record's code, phase/severity/recovery, and that its primary
 * span starts exactly at `marker` (and, when len > 0, spans exactly
 * `len` bytes). */
static void check_fail_span(const DiagRecord *r, const char *src,
                            const char *code, const char *marker,
                            int64_t len)
{
    const char *hit;
    int64_t off;
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->code && strcmp(r->code, code) == 0);
    CHECK(r->severity && strcmp(r->severity, DIAG_SEVERITY_ERROR) == 0);
    CHECK(r->phase && strcmp(r->phase, DIAG_PHASE_SEMANTIC) == 0);
    CHECK(r->recovery &&
          strcmp(r->recovery, DIAG_RECOVERY_AUTHORITATIVE) == 0);
    CHECK(r->message != NULL && r->message[0] != '\0');
    CHECK(r->primary_span != NULL);
    if (!r->primary_span) return;
    hit = strstr(src, marker);
    CHECK(hit != NULL);
    if (!hit) return;
    off = (int64_t)(hit - src);
    CHECK(r->primary_span->start.offset == off);
    if (len > 0) CHECK(r->primary_span->end.offset == off + len);
}

/* Check a record's message equals the exact expected text. */
static void check_message(const DiagRecord *r, const char *expect)
{
    CHECK(r != NULL);
    if (!r) return;
    CHECK(r->message && strcmp(r->message, expect) == 0);
}

/* ---------------------------------------------------------------------------
 * 1. Valid programs: main() -> i32, main() -> void, pub main, void main
 *    with a bare return, plus helper functions - all clean
 * ------------------------------------------------------------------------- */

static void test_entry_ok(void)
{
    static const char src_i32[] =
        "module main;\n"
        "fn helper(x: i32) -> i32 { return x + 1; }\n"
        "fn main() -> i32 { return helper(0); }\n";
    static const char src_void[] =
        "module main;\n"
        "fn main() -> void {}\n";
    static const char src_pub[] =
        "module main;\n"
        "pub fn main() -> i32 { return 0; }\n";
    static const char src_void_return[] =
        "module main;\n"
        "fn main() -> void { return; }\n";
    static const char src_void_blockend[] =
        "module main;\n"
        "fn main() -> void {\n"
        "  var x: i32 = 1;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src_i32);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_void);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_pub);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_void_return);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_void_blockend);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 2. E0418 corpus anchor: derived-semantic-main-missing (the committed
 *    input.ai is embedded verbatim; the record must match the committed
 *    expected.json: message, module-declaration span 1:1..13 /
 *    offsets 0..12)
 * ------------------------------------------------------------------------- */

static void test_e0418_missing(void)
{
    static const char src[] =
        "module main;\n"
        "fn notmain() -> i32 { return 0; }";
    Pipeline p;

    pipeline_run_mem(&p, src);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_message(p.mrecs[0], "entry 'main' signature invalid or missing");
    /* primary span = the whole module declaration `module main;`
     * (offsets 0..12), corpus-pinned */
    check_fail_span(p.mrecs[0], src, "AIC-E0418", "module main;", 12);
    CHECK(p.mrecs[0]->primary_span->start.line == 1);
    CHECK(p.mrecs[0]->primary_span->start.col == 1);
    CHECK(p.mrecs[0]->primary_span->end.line == 1);
    CHECK(p.mrecs[0]->primary_span->end.col == 13);
    /* the file name is the entry source's name */
    CHECK(p.mrecs[0]->primary_span->file &&
          strcmp(p.mrecs[0]->primary_span->file, "input.ai") == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 3. E0418 mis-typed main: parameters, non-i32/void return type,
 *    main declared as a non-function (struct / global var)
 * ------------------------------------------------------------------------- */

static void test_e0418_mistyped(void)
{
    static const char src_params[] =
        "module main;\n"
        "fn main(x: i32) -> i32 { return x; }\n";
    static const char src_ret_bool[] =
        "module main;\n"
        "fn main() -> bool { return true; }\n";
    static const char src_ret_u8[] =
        "module main;\n"
        "fn main() -> u8 { return 0u8; }\n";
    static const char src_ret_str[] =
        "module main;\n"
        "fn main() -> str { return \"\"; }\n";
    static const char src_struct_main[] =
        "module main;\n"
        "struct main { x: i32; }\n"
        "fn notmain() -> i32 { return 0; }\n";
    static const char src_var_main[] =
        "module main;\n"
        "var main: i32 = 0;\n"
        "fn notmain() -> i32 { return 0; }\n";
    Pipeline p;

    /* parameters */
    pipeline_run_mem(&p, src_params);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_message(p.mrecs[0], "entry 'main' signature invalid or missing");
    /* primary span = the whole fn main declaration (documented
     * decision; no corpus anchor): from `fn` through the closing `}` */
    check_fail_span(p.mrecs[0], src_params, "AIC-E0418", "fn main(x: i32)", 0);
    {
        const char *close = strchr(src_params, '}');
        CHECK(close != NULL);
        if (close) {
            CHECK(p.mrecs[0]->primary_span->end.offset ==
                  (int64_t)(close - src_params) + 1);
        }
    }
    pipeline_free(&p);

    /* non-i32/void return types */
    pipeline_run_mem(&p, src_ret_bool);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.mrecs[0], src_ret_bool, "AIC-E0418", "fn main()", 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_ret_u8);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.mrecs[0], src_ret_u8, "AIC-E0418", "fn main()", 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_ret_str);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.mrecs[0], src_ret_str, "AIC-E0418", "fn main()", 0);
    pipeline_free(&p);

    /* main declared as a struct (not a function) */
    pipeline_run_mem(&p, src_struct_main);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.mrecs[0], src_struct_main, "AIC-E0418",
                    "struct main", 0);
    {
        const char *close = strchr(src_struct_main, '}');
        CHECK(close != NULL);
        if (close) {
            CHECK(p.mrecs[0]->primary_span->end.offset ==
                  (int64_t)(close - src_struct_main) + 1);
        }
    }
    pipeline_free(&p);

    /* main declared as a global var (not a function) */
    pipeline_run_mem(&p, src_var_main);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.mrecs[0], src_var_main, "AIC-E0418", "var main", 0);
    {
        const char *semi = strchr(src_var_main + 13, ';');
        CHECK(semi != NULL);
        if (semi) {
            CHECK(p.mrecs[0]->primary_span->end.offset ==
                  (int64_t)(semi - src_var_main) + 1);
        }
    }
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 4. Reserved names (spec sec. 4.5): every reachable reserved spelling
 *    is rejected BEFORE this stage (parse AIC-S0101 for the lexer
 *    keyword built-ins; name AIC-N0207/N0208/N0209 for rt.*), and
 *    fn_main never emits a record for them. Near-miss names and a
 *    module-scope symbol literally named `rt` (FQN main.rt, not the
 *    reserved module name) are accepted.
 * ------------------------------------------------------------------------- */

static void test_reserved_names(void)
{
    /* lexer keywords: these can never parse as declarations */
    static const char src_fn_cast[] =
        "module main;\n"
        "fn cast() -> i32 { return 0; }\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_fn_wrap[] =
        "module main;\n"
        "fn wrap() -> void {}\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_fn_len[] =
        "module main;\n"
        "fn len() -> i32 { return 0; }\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_fn_ptr[] =
        "module main;\n"
        "fn ptr() -> i32 { return 0; }\n"
        "fn main() -> i32 { return 0; }\n";
    /* rt.* reserved module names: rejected by the name phase */
    static const char src_module_rt[] =
        "module rt;\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_import_bare_rt[] =
        "module main;\n"
        "import rt;\n"
        "fn main() -> i32 { return 0; }\n";
    static const char src_import_rt_zzz[] =
        "module main;\n"
        "import rt.zzz;\n"
        "fn main() -> i32 { return 0; }\n";
    /* near misses and the non-reserved `rt` symbol spelling: accepted */
    static const char src_near_miss[] =
        "module main;\n"
        "fn report() -> void {}\n"
        "fn mem() -> void {}\n"
        "fn alloc_bytes() -> void {}\n"
        "var lenx: i32 = 0;\n"
        "fn ptr_() -> void {}\n"
        "fn rt() -> void {}\n"
        "fn main() -> i32 { return 0; }\n";
    Pipeline p;
    size_t i;

    /* --- parse rejections (AIC-S0101) --- */
    {
        static const char *const kSpellings[] = {
            src_fn_cast, src_fn_wrap, src_fn_len, src_fn_ptr
        };
        for (i = 0; i < sizeof(kSpellings) / sizeof(kSpellings[0]); i++) {
            bool found = false;
            size_t j;
            pipeline_run_mem_parse_fail(&p, kSpellings[i]);
            CHECK(p.ld == LOAD_OK);
            CHECK(p.lx == LEX_OK);
            CHECK(p.ps != PARSE_OK);
            CHECK(p.rn >= 1);
            for (j = 0; j < p.rn; j++) {
                if (p.recs[j] && p.recs[j]->code &&
                    strcmp(p.recs[j]->code, "AIC-S0101") == 0) {
                    found = true;
                    break;
                }
            }
            CHECK(found);
            pipeline_free(&p);
        }
    }

    /* --- name-phase rejections (rt.*) --- */
    pipeline_run_mem(&p, src_module_rt);
    CHECK(p.st != NAME_OK);
    if (p.st != NAME_OK) {
        CHECK(p.rn >= 1 && p.recs[0] && p.recs[0]->code &&
              strcmp(p.recs[0]->code, "AIC-N0207") == 0);
    }
    pipeline_free(&p);

    pipeline_run_mem(&p, src_import_bare_rt);
    CHECK(p.st != NAME_OK);
    if (p.st != NAME_OK) {
        CHECK(p.rn >= 1 && p.recs[0] && p.recs[0]->code &&
              strcmp(p.recs[0]->code, "AIC-N0209") == 0);
    }
    pipeline_free(&p);

    pipeline_run_mem(&p, src_import_rt_zzz);
    CHECK(p.st != NAME_OK);
    if (p.st != NAME_OK) {
        CHECK(p.rn >= 1 && p.recs[0] && p.recs[0]->code &&
              strcmp(p.recs[0]->code, "AIC-N0208") == 0);
    }
    pipeline_free(&p);

    /* --- near misses and `rt` symbol: clean build, fn_main silent --- */
    pipeline_run_mem(&p, src_near_miss);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 5. Boundaries: fn_main emits only AIC-E0418; sibling sema records
 *    (E0412..E0417, E0420) and name records are never produced here;
 *    a mis-typed main coexists with sibling records (E0415 + E0418)
 * ------------------------------------------------------------------------- */

static void test_boundaries(void)
{
    /* E0415 (13d1) fires for `return 5;` in a void fn; fn_main stays
     * silent (valid main present) */
    static const char src_e0415[] =
        "module main;\n"
        "fn noop() -> void {\n"
        "  return 5;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* E0416 (13c2) fires; fn_main stays silent */
    static const char src_e0416[] =
        "module main;\n"
        "fn missing(n: i32) -> i32 {\n"
        "  if (n > 0) { return n; }\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* E0417 (13c2) fires; fn_main stays silent */
    static const char src_e0417[] =
        "module main;\n"
        "fn h() -> void {\n"
        "  return;\n"
        "  var x: i32 = 1;\n"
        "}\n"
        "fn main() -> i32 { return 0; }\n";
    /* a mis-typed main coexists with an E0415 return rule record:
     * fn_core emits E0415 (bare return in non-void) and fn_main emits
     * E0418 (main with parameters) - distinct rules, distinct spans */
    static const char src_e0415_e0418[] =
        "module main;\n"
        "fn main(x: i32) -> i32 {\n"
        "  return;\n"
        "}\n";
    Pipeline p;

    pipeline_run_mem(&p, src_e0415);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 1);
    check_fail_span(p.frecs[0], src_e0415, "AIC-E0415", "return 5;", 9);
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_e0416);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    check_fail_span(p.rrecs2[0], src_e0416, "AIC-E0416", "fn missing", 10);
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_e0417);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.rsc2 == STMT_REACH_DIAG_ERROR);
    CHECK(p.rrn2 == 1);
    check_fail_span(p.rrecs2[0], src_e0417, "AIC-E0417",
                    "var x: i32 = 1;", 15);
    CHECK(p.msc == FN_MAIN_OK);
    CHECK(p.mrn == 0);
    pipeline_free(&p);

    pipeline_run_mem(&p, src_e0415_e0418);
    CHECK(p.st == NAME_OK);
    if (p.st != NAME_OK) { pipeline_free(&p); return; }
    CHECK(p.fsc == FN_CORE_DIAG_ERROR);
    CHECK(p.frn == 1);
    check_fail_span(p.frecs[0], src_e0415_e0418, "AIC-E0415", "return;", 7);
    CHECK(p.msc == FN_MAIN_DIAG_ERROR);
    CHECK(p.mrn == 1);
    if (p.mrn != 1) { pipeline_free(&p); return; }
    check_fail_span(p.mrecs[0], src_e0415_e0418, "AIC-E0418",
                    "fn main(x: i32)", 0);
    {
        const char *close = strchr(src_e0415_e0418, '}');
        CHECK(close != NULL);
        if (close) {
            CHECK(p.mrecs[0]->primary_span->end.offset ==
                  (int64_t)(close - src_e0415_e0418) + 1);
        }
    }
    /* fn_main never emits sibling sema codes */
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0412") != 0);
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0413") != 0);
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0414") != 0);
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0415") != 0);
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0416") != 0);
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0417") != 0);
    CHECK(strcmp(p.mrecs[0]->code, "AIC-E0420") != 0);
    pipeline_free(&p);
}

/* ---------------------------------------------------------------------------
 * 6. Determinism: two runs produce byte-identical records
 * ------------------------------------------------------------------------- */

static void test_determinism(void)
{
    static const char src[] =
        "module main;\n"
        "fn main(x: i32) -> i32 {\n"
        "  return;\n"
        "}\n"
        "fn extra() -> void {\n"
        "  return 5;\n"
        "}\n";
    Pipeline p1, p2;
    size_t i;

    pipeline_run_mem(&p1, src);
    pipeline_run_mem(&p2, src);
    CHECK(p1.st == NAME_OK);
    if (p1.st != NAME_OK) { pipeline_free(&p1); pipeline_free(&p2); return; }
    CHECK(p1.msc == p2.msc);
    CHECK(p1.mrn == p2.mrn);
    CHECK(p1.mrn == 1);
    if (p1.mrn != p2.mrn || p1.mrn != 1) {
        pipeline_free(&p1);
        pipeline_free(&p2);
        return;
    }
    for (i = 0; i < p1.mrn; i++) {
        DiagBuf b1, b2;
        diag_buf_init(&b1);
        diag_buf_init(&b2);
        CHECK(diag_emit_record(&b1, p1.mrecs[i]));
        CHECK(diag_emit_record(&b2, p2.mrecs[i]));
        CHECK(b1.len == b2.len);
        CHECK(b1.len == 0 || memcmp(b1.data, b2.data, b1.len) == 0);
        diag_buf_free(&b1);
        diag_buf_free(&b2);
    }
    pipeline_free(&p1);
    pipeline_free(&p2);
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_entry_ok();
    fprintf(stderr, "after test_entry_ok\n");
    test_e0418_missing();
    fprintf(stderr, "after test_e0418_missing\n");
    test_e0418_mistyped();
    fprintf(stderr, "after test_e0418_mistyped\n");
    test_reserved_names();
    fprintf(stderr, "after test_reserved_names\n");
    test_boundaries();
    fprintf(stderr, "after test_boundaries\n");
    test_determinism();
    fprintf(stderr, "after test_determinism\n");

    if (g_failures) {
        fprintf(stderr, "fn_main_test: %d checks, %d FAILURES\n",
                g_checks, g_failures);
        return 1;
    }
    printf("fn_main_test: %d checks, 0 failures\n", g_checks);
    return 0;
}
