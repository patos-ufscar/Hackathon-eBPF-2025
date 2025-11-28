# Intro

This write-up wants to discuss and teach how the SCX_MUS works.

First, for those who do not know, sched_ext is a kernel framework that allows us to write a custom scheduler and load it, effectively replacing the great CFS (Completely Fair Scheduler) with our GREATER SCX_MUS for a set of tasks.

SCX_MUS is designed to be a Weighted Virtual Time Scheduler with a specific focus on container prioritization. It uses eBPF maps to track high-priority cgroups and adjusts their execution time accounting to ensure latency-sensitive applications get CPU time faster than background noise. !!!!!!!!!!!!!!!

# How SCX_MUS Works?

SCX_MUS utilizes a Global Dispatch Queue (DSQ) architecture combined with Weighted Virtual Time (vtime) scheduling (similar to CFS).