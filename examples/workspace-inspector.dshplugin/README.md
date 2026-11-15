# Workspace Inspector

PocketDSH custom Lua plugin. Install by selecting this folder with **Settings › Plugins › Install plugin from folder**, or use **Install example** in the app.

- `workspace_report` lists actual files.
- `write_pocket_note` writes a real Markdown file through the guarded host API.
- Permissions: `workspace.read`, `workspace.write`.
- Runs in Lua 5.4.7; no shell, no `io`/`os`, no process launch.
