# MCP-CONFIG.md — MCP Server Specification (graphify + Obsidian + GitHub)

Companion to `mcp.json` (portable `mcpServers` config) and
`SETUP-GRAPHIFY.md` (installation procedure).
Verified on: macOS (darwin, zsh), Python 3.14, graphifyy 0.9.6,
Node v22.22.3. MCP handshake + `tools/list` + `query_graph` call
verified against `graphify-out/graph.json` 2026-09-12.

## 1. Architecture

```
Agent ──MCP(stdio)──▶ graphify serve ──▶ graphify-out/graph.json
      ──MCP(stdio)──▶ obsidian-mcp ──▶ $OBSIDIAN_VAULT_PATH (*.md)
      ──MCP(remote)─▶ github (https://api.githubcopilot.com/mcp/) ──▶ staycool1374-Ger/nexios
```

Three servers, one per concern: graph queries (read-only
index), vault CRUD (authoritative notes), and GitHub (issues/PRs/repos).
Local stdio servers use no network listeners; `github` is remote and
requires `GITHUB_PERSONAL_ACCESS_TOKEN`.

## 2. Servers

| Server | Transport | Command | Corpus |
|---|---|---|---|
| `graphify` | stdio | `python3 -m graphify.serve <graph.json>` | `graphify-out/graph.json` |
| `obsidian` | stdio | `npx -y obsidian-mcp@2 serve --vault jarvis=<vault>` | `$OBSIDIAN_VAULT_PATH` |
| `github` | remote | `https://api.githubcopilot.com/mcp/` (`opencode.json` `mcp.github`, `Bearer {env:GITHUB_PERSONAL_ACCESS_TOKEN}`) | `staycool1374-Ger/nexios` |

`graphify` prerequisites: `pip install "graphifyy[mcp]"` **then pin
`mcp==1.8.0`** — graphifyy 0.9.6 `serve.py` imports `AnyUrl` from
`mcp.types`, removed in mcp 2.x (handshake fails otherwise).
`obsidian` prerequisites: Node 22+, vault dir exists with an
`.obsidian/` directory, absolute vault path.

## 3. opencode Variant

Schema (`https://opencode.ai/config.json`, `McpLocalConfig`):
`command` is an **array** of strings, `additionalProperties: false`
(no `args` key — string-command + `args` fails validation and the
server is silently dropped). Restart opencode after saving:

```json
"mcp": {
  "graphify": {
    "type": "local",
    "enabled": true,
    "command": ["/opt/homebrew/opt/python@3.14/bin/python3.14", "-m", "graphify.serve", "/Users/arnold/jarvis/graphify-out/graph.json"],
    "environment": {}
  },
  "obsidian": {
    "type": "local",
    "enabled": true,
    "command": ["npx", "-y", "obsidian-mcp@2", "serve", "--vault", "jarvis=/Users/arnold/vaults/jarvis"],
    "environment": { "OBSIDIAN_VAULT_PATH": "/Users/arnold/vaults/jarvis" }
  }
}
```

## 4. Canonical Tool Mapping

Agent-facing names (used in `AGENTS.md` / `PROMPT-*.md`) → real tools:

| Canonical | Server tool | Notes |
|---|---|---|
| `mcp__graphify__query` | `query_graph` | BFS/DFS + `token_budget` |
| `mcp__graphify__path` | `shortest_path` | two-node connection trace |
| `mcp__graphify__explain` | `get_node` + `get_neighbors` | node detail + edges |
| `mcp__graphify__update` | *(none — CLI only)* | run `graphify update .` via bash |
| `mcp__obsidian__read_note` | `obsidian_read_note` | bounded page + `etag` |
| `mcp__obsidian__write_note` | `obsidian_create_note` / `obsidian_edit_note` | atomic; pass `if_match` etag on edit |
| `mcp__obsidian__search` | `obsidian_search_vault` | content/filename/tag search |

Other verified graphify tools: `get_community`, `god_nodes`,
`graph_stats`, `list_prs`, `get_pr_impact`, `triage_prs`.

| Canonical | Server tool | Notes |
|---|---|---|
| `mcp__github__issue_read` | `github_issue_read` (`get`/`get_comments`/`get_labels`) | pass `owner`+`repo` explicitly |
| `mcp__github__issue_write` | `github_issue_write` (`create`/`update`) + `github_add_issue_comment` | `closes #<n>` handling via body/commit |
| `mcp__github__issue_search` | `github_search_issues` / `github_list_issues` | `search_*` targeted, `list_*` broad pagination |
| `mcp__github__pr` | `github_pull_request_read` (`get`/`get_diff`/`get_files`/`get_check_runs`/`get_review_comments`) + `github_list/search_pull_requests` | PR review via pending-review flow |
| `mcp__github__repo_content` | `github_get_file_contents` / `repo://{owner}/{repo}/...` templates | branch/commit/PR/tag content without clone |

## 5. Hierarchy & Fallback

1. MCP tools first when servers are configured and reachable.
2. Raw CLI fallback (`graphify query/path/explain/update`; `gh` CLI for GitHub) when MCP
   is unreachable — never block on MCP.
3. `graphify update .` is CLI-only; no MCP update tool exists.
4. GitHub: `GITHUB_PERSONAL_ACCESS_TOKEN` required for MCP; on auth/unreachable use `gh` CLI (`export PATH="/opt/homebrew/bin:$PATH"` if needed).

## 6. Verification Protocol

```bash
# graphify MCP: handshake + tool call (expect 10 tools, Scheduler hit)
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"probe","version":"0"}}}' \
 '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
 '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"query_graph","arguments":{"question":"scheduler","token_budget":300}}}' \
 | timeout 25 /opt/homebrew/opt/python@3.14/bin/python3.14 \
 -m graphify.serve graphify-out/graph.json 2>/dev/null | tail -1 | head -c 300
# obsidian MCP: readiness (vault must exist with .obsidian/)
npx -y obsidian-mcp@2 doctor --vault jarvis=/Users/arnold/vaults/jarvis
```

Agent-level checks: `mcp__graphify__query("scheduler")` returns
`NODE Scheduler [src=src/kernel/task/scheduler.hpp ...]`;
`mcp__obsidian__read_note` returns the note plus `etag`.

## 7. Gotchas

- `graphify . --mcp` is NOT an MCP launcher in graphifyy 0.9.6 (runs
  the extraction pipeline) — serve via `python -m graphify.serve`.
- There is no `graphify build` subcommand — index via the `/graphify`
  skill pipeline, refresh via `graphify update .`.
- obsidian-mcp refuses vaults without `.obsidian/` and requires
  absolute `--vault` paths; max ten vaults per server.
- Dirty `graphify-out/` files after updates are expected; never a
  reason to skip graph/MCP usage.
- `github` MCP has no default repo — every call needs
  `owner=staycool1374-Ger, repo=nexios` explicitly (unlike `gh -R`).
- `github` `search_*` vs `list_*`: `search_*` for targeted keyword queries,
  `list_*` for broad pagination (5-10/page, minimal `fields`, omit `body`).
