#include "rcd.h"
#include "linux.h"
#include "acid.h"
#include "../src/quark-internal.h"
#include "ifc.h"

#pragma librcd

list(fstr_t)* vis_snaps;
lwt_heap_t* vis_heap;

static void vis_init() {
    vis_heap = lwt_alloc_heap();
    switch_heap (vis_heap) {
        vis_snaps = new_list(fstr_t);
    }
}

static void vis_render(qk_ctx_t* qk) {
    switch_heap(vis_heap) {
        void qk_vis_render(qk_ctx_t* ctx, rio_t* out_h, list(fstr_t)* states);
        rio_t* fh = rio_file_open("qk-vis-dump.html", false, true);
        rio_file_truncate(fh, 0);
        qk_vis_render(qk, fh, vis_snaps);
    }
    lwt_alloc_free(vis_heap);
    vis_init();
    DBGFN("rendered snapshots");
}

static void vis_snapshot(qk_ctx_t* ctx) { switch_heap(vis_heap) {
    fstr_mem_t* qk_vis_dump_graph(qk_ctx_t* ctx);
    list_push_end(vis_snaps, fstr_t, fss(qk_vis_dump_graph(ctx)));
    DBGFN("taking snapshot #", list_count(vis_snaps, fstr_t));
}}

static void print_stats(qk_ctx_t* qk) { sub_heap {
    DBGFN(fss(json_stringify_pretty(qk_get_stats(qk))));
}}

static fstr_t test_get_db_path() {
    return concs("/var/tmp/.librcd-acid-test.", lwt_rdrand64());
}

static void test_open_new_qk(fstr_t db_path, qk_ctx_t** out_qk, acid_h** out_ah) {
    fstr_t data_path = concs(db_path, ".data");
    fstr_t journal_path = concs(db_path, ".jrnl");
    acid_h* ah = acid_open(data_path, journal_path, ACID_ADDR_0, 0);
    qk_opt_t opt = {
        // Allow test to be deterministic.
        .dtrm_seed = 1,
        // Very low target ipp for testing.
        .target_ipp = 4,
    };
    qk_ctx_t* qk = qk_open(ah, &opt);
    *out_qk = qk;
    *out_ah = ah;
}

static void test_rm_db(fstr_t db_path) { sub_heap {
    fstr_t data_path = concs(db_path, ".data");
    fstr_t journal_path = concs(db_path, ".jrnl");
    rio_file_unlink(data_path);
    rio_file_unlink(journal_path);
}}

static void test0() { sub_heap {
    rio_debug("running test0\n");
    qk_ctx_t* qk;
    acid_h* ah;
    fstr_t db_path = test_get_db_path();
    test_open_new_qk(db_path, &qk, &ah);
    //x-dbg/ vis_snapshot(qk);
    qk_insert(qk, "50", "fifty");
    qk_insert(qk, "25", "twentyfive");
    qk_insert(qk, "75", "seventyfive");
    qk_insert(qk, "30", "thirty");
    qk_insert(qk, "60", "sixty");
    qk_insert(qk, "90", "ninety");
    qk_insert(qk, "70", "seventy");
    qk_insert(qk, "80", "eighty");
    qk_insert(qk, "10", "ten");
    qk_insert(qk, "20", "twenty");
    //x-dbg/ vis_snapshot(qk);
    qk_insert(qk, "51", "fiftyone");
    qk_insert(qk, "26", "twentysix");
    qk_insert(qk, "76", "seventysix");
    qk_insert(qk, "31", "thirtyone");
    qk_insert(qk, "61", "sixtyone");
    qk_insert(qk, "91", "ninetyone");
    qk_insert(qk, "71", "seventyone");
    qk_insert(qk, "81", "eightyone");
    qk_insert(qk, "11", "eleven");
    qk_insert(qk, "21", "twentyone");
    //x-dbg/ vis_snapshot(qk);
    //x-dbg/ vis_render(qk);
    acid_fsync(ah);
    acid_close(ah);
    test_rm_db(db_path);
}}

static void test1() { sub_heap {
    rio_debug("running test1\n");
    qk_ctx_t* qk;
    acid_h* ah;
    fstr_t db_path = test_get_db_path();
    test_open_new_qk(db_path, &qk, &ah);
    //x-dbg/ vis_snapshot(qk);
    //x-dbg/ print_stats(qk);
    size_t n = 0;
    extern fstr_t capitals;
    bool found_andorra = false;
    for (fstr_t row, tail = capitals; fstr_iterate_trim(&tail, "\n", &row);) {
        fstr_t country, capital;
        if (!fstr_divide(row, ",", &country, &capital))
            continue;

        fstr_t value;
        atest(!qk_get(qk, capital, &value));
        atest(qk_insert(qk, capital, country));
        atest(qk_get(qk, capital, &value));

        atest(fstr_equal(value, country));
        if (fstr_equal(capital, "Andorra la Vella")) {
            found_andorra = true;
        }
        if (found_andorra) {
            atest(qk_get(qk, "Andorra la Vella", &value));
            atest(fstr_equal(value, "Andorra"));
        } else {
            atest(!qk_get(qk, "Andorra la Vella", &value));
        }
    }
    //x-dbg/ vis_snapshot(qk);
    //x-dbg/ vis_render(qk);
    //x-dbg/ print_stats(qk);
    for (fstr_t row, tail = capitals; fstr_iterate_trim(&tail, "\n", &row);) { sub_heap {
        fstr_t country, capital;
        if (!fstr_divide(row, ",", &country, &capital))
            continue;
        fstr_t value;
        //x-dbg/ rio_debug(concs("looking up [", capital, "]"));
        atest(qk_get(qk, capital, &value));
        atest(fstr_equal(value, country));
        atest(!qk_get(qk, concs(capital, "\x00"), &value));
    }}
    acid_fsync(ah);
    acid_close(ah);
    test_rm_db(db_path);
}}

static void test2() { sub_heap {
    rio_debug("running test2\n");

}}

void rcd_main(list(fstr_t)* main_args, list(fstr_t)* main_env) {
    vis_init();
    test0();
    test1();
    test2();
    rio_debug("tests done\n");
    exit(0);
}
