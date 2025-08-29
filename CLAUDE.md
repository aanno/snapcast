# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Instructions

Currently, we want to extend the client with pipewire support.

For this, we have 2 new files already created:

- `client/player/pipewire_player.hpp`
- `client/player/pipewire_player.cpp`

The PipeWire player implementation has been fixed to resolve segmentation fault crashes.

### PipeWire Player Status: ✅ FIXED

The original crash was caused by improper buffer handling in the `on_process` callback. The implementation has been rewritten to follow official PipeWire examples and best practices.

#### Key Issues Resolved:
- **Segmentation fault**: Fixed null pointer dereference in `d->chunk->offset` access
- **Buffer calculation**: Now uses direct `d->maxsize / stride` calculation (official pattern)
- **Chunk metadata**: Only sets `chunk->size`, avoids accessing potentially NULL chunk fields
- **Include dependencies**: Added missing `common/time_defs.hpp` for `chronos` namespace
- **Latency calculation**: Improved audio timing calculation based on buffer size

#### Technical References:
- **PipeWire Examples**: https://docs.pipewire.org/examples.html
- **Reference Implementation**: https://raw.githubusercontent.com/PipeWire/pipewire/refs/heads/master/src/examples/audio-src.c
- **pw-cat Source**: https://raw.githubusercontent.com/PipeWire/pipewire/refs/heads/master/src/tools/pw-cat.c

The implementation now follows the official `audio-src.c` example pattern exactly, ensuring compatibility and stability.

- If you are technically stuck or unsure about the next step, ask for help.
- Use gw-memory to store and retrieve information about the codebase.
  + Tag all entries with 'snapcast' to indicate they are related to this project.
  + After you have been started, it is a good idea to retrieve what's has been stored lately, so you have the latest context.

## Testing

You run in a devcontainer environment and you are _not_ able to test the client. You have to use me as 'tool' to test the client. I will run the client on my machine and report back the results.

This is the test procedure:

* I will start snapserver with: `scripts/run-snapserver.sh`
* I will start snapclient with: `scripts/run-snapclient.sh`

Remember that you are not able to do this on your own. You have to use me as 'tool' to do this.

## Building

This is the build procedure:

- `rm -r build; mkdir build` - only for a clean build (and first time)
- `cd build/`
- `../scripts/cmake.sh` - only to regenerate make files
- `make -j8` - to build the project

## MCP tool usage

Don't use tool 'vscode-mcp-server - execute_shell_command_code (MCP)' because of
issues. Instead, use bash directly.

For editing file, use tool 'Opened changes in Visual Studio Code'. This is 
much better than tool 'update'. But if you use tool 'update', don't forget to 
use tool 'filesystem - read_text_file (MCP)' before that. Otherwise you get the
following error: File has not been read yet. Read it first before writing to it.


## Project Overview

Snapcast is a multiroom client-server audio player, where all clients are time synchronized with the server to play perfectly synced audio. It's not a standalone player, but an extension that turns your existing audio player into a Sonos-like multiroom solution.

The project is written in C++ with cmake as build system and consists of several components:

- **snapserver**: The server component that streams audio to clients, source folder `server`.
- **snapclient**: The client component that receives and plays audio, source folder `client`.
- **common**: Shared code used by both server and client, source folder `common`.
