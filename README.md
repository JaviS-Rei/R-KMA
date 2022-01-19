# R-KMA
## Kernel Memory Allocator
The Kernel Memory Allocator (KMA) is a subsystem that tries to satisfy the requests for memory areas from all parts of the system. Some of these requests come from other kernel subsystems needing memory for kernel use, and some requests come via system calls from user programs to increase their processes’ address spaces.

R-KMA present a high performance KMA under Multiprocessor setting.