Run clang-tidy on changed C++ files and report findings.

If arguments were provided, check those specific files:
```bash
clang-tidy -p build $ARGUMENTS
```

If no arguments were provided, check all uncommitted C++ files:
```bash
git diff --name-only --diff-filter=ACMR -- '*.hpp' '*.cpp' | xargs clang-tidy -p build 2>&1
```

Group findings by severity (error, warning, note). For each finding, show the file, line, and the diagnostic message. Suggest fixes for the most critical ones.

Argument: $ARGUMENTS
