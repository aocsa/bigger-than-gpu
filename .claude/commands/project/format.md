Run clang-format on changed files to enforce the project's code style.

If arguments were provided, format those specific files:
```bash
clang-format -i $ARGUMENTS
```

If no arguments were provided, format all uncommitted C++ files:
```bash
git diff --name-only --diff-filter=ACMR -- '*.hpp' '*.cpp' | xargs clang-format -i
git diff --staged --name-only --diff-filter=ACMR -- '*.hpp' '*.cpp' | xargs clang-format -i
```

After formatting, show which files were modified:
```bash
git diff --name-only
```

Argument: $ARGUMENTS
