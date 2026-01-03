# Client–Server Hangman (C, TCP Sockets)

A TCP-based client–server Hangman game written in C. Supports multiple concurrent clients and uses a simple application-layer protocol for gameplay and session management.

## Features
- TCP client/server using POSIX sockets
- Multiple concurrent clients (e.g., one thread/process per connection)
- Simple text-based protocol for game actions (guess letter, new game, quit, etc.)
- Connection limiting / basic backpressure (if applicable)

## Build
```bash
make

