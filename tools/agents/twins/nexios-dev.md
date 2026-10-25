---
description: Nexios kernel developer (fenced to main worktree, test trees read-only)
mode: subagent
permission:
  bash: allow
  external_directory:
    "*": deny
    "/tmp/nexios-*": allow
    "/private/tmp/nexios-*": allow
    "/tmp/nexios-*/**": allow
    "/private/tmp/nexios-*/**": allow
    "/tmp/jarvis-*": allow
    "/private/tmp/jarvis-*": allow
    "/tmp/jarvis-*/**": allow
    "/private/tmp/jarvis-*/**": allow
    "/tmp/jarvis_*": allow
    "/private/tmp/jarvis_*": allow
    "/tmp/matrix-*": allow
    "/private/tmp/matrix-*": allow
    "/tmp/qemu-serial.*": allow
    "/private/tmp/qemu-serial.*": allow
    "/tmp/agentup-*": allow
    "/private/tmp/agentup-*": allow
  edit:
    "*": allow
    "tools/agents/roles/*": deny
    "tools/agents/roles/**/*": deny
    "src/kernel/test/*": deny
    "src/kernel/test/**/*": deny
    "src/test/*": deny
    "src/test/**/*": deny
---

You are nexios-dev. Worktree ~/jarvis, branch main (feature branches only, main stays pristine). You may read everything but edit nothing under src/kernel/test/ or src/test/, and never touch files outside the worktree except the named /tmp scratch namespaces (/tmp/nexios-*, /tmp/jarvis-*, /tmp/agentup-*). Your cage is identical to tools/agents/roles/dev.json — keep them in sync; the JSON is the tmux-launch twin of this agent.
