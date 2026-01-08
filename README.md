## Mycord TCP Chat Client (C)
Final Project for CMPSC311 at Penn State University

This project is a **custom TCP chat client written in C** that connects to a remote chat server using a defined binary protocol over **IPv4 TCP sockets**. The client demonstrates low-level networking, protocol implementation, concurrent I/O, and robust error handling on Linux/Unix systems.

### Key Networking & Systems Concepts Demonstrated

- **TCP/IP Networking**
  - Creates and manages an IPv4 **TCP socket** using `socket()`, `connect()`, `read()`, `write()`, and `close()`
  - Supports direct IP connections (`--ip`) and **DNS-based hostname resolution** (`--domain`) via `gethostbyname()`
  - Proper use of network byte order (`htonl`, `htons`, `ntohl`) to ensure protocol correctness

- **Custom Binary Protocol Implementation**
  - Implements a fixed-size **1,064-byte message protocol** over TCP
  - Uses a packed C struct to match the server’s expected memory layout
  - Handles multiple message types including:
    - Login / Logout
    - Client message sending
    - Server message receiving
    - System notifications
    - Server-initiated disconnects
  - Ensures full message integrity across TCP reads and writes

- **Concurrent Network I/O**
  - Uses **POSIX threads (`pthread`)** to allow:
    - A background receive thread that continuously listens to the TCP socket
    - A main thread that simultaneously reads user input and sends messages
  - Prevents blocking behavior and enables real-time chat functionality

- **Signal-Safe Network Shutdown**
  - Gracefully handles `SIGINT` and `SIGTERM`
  - Sends a LOGOUT message before closing the TCP connection
  - Ensures clean shutdown on EOF (Ctrl+D) or server disconnects

- **Robust Input Validation & Error Handling**
  - Validates all outbound messages before sending to avoid server disconnects
  - Handles network failures, invalid protocol messages, and connection errors safely
  - Prints consistent, descriptive error messages to `STDERR`

### Protocol Overview

Each message exchanged with the server follows a strict binary format:

| Field | Size |
|------|------|
| Message Type | 4 bytes |
| UNIX Timestamp | 4 bytes |
| Username | 32 bytes |
| Message Body | 1024 bytes |

Messages are sent and received **as atomic struct-sized TCP writes and reads**, demonstrating correct handling of structured data over a stream-based protocol.

### Features

- Real-time chat over TCP
- DNS hostname resolution
- Threaded message receiving
- UNIX signal handling
- ANSI color highlighting for mentions and system events
- Clean shutdown and resource management

### Build & Run

```bash
gcc client.c -o client -pthread
./client --domain mycord.devic.dev
```

### Why This Project Matters

This project showcases practical experience with:
- Low-level socket programming
- TCP stream semantics
- Concurrent programming in C
- Network protocol design and implementation
- Linux system calls and signals

It reflects real-world systems programming patterns used in network clients, messaging software, and distributed systems.
