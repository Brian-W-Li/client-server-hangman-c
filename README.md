# Client–Server Hangman (C, TCP Sockets)

A TCP-based client–server Hangman game written in C. Supports multiple concurrent clients and uses a simple application-layer protocol for gameplay and session management.

## Overview

This project is a low level networked game using POSIX sockets.
A central server accepts TCP connections, forks a child process per client, and maintains independent Hangman game sessions for each connection.

## Features
- TCP client/server using POSIX sockets
- Multiple concurrent clients via fork()
- Simple text-based protocol for game actions
- Connection limiting
- Child process cleanup with waitpid
- Clean build system via Makefile

## Architecture
Server
- Listens on a TCP socket
- Forks a child process for each accepted client
- Manages per-client game state 
Client
- Connects to server over TCP
- Sends guesses and receives game state updates
- Renders gameplay in a terminal interface

## Build and Run

make
./hangman_server <port>
./hangman_cleint <server_ip> <port>

