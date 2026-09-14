#!/usr/bin/env python3
"""Drives shimback's interactive `add` wizard through a real pseudo-terminal.

The wizard is gated on stdin/stdout both being a real tty (see add.c), so
it can't be exercised by the project's other, sandboxed-but-non-tty shell
tests. This script spawns the built binary attached to a pty via
subprocess + pty.openpty(), feeds scripted keystrokes (including raw ANSI
escape sequences for arrow keys), and inspects the resulting
config.toml/symlink in a sandboxed $HOME/$XDG_CONFIG_HOME/$XDG_DATA_HOME
to verify each scenario.

Page order (see add_wizard.c's next_page()): NAME -> POLICY -> SOURCE ->
SOURCE_ARGS -> FALLBACK -> FALLBACK_ARGS -> <policy-specific> ->
DIAGNOSTIC -> DONE. SOURCE_ARGS/FALLBACK_ARGS are always "present" (like
SOURCE itself) -- an empty list is a valid, always-known default -- so
they're silently auto-seeded (never a live/blocking page) whenever
source/fallback themselves are already fully known, but still need their
own blank Enter to pass through whenever the wizard reaches them live.
"""
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

FAILURES = []


def fail(msg):
    FAILURES.append(msg)
    print(f"FAIL: {msg}", file=sys.stderr)


def assert_eq(desc, expected, actual):
    if expected != actual:
        fail(f"{desc}: expected [{expected}], got [{actual}]")


def assert_contains(desc, haystack, needle):
    if needle not in haystack:
        fail(f"{desc}: expected to find [{needle}] in [{haystack}]")


def assert_not_contains(desc, haystack, needle):
    if needle in haystack:
        fail(f"{desc}: expected NOT to find [{needle}] in [{haystack}]")


def real(path):
    """The wizard stores source/fallback canonicalized (realpath(3), same as
    the C code's canonicalize()), so a config-file assertion must compare
    against the resolved path too. On a usrmerged Linux system /bin is a
    symlink to /usr/bin, so e.g. "/bin/cat" resolves to "/usr/bin/cat" --
    on macOS (and non-usrmerged Linux) realpath is a no-op here since /bin
    is a real directory. Terminal-output assertions checking the wizard's
    live breadcrumb echo of what was just typed should NOT use this: that
    text is the raw, pre-resolution input."""
    return os.path.realpath(path)


class Sandbox:
    def __init__(self, binary):
        self.binary = binary
        self.root = tempfile.mkdtemp(prefix="sb_wizard_test_")
        self.home = os.path.join(self.root, "home")
        os.makedirs(self.home, exist_ok=True)
        self.env = dict(os.environ)
        self.env["HOME"] = self.home
        self.env["XDG_CONFIG_HOME"] = os.path.join(self.home, ".config")
        self.env["XDG_DATA_HOME"] = os.path.join(self.home, ".local", "share")
        self.env["TERM"] = "xterm"
        self.master = None
        self.proc = None

    def cleanup(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if self.master is not None:
            try:
                os.close(self.master)
            except OSError:
                pass
        shutil.rmtree(self.root, ignore_errors=True)

    def spawn(self, argv):
        master, slave = os.openpty()
        self.master = master
        self.proc = subprocess.Popen(
            [self.binary] + argv,
            stdin=slave, stdout=slave, stderr=slave,
            env=self.env, close_fds=True,
        )
        os.close(slave)
        time.sleep(0.2)
        return self.drain()

    def drain(self, timeout=0.3):
        buf = b""
        while True:
            r, _, _ = select.select([self.master], [], [], timeout)
            if not r:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
        return buf.decode(errors="replace")

    def send(self, data, delay=0.1):
        os.write(self.master, data)
        time.sleep(delay)
        return self.drain()

    def wait(self, timeout=5):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        return self.proc.returncode

    def config_path(self):
        return os.path.join(self.env["XDG_CONFIG_HOME"], "shimback", "config.toml")

    def config_text(self):
        p = self.config_path()
        return open(p).read() if os.path.exists(p) else ""

    def shim_path(self, name):
        return os.path.join(self.env["XDG_DATA_HOME"], "shimback", "bin", name)


BIN = sys.argv[1]

ENTER = b"\r"
ESC = b"\x1b"
CTRL_C = b"\x03"
UP = b"\x1b[A"
DOWN = b"\x1b[B"
LEFT = b"\x1b[D"
RIGHT = b"\x1b[C"


# --- 1a: fully blank wizard, default exit-code policy ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"tool1" + ENTER)
    sb.send(ENTER)          # policy: exit-code (default)
    sb.send(ENTER)          # source: skip (auto)
    sb.send(ENTER)          # source args: none
    sb.send(b"/bin/cat" + ENTER)  # fallback
    sb.send(ENTER)          # fallback args: none
    sb.send(ENTER, 0.5)     # diagnostic: no
    code = sb.wait()
    assert_eq("1a exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("1a config has shim", cfg, "[shims.tool1]")
    assert_contains("1a fallback stored", cfg, f'fallback = "{real("/bin/cat")}"')
    assert_contains("1a policy stored", cfg, 'policy = "exit-code"')
    assert_not_contains("1a no explicit source (auto)", cfg, "source =")
    if not os.path.islink(sb.shim_path("tool1")):
        fail("1a: expected a symlink for tool1")
finally:
    sb.cleanup()

# --- 1b: fully blank wizard, route-args policy (a list page + boolean) ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"tool2" + ENTER)
    sb.send(DOWN + DOWN + DOWN + ENTER)  # exit-code -> heuristic -> exit-code-match -> route-args
    sb.send(ENTER)                # source: auto
    sb.send(ENTER)                # source args: none
    sb.send(b"/bin/echo" + ENTER)  # fallback
    sb.send(ENTER)                # fallback args: none
    sb.send(b"special" + ENTER)   # route-arg 1
    sb.send(ENTER)                # finish list
    sb.send(b"y" + ENTER)         # strip-matched-args: yes
    sb.send(ENTER, 0.5)           # diagnostic: no
    code = sb.wait()
    assert_eq("1b exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("1b policy is route-args", cfg, 'policy = "route-args"')
    assert_contains("1b route_args stored", cfg, 'route_args = ["special"]')
    assert_contains("1b strip_matched_args stored", cfg, "strip_matched_args = true")
finally:
    sb.cleanup()

# --- 2: partial CLI args (name + source) resume at the first missing page
# (fallback, since SOURCE_ARGS is always auto-seeded as an empty, "already
# known" default -- not a live page -- right along with SOURCE itself),
# and preserve the already-given name/source ---
sb = Sandbox(BIN)
try:
    out = sb.spawn(["add", "seeded", "-s", "/bin/ls"])
    assert_contains("2: resumes directly at the fallback page", out, "Fallback command")
    assert_contains("2: name preserved in breadcrumb", out, "seeded")
    assert_contains("2: source preserved in breadcrumb", out, "/bin/ls")
    sb.send(b"/bin/cat" + ENTER)
    sb.send(ENTER)       # fallback args: none
    sb.send(ENTER, 0.5)  # diagnostic: no
    code = sb.wait()
    assert_eq("2 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("2 source preserved in config", cfg, f'source = "{real("/bin/ls")}"')
    assert_contains("2 fallback set in config", cfg, f'fallback = "{real("/bin/cat")}"')
finally:
    sb.cleanup()

# --- 3: Left-then-edit-then-forward on a non-policy page preserves
# already-entered later data ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"tool3" + ENTER)
    sb.send(ENTER)                  # policy: exit-code
    sb.send(b"/bin/ls" + ENTER)     # source
    sb.send(ENTER)                  # source args: none -> now on fallback page
    sb.send(b"/bin/cat" + ENTER)    # fallback -> now on fallback_args page
    sb.send(LEFT)                   # back to fallback
    sb.send(LEFT)                   # back to source_args
    out = sb.send(LEFT)             # back to source
    assert_contains("3: back-nav shows previously typed source", out, "/bin/ls")
    sb.send(RIGHT)                  # forward to source_args
    out = sb.send(RIGHT)            # forward to fallback (unedited)
    assert_contains("3: forward-nav still shows fallback page with prior value", out, "/bin/cat")
    sb.send(ENTER)                  # re-accept fallback unchanged -> advances into fallback_args
    sb.send(ENTER)                  # re-accept fallback_args unchanged (still empty) -> diagnostic
    sb.send(ENTER, 0.5)             # diagnostic: no
    code = sb.wait()
    assert_eq("3 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("3 source preserved after back+forward", cfg, f'source = "{real("/bin/ls")}"')
    assert_contains("3 fallback preserved after back+forward", cfg,
                     f'fallback = "{real("/bin/cat")}"')
finally:
    sb.cleanup()

# --- 4: changing the policy after going back truncates/resets later pages ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"tool4" + ENTER)
    sb.send(ENTER)                  # policy: exit-code
    sb.send(ENTER)                  # source: auto
    sb.send(ENTER)                  # source args: none
    sb.send(b"/bin/cat" + ENTER)    # fallback -> now on fallback_args page
    out = sb.send(LEFT + LEFT + LEFT + LEFT)  # back to policy page
    assert_contains("4: back-nav reaches policy page", out, "Policy:")
    out = sb.send(DOWN + ENTER)     # exit-code -> heuristic, commit (a real change)
    assert_contains("4: policy change resets to a fresh source page", out, "Source command")
    sb.send(ENTER)                  # source: auto (freshly re-asked)
    sb.send(ENTER)                  # source args: none
    sb.send(b"/bin/ls" + ENTER)     # fallback (freshly re-asked, different value)
    sb.send(ENTER)                  # fallback args: none
    sb.send(b"invalid option" + ENTER)  # pattern
    sb.send(ENTER)                  # finish list
    sb.send(ENTER, 0.5)             # diagnostic: no
    code = sb.wait()
    assert_eq("4 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("4 policy changed to heuristic", cfg, 'policy = "heuristic"')
    assert_contains("4 new fallback value used", cfg, f'fallback = "{real("/bin/ls")}"')
    assert_contains("4 pattern set after policy change", cfg, 'error_patterns = ["invalid option"]')
finally:
    sb.cleanup()

# --- 5: abort via Esc leaves no symlink/config entry ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"esctool" + ENTER)
    sb.send(ESC, 0.3)
    code = sb.wait()
    assert_eq("5 exit code", 1, code)
    assert_not_contains("5: nothing written to config", sb.config_text(), "esctool")
    if os.path.exists(sb.shim_path("esctool")):
        fail("5: no symlink should exist after Esc abort")
finally:
    sb.cleanup()

# --- 6: abort via Ctrl-C leaves no symlink/config entry ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"ctrlctool" + ENTER)
    sb.send(CTRL_C, 0.3)
    code = sb.wait()
    assert_eq("6 exit code", 1, code)
    assert_not_contains("6: nothing written to config", sb.config_text(), "ctrlctool")
    if os.path.exists(sb.shim_path("ctrlctool")):
        fail("6: no symlink should exist after Ctrl-C abort")
finally:
    sb.cleanup()

# --- 7: an optional field (source) skipped via blank Enter results in no
# explicit source (auto) in the config ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"tool7" + ENTER)
    sb.send(ENTER)                # policy: exit-code
    sb.send(ENTER)                # source: blank -> skip/auto
    sb.send(ENTER)                # source args: none
    sb.send(b"/bin/cat" + ENTER)  # fallback
    sb.send(ENTER)                # fallback args: none
    sb.send(ENTER, 0.5)           # diagnostic: no
    code = sb.wait()
    assert_eq("7 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("7: shim present", cfg, "[shims.tool7]")
    assert_not_contains("7: no explicit source line (auto)", cfg, "source =")
finally:
    sb.cleanup()

# --- 8: a list page refuses to finish with zero items. Fully seeded except
# --policy heuristic's required patterns, so SOURCE/SOURCE_ARGS/FALLBACK/
# FALLBACK_ARGS are all silently auto-seeded and the wizard starts live
# right on the (required) patterns page -- no extra keystrokes needed for
# the new optional pages here at all. ---
sb = Sandbox(BIN)
try:
    out = sb.spawn(["add", "tool8", "-f", "/bin/cat", "--policy", "heuristic"])
    assert_contains("8: starts on the patterns page", out, "Error patterns")
    out = sb.send(ENTER, 0.2)  # blank with 0 items -> must NOT advance
    assert_contains("8: refuses to finish with zero items", out, "At least one is required")
    assert_contains("8: still on the patterns page", out, "Error patterns")
    sb.send(b"invalid option" + ENTER)  # add one item
    sb.send(ENTER)                       # now blank-finish is allowed
    sb.send(ENTER, 0.5)                  # diagnostic: no
    code = sb.wait()
    assert_eq("8 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("8: pattern stored", cfg, 'error_patterns = ["invalid option"]')
finally:
    sb.cleanup()


# --- 9: revisiting a boolean page (strip-matched-args) and re-confirming it
# unchanged (blank Enter) must NOT silently reset a prior "yes" to "no" ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"tool9" + ENTER)
    sb.send(DOWN + DOWN + DOWN + ENTER)  # exit-code -> ... -> route-args
    sb.send(ENTER)                # source: auto
    sb.send(ENTER)                # source args: none
    sb.send(b"/bin/echo" + ENTER)  # fallback
    sb.send(ENTER)                # fallback args: none
    sb.send(b"special" + ENTER)   # route-arg 1
    sb.send(ENTER)                # finish list
    sb.send(b"y" + ENTER)         # strip-matched-args: yes -> now on diagnostic page
    out = sb.send(LEFT)           # back to strip-matched-args page
    assert_contains("9: revisited page pre-fills the prior \"yes\"", out, "> y")
    sb.send(ENTER)                # re-confirm unchanged -> forward to diagnostic
    sb.send(ENTER, 0.5)           # diagnostic: no
    code = sb.wait()
    assert_eq("9 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("9: strip_matched_args stayed true after revisit", cfg,
                     "strip_matched_args = true")
finally:
    sb.cleanup()


# --- 10: source_args/fallback_args let a shim double as a regular alias --
# a fixed extra argument, entered on the new optional pages, ends up baked
# into the stored config (this is the actual feature: e.g. source "ls"
# plus source_args ["-ltrah"], so the shim always runs "ls -ltrah ..."). ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"cools" + ENTER)
    sb.send(ENTER)                 # policy: exit-code
    sb.send(b"/bin/ls" + ENTER)    # source
    out = sb.send(b"-ltrah" + ENTER)  # source arg 1
    assert_contains("10: source arg shows up in the accumulated list", out, "1. -ltrah")
    sb.send(ENTER)                 # finish source args list (optional, one item is enough)
    sb.send(b"/bin/cat" + ENTER)   # fallback
    sb.send(b"-A" + ENTER)         # fallback arg 1
    sb.send(ENTER)                 # finish fallback args list
    sb.send(ENTER, 0.5)            # diagnostic: no
    code = sb.wait()
    assert_eq("10 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("10: source_args stored", cfg, 'source_args = ["-ltrah"]')
    assert_contains("10: fallback_args stored", cfg, 'fallback_args = ["-A"]')
finally:
    sb.cleanup()


# --- 11: under --policy rewrite (the one policy where fallback is
# optional), leaving fallback blank must skip the Fallback args page
# entirely -- there's no fallback for it to attach to -- landing straight
# on the policy-specific page instead. ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"rwtool2" + ENTER)
    sb.send(DOWN + DOWN + DOWN + DOWN + ENTER)  # exit-code -> ... -> rewrite
    sb.send(ENTER)                # source: auto
    sb.send(ENTER)                # source args: none
    out = sb.send(ENTER)          # fallback: blank -> should skip straight to rewrite rules
    assert_contains("11: skips straight to the rewrite rules page", out, "Rewrite rules")
    assert_not_contains("11: fallback args page never shown", out, "Fallback args")
    sb.send(b"all=ls" + ENTER)    # rewrite rule
    sb.send(ENTER)                # finish list
    sb.send(ENTER, 0.5)           # diagnostic: no
    code = sb.wait()
    assert_eq("11 exit code", 0, code)
    cfg = sb.config_text()
    assert_not_contains("11: no fallback_args key stored at all", cfg, "fallback_args")
finally:
    sb.cleanup()


# --- 12: the wizard refuses to finish the fallback-args page when source
# and fallback would be truly indistinguishable (same resolved binary, same
# extra args) -- but a differentiating fallback arg fixes it, matching
# finish_add's own authoritative "no-op shim" check. ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"samebintool" + ENTER)
    sb.send(ENTER)                 # policy: exit-code
    sb.send(b"/bin/ls" + ENTER)    # source
    sb.send(b"-x" + ENTER)         # source arg
    sb.send(ENTER)                 # finish source args
    sb.send(b"/bin/ls" + ENTER)    # fallback: same binary as source
    out = sb.send(b"-x" + ENTER)   # fallback arg: same as source's -- try to finish
    out = sb.send(ENTER)           # blank -> attempt to finish the list
    assert_contains("12: refuses to finish with identical source/fallback", out,
                     "same arguments")
    assert_contains("12: still on the fallback args page", out, "Fallback args")
    sb.send(b"-y" + ENTER)         # add a differentiating fallback arg
    sb.send(ENTER)                 # now blank-finish is allowed
    sb.send(ENTER, 0.5)            # diagnostic: no
    code = sb.wait()
    assert_eq("12 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("12: source_args stored", cfg, 'source_args = ["-x"]')
    assert_contains("12: fallback_args stored (differentiated)", cfg,
                     'fallback_args = ["-x", "-y"]')
finally:
    sb.cleanup()


# --- 13: split-args policy through the wizard -- both route-arg-list pages
# (source's own and fallback's own) are required, at least one entry each,
# then the shared strip-matched-args page. ---
sb = Sandbox(BIN)
try:
    sb.spawn(["add"])
    sb.send(b"sptool" + ENTER)
    sb.send(DOWN * 5 + ENTER)      # exit-code -> ... -> split-args
    sb.send(ENTER)                 # source: auto
    sb.send(ENTER)                 # source args: none
    sb.send(b"/bin/echo" + ENTER)  # fallback
    sb.send(ENTER)                 # fallback args: none
    out = sb.send(ENTER)           # source route args: blank -> at least one required
    assert_contains("13: source route args require at least one", out, "At least one is required.")
    sb.send(b"-1" + ENTER)         # source route arg 1
    sb.send(b"-2" + ENTER)         # source route arg 2
    sb.send(ENTER)                 # finish source route args list
    out = sb.send(ENTER)           # fallback route args: blank -> at least one required
    assert_contains("13: fallback route args require at least one", out,
                     "At least one is required.")
    sb.send(b"-1" + ENTER)
    sb.send(b"-2" + ENTER)
    sb.send(b"-3" + ENTER)
    sb.send(ENTER)                 # finish fallback route args list
    sb.send(b"y" + ENTER)          # strip-matched-args: yes
    sb.send(ENTER, 0.5)            # diagnostic: no
    code = sb.wait()
    assert_eq("13 exit code", 0, code)
    cfg = sb.config_text()
    assert_contains("13: policy is split-args", cfg, 'policy = "split-args"')
    assert_contains("13: source_route_args stored", cfg, 'source_route_args = ["-1", "-2"]')
    assert_contains("13: fallback_route_args stored", cfg,
                     'fallback_route_args = ["-1", "-2", "-3"]')
    assert_contains("13: strip_matched_args stored", cfg, "strip_matched_args = true")
finally:
    sb.cleanup()


if FAILURES:
    print(f"{len(FAILURES)} assertion(s) failed", file=sys.stderr)
    sys.exit(1)
print("OK")
sys.exit(0)
