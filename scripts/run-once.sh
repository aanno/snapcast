#!/bin/bash -x

source /home/vscode/.bashrc

~/.volta/bin/pnpm set -g store-dir /pnpm
~/.volta/bin/pnpm setup

source /home/vscode/.bashrc
pnpm install -g @anthropic-ai/claude-code
# ln -s /home/vscode/.local/share/pnpm/global/5/node_modules/\@anthropic-ai/claude-code/cli.js ~/.volta/bin/claude
