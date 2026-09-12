# SETUP-GRAPHIFY.md — graphify + Obsidian + MCP Setup

Target: `graphify` CLI as graph index over code + Obsidian notes,
exposed to agents via MCP stdio servers (`mcp.json`).
Verified on: macOS (darwin, zsh), Python 3.14, graphifyy 0.9.6,
Node v22.22.3, `graphify` at `/opt/homebrew/bin/graphify`.
Full spec: `MCP-CONFIG.md`. Config: `mcp.json`.

## 1. Prerequisites

- Python 3.10+ and Node.js 22+ (`python3 --version`, `node --version`)
- Obsidian vault dir containing an `.obsidian/` directory
- Existing `graphify-out/graph.json` is reused if present

```bash
python3 --version          # >= 3.10
node --version             # >= v22 (obsidian-mcp@2 requirement)
mkdir -p ~/vaults/jarvis/.obsidian
```

## 2. graphify CLI + MCP Serve Extra

```bash
python3 -m pip install graphifyy -q --break-system-packages
python3 -m pip install -q "graphifyy[mcp]" --break-system-packages
# Pin: graphifyy 0.9.6 serve.py needs AnyUrl (removed in mcp 2.x)
python3 -m pip install -q "mcp==1.8.0" --break-system-packages
```

Verify:

```bash
which graphify
"$(cat graphify-out/.graphify_python 2>/dev/null || echo python3)" \
 -c "import graphify.serve, mcp; print('serve: OK')"
```

## 3. Path & Environment Configuration

```bash
export OBSIDIAN_VAULT_PATH="$HOME/vaults/jarvis"
export GRAPHIFY_GRAPH="$HOME/jarvis/graphify-out/graph.json"
export PATH="$HOME/.local/bin:/opt/homebrew/bin:$PATH"
```

Persist in `~/.zshrc`, then `source ~/.zshrc` and confirm both vars.

## 4. Graph Initialization (CLI)

Full index (first run — agent-driven `/graphify .` pipeline; writes
`graphify-out/graph.json`, `GRAPH_REPORT.md`, `manifest.json`).
Incremental refresh after any code/note change (mandatory post-step):

```bash
graphify update .
```

## 5. MCP Servers (`mcp.json`)

`mcp.json` defines two stdio servers: `graphify`
(`python -m graphify.serve <graph.json>`, 10 tools) and `obsidian`
(`npx -y obsidian-mcp@2 serve --vault jarvis=<vault>`).
Copy the blocks into your client config (Claude Desktop / Cursor /
Windsurf accept `mcp.json` as-is); for opencode, merge the `mcp`
variant from `MCP-CONFIG.md` §3 into `opencode.json` and restart.

Install + verify:

```bash
# obsidian server readiness (vault must exist with .obsidian/)
npx -y obsidian-mcp@2 doctor --vault jarvis="$OBSIDIAN_VAULT_PATH"
```

Agent-level checks: `mcp__graphify__query("scheduler")` must return
`NODE Scheduler [src=src/kernel/task/scheduler.hpp ...]`;
`mcp__obsidian__read_note` must return the note plus `etag`.

## 6. CLI Verification (Fallback Path)

```bash
graphify query "test"          # must return nodes, not an error
graphify path "Scheduler" "IPC"
graphify explain "Scheduler"
ls graphify-out/graph.json
```

## 7. Obsidian Vault Convention

- Authority docs live in the vault: specs (`specs/`), roadmaps
  (`roadmaps/`), test cases/logs (`tests/`).
- MCP writes must preserve YAML frontmatter, `[[wikilinks]]`, and
  folder structure — links are graph edges.
- Agents read via `mcp__graphify__query` / `mcp__graphify__explain`
  (or CLI fallback), never by ingesting raw spec files wholesale.

## 8. Gotchas

- No `graphify build` subcommand — index via `/graphify .`, refresh
  via `graphify update .` (CLI-only; no MCP update tool exists).
- `graphify . --mcp` does NOT launch MCP in graphifyy 0.9.6 — serve
  via `python -m graphify.serve`.
- `graphify-out/.graphify_python` missing → re-resolve interpreter
  per the skill guard before running subcommands.
- Dirty `graphify-out/` files after updates are expected.
