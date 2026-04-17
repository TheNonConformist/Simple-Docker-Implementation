# Multi-Container Runtime

## Team Information
- Student 1: [Mohammed Rizwan Shaikh] - [PES1Ug24CS272]
- Student 2: [Muhammad Affan Dumba] - PES1UG24CS280

## Architecture Overview
This project implements a lightweight Linux container runtime with a unified user-space daemon and kernel-space memory monitor.

- **Supervisor**: A long-running parent daemon managing multiple isolated containers, logging, and metadata.
- **Client CLI**: A short-lived command interface to communicate with the supervisor over UNIX Domain Sockets.
- **Kernel Monitor**: A custom Loadable Kernel Module (LKM) monitoring PID memory consumption and enforcing constraints in Ring 0.

## Build Instructions
1. Install dependencies (on Ubuntu 22.04/24.04 without WSL & Secure Boot OFF):
   ```bash
   sudo apt update
   sudo apt install -y build-essential linux-headers-$(uname -r)
   ```
2. Build the project:
   ```bash
   cd container-runtime
   make
   ```
3. Load the memory monitor kernel module:
   ```bash
   sudo insmod monitor.ko
   ```

## Setup Instructions (Alpine rootfs)
1. Prepare the base Alpine mini root filesystem:
   ```bash
   mkdir rootfs-base
   wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
   tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
   ```
2. Create isolated copies for individual containers:
   ```bash
   cp -a ./rootfs-base ./rootfs-alpha
   cp -a ./rootfs-base ./rootfs-beta
   ```

## Running Instructions
Open two terminal windows.

**Terminal 1 (Supervisor Daemon)**
```bash
sudo ./engine supervisor ./rootfs-base
```

**Terminal 2 (Client Interaction)**
```bash
# Launch a background container
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80

# Check container states
sudo ./engine ps

# Review logs
sudo ./engine logs alpha

# Cleanly stop the container
sudo ./engine stop alpha
```

## CLI Commands
| Command | Description |
|---------|-------------|
| `engine supervisor <base-rootfs>` | Launches the persistent runtime server. |
| `engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]` | Asynchronously spawns an isolated container. |
| `engine run <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]` | Synchronously launches a container, blocking until process termination. |
| `engine ps` | Prints all tracked container processes and live metadata. |
| `engine logs <id>` | Fetches streamed output stored by the logging pipeline. |
| `engine stop <id>` | Issues a SIGTERM to gracefully terminate an active container. |

## Demonstrations (Screenshots): included in submitted PDF

## Engineering Analysis

### 1. Isolation Mechanisms
We deploy `clone(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS)` immediately upon instantiating a process to forge boundaries. `chroot()` pairs structurally to redirect the perceived location of `/` directly onto an independent `rootfs`. 

### 2. Supervisor and Process Lifecycle
Maintaining a central daemon centralizes log streams preventing descriptor leakages and orphaned states (zombie exhaustion). It catches native `SIGCHLD` kernel pulses instantly, allowing zero-latency updates out to client polling connections.

### 3. IPC, Threads, and Synchronization
`AF_UNIX` (Unix Domain Sockets) facilitate command transfer natively without string pipe collisions. Our bounded buffer leverages precise `pthread_mutex_lock` logic fused with `pthread_cond_wait`/`pthread_cond_signal` handling. Producer threads wait seamlessly upon full thresholds removing spin loops.

### 4. Memory Management and Enforcement
The Ring 0 visibility ensures OOM anomalies cannot swamp the parent host via latency. The kernel loops efficiently resolving exact RSS footprint footprints utilizing `.mm_struct` allocations.

### 5. Scheduling Behavior
Passing explicit weights via `nice()` filters natively to the CFS (Completely Fair Scheduler).

## Design Decisions
1. **Namepsace & Chroot Pairing**: Simplifies escaping vulnerabilities compared to directory binds, optimizing lightweight testing constraints.
2. **Unix Domain Sockets**: Bypasses network stack overhead entirely while ensuring clean bi-directional streaming for CLI status feedback.
3. **Synchronized Log Queues**: Disconnects high-IO container outputs from disk bottlenecks, preventing cascading lock delays inside applications.
4. **Kernel Space Kill Execution**: Eliminates the unpredictable scheduler latency found in pure user-space `while(1)` polling loops assessing memory pressure.
