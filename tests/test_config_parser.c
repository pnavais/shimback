/* Standalone unit test for the hand-rolled TOML-subset config parser and
 * serializer -- no framework, just a sequence of checks tallying failures.
 * Run directly, or via `ctest`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

static int failures = 0;

static void check(bool cond, const char *desc) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", desc);
        failures++;
    }
}

static void check_str_eq(const char *desc, const char *expected, const char *actual) {
    bool ok = (expected == NULL && actual == NULL) ||
              (expected != NULL && actual != NULL && strcmp(expected, actual) == 0);
    if (!ok) {
        fprintf(stderr, "FAIL: %s: expected [%s], got [%s]\n", desc, expected ? expected : "(null)",
                actual ? actual : "(null)");
        failures++;
    }
}

static char *make_temp_path(const char *tag) {
    char *buf = xmalloc(256);
    snprintf(buf, 256, "/tmp/shimback_test_%s_%d.toml", tag, (int)getpid());
    return buf;
}

static void test_round_trip(void) {
    Config cfg;
    config_init(&cfg);
    cfg.version = 1;
    cfg.verbose = true;
    cfg.capture_timeout_ms = 5000;
    cfg.capture_limit_bytes = 16 * 1024 * 1024;

    size_t sed_idx = config_upsert(&cfg, "sed");
    cfg.shims[sed_idx].fallback = xstrdup("/usr/bin/sed");
    cfg.shims[sed_idx].policy = POLICY_EXIT_CODE;

    size_t awk_idx = config_upsert(&cfg, "awk");
    cfg.shims[awk_idx].source = xstrdup("/opt/homebrew/bin/gawk");
    cfg.shims[awk_idx].fallback = xstrdup("/usr/bin/awk");
    cfg.shims[awk_idx].policy = POLICY_HEURISTIC;
    cfg.shims[awk_idx].diagnostic = true;
    cfg.shims[awk_idx].force = true;
    cfg.shims[awk_idx].capture_timeout_set = true;
    cfg.shims[awk_idx].capture_timeout_ms = 500;
    cfg.shims[awk_idx].capture_limit_set = true;
    cfg.shims[awk_idx].capture_limit_bytes = 2048;
    cfg.shims[awk_idx].fallback_args = xmalloc(1 * sizeof(char *));
    cfg.shims[awk_idx].fallback_args[0] = xstrdup("--posix");
    cfg.shims[awk_idx].fallback_arg_count = 1;
    cfg.shims[awk_idx].error_patterns = xmalloc(3 * sizeof(char *));
    cfg.shims[awk_idx].error_patterns[0] = xstrdup("invalid option");
    cfg.shims[awk_idx].error_patterns[1] = xstrdup("illegal option");
    cfg.shims[awk_idx].error_patterns[2] = xstrdup("unrecognized option");
    cfg.shims[awk_idx].error_pattern_count = 3;

    size_t grep_idx = config_upsert(&cfg, "grep");
    cfg.shims[grep_idx].fallback = xstrdup("/usr/bin/grep");
    cfg.shims[grep_idx].policy = POLICY_EXIT_CODE_MATCH;
    cfg.shims[grep_idx].exit_codes = xmalloc(2 * sizeof(int));
    cfg.shims[grep_idx].exit_codes[0] = 2;
    cfg.shims[grep_idx].exit_codes[1] = 64;
    cfg.shims[grep_idx].exit_code_count = 2;

    size_t cagao_idx = config_upsert(&cfg, "cagao");
    cfg.shims[cagao_idx].source = xstrdup("/bin/ls");
    cfg.shims[cagao_idx].fallback = xstrdup("/usr/local/bin/eza");
    cfg.shims[cagao_idx].policy = POLICY_ROUTE_ARGS;
    cfg.shims[cagao_idx].strip_matched_args = true;
    cfg.shims[cagao_idx].route_args = xmalloc(1 * sizeof(char *));
    cfg.shims[cagao_idx].route_args[0] = xstrdup("x");
    cfg.shims[cagao_idx].route_arg_count = 1;
    cfg.shims[cagao_idx].source_args = xmalloc(2 * sizeof(char *));
    cfg.shims[cagao_idx].source_args[0] = xstrdup("-l");
    cfg.shims[cagao_idx].source_args[1] = xstrdup("-a");
    cfg.shims[cagao_idx].source_arg_count = 2;

    size_t split_idx = config_upsert(&cfg, "spool");
    cfg.shims[split_idx].source = xstrdup("/bin/foo");
    cfg.shims[split_idx].fallback = xstrdup("/bin/bar");
    cfg.shims[split_idx].policy = POLICY_SPLIT_ARGS;
    cfg.shims[split_idx].strip_matched_args = true;
    cfg.shims[split_idx].source_route_args = xmalloc(2 * sizeof(char *));
    cfg.shims[split_idx].source_route_args[0] = xstrdup("-1");
    cfg.shims[split_idx].source_route_args[1] = xstrdup("-2");
    cfg.shims[split_idx].source_route_arg_count = 2;
    cfg.shims[split_idx].fallback_route_args = xmalloc(3 * sizeof(char *));
    cfg.shims[split_idx].fallback_route_args[0] = xstrdup("-1");
    cfg.shims[split_idx].fallback_route_args[1] = xstrdup("-2");
    cfg.shims[split_idx].fallback_route_args[2] = xstrdup("-3");
    cfg.shims[split_idx].fallback_route_arg_count = 3;

    size_t ls_idx = config_upsert(&cfg, "ls");
    cfg.shims[ls_idx].source = xstrdup("/bin/ls");
    cfg.shims[ls_idx].fallback = NULL; /* optional, and unused, for rewrite */
    cfg.shims[ls_idx].policy = POLICY_REWRITE;
    cfg.shims[ls_idx].rewrite_from = xmalloc(2 * sizeof(char *));
    cfg.shims[ls_idx].rewrite_from[0] = xstrdup("--full");
    cfg.shims[ls_idx].rewrite_from[1] = xstrdup("all");
    cfg.shims[ls_idx].rewrite_from_count = 2;
    cfg.shims[ls_idx].rewrite_to = xmalloc(2 * sizeof(char *));
    cfg.shims[ls_idx].rewrite_to[0] = xstrdup("-ltrah");
    cfg.shims[ls_idx].rewrite_to[1] = xstrdup("-c -z -f backup.tar.gz");
    cfg.shims[ls_idx].rewrite_to_count = 2;

    char *path = make_temp_path("roundtrip");
    char errbuf[256];

    ConfigStatus st = config_save(&cfg, path, errbuf, sizeof(errbuf));
    check(st == CONFIG_OK, "round-trip: config_save succeeds");

    Config reloaded;
    st = config_load(path, &reloaded, errbuf, sizeof(errbuf));
    check(st == CONFIG_OK, "round-trip: config_load succeeds");
    check(reloaded.version == 1, "round-trip: version is 1");
    check(reloaded.verbose == true, "round-trip: verbose == true");
    check(reloaded.capture_timeout_ms == 5000, "round-trip: global capture_timeout_ms == 5000");
    check(reloaded.capture_limit_bytes == 16 * 1024 * 1024,
          "round-trip: global capture_limit_bytes == 16MiB");
    check(reloaded.count == 6, "round-trip: shim count is 6");

    ShimEntry *sed = config_find(&reloaded, "sed");
    check(sed != NULL, "round-trip: sed entry found");
    if (sed) {
        check_str_eq("sed.source", NULL, sed->source);
        check_str_eq("sed.fallback", "/usr/bin/sed", sed->fallback);
        check(sed->policy == POLICY_EXIT_CODE, "sed.policy == exit-code");
        check(sed->error_pattern_count == 0, "sed.error_pattern_count == 0");
        check(sed->diagnostic == false, "sed.diagnostic == false");
        check(sed->force == false, "sed.force == false");
        check(sed->capture_timeout_set == false, "sed.capture_timeout_set == false");
        check(sed->capture_limit_set == false, "sed.capture_limit_set == false");
    }

    ShimEntry *awk = config_find(&reloaded, "awk");
    check(awk != NULL, "round-trip: awk entry found");
    if (awk) {
        check_str_eq("awk.source", "/opt/homebrew/bin/gawk", awk->source);
        check_str_eq("awk.fallback", "/usr/bin/awk", awk->fallback);
        check(awk->policy == POLICY_HEURISTIC, "awk.policy == heuristic");
        check(awk->error_pattern_count == 3, "awk.error_pattern_count == 3");
        if (awk->error_pattern_count == 3) {
            check_str_eq("awk.error_patterns[0]", "invalid option", awk->error_patterns[0]);
            check_str_eq("awk.error_patterns[1]", "illegal option", awk->error_patterns[1]);
            check_str_eq("awk.error_patterns[2]", "unrecognized option", awk->error_patterns[2]);
        }
        check(awk->diagnostic == true, "awk.diagnostic == true");
        check(awk->force == true, "awk.force == true");
        check(awk->capture_timeout_set == true, "awk.capture_timeout_set == true");
        check(awk->capture_timeout_ms == 500, "awk.capture_timeout_ms == 500");
        check(awk->capture_limit_set == true, "awk.capture_limit_set == true");
        check(awk->capture_limit_bytes == 2048, "awk.capture_limit_bytes == 2048");
        check(awk->fallback_arg_count == 1, "awk.fallback_arg_count == 1");
        if (awk->fallback_arg_count == 1) {
            check_str_eq("awk.fallback_args[0]", "--posix", awk->fallback_args[0]);
        }
        check(awk->source_arg_count == 0, "awk.source_arg_count == 0 (none configured)");
    }

    ShimEntry *grep = config_find(&reloaded, "grep");
    check(grep != NULL, "round-trip: grep entry found");
    if (grep) {
        check(grep->policy == POLICY_EXIT_CODE_MATCH, "grep.policy == exit-code-match");
        check(grep->exit_code_count == 2, "grep.exit_code_count == 2");
        if (grep->exit_code_count == 2) {
            check(grep->exit_codes[0] == 2, "grep.exit_codes[0] == 2");
            check(grep->exit_codes[1] == 64, "grep.exit_codes[1] == 64");
        }
    }

    ShimEntry *cagao = config_find(&reloaded, "cagao");
    check(cagao != NULL, "round-trip: cagao entry found");
    if (cagao) {
        check_str_eq("cagao.source", "/bin/ls", cagao->source);
        check_str_eq("cagao.fallback", "/usr/local/bin/eza", cagao->fallback);
        check(cagao->policy == POLICY_ROUTE_ARGS, "cagao.policy == route-args");
        check(cagao->strip_matched_args == true, "cagao.strip_matched_args == true");
        check(cagao->route_arg_count == 1, "cagao.route_arg_count == 1");
        if (cagao->route_arg_count == 1) {
            check_str_eq("cagao.route_args[0]", "x", cagao->route_args[0]);
        }
        check(cagao->source_arg_count == 2, "cagao.source_arg_count == 2");
        if (cagao->source_arg_count == 2) {
            check_str_eq("cagao.source_args[0]", "-l", cagao->source_args[0]);
            check_str_eq("cagao.source_args[1]", "-a", cagao->source_args[1]);
        }
        check(cagao->fallback_arg_count == 0, "cagao.fallback_arg_count == 0 (none configured)");
    }

    ShimEntry *spool = config_find(&reloaded, "spool");
    check(spool != NULL, "round-trip: spool entry found");
    if (spool) {
        check(spool->policy == POLICY_SPLIT_ARGS, "spool.policy == split-args");
        check(spool->strip_matched_args == true, "spool.strip_matched_args == true");
        check(spool->source_route_arg_count == 2, "spool.source_route_arg_count == 2");
        if (spool->source_route_arg_count == 2) {
            check_str_eq("spool.source_route_args[0]", "-1", spool->source_route_args[0]);
            check_str_eq("spool.source_route_args[1]", "-2", spool->source_route_args[1]);
        }
        check(spool->fallback_route_arg_count == 3, "spool.fallback_route_arg_count == 3");
        if (spool->fallback_route_arg_count == 3) {
            check_str_eq("spool.fallback_route_args[0]", "-1", spool->fallback_route_args[0]);
            check_str_eq("spool.fallback_route_args[1]", "-2", spool->fallback_route_args[1]);
            check_str_eq("spool.fallback_route_args[2]", "-3", spool->fallback_route_args[2]);
        }
    }

    ShimEntry *ls = config_find(&reloaded, "ls");
    check(ls != NULL, "round-trip: ls entry found");
    if (ls) {
        check_str_eq("ls.source", "/bin/ls", ls->source);
        check_str_eq("ls.fallback", NULL, ls->fallback);
        check(ls->policy == POLICY_REWRITE, "ls.policy == rewrite");
        check(ls->rewrite_from_count == 2, "ls.rewrite_from_count == 2");
        check(ls->rewrite_to_count == 2, "ls.rewrite_to_count == 2");
        if (ls->rewrite_from_count == 2 && ls->rewrite_to_count == 2) {
            check_str_eq("ls.rewrite_from[0]", "--full", ls->rewrite_from[0]);
            check_str_eq("ls.rewrite_to[0]", "-ltrah", ls->rewrite_to[0]);
            check_str_eq("ls.rewrite_from[1]", "all", ls->rewrite_from[1]);
            check_str_eq("ls.rewrite_to[1]", "-c -z -f backup.tar.gz", ls->rewrite_to[1]);
        }
    }

    config_free(&cfg);
    config_free(&reloaded);
    unlink(path);
    free(path);
}

static void test_literal_parse(void) {
    const char *literal =
        "# a leading comment\n"
        "version = 1\n"
        "\n"
        "[shims.sed]\n"
        "fallback = \"/usr/bin/sed\" # trailing comment\n"
        "policy = \"exit-code\"\n";

    char *path = make_temp_path("literal");
    FILE *f = fopen(path, "wb");
    check(f != NULL, "literal: temp file created");
    if (f) {
        fwrite(literal, 1, strlen(literal), f);
        fclose(f);
    }

    Config cfg;
    char errbuf[256];
    ConfigStatus st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_OK, "literal: config_load succeeds");
    check(cfg.count == 1, "literal: one shim entry");

    ShimEntry *sed = config_find(&cfg, "sed");
    check(sed != NULL, "literal: sed entry found");
    if (sed) {
        check_str_eq("literal: trailing comment stripped from fallback", "/usr/bin/sed",
                     sed->fallback);
    }

    config_free(&cfg);
    unlink(path);
    free(path);
}

static void test_validation_errors(void) {
    const char *missing_fallback = "version = 1\n\n[shims.sed]\npolicy = \"exit-code\"\n";
    const char *missing_patterns =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\npolicy = \"heuristic\"\n";
    const char *missing_exit_codes =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\npolicy = \"exit-code-match\"\n";
    const char *missing_route_args =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\npolicy = \"route-args\"\n";
    const char *missing_split_args =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\npolicy = \"split-args\"\n"
        "source_route_args = [\"-1\"]\n";
    const char *missing_rewrite_rules =
        "version = 1\n\n[shims.sed]\nsource = \"/bin/ls\"\npolicy = \"rewrite\"\n";
    const char *mismatched_rewrite_arrays =
        "version = 1\n\n[shims.sed]\nsource = \"/bin/ls\"\npolicy = \"rewrite\"\n"
        "rewrite_from = [\"a\", \"b\"]\nrewrite_to = [\"x\"]\n";

    char *path = make_temp_path("invalid");
    Config cfg;
    char errbuf[256];

    FILE *f = fopen(path, "wb");
    fwrite(missing_fallback, 1, strlen(missing_fallback), f);
    fclose(f);
    ConfigStatus st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION, "missing fallback is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(missing_patterns, 1, strlen(missing_patterns), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION, "heuristic without error_patterns is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(missing_exit_codes, 1, strlen(missing_exit_codes), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION, "exit-code-match without exit_codes is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(missing_route_args, 1, strlen(missing_route_args), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION, "route-args without route_args is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(missing_split_args, 1, strlen(missing_split_args), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION,
          "split-args with only source_route_args (no fallback_route_args) is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(missing_rewrite_rules, 1, strlen(missing_rewrite_rules), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION, "rewrite without rewrite rules is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(mismatched_rewrite_arrays, 1, strlen(mismatched_rewrite_arrays), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_VALIDATION,
          "mismatched rewrite_from/rewrite_to array lengths are rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    /* A malformed capture_timeout_ms must be a clear parse error, not
     * silently accepted as 0 (which, since it directly drives dispatch's
     * capture cutover, would silently change a shim's behavior -- see
     * review.md). Checked at both the top level and per-shim. */
    const char *bad_global_timeout = "version = 1\ncapture_timeout_ms = nope\n";
    const char *bad_shim_timeout = "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\n"
                                    "policy = \"exit-code\"\ncapture_timeout_ms = nope\n";
    const char *negative_timeout = "version = 1\ncapture_timeout_ms = -5\n";
    const char *overflow_timeout = "version = 1\ncapture_timeout_ms = 99999999999999999999\n";

    f = fopen(path, "wb");
    fwrite(bad_global_timeout, 1, strlen(bad_global_timeout), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "non-numeric global capture_timeout_ms is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(bad_shim_timeout, 1, strlen(bad_shim_timeout), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "non-numeric per-shim capture_timeout_ms is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(negative_timeout, 1, strlen(negative_timeout), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "negative capture_timeout_ms is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(overflow_timeout, 1, strlen(overflow_timeout), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "overflowing capture_timeout_ms is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    /* A [shims.<name>] section name containing '/'/'..' must be rejected
     * at load time, not silently accepted and trusted downstream -- it
     * would otherwise reach path construction (split_config_filename, via
     * uninstall --full's sweep, doctor, list, ...) with traversal
     * components still active (see review.md). */
    const char *traversal_name =
        "version = 1\n\n[shims.../../../victim]\nfallback = \"/bin/echo\"\n"
        "policy = \"exit-code\"\n";
    f = fopen(path, "wb");
    fwrite(traversal_name, 1, strlen(traversal_name), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "a path-traversal shim name in a section header is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    const char *bad_char_name =
        "version = 1\n\n[shims.bad#name]\nfallback = \"/bin/echo\"\npolicy = \"exit-code\"\n";
    f = fopen(path, "wb");
    fwrite(bad_char_name, 1, strlen(bad_char_name), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "a '#'-containing shim name in a section header is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    /* Trailing garbage after a value must be a parse error, not silently
     * ignored -- an operator mistake like `fallback = "/bin/echo" garbage`
     * used to be accepted, with " garbage" simply dropped (and then
     * dropped for good the next time shimback rewrote the file), hiding
     * what was actually a malformed line instead of rejecting it. Checked
     * across a quoted-string field, an array field, a scalar-within-a-
     * quoted-string field (capture_limit), and the top-level version key,
     * both at global and per-shim scope where that distinction exists (see
     * review.md). */
    const char *trailing_garbage_fallback =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\" garbage\n"
        "policy = \"exit-code\"\n";
    const char *trailing_garbage_policy =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\n"
        "policy = \"exit-code\" garbage\n";
    const char *trailing_garbage_array =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\npolicy = \"exit-code\"\n"
        "source_args = [\"-n\"] garbage\n";
    const char *trailing_garbage_capture_limit_shim =
        "version = 1\n\n[shims.sed]\nfallback = \"/usr/bin/sed\"\npolicy = \"exit-code\"\n"
        "capture_limit = \"8MiB\" garbage\n";
    const char *trailing_garbage_capture_limit_global =
        "version = 1\ncapture_limit = \"8MiB\" garbage\n";
    const char *trailing_garbage_version = "version = 1 garbage\n";

    f = fopen(path, "wb");
    fwrite(trailing_garbage_fallback, 1, strlen(trailing_garbage_fallback), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "trailing garbage after a quoted-string value is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(trailing_garbage_policy, 1, strlen(trailing_garbage_policy), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "trailing garbage after 'policy' is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(trailing_garbage_array, 1, strlen(trailing_garbage_array), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "trailing garbage after an array value is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(trailing_garbage_capture_limit_shim, 1, strlen(trailing_garbage_capture_limit_shim), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "trailing garbage after per-shim 'capture_limit' is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(trailing_garbage_capture_limit_global, 1, strlen(trailing_garbage_capture_limit_global),
           f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "trailing garbage after global 'capture_limit' is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    f = fopen(path, "wb");
    fwrite(trailing_garbage_version, 1, strlen(trailing_garbage_version), f);
    fclose(f);
    st = config_load(path, &cfg, errbuf, sizeof(errbuf));
    check(st == CONFIG_ERR_PARSE, "trailing garbage after 'version' is rejected");
    if (st == CONFIG_OK) {
        config_free(&cfg);
    }

    unlink(path);
    free(path);
}

static void test_missing_file_is_empty_config(void) {
    Config cfg;
    char errbuf[256];
    ConfigStatus st = config_load("/tmp/shimback_test_does_not_exist.toml", &cfg, errbuf,
                                   sizeof(errbuf));
    check(st == CONFIG_OK, "missing config file is not an error");
    check(cfg.count == 0, "missing config file yields zero shims");
    check(cfg.verbose == false, "missing config file defaults verbose to false");
    check(cfg.capture_timeout_ms == SHIMBACK_DEFAULT_CAPTURE_TIMEOUT_MS,
          "missing config file defaults capture_timeout_ms to the hardcoded default");
    check(cfg.capture_limit_bytes == SHIMBACK_DEFAULT_CAPTURE_LIMIT_BYTES,
          "missing config file defaults capture_limit_bytes to the hardcoded default");
    config_free(&cfg);
}

static void test_parse_size_bytes(void) {
    size_t v;

    check(parse_size_bytes("0", &v) && v == 0, "parse_size_bytes: 0 bytes");
    check(parse_size_bytes("8388608", &v) && v == 8388608, "parse_size_bytes: plain bytes");
    check(parse_size_bytes("8B", &v) && v == 8, "parse_size_bytes: 8B");
    check(parse_size_bytes("8b", &v) && v == 8, "parse_size_bytes: 8b (lowercase)");
    check(parse_size_bytes("1K", &v) && v == 1000, "parse_size_bytes: 1K == 1000");
    check(parse_size_bytes("1KB", &v) && v == 1000, "parse_size_bytes: 1KB == 1000");
    check(parse_size_bytes("1kb", &v) && v == 1000, "parse_size_bytes: 1kb (lowercase)");
    check(parse_size_bytes("1Ki", &v) && v == 1024, "parse_size_bytes: 1Ki == 1024");
    check(parse_size_bytes("1KiB", &v) && v == 1024, "parse_size_bytes: 1KiB == 1024");
    check(parse_size_bytes("1kib", &v) && v == 1024, "parse_size_bytes: 1kib (lowercase)");
    check(parse_size_bytes("8MiB", &v) && v == 8u * 1024 * 1024, "parse_size_bytes: 8MiB");
    check(parse_size_bytes("1M", &v) && v == 1000000, "parse_size_bytes: 1M == 1000000");
    check(parse_size_bytes("1MB", &v) && v == 1000000, "parse_size_bytes: 1MB == 1000000");
    check(parse_size_bytes("1G", &v) && v == 1000000000ULL, "parse_size_bytes: 1G == 1e9");
    check(parse_size_bytes("1GiB", &v) && v == 1024ULL * 1024 * 1024,
          "parse_size_bytes: 1GiB == 1024^3");
    check(parse_size_bytes("8192KiB", &v) && v == 8u * 1024 * 1024,
          "parse_size_bytes: 8192KiB == 8MiB (equivalent forms agree)");

    check(!parse_size_bytes("", &v), "parse_size_bytes: empty string rejected");
    check(!parse_size_bytes("MiB", &v), "parse_size_bytes: no leading digits rejected");
    check(!parse_size_bytes("-5MiB", &v), "parse_size_bytes: negative value rejected");
    check(!parse_size_bytes("5XB", &v), "parse_size_bytes: unrecognized suffix rejected");
    check(!parse_size_bytes("5 MB", &v), "parse_size_bytes: whitespace before suffix rejected");
    check(!parse_size_bytes("99999999999999999999999999GiB", &v),
          "parse_size_bytes: overflow rejected");
}

int main(void) {
    test_round_trip();
    test_literal_parse();
    test_validation_errors();
    test_parse_size_bytes();
    test_missing_file_is_empty_config();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
