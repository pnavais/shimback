#include "commands.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cli.h"
#include "../config.h"
#include "../paths.h"
#include "../util.h"

static const char *USAGE = "usage: shimback completions bash|zsh|fish|powershell\n";

/* ---- Per-command completion data: the one place flag names live for
 * completion purposes, independent of (and not derived from) each
 * command's own getopt_long table or cli.c's COMMAND_HELP prose -- the
 * same accepted duplication this project already has between those two
 * (see this feature's own plan). Keep in sync by hand when a command's
 * flags change, same discipline COMMAND_HELP already needs. ---- */

typedef struct {
    const char *long_name; /* without leading "--" */
    char short_name;       /* '\0' if this flag has no short form */
} FlagSpec;

typedef struct {
    const char *command; /* canonical name, matching a cli_command_name() string */
    const FlagSpec *flags;
    size_t flag_count;
    bool takes_shim_name; /* remove/edit/info: position-2 argument completes from
                            * `shimback __complete-names`'s live output, not a
                            * fixed list -- see cmd_complete_names. */
} CommandSpec;

static const FlagSpec ADD_FLAGS[] = {
    {"source", 's'},
    {"source-arg", '\0'},
    {"fallback", 'f'},
    {"fallback-arg", '\0'},
    {"policy", 'p'},
    {"error-pattern", 'e'},
    {"exit-code", 'x'},
    {"route-arg", 'r'},
    {"strip-matched-args", '\0'},
    {"split-source-arg", '\0'},
    {"split-fallback-arg", '\0'},
    {"rewrite", 'w'},
    {"route", '\0'},
    {"diagnostic", 'd'},
    {"force", '\0'},
    {"verbose", 'v'},
    {"capture-timeout", '\0'},
    {"capture-limit", '\0'},
    {"split-config", '\0'},
};
static const FlagSpec REMOVE_FLAGS[] = {{"yes", 'y'}};
static const FlagSpec LIST_FLAGS[] = {{"full", 'f'}};
static const FlagSpec INSTALL_FLAGS[] = {
    {"prefix", 'p'},
    {"shell", 's'},
    {"all", 'a'},
    {"force", 'f'},
};
static const FlagSpec UNINSTALL_FLAGS[] = {{"prefix", 'p'}, {"full", 'f'}};
static const FlagSpec UPDATE_FLAGS[] = {{"check", 'c'}, {"yes", 'y'}};
static const FlagSpec EXPORT_FLAGS[] = {{"output", 'o'}, {"yes", 'y'}, {"override", '\0'}};

#define FLAGS(arr) arr, sizeof(arr) / sizeof(arr[0])

static const CommandSpec COMMAND_SPECS[] = {
    {"add", FLAGS(ADD_FLAGS), false},
    {"remove", FLAGS(REMOVE_FLAGS), true},
    {"init", NULL, 0, false},
    {"list", FLAGS(LIST_FLAGS), false},
    {"doctor", NULL, 0, false}, /* "fix [-y|--yes]" handled as its own hand-written
                                 * special case in each shell script below -- the
                                 * only "flags after a literal positional" shape in
                                 * the whole CLI, not worth generalizing the data
                                 * model in this table for just one instance. */
    {"install", FLAGS(INSTALL_FLAGS), false},
    {"uninstall", FLAGS(UNINSTALL_FLAGS), false},
    {"edit", NULL, 0, true},
    {"info", NULL, 0, true},
    {"update", FLAGS(UPDATE_FLAGS), false},
    {"export", FLAGS(EXPORT_FLAGS), false},
    {"completions", NULL, 0, false}, /* its own "bash|zsh|fish|powershell" literal
                                       * list also hand-written per shell below,
                                       * same reasoning as doctor's "fix". */
};
#define COMMAND_SPEC_COUNT (sizeof(COMMAND_SPECS) / sizeof(COMMAND_SPECS[0]))

static const CommandSpec *find_spec(const char *name) {
    for (size_t i = 0; i < COMMAND_SPEC_COUNT; i++) {
        if (strcmp(COMMAND_SPECS[i].command, name) == 0) {
            return &COMMAND_SPECS[i];
        }
    }
    return NULL;
}

/* ---- Shared data-emission helpers, reused across shell generators ---- */

/* Appends every subcommand's own name and alias (if any), `sep`-joined,
 * reading live from cli.h's accessors rather than a second hardcoded
 * list -- see this feature's own plan for why this one part doesn't need
 * the hand-duplication the flag tables above do. */
static void append_command_names(DynBuf *out, const char *sep) {
    bool first = true;
    for (size_t i = 0;; i++) {
        const char *name = cli_command_name(i);
        if (!name) {
            break;
        }
        if (!first) {
            dynbuf_append_str(out, sep);
        }
        dynbuf_append_str(out, name);
        first = false;
        const char *alias = cli_command_alias(i);
        if (alias) {
            dynbuf_append_str(out, sep);
            dynbuf_append_str(out, alias);
        }
    }
}

/* Appends `sep`-joined "name|alias|..." for every command with
 * takes_shim_name set -- the case/switch pattern each shell script uses
 * to decide when to shell out to `shimback __complete-names`. */
static void append_shim_name_command_pattern(DynBuf *out, const char *sep) {
    bool first = true;
    for (size_t i = 0;; i++) {
        const char *name = cli_command_name(i);
        if (!name) {
            break;
        }
        const CommandSpec *spec = find_spec(name);
        if (spec && spec->takes_shim_name) {
            if (!first) {
                dynbuf_append_str(out, sep);
            }
            dynbuf_append_str(out, name);
            first = false;
            const char *alias = cli_command_alias(i);
            if (alias) {
                dynbuf_append_str(out, sep);
                dynbuf_append_str(out, alias);
            }
        }
    }
}

/* Appends `spec`'s own flags as `sep`-joined "--long -s --long2 ..." --
 * long form first for each flag, short form (if any) right after it. */
static void append_flags(DynBuf *out, const CommandSpec *spec, const char *sep) {
    bool first = true;
    for (size_t i = 0; i < spec->flag_count; i++) {
        if (!first) {
            dynbuf_append_str(out, sep);
        }
        dynbuf_append_str(out, "--");
        dynbuf_append_str(out, spec->flags[i].long_name);
        first = false;
        if (spec->flags[i].short_name != '\0') {
            dynbuf_append_str(out, sep);
            dynbuf_append_char(out, '-');
            dynbuf_append_char(out, spec->flags[i].short_name);
        }
    }
}

/* ---- bash ---- */

static void build_bash_script(DynBuf *out) {
    dynbuf_append_str(out,
                       "#!/usr/bin/env bash\n"
                       "# shimback completion for bash -- generated by `shimback completions "
                       "bash`.\n"
                       "# Install: add this line to ~/.bashrc (or wherever your bash startup\n"
                       "# file sources things):\n"
                       "#   source <(shimback completions bash)\n"
                       "# or save the output once under your bash-completion.d directory.\n"
                       "# shellcheck disable=SC2207  # COMPREPLY=($(compgen ...)) is the standard\n"
                       "# bash-completion idiom; compgen's own output is safe to split here.\n"
                       "\n"
                       "_shimback_complete() {\n"
                       "    local cur cmd\n"
                       "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                       "    cmd=\"${COMP_WORDS[1]:-}\"\n"
                       "\n"
                       "    if [ \"$COMP_CWORD\" -eq 1 ]; then\n"
                       "        COMPREPLY=($(compgen -W \"");
    append_command_names(out, " ");
    dynbuf_append_str(out,
                       "\" -- \"$cur\"))\n"
                       "        return 0\n"
                       "    fi\n"
                       "\n"
                       "    case \"$cmd\" in\n"
                       "        doctor)\n"
                       "            if [ \"$COMP_CWORD\" -eq 2 ]; then\n"
                       "                COMPREPLY=($(compgen -W \"fix\" -- \"$cur\"))\n"
                       "            elif [ \"${COMP_WORDS[2]}\" = \"fix\" ]; then\n"
                       "                COMPREPLY=($(compgen -W \"--yes -y\" -- \"$cur\"))\n"
                       "            fi\n"
                       "            return 0\n"
                       "            ;;\n"
                       "        completions)\n"
                       "            if [ \"$COMP_CWORD\" -eq 2 ]; then\n"
                       "                COMPREPLY=($(compgen -W \"bash zsh fish powershell\" -- "
                       "\"$cur\"))\n"
                       "            fi\n"
                       "            return 0\n"
                       "            ;;\n"
                       "    esac\n"
                       "\n"
                       "    if [ \"$COMP_CWORD\" -eq 2 ] && [[ \"$cur\" != -* ]]; then\n"
                       "        case \"$cmd\" in\n"
                       "            ");
    append_shim_name_command_pattern(out, "|");
    dynbuf_append_str(out,
                       ")\n"
                       "                COMPREPLY=($(compgen -W \"$(shimback __complete-names "
                       "2>/dev/null)\" -- \"$cur\"))\n"
                       "                return 0\n"
                       "                ;;\n"
                       "        esac\n"
                       "    fi\n"
                       "\n"
                       "    case \"$cmd\" in\n");
    for (size_t i = 0; i < COMMAND_SPEC_COUNT; i++) {
        const CommandSpec *spec = &COMMAND_SPECS[i];
        if (spec->flag_count == 0) {
            continue;
        }
        dynbuf_append_str(out, "        ");
        dynbuf_append_str(out, spec->command);
        dynbuf_append_str(out, ") COMPREPLY=($(compgen -W \"");
        append_flags(out, spec, " ");
        dynbuf_append_str(out, "\" -- \"$cur\")) ;;\n");
    }
    dynbuf_append_str(out,
                       "    esac\n"
                       "}\n"
                       "complete -F _shimback_complete shimback\n");
}

/* ---- zsh ---- */

static void build_zsh_script(DynBuf *out) {
    dynbuf_append_str(
        out,
        "#compdef shimback\n"
        "# shimback completion for zsh -- generated by `shimback completions zsh`.\n"
        "# Install: add this line to ~/.zshrc, *after* your compinit call (most\n"
        "# .zshrc files already run compinit near the top, so appending near the\n"
        "# end is usually safe):\n"
        "#   source <(shimback completions zsh)\n"
        "# Sourced before compinit, registration is silently skipped (see the\n"
        "# compdef guard at the bottom) -- compinit has no way to retroactively\n"
        "# discover an already-sourced function's #compdef line, only a file's.\n"
        "# To avoid caring about ordering at all, save it as a file named\n"
        "# _shimback somewhere in your $fpath instead; compinit's own file scan\n"
        "# picks up a #compdef line from a real file regardless of when it runs.\n"
        "\n"
        "_shimback() {\n"
        "    local cur cmd\n"
        "    cur=\"${words[CURRENT]}\"\n"
        "    cmd=\"${words[2]:-}\"\n"
        "\n"
        "    if [ \"$CURRENT\" -eq 2 ]; then\n"
        "        compadd -- ");
    append_command_names(out, " ");
    dynbuf_append_str(out,
                       "\n"
                       "        return 0\n"
                       "    fi\n"
                       "\n"
                       "    case \"$cmd\" in\n"
                       "        doctor)\n"
                       "            if [ \"$CURRENT\" -eq 3 ]; then\n"
                       "                compadd -- fix\n"
                       "            elif [ \"${words[3]}\" = \"fix\" ]; then\n"
                       "                compadd -- --yes -y\n"
                       "            fi\n"
                       "            return 0\n"
                       "            ;;\n"
                       "        completions)\n"
                       "            if [ \"$CURRENT\" -eq 3 ]; then\n"
                       "                compadd -- bash zsh fish powershell\n"
                       "            fi\n"
                       "            return 0\n"
                       "            ;;\n"
                       "    esac\n"
                       "\n"
                       "    if [ \"$CURRENT\" -eq 3 ] && [[ \"$cur\" != -* ]]; then\n"
                       "        case \"$cmd\" in\n"
                       "            ");
    append_shim_name_command_pattern(out, "|");
    dynbuf_append_str(out,
                       ")\n"
                       "                compadd -- $(shimback __complete-names 2>/dev/null)\n"
                       "                return 0\n"
                       "                ;;\n"
                       "        esac\n"
                       "    fi\n"
                       "\n"
                       "    case \"$cmd\" in\n");
    for (size_t i = 0; i < COMMAND_SPEC_COUNT; i++) {
        const CommandSpec *spec = &COMMAND_SPECS[i];
        if (spec->flag_count == 0) {
            continue;
        }
        dynbuf_append_str(out, "        ");
        dynbuf_append_str(out, spec->command);
        dynbuf_append_str(out, ") compadd -- ");
        append_flags(out, spec, " ");
        dynbuf_append_str(out, " ;;\n");
    }
    dynbuf_append_str(out,
                       "    esac\n"
                       "}\n"
                       "# compdef is only defined once zsh's completion system (compinit) has\n"
                       "# run -- guard it so sourcing this before compinit degrades silently\n"
                       "# (no completion registered yet) instead of a \"command not found\"\n"
                       "# error. See this file's own header comment for the ordering this\n"
                       "# actually needs.\n"
                       "(( $+functions[compdef] )) && compdef _shimback shimback\n");
}

/* ---- fish ---- */

static void build_fish_script(DynBuf *out) {
    dynbuf_append_str(
        out,
        "# shimback completion for fish -- generated by `shimback completions fish`.\n"
        "# Install: shimback completions fish > "
        "~/.config/fish/completions/shimback.fish\n"
        "\n"
        "complete -c shimback -f\n"
        "\n"
        "complete -c shimback -n '__fish_use_subcommand' -a '");
    append_command_names(out, " ");
    dynbuf_append_str(
        out,
        "'\n"
        "\n"
        "complete -c shimback -n '__fish_seen_subcommand_from doctor; and not "
        "__fish_seen_subcommand_from fix' -a fix\n"
        "complete -c shimback -n '__fish_seen_subcommand_from doctor; and "
        "__fish_seen_subcommand_from fix' -l yes -s y\n"
        "\n"
        "complete -c shimback -n '__fish_seen_subcommand_from completions' -a 'bash zsh fish "
        "powershell'\n"
        "\n"
        "complete -c shimback -n '__fish_seen_subcommand_from ");
    append_shim_name_command_pattern(out, " ");
    dynbuf_append_str(out, "' -a '(shimback __complete-names 2>/dev/null)'\n\n");

    for (size_t i = 0; i < COMMAND_SPEC_COUNT; i++) {
        const CommandSpec *spec = &COMMAND_SPECS[i];
        for (size_t j = 0; j < spec->flag_count; j++) {
            dynbuf_append_str(out, "complete -c shimback -n '__fish_seen_subcommand_from ");
            dynbuf_append_str(out, spec->command);
            dynbuf_append_str(out, "' -l ");
            dynbuf_append_str(out, spec->flags[j].long_name);
            if (spec->flags[j].short_name != '\0') {
                dynbuf_append_str(out, " -s ");
                dynbuf_append_char(out, spec->flags[j].short_name);
            }
            dynbuf_append_char(out, '\n');
        }
    }
}

/* ---- PowerShell ---- */

/* Appends `spec`'s flags as a single-quoted PowerShell array literal:
 * '--long','-s','--long2',... -- empty array (@()) if none. */
static void append_flags_ps_array(DynBuf *out, const CommandSpec *spec) {
    if (spec->flag_count == 0) {
        dynbuf_append_str(out, "@()");
        return;
    }
    dynbuf_append_str(out, "@(");
    bool first = true;
    for (size_t i = 0; i < spec->flag_count; i++) {
        if (!first) {
            dynbuf_append_str(out, ",");
        }
        dynbuf_append_str(out, "'--");
        dynbuf_append_str(out, spec->flags[i].long_name);
        dynbuf_append_str(out, "'");
        first = false;
        if (spec->flags[i].short_name != '\0') {
            dynbuf_append_str(out, ",'-");
            dynbuf_append_char(out, spec->flags[i].short_name);
            dynbuf_append_str(out, "'");
        }
    }
    dynbuf_append_str(out, ")");
}

static void build_powershell_script(DynBuf *out) {
    dynbuf_append_str(
        out,
        "# shimback completion for PowerShell -- generated by `shimback completions "
        "powershell`.\n"
        "# Install: pipe through Invoke-Expression for this session, or append to "
        "$PROFILE for every session:\n"
        "#   shimback completions powershell | Out-String | Invoke-Expression\n"
        "#   shimback completions powershell >> $PROFILE\n"
        "\n"
        "Register-ArgumentCompleter -Native -CommandName shimback -ScriptBlock {\n"
        "    param($wordToComplete, $commandAst, $cursorPosition)\n"
        "\n"
        "    $words = @()\n"
        "    foreach ($e in $commandAst.CommandElements) { $words += $e.ToString() }\n"
        "    # A not-yet-typed word after a trailing space has no CommandElement of\n"
        "    # its own (unlike bash's COMP_WORDS/fish's complete -C, which both give\n"
        "    # it an empty slot) -- $wordToComplete is empty in exactly that case, so\n"
        "    # count it as one more position than $words.Count actually shows.\n"
        "    $effectiveCount = $words.Count\n"
        "    if ($wordToComplete -eq '') { $effectiveCount++ }\n"
        "\n"
        "    function Emit($candidates) {\n"
        "        $candidates | Where-Object { $_ -like \"$wordToComplete*\" } | "
        "ForEach-Object {\n"
        "            [System.Management.Automation.CompletionResult]::new($_, $_, "
        "'ParameterValue', $_)\n"
        "        }\n"
        "    }\n"
        "\n"
        "    if ($effectiveCount -le 2) {\n"
        "        Emit @(");
    bool first = true;
    for (size_t i = 0;; i++) {
        const char *name = cli_command_name(i);
        if (!name) {
            break;
        }
        if (!first) {
            dynbuf_append_str(out, ",");
        }
        dynbuf_append_str(out, "'");
        dynbuf_append_str(out, name);
        dynbuf_append_str(out, "'");
        first = false;
        const char *alias = cli_command_alias(i);
        if (alias) {
            dynbuf_append_str(out, ",'");
            dynbuf_append_str(out, alias);
            dynbuf_append_str(out, "'");
        }
    }
    dynbuf_append_str(out,
                       ")\n"
                       "        return\n"
                       "    }\n"
                       "\n"
                       "    $cmd = $words[1]\n"
                       "\n"
                       "    switch ($cmd) {\n"
                       "        'doctor' {\n"
                       "            if ($effectiveCount -eq 3) {\n"
                       "                Emit @('fix')\n"
                       "            } elseif ($words[2] -eq 'fix') {\n"
                       "                Emit @('--yes','-y')\n"
                       "            }\n"
                       "            return\n"
                       "        }\n"
                       "        'completions' {\n"
                       "            if ($effectiveCount -eq 3) {\n"
                       "                Emit @('bash','zsh','fish','powershell')\n"
                       "            }\n"
                       "            return\n"
                       "        }\n"
                       "    }\n"
                       "\n"
                       "    if ($effectiveCount -eq 3 -and $wordToComplete -notlike '-*') {\n"
                       "        $shimNameCommands = @(");
    first = true;
    for (size_t i = 0;; i++) {
        const char *name = cli_command_name(i);
        if (!name) {
            break;
        }
        const CommandSpec *spec = find_spec(name);
        if (spec && spec->takes_shim_name) {
            if (!first) {
                dynbuf_append_str(out, ",");
            }
            dynbuf_append_str(out, "'");
            dynbuf_append_str(out, name);
            dynbuf_append_str(out, "'");
            first = false;
            const char *alias = cli_command_alias(i);
            if (alias) {
                dynbuf_append_str(out, ",'");
                dynbuf_append_str(out, alias);
                dynbuf_append_str(out, "'");
            }
        }
    }
    dynbuf_append_str(
        out,
        ")\n"
        "        if ($shimNameCommands -contains $cmd) {\n"
        "            $names = & shimback __complete-names 2>$null\n"
        "            if ($names) { Emit @($names -split \"`n\" | Where-Object { $_ }) }\n"
        "            return\n"
        "        }\n"
        "    }\n"
        "\n"
        "    $flags = switch ($cmd) {\n");
    for (size_t i = 0; i < COMMAND_SPEC_COUNT; i++) {
        const CommandSpec *spec = &COMMAND_SPECS[i];
        if (spec->flag_count == 0) {
            continue;
        }
        dynbuf_append_str(out, "        '");
        dynbuf_append_str(out, spec->command);
        dynbuf_append_str(out, "' { ");
        append_flags_ps_array(out, spec);
        dynbuf_append_str(out, " }\n");
    }
    dynbuf_append_str(out,
                       "        default { @() }\n"
                       "    }\n"
                       "    Emit $flags\n"
                       "}\n");
}

int cmd_completions(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "%s", USAGE);
        if (argc < 2) {
            die("completions: missing shell name");
        }
        die("completions: unexpected argument '%s'", argv[2]);
    }

    const char *shell = argv[1];
    DynBuf out;
    dynbuf_init(&out);

    if (strcmp(shell, "bash") == 0) {
        build_bash_script(&out);
    } else if (strcmp(shell, "zsh") == 0) {
        build_zsh_script(&out);
    } else if (strcmp(shell, "fish") == 0) {
        build_fish_script(&out);
    } else if (strcmp(shell, "powershell") == 0) {
        build_powershell_script(&out);
    } else {
        dynbuf_free(&out);
        die("completions: unknown shell '%s' -- expected bash, zsh, fish, or powershell", shell);
    }

    fputs(dynbuf_cstr(&out), stdout);
    dynbuf_free(&out);
    return 0;
}

int cmd_complete_names(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char *cfg_path = config_file_path();
    Config cfg;
    char errbuf[256];
    if (config_load(cfg_path, &cfg, errbuf, sizeof(errbuf)) != CONFIG_OK) {
        free(cfg_path);
        return 0;
    }
    free(cfg_path);

    size_t name_count = 0;
    char **names = collect_all_shim_names(&cfg, &name_count);
    for (size_t i = 0; i < name_count; i++) {
        printf("%s\n", names[i]);
    }
    str_array_free(names, name_count);
    config_free(&cfg);
    return 0;
}
