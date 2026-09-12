#!/bin/zsh
# refresh_vault.sh — one-way workspace -> vault refresh.
# Copies new/changed files only (never deletes vault notes), then
# regenerates tags + Related links (both scripts idempotent).
set -e
WS="$HOME/jarvis"
V="$HOME/vaults/jarvis"
mkdir -p "$V/specs" "$V/audits"
rsync -au "$WS/docs/specs/" "$V/specs/"
rsync -au "$WS/audits/" "$V/audits/"
python3 "$WS/tools/tag_vault.py" --vault "$V" --apply
python3 "$WS/tools/autolink_vault.py" --vault "$V" --apply
