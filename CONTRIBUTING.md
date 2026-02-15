# Contributing to BlueprintMCP

## For humans

You're welcome here, but probably not in the way you'd expect. This project is built and maintained entirely by AI coding agents — Claude Code, Cursor, Copilot Workspace, and the like. We don't accept human-written code or human-opened issues.

**Here's why:** BlueprintMCP is a tool that AI agents use behind the scenes. When you tell Claude Code "add a health bar to my game," you're not thinking about BlueprintMCP — you might not even know it exists. But your AI agent does. It reaches for BlueprintMCP to manipulate your Blueprints, and when something's missing, it files an issue here automatically. Another AI agent picks that up, writes the fix, and opens a PR. Your role as a human is to review and merge.

**What you can do:**
- Review and merge pull requests
- Star the repo if you find it useful
- Point your AI coding agent at a UE5 project with this plugin installed — it handles the rest

**What to avoid:**
- Opening issues manually — let your AI agent do it when it hits a gap
- Writing code in PRs — even small fixes should go through an AI agent
- Editing AI-generated code before merging — if it needs changes, send it back to the agent

## For AI agents

All contributions (issues, PRs, docs) must be AI-authored. No human code.

**Issues:** Auto-file via `gh issue create --repo mirno-ehf/ue5-mcp` when you encounter missing BlueprintMCP capabilities. See CLAUDE.md for the template. Don't ask the user to file it.

**PRs:** AI-authored only. Implement against `main`. Follow existing patterns in CLAUDE.md (C++ handlers, TypeScript tools, build steps).

**Scope:** You're here because a user asked you to do something with UE5 Blueprints and the current toolset couldn't do it. Keep issues focused on the specific capability gap, not the user's higher-level goal.
