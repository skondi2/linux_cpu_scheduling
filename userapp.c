#include "userapp.h"
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

int fibonacci(int n) {
	if (n == 0 || n == 1) return n;
	return fibonacci(n - 2)  + fibonacci(n - 1);
}

int main(int argc, char *argv[]){
    FILE* write_ptr = fopen("/proc/mp2/status", "w");
    
    int pid = getpid();
    int period = atoi(argv[1]);
    int processing_time = 16099; // milliseconds for n = 45

    // R,<pid>,<period>,<processing time>
    fprintf(write_ptr, "R,%d,%d,%d", pid, period, processing_time);
    fclose(write_ptr);

    // read from registered process list, exit 1
    FILE* read_ptr = fopen("/proc/mp2/status", "r");
    int registered = 0;
    ssize_t read;
    char* line;
    size_t len = 0;
    while ((read = getline(&line, &len, read_ptr)) != -1) {
        //printf("Retrieved line of length %zu:\n", read);
        //printf("%s", line);
        char* token = strtok(line, ":");
        if (pid == atoi(token)) {
            registered = 1;
        }
    }
    if (registered == 0) {
        printf("PID not in list");
        return 1;
    }
    fclose(read_ptr);

    // Y,<pid>
    FILE* write_ptr1 = fopen("/proc/mp2/status", "w");
    fprintf(write_ptr1, "Y,%d", pid);

    int num_jobs = 2;
    int n = 45;
    struct timeval after;
    struct timeval before;

    //double sum = 0.0;

    while (num_jobs > 0) {
        //gettimeofday(&before, NULL);

        fibonacci(n);

        //double elapsed = 0.0;
        //gettimeofday(&after, NULL);

        //elapsed = (after.tv_sec - before.tv_sec) + ((after.tv_usec - before.tv_usec) / 1000000.0);
        //printf("elapsed = %f\n", elapsed);
        //sum += elapsed;
        printf("finished job iteration = %d\n", num_jobs);
        num_jobs--;
        fflush(write_ptr1);
        fprintf(write_ptr1, "Y,%d", pid);
    }

    // D,<pid>
    fprintf(write_ptr1, "D,%d", pid);

    fclose(write_ptr1);
    printf("Finished\n");
    //printf("%f\n", sum / 10);
    
    return 0;
}

