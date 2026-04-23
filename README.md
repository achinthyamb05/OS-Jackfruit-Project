LIGHTWEIGHT CONTAINER RUNTIME WITH KERNEL MEMORY MONITOR

Team:


Achinthya MB     PES1UG24CS019

Aadya Santhosh   PES1UG24CS005





Project Overview

This project implements a lightweight Linux container runtime in C with:
1. A long running supervisor that manages multiple containers
2. a bounded buffer logging system for concurrent output capture
3. A CLI interface for interacting with containers
4. A Linux Kernel Module for enforcing memory limits
5. Support for controlled scheduling experiments

The system demonstrated core OS concepts such as process isolation, IPC, synchronization, memory management and scheduling.


SYSTEM ARCHITECTURE

Components

User-space runtime (engine.c)
1. runs as
   * Supervisor daemon
   * CLI client
2. Responsibilities:
   * Container lifecycle management
   * Metadata tracking
   * IPC handling
   * Logging pipeline
     
Kernel-space Monitor (monitor.c)
1. Tracks container processes
2. Enforces:
   * Soft memory limits (warnings)
   * Hard memory limit (kills process)

IPC paths
1. Path A (Logging): Container --> Supervisor (pipes)
2. Path B (Control): CLI --> Supervisor (shared memory)
   


Build, Load and Run

Step 1: Install dependencies
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)

Step 2: Build project
make

Step 3: Load kernel module
sudo insmod monitor.ko
ls -l /dev/container_monitor

Step 4: Prepare root filesystem
mkdir rootfs-base wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

Step 5: Start Supervisor
sudo ./engine supervisor ./rootfs-base

Step 6: Create container rootfs copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

Step 7: Start containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80 sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96

Step 8: CLI operations
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha

Step 9: Unload module
sudo rmmod monitor


DEMO WITH SCREENSHOTS
1. Multi-container supervision
<img width="547" height="135" alt="image" src="https://github.com/user-attachments/assets/72dcb1f8-2366-4a49-aeda-c2ba328dd3ad" />


2. Metadata tracking
<img width="387" height="116" alt="image" src="https://github.com/user-attachments/assets/07457718-476b-4b4e-acb4-3f32061301e6" />


3. Logging pipeline     
<img width="621" height="97" alt="image" src="https://github.com/user-attachments/assets/607c4c32-79af-4715-87be-25f7374e0a7d" />


4. CLI and IPC    cli command supervisor response
<img width="1193" height="222" alt="image" src="https://github.com/user-attachments/assets/99ba5316-0936-4341-b16b-e352263c7177" />
<img width="806" height="156" alt="image" src="https://github.com/user-attachments/assets/520f94c0-9cc9-4e7a-9925-c2d1c2f761bf" />



5. Soft limit warning   dmesg | tail
<img width="636" height="48" alt="image" src="https://github.com/user-attachments/assets/7db701e8-81b1-4fdc-8597-7968ab972c03" />


6. Hard limit enforcement   container killed and metadata shows stopped
<img width="718" height="79" alt="image" src="https://github.com/user-attachments/assets/fef5f5a4-2f24-46ba-a449-86c82c8da9bf" />
<img width="218" height="96" alt="image" src="https://github.com/user-attachments/assets/751f933d-43c8-474e-abeb-b2be5f547eee" />
<img width="648" height="167" alt="image" src="https://github.com/user-attachments/assets/66a2dd9e-f387-41a1-9b62-e81fa554efa9" />


Kernel monitor logs showing soft-limit warnings and hard-limit enforcement (process termination).


7. Scheduling experiment    o/p of workloads   and diff in execution behavior
<img width="719" height="220" alt="image" src="https://github.com/user-attachments/assets/026ce50f-acd4-4e54-b8f5-3fb2f88c942b" />
<img width="711" height="571" alt="image" src="https://github.com/user-attachments/assets/2f06db8e-1c3a-42c0-9c65-f2e95ccec6f9" />


8. Clean teardown   ps aux   no zombie proccess
<img width="644" height="36" alt="image" src="https://github.com/user-attachments/assets/c22a4882-fb7d-490c-99db-0d154dd9e779" />



ENGINEERING ANALYSIS
Isolation mechanisms
1. Uses PID, UTS, mount namespaces
2. Each container has its own /proc
3. Filesystem isolation using chroot/pivot_root
4. Kernel is shared across containers

Supervisor and lifecycles
1. Long-running supervisor:
   * Tracks metadata
   * Reaps children
2. Prevents zombie processes
3. Handles signals correctly

IPC and synchronization
1. Logging uses pipes+bounded buffer
2. CLI uses separate IPC channel
3. Synchronization via:
   * Mutex
   * Condition variables
4. Prevents race conditions:
   * Lost logs
   * Buffer overflow
   * deadlocks
 
Memory Management
1. RSS measures physical memory usage
2. Soft limit:
     Warning only
3. Hard limit:
     Force kill (SIGKILL)
4. Kernel enforcement is necessary for accuracy

Scheduling Behavior
1. Experiments show:
   * CPU-bound tasks dominate CPU
   * Nice values affect priority
2. Demonstrates fairness and responsiveness


Design decision and tradeoffs
Component               Choice                       Tradeoff
Isolation               chroot             Less secure than pivot_root
IPC                  pipes + socket                   Complexity
Logging               bounded buffer               Memory overhead
Monitor               kernel module                  Requires root



CONCLUSION

This project demonstrates real-world OS concepts including containerization, memory enforcement, and scheduling using a custom-built runtime.







