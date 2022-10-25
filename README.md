# MP2: Rate-Monotonic CPU Scheduling
Linux kernel module that implements the Liu and Layland Periodic Task Model. Tasks are defined by their PID, processing time, and period.

This is a preemptive scheduling algorithm, where tasks with shorter periods are given priority. 

## Task State
Applications registered in the module will be assigned one of the following states:
- READY: application is ready to schedule a task
- RUNNING: application is currently running its task on the CPU
- SLEEPING: application finished executing its task and is waiting for the next period

## Registration
To add an application to the scheduling algorithm, the application must register itself.

The application must write "R,`pid`,`period`,`processing time`" to `/proc/mp2/status`
  
The algorithm controls whether an application can be registered with a utilization bound-based method:
An application will only be successfully registered when the sum of the ratio between the processing times and periods
of the tasks registered is <= 0.693

## Yielding
An application must yield when it wants to run a task for the first time and after each time it is finished running a task.

The application must write "Y,`pid`" to `/proc/mp2/status`

The application will be set to SLEEPING for the remainder of the current period. Each task has a timer to count how long it should
sleep for. Once this timer expires, the task is set to the READY state.

A kernel thread responsible for performing context switching will wake up. The next task that is run is set to the READY state.

## Deregistration
When an application is done performing all its tasks, it can remove itself from the scheduling algorithm.

The application must write "D,`pid`" to `proc/mp2/status`

## Context Switching
When the context switching kernel thread is woken up, the current task is preempted and the task with the shortest period 
that is in the READY state is scheduled next.
This is done by setting the next task's scheduling policy to FIFO, setting its priority to 0, and using the Linux `schedule()` API.

If there is no such task, the currently running task is simply preempted. This is done by setting its priority to 0 and using the
Linux `schedule()` API.

## Get Registration State
To see the current applications that are registered, a process can read from `/proc/mp2/status`

## Global State
All accesses to the global linked list of registered applications and the variable storing the currently running task are protected by a spinlock
