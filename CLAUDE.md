# CLAUDE.md

## Git

- Commit and push directly to `main`. Do not create branches or pull requests unless asked.
- Never put links to Claude sessions or chats (`claude.ai/...`, `Claude-Session:` lines, "Generated with Claude Code" links) in commit messages, PR titles or descriptions, comments, or any file in the repo.

## Tests

- Don't rerun the test suite after every edit. Rebuilding takes a long time, so rerun only after meaningful changes (logic, build config, interfaces). Skip it for docs, comments, renames, and trivial tweaks.

## README

- Before ending a session, check whether the README needs an update for what changed (features, env vars, commands, data sources). If it does, update it.
