# UE5 MCP — AI-Powered Blueprint Editing for Unreal Engine

Vibe code your Blueprints. This plugin lets Claude Code (or any MCP client) read, modify, and create Unreal Engine 5 Blueprints — just describe what you want in plain English.

> "Add a health component to my player character" · "Find everywhere I use GetActorLocation and replace it" · "What does my damage system do?"

https://github.com/user-attachments/assets/c52a4cae-d882-4d60-acf9-2204ddd2ab89

## Getting Started

Tell Claude Code:

```
Set up https://github.com/mirno-ehf/ue5-mcp in my project
```

## How It Works

A UE5 editor plugin exposes your project's Blueprints over a local HTTP server. An [MCP](https://modelcontextprotocol.io) wrapper connects that to AI tools like Claude Code. When the editor is open, it runs inside the editor process with zero overhead. When the editor is closed, it can spawn a headless process instead.

## License

[MIT](LICENSE)
