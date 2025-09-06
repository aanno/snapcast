# devcontainer prepare

```bash
rm -rf .volta/
curl https://get.volta.sh | bash
volta install node pnpm
~/.volta/bin/pnpm set -g store-dir /pnpm
~/.volta/bin/pnpm setup
. ~/.bashrc
pnpm install -g @anthropic-ai/claude-code
```

## vscode

* install mcp server
* install claude-code extension
* install gitlens extension
