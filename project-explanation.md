# OS-Jackfruit: Multi-Container Runtime & Kernel Monitor
**A Technical Review & Explanation Guide**

This guide is designed to prepare you for discussing your project with an interviewer. It explains the high-level architecture, the low-level technical specifics, and the rationale behind the major design decisions in the project.

---

## 1. High-Level Description: What Did I Build?
At a high level, this project builds a **custom, lightweight Linux container runtime** similar to a stripped-down version of Docker or rootless Podman, paired with a custom **kernel module** that monitors and restricts what those containers are allowed to do.

It consists of two main pieces:
1. **User-Space Runtime (`engine.c`)**: This is the process manager. It acts as a long-running supervisor that receives commands (start, stop, logs) from a command-line interface. It creates sandboxed processes (containers) using Linux namespaces, manages their lifecycles, and securely collects their outputs into log files without losing data.
2. **Kernel-Space Monitor (`monitor.c`)**: This is a custom Linux Kernel Module (LKM). Since user-space processes can't easily force strict rules on Memory (without Cgroups, which we purposefully bypass for learning), the Kernel module sits in Ring 0. The supervisor tells the kernel module "watch this container's PID". The kernel module aggressively monitors its RAM usage via a timer, warning it if it hits a "soft limit" or unapologetically killing it if it hits a "hard limit".

---

## 2. Technical Component Breakdown 

### A. Process Isolation & Container Creation (Namespaces)
When a user runs `engine start`, the supervisor spawns a new process. But to make it a "container," it must be isolated.
* **Mechanism Used**: The system call `clone()` is invoked with `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS`.
* **What it means**: 
  - `CLONE_NEWPID` gives the container its own PID tree (it thinks it's PID 1).
  - `CLONE_NEWNS` isolates the filesystem mounts. It can't see the host's `/var` or `/etc`.
  - `CLONE_NEWUTS` allows the container to have its own Hostname isolated from the parent. 
* **The Rootfs**: We then use the system call `chroot()` to trap the process inside a specific folder (like an Alpine Linux folder). We also isolated `/proc` by calling `mount("proc", "/proc", "proc", 0, NULL)` after the chroot, so tools like `ps` work correctly inside the container sand-box.

### B. IPC: Inter-Process Communication (Control Plane)
The project runs as a single binary `engine`. How does `engine start` send a command to the `engine supervisor` that is running in the background?
* **Mechanism Used**: **UNIX Domain Sockets (UDS)**. 
* **Why**: UDS allow two unrelated processes on the same machine to talk safely. The supervisor `bind()`s to an address `/tmp/mini_runtime.sock` and blocks on `accept()` (handled asynchronously using `poll()`). The client command creates a socket, `connect()`s, and writes a C-struct (`control_request_t`) containing the container instructions.
* **Asynchronous Reaping**: The supervisor uses `poll()` to handle multiplexed connections: new client connections, catching `SIGCHLD` signals when containers die (via a self-pipe trick to avoid deadlocks in signal handlers), and catching `SIGINT/SIGTERM` for teardowns.

### C. The Logging Pipeline (Bounded Buffer Synchronization)
Containers stdout/stderr outputs need to be collected asynchronously so that multiple running containers don't block each other.
* **The Flow**: 
  1. Before calling `clone()`, we create a standard `pipe(fd)`. The child container drops `stdout` and `stderr` and redirects them into the write-end of the pipe using `dup2()`.
  2. The supervisor spawns a **Producer Thread** for that container, which continuously `read()`s from the pipe into chunks of data.
  3. The Producer places these chunks into a heavily synchronized **Bounded Buffer**.
  4. A singular **Consumer Thread** (`logging_thread()`) continuously pops off the Buffer and cleanly writes the logs to disk (`logs/<container_id>.log`).
* **Concurrency (Mutexes & Condition Variables)**: To avoid race conditions, we use a `pthread_mutex_t` to lock the buffer. If the buffer is full, the producer calls `pthread_cond_wait(&not_full)` to block without eating CPU cycles. When the consumer pops an item, it calls `pthread_cond_signal(&not_full)` to wake up the blocked producer. This completely prevents deadlocks and lost data.

### D. Kernel-Level Memory Enforcement (`monitor.ko`)
User-space tools (like the supervisor) can't forcefully revoke memory instantly reliably. This is an OS job.
* **Mechanism**: A Loadable Kernel Module (LKM) defining a character device `/dev/container_monitor`.
* **Registration**: After `clone()`ing a container, the supervisor talks to the kernel using `ioctl(MONITOR_REGISTER)` conveying the PID, Soft Limit, and Hard Limit.
* **Periodic Timer**: The module schedules a kernel timer `struct timer_list` that wakes up repeatedly (e.g. every 1 second). During wake up, it locks a kernel `mutex_lock`, iterates over a linked list (`struct list_head`) of all registered PIDs, and uses internal Kernel APIs (`pid_task`, `get_mm_rss`) to check how much physical RAM (Resident Set Size - RSS) the process is holding.
* **Enforcement**: 
  - If `RSS > soft_limit`: Prints a `KERN_WARNING` warning to `dmesg` but allows it to live.
  - If `RSS > hard_limit`: Constructs a `SIGKILL` inside kernel space using `send_sig()` and drops an execution guillotine on the task, protecting the host system from Out-Of-Memory (OOM) crashes.

---

## 3. Interview Discussion Points: Design Decisions & Trade-offs

If an interviewer asks "Why did you build it like this?", use these points:

**"Why Unix Domain Sockets over FIFOs or Pipes for the CLI Control Plane?"**
> *While a FIFO (named pipe) could transfer data, a UDS creates a true client-server connection block. It intrinsically supports two-way communications easily, allowing our supervisor to tell the CLI client exactly what the `exit_code` status was. FIFOs are messy with two-way streams.*

**"Why a Bounded Buffer? Why not write directly to the log file from the child?"**
> *If the container writes straight to the file, and that file is on a slow Hard Drive, the container's execution speed is bottlenecked by the disk (I/O wait). A bounded buffer decouples generation from I/O storage. The container just writes fast to a pipe and moves back to CPU work, while our separate logging thread absorbs the I/O cost asynchronously.*

**"Why enforce Hard Limits in the Kernel instead of User-Space via `/proc/<pid>/statm`?"**
> *Enforcing it in user space implies the supervisor uses a `while(1) { sleep(1); }` loop to scan `/proc`. The delay of context switching back into user-space means a malicious leaking container can consume 100% of memory in less than a second, causing an irreversible Host System lockup. The kernel timer operates with higher priority directly in ring 0, reacting instantly.*

**"Scheduler Experiments & Nice Values"**
> *We implemented `--nice` in the `engine`. This allows us to observe the Linux Completely Fair Scheduler (CFS). For our experiment, passing something like `--nice 10` drops the process's priority (it gives it a smaller "weight"), resulting in the CFS explicitly giving its CPU time slices away to a `--nice 0` (normal priority) container running simultaneously. It demonstrates that the OS uses calculated weights, rather than simple round-robin scheduling.*

---

## 4. How to confidently present this
1. **Explain the Life of a Command**: Start with "When I type `engine run...`". Trace how the string goes over UDS to the Supervisor, which `clones` the PID, connects the pipes, populates a struct, registers the PID to the Kernel via `ioctl`, and sits waiting.
2. **Explain the Death of a Container**: When the container completes, the Linux kernel sends a `SIGCHLD` internally to the Supervisor. The Supervisor catches it, `waitpid()` reaps it (clearing zombie status), records its death inside its linked list structure, stops the producer thread cleanly, and responds across the UDS socket back to the CLI so the user knows it finished.
3. **Be ready with concurrency terminology**: Emphasize terms like "Mutex", "Deadlock", "Condition Variables", and "Producer-Consumer problem". Operating Systems interviewers love when you demonstrate that your data structures are naturally immune to Race Conditions.
