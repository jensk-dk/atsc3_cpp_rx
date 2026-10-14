#!/usr/bin/env python3
"""Checks the root README.md against the tree so that it cannot go stale silently (ctest entry docs.readme).

    python3 -I scripts/readme-check.py <repository root>

What is verified, in the fenced ```bash blocks and in backticked tokens of the prose:
  - `cmake --preset X` / `ctest --preset X` name a configure/test preset of CMakePresets.json; `cmake --install
    DIR --component C` names a CPack component; `ctest -L X` a label and `ctest -R X` a registered test name
  - every `--flag` given to atsc3d, atsc3scan, stltp2alp or routedump is one the program's source accepts; `--atsc3-<name>` flags and `atsc3://...?k=v` query keys exist in the VLC plugin
  - `ATSC3_*` names are options of cmake/Options.cmake or appear in the implementation sources (environment
    variables such as ATSC3_E2E_PORT); `npm run X` is a package.json script; `--project=X` and
    `PLAYWRIGHT_PROJECT=X` are Playwright projects
  - paths under the repository exist (`build/<preset>/...` is checked against the preset and the target name
    instead, because build trees are not part of the checkout; `samples/` is skipped, the corpus is optional)
Prose is not checked beyond its backticked tokens.
"""
import json
import os
import re
import shlex
import sys

PROGRAM_SOURCES = {
    "atsc3d": "implementation/integrations/web/atsc3d/main.cpp",
    "atsc3scan": "implementation/tools/atsc3scan/main.cpp",
    "stltp2alp": "implementation/tools/stltp2alp/main.cpp",
    "routedump": "implementation/tools/routedump/main.cpp",
    "setup.sh": "setup.sh",
}
VLC_MODULE = "implementation/integrations/vlc/src/module.cpp"
VLC_MRL = "implementation/integrations/vlc/src/mrl.cpp"
PRESETS = "implementation/CMakePresets.json"
OPTIONS = "implementation/cmake/Options.cmake"
PACKAGE_JSON = "implementation/integrations/web/frontend/package.json"
PLAYWRIGHT = "implementation/integrations/web/frontend/playwright.config.ts"
PATH_PREFIXES = ("implementation/", "tools/", "specs/", "tests/", "scripts/", "integrations/", "cmake/",
                 "src/", "include/", "tuners/", "build/", "third_party/")
SKIP_PATH_PREFIXES = ("samples/", "../samples", "/abs/", "http://", "https://")
GENERIC_FLAGS = {"--preset", "--build", "--install", "--component", "--project", "--sout", "--version"}
IGNORED_DIRS = {"build", "node_modules", "dist", "__pycache__", ".git"}


class Checker:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.problems = []
        self.checked = 0
        presets = json.load(open(self.path(PRESETS), encoding="utf-8"))
        self.configure_presets = {p["name"] for p in presets["configurePresets"] if not p.get("hidden")}
        self.test_presets = {p["name"] for p in presets.get("testPresets", [])}
        self.options = set(re.findall(r"^(?:option|set)\((ATSC3_[A-Z_]+)", self.read(OPTIONS), re.M))
        cmake_text = self.cmake_sources()
        self.labels = set()
        for group in re.findall(r"LABELS\s+\"?([A-Za-z0-9_;-]+)\"?", cmake_text):
            self.labels.update(group.split(";"))
        self.test_names = set(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_.-]+)", cmake_text))
        self.components = set(re.findall(r"COMPONENT\s+([A-Za-z0-9_-]+)", cmake_text))
        self.targets = set(re.findall(r"add_executable\(\s*(\w+)", cmake_text))
        self.targets.update(re.findall(r"OUTPUT_NAME\s+\"?(\w+)\"?", cmake_text))
        self.program_flags = {name: set(re.findall(r"--[a-z][a-z0-9-]*", self.read(src)))
                              for name, src in PROGRAM_SOURCES.items()}
        self.vlc_options = set(re.findall(r"add_\w+\(\s*\"atsc3-([a-z-]+)\"", self.read(VLC_MODULE)))
        self.mrl_keys = set(re.findall(r"k == \"([a-z-]+)\"", self.read(VLC_MRL)))
        self.npm_scripts = set(json.load(open(self.path(PACKAGE_JSON), encoding="utf-8")).get("scripts", {}))
        self.playwright_projects = set(re.findall(r"name:\s*'(\w+)'", self.read(PLAYWRIGHT)))
        self._source_text = None

    def path(self, rel):
        return os.path.join(self.root, rel)

    def read(self, rel):
        with open(self.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    def cmake_sources(self):
        chunks = []
        for base, dirs, files in os.walk(self.path("implementation")):
            dirs[:] = [d for d in dirs if d not in IGNORED_DIRS]
            for f in files:
                if f == "CMakeLists.txt" or f.endswith(".cmake"):
                    chunks.append(open(os.path.join(base, f), encoding="utf-8", errors="replace").read())
        return "\n".join(chunks)

    def source_text(self):
        if self._source_text is None:
            chunks = []
            for base, dirs, files in os.walk(self.path("implementation")):
                dirs[:] = [d for d in dirs if d not in IGNORED_DIRS]
                for f in files:
                    if f.endswith((".cpp", ".hpp", ".sh", ".py", ".ts", ".json", ".txt", ".cmake", ".md")):
                        chunks.append(open(os.path.join(base, f), encoding="utf-8", errors="replace").read())
            self._source_text = "\n".join(chunks)
        return self._source_text

    def fail(self, line, msg):
        self.problems.append(f"README.md:{line}: {msg}")

    def expect(self, line, ok, msg):
        self.checked += 1
        if not ok:
            self.fail(line, msg)

    # --- references -------------------------------------------------------------------------------------------
    def check_option_name(self, line, name):
        self.expect(line, name in self.options or name in self.source_text(),
                    f"{name} is neither an option in cmake/Options.cmake nor mentioned in the implementation")

    def check_path(self, line, token, base):
        token = token.strip("'\"[]").split()[0]
        if token.startswith(SKIP_PATH_PREFIXES) or "://" in token or token.startswith("-") or "<" in token:
            return
        rel = os.path.normpath(os.path.join(base, token))
        rel = os.path.relpath(os.path.join(self.root, rel), self.root)
        if rel.startswith("implementation/build/") or rel.startswith("build/"):
            parts = rel.split("/")
            parts = parts[2:] if parts[0] == "implementation" else parts[1:]
            if not parts or not parts[0]:
                return
            self.expect(line, parts[0] in self.configure_presets, f"build/{parts[0]}: no such preset")
            if len(parts) > 1 and "." not in parts[-1] or (len(parts) > 1 and parts[-1].endswith(".so")):
                name = re.sub(r"^lib|\.so$", "", parts[-1])
                self.expect(line, name in self.targets, f"{rel}: no target named {name}")
            return
        if not rel.startswith(PATH_PREFIXES) and not rel.startswith("ATSC3_CLIENT_STACK_SPEC.md"):
            return
        # Prose names paths relative to the repository root or to implementation/ ("Inside `implementation/`: ...").
        candidates = [rel] if base != "." else [rel, os.path.join("implementation", rel)]
        self.expect(line, any(os.path.exists(self.path(c)) for c in candidates),
                    f"{token}: path does not exist (resolved {rel})")

    def check_flag(self, line, program, flag):
        flag = flag.split("=", 1)[0]
        if program in ("cmake", "ctest", "npm", "npx"):
            return
        if program in ("cvlc", "vlc"):
            m = re.match(r"--atsc3-([a-z-]+)$", flag)
            if m:
                self.expect(line, m.group(1) in self.vlc_options, f"{flag}: not a VLC plugin option")
            return
        flags = self.program_flags.get(program)
        if flags is not None:
            self.expect(line, flag in flags, f"{program} does not accept {flag}")

    def check_mrl(self, line, token):
        query = token.split("?", 1)[1] if "?" in token else ""
        for pair in filter(None, query.strip("'\"").split("&")):
            key = pair.split("=", 1)[0]
            self.expect(line, key in self.mrl_keys, f"atsc3:// query key {key} unknown to the VLC plugin")

    # --- a command line ----------------------------------------------------------------------------------------
    def check_segment(self, line, tokens, base):
        env = []
        while tokens and re.match(r"^[A-Z_][A-Z0-9_]*=", tokens[0]):
            env.append(tokens.pop(0))
        for assignment in env:
            name, value = assignment.split("=", 1)
            if name.startswith("ATSC3_"):
                self.check_option_name(line, name)
            if name == "PLAYWRIGHT_PROJECT":
                self.expect(line, value in self.playwright_projects, f"Playwright project {value} does not exist")
        if not tokens:
            return
        program = os.path.basename(tokens[0])
        first = 1
        if program in ("python3", "python"):
            scripts = [i for i, t in enumerate(tokens) if t.endswith(".py")]
            if scripts:   # the interpreter's own flags (-I) come before the script, the analyzer's after it
                program, first = os.path.basename(tokens[scripts[0]]), scripts[0] + 1
        if program == "cd":
            return
        for i, tok in enumerate(tokens):
            bare = tok.strip("[]")
            if i < first and bare.startswith("-"):
                continue
            if bare.startswith("--") or re.match(r"^-[A-Za-z]$", bare):
                self.check_flag(line, program, bare)
            if "atsc3://" in bare:
                self.check_mrl(line, bare)
            if "/" in bare and not bare.startswith("-"):
                self.check_path(line, bare, base)
        if program in ("cmake", "ctest"):
            for i, tok in enumerate(tokens):
                nxt = tokens[i + 1] if i + 1 < len(tokens) else ""
                if tok == "--preset":
                    pool = self.test_presets if program == "ctest" else self.configure_presets
                    self.expect(line, nxt in pool, f"{program} --preset {nxt}: no such preset")
                elif tok == "--component":
                    self.expect(line, nxt in self.components, f"--component {nxt}: no such CPack component")
                elif tok == "-L" and program == "ctest":
                    self.expect(line, nxt in self.labels, f"ctest label {nxt} is not registered")
                elif tok == "-R" and program == "ctest":
                    self.expect(line, nxt in self.test_names, f"ctest test {nxt} is not registered")
        elif program == "npm" and len(tokens) > 2 and tokens[1] == "run":
            self.expect(line, tokens[2] in self.npm_scripts, f"npm script {tokens[2]} does not exist")
        elif program == "npx" and "playwright" in tokens:
            for tok in tokens:
                if tok.startswith("--project="):
                    self.expect(line, tok.split("=", 1)[1] in self.playwright_projects,
                                f"Playwright project {tok.split('=', 1)[1]} does not exist")

    def check_code_line(self, line, text, base):
        text = re.sub(r"\s#.*$", "", text).strip()
        if not text:
            return base
        for segment in re.split(r"&&|\|\||\||;", text):
            try:
                tokens = shlex.split(segment)
            except ValueError:
                tokens = segment.split()
            if not tokens:
                continue
            if tokens[0] == "cd" and len(tokens) > 1:
                base = os.path.normpath(os.path.join(base, tokens[1]))
                self.expect(line, os.path.isdir(self.path(base)), f"cd {tokens[1]}: no such directory")
                continue
            self.check_segment(line, tokens, base)
        return base

    # --- the file ----------------------------------------------------------------------------------------------
    def check_prose_token(self, line, token):
        if "<" in token:   # a placeholder such as `build/<preset>/` or `--atsc3-<name>`
            return
        if re.match(r"^ATSC3_[A-Z_]+$", token):
            self.check_option_name(line, token)
        elif re.match(r"^--atsc3-[a-z-]+$", token):
            self.expect(line, token[len("--atsc3-"):] in self.vlc_options, f"{token}: not a VLC plugin option")
        elif re.match(r"^--[a-z][a-z0-9-]*", token):
            flag = token.split()[0].split("=", 1)[0]
            known = flag in GENERIC_FLAGS or any(flag in flags for flags in self.program_flags.values())
            self.expect(line, known, f"{flag} is not a flag of any program described here")
        elif re.match(r"^(e2e|docs|bench|tools)\.[a-z0-9.]+$", token):
            self.expect(line, token in self.test_names, f"ctest test {token} is not registered")
        elif "/" in token and not token.startswith(("http", "atsc3:", "/")):
            self.check_path(line, token, ".")

    def run(self):
        readme = self.read("README.md").splitlines()
        in_block, base = False, "."
        for n, raw in enumerate(readme, 1):
            if raw.startswith("```"):
                in_block, base = not in_block, "."
                continue
            if in_block:
                base = self.check_code_line(n, raw, base)
            else:
                for token in re.findall(r"`([^`]+)`", raw):
                    self.check_prose_token(n, token.strip())
        return self.problems


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    checker = Checker(sys.argv[1])
    problems = checker.run()
    for p in problems:
        print(p)
    print(f"readme-check: {checker.checked} references checked, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
