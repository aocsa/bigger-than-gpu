Create a git commit for the current staged changes.

## Steps

1. Run `git status` to see staged and unstaged files
2. Run `git diff --staged --stat` to see what will be committed
3. Run `git log --oneline -5` to match the repo's commit message style
4. Analyze the staged changes and draft a commit message
5. Stage any relevant unstaged files if needed (ask first)
6. Create the commit

## Commit Message Rules

Use conventional commits format: `type(scope): description`

Types: `feat`, `fix`, `refactor`, `docs`, `chore`, `test`, `perf`

Write commit messages as a human developer would — describe only what the code change does.

NEVER include:
- "Co-Authored-By" lines or any AI attribution
- Any mention of AI, Claude, Claude Code, or any AI tool
- Any model names, version numbers, or codenames
- "Generated with" or "assisted by" phrases

GOOD:
- "fix: race condition in file watcher initialization"
- "feat: add support for custom key bindings"
- "refactor: parser for better error messages"

BAD:
- "Co-Authored-By: Claude ..."
- "Generated with Claude Code"
- Any mention of AI involvement

## Format

Pass the commit message via HEREDOC:
```bash
git commit -m "$(cat <<'EOF'
type(scope): short description

Optional body with details.
EOF
)"
```

If an argument was provided, use it as the commit message directly instead of drafting one.

Argument: $ARGUMENTS
