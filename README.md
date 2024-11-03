# Multi-Threaded Proxy Server

This project implements a multi-threaded proxy server in C, utilizing HTTP parsing methods inspired by this proxy server implementation.

## Features

### Multi-threading
- Implemented with pthread and semaphores
- Enables efficient handling of multiple client requests
- Uses thread synchronization mechanisms:
  - pthread_join() with thread ID for synchronization
  - Semaphore-based synchronization (sem_wait() and sem_post())
  - Semaphores used as simpler alternative to condition variables

### Caching
- Implements response caching for improved performance on repeated requests
- See limitations section for caching constraints

## Limitations

### Multiple Clients for the Same URL
- Each client's response for the same URL is stored as a separate cache element
- When retrieving from cache, only chunks of the response may be sent
- May result in incomplete page loads

### Fixed Cache Size
- Cache has a fixed element size
- Larger websites may not be fully stored in cache

## Getting Started

### Prerequisites
- C compiler
- Make utility
- Git

### Installation and Usage

1. Clone the repository:
   ```bash
   git clone https://github.com/warsiKamran/multithreaded-proxy-server.git
   ```

2. Navigate to the project directory:
   ```bash
   cd MultiThreadedProxyServerClient
   ```

3. Build the project:
   ```bash
   make all
   ```

4. Start the proxy server:
   ```bash
   ./proxy <port no.>
   ```
   Note: Use port 8080

5. Access through browser:
   ```
   http://localhost:<port>/https://www.cs.princeton.edu/
   ```
