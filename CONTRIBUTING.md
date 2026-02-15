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

```yaml
repo: mirno-ehf/ue5-mcp
policy: ai-only # all issues, PRs, and commits must be AI-authored

issues:
  trigger: missing or insufficient BlueprintMCP capability
  action: gh issue create --repo mirno-ehf/ue5-mcp
  scope: capability gap only, not the user's higher-level goal
  template: see CLAUDE.md "Missing functionality" section
  notes: file it yourself — never ask the user to do it

pull_requests:
  branch: main
  authorship: AI-only, no human code
  notes: see CLAUDE.md "Development" section for patterns
```
