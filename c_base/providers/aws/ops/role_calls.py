#!/usr/bin/env python3
"""role_calls.py DIR: every AWS call the Python deploy tool's modules (DIR/*.py) make, one
"SERVICE COMMAND" line each ("SERVICE wait WAITER" for a waiter), sorted and unique; role_covers_deploy.sh
reads them. It reads the source with ast, not by running it, in each form the tool makes a call
(cbase/tools/cbase_cli/aws.py and stack.py):
  call("SERVICE", "COMMAND", ...)  and  aws.call(...)     the one call wrapper; "--region R" may come
                                                         before the command (acm in us-east-1)
  callv(["SERVICE", "--region", R], "COMMAND", ...)       the same with a head list (CloudFormation)
  _cfn("COMMAND", ...)                                    CloudFormation in the stack's region
  _cs(...)                                                describe-change-set on the stack's change set
  cli.run(["aws", "SERVICE", "COMMAND", ...])             the CLI run directly (aws configure get region)
A call whose words are a starred name (_cfn(*cw), call(*args)) is read from the list literal last
assigned to that name in the same function. A call it cannot resolve to a service and a command is
printed as "UNRESOLVED file:line" so the check stops on it: a new form of call must be taught here, never
skipped. A "wait" whose waiter is not a literal (stack.py's _wait(waiter)) is "SERVICE wait", which the
table maps to the service's describe action."""
import ast
import os
import sys

WRAPPERS = {"call", "callv", "_cfn", "_cs"}
CFN = ["cloudformation", "--region", "*"]


def _words(node, assigns):
    """The leading literal words of an argument list, a non-literal as "*"; None when a starred name has
    no list literal to read."""
    out = []
    for a in node:
        if isinstance(a, ast.Starred):
            if isinstance(a.value, ast.Name) and a.value.id in assigns:
                out += _words(assigns[a.value.id], assigns)
                continue
            return None
        if isinstance(a, ast.Constant) and isinstance(a.value, str):
            out.append(a.value)
        elif isinstance(a, ast.List):
            sub = _words(a.elts, assigns)
            if sub is None:
                return None
            out += sub
        else:
            out.append("*")
    return out


def _name(func):
    if isinstance(func, ast.Name):
        return func.id
    if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) and func.value.id == "aws":
        return func.attr
    return None


def _service_command(words):
    """SERVICE COMMAND (or SERVICE wait WAITER) from the call's words, skipping "--region R"."""
    w = list(words)
    if not w or w[0] in ("*",) or w[0].startswith("-"):
        return None
    svc = w.pop(0)
    while len(w) >= 2 and w[0] == "--region":
        w = w[2:]
    if not w or w[0] == "*" or w[0].startswith("-"):
        return None
    if w[0] == "wait":
        if len(w) >= 2 and w[1] != "*" and not w[1].startswith("-"):
            return "%s wait %s" % (svc, w[1])
        return "%s wait" % svc
    return "%s %s" % (svc, w[0])


def calls_in(path, out, bad):
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read(), path)
    for fn in [n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.Module))]:
        if isinstance(fn, ast.Module):
            continue
        if fn.name in WRAPPERS or fn.name == "exec_":   # the wrappers' own bodies pass their words on
            continue
        assigns = {}
        for n in ast.walk(fn):
            if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name) \
                    and isinstance(n.value, ast.List):
                assigns[n.targets[0].id] = n.value.elts
        for n in ast.walk(fn):
            if not isinstance(n, ast.Call):
                continue
            name = _name(n.func)
            where = "%s:%d" % (os.path.basename(path), n.lineno)
            if isinstance(n.func, ast.Attribute) and n.func.attr == "run" and isinstance(n.func.value, ast.Name) \
                    and n.func.value.id == "cli":
                # A command run directly: an AWS call when its first word is "aws"; a first word that is not a
                # literal could be one, so it stops the check.
                if not n.args or not isinstance(n.args[0], ast.List):
                    bad.append(where)
                    continue
                words = _words(n.args[0].elts, assigns)
                if words is None or not words or words[0] == "*":
                    bad.append(where)
                    continue
                if words[0] != "aws":
                    continue
                sc = _service_command(words[1:])
                if sc is None:
                    bad.append(where)
                else:
                    out.add(sc)
                continue
            if name not in WRAPPERS:
                continue
            words = _words(n.args, assigns)
            if words is None:
                bad.append(where)
                continue
            if name == "_cfn":
                words = CFN + words
            elif name == "_cs":
                words = CFN + ["describe-change-set"] + words
            sc = _service_command(words)
            if sc is None:
                bad.append(where)
            else:
                out.add(sc)


def main():
    d = sys.argv[1]
    out, bad = set(), []
    files = sorted(f for f in os.listdir(d) if f.endswith(".py"))
    for f in files:
        calls_in(os.path.join(d, f), out, bad)
    for c in sorted(out):
        print(c)
    for b in bad:
        print("UNRESOLVED " + b)


main()
