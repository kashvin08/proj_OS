================================================================
           RACE-30: HYBRID MULTI-PROCESS GAME SERVER
================================================================

Project Overview
----------------
Race-30 is a high-performance C game server built using a hybrid concurrency model. It combines multi-process session handling with multi-threaded system management to ensure a lag-free, 
fault-tolerant experience. A player starts by adding a number (1-3) to 0 (initial sum) which is later followed by the remaining players until a player reaches 30 first.

-------------------------------------

Core Architecture (My Contributions)
-------------------------------------
The server operates on three distinct layers of concurrency:

1. THE LOBBY (Parent Process): 
   Uses POSIX 'select()' to monitor the Lobby FIFO. It manages 
   incoming 'JOIN' requests and acts as the process spawner.

2. SESSION HANDLERS (Child Processes): 
   Every player is assigned a dedicated child process via 'fork()'.
   This ensures "Fault Isolation"—if one player's session crashes, 
   the rest of the server remains unaffected.

3. SYSTEM SERVICES (Background Threads):
   - Scheduler Thread: Enforces a 30-second Round-Robin quantum.
   - Logger Thread: An asynchronous worker that writes game events 
     to 'game.log' without blocking the game logic.


Technical Features
------------------
* IPC: Uses Shared Memory (shm) for the global game state and 
  POSIX Semaphores for synchronization.
* Communication: Driven by Named Pipes (FIFOs) for private 
  request/reply channels between clients and the server.
* Persistence: Automatic score saving/loading via 'scores.txt'.
* Saftey: Implements 'pthread_atfork' handlers to prevent 
  deadlocks during process cloning.

Compilation & Execution
-----------------------
Requirements: GCC compiler and a Linux/Unix environment.

1. To Compile:
   Type 'make' in the terminal.

2. To Start the Server:
   ./RRserver

3. To Connect a Client:
   ./client (enter a name)

Controls
--------
- Players type numbers 1, 2, or 3 to add to the total.
- The goal is to reach exactly 30.
- Type 'quit' to exit the session.

File Structure:
--------------
- RRserver.c      : Main server core, threads, and process logic.
- game.c          : Move validation and win-condition logic.
- include/state.h : Shared memory structure definitions.
- game.log        : Auto-generated event log.
- scores.txt      : Persistent player win counts.

================================================================

Tech Stack:
--------------
C Programming
POSIX Threads (pthreads)
Process Management (fork, select)
IPC (Shared Memory, Semaphores, FIFOs)
