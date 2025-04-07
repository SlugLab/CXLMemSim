#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
int main(int argc, char *argv[]) {
    pid_t pid_memcached, pid_ycsb;
    int status;

    // 1. 启动 memcached
    pid_memcached = fork();
    if (pid_memcached == 0) {
        // Child: 启动memcached服务
        execlp("memcached", "memcached", "-p", "11211", (char *) NULL);
        perror("memcached exec failed");
        exit(EXIT_FAILURE);
    } else if (pid_memcached < 0) {
        perror("fork memcached failed");
        exit(EXIT_FAILURE);
    }

    // 2. 启动YCSB Client
    pid_ycsb = fork();
    if (pid_ycsb == 0) {
        // Child: 启动YCSB client, 输出latency数据到文件
        FILE *out_file = fopen("latency_output.txt", "w");
        dup2(fileno(out_file), STDOUT_FILENO);
        chdir("../workloads/YCSB");
        system("pwd");
        char *args[] = {
            "ycsb", 
            "run",
            "memcached",
            "-s",
            "-P",
            "./workloads/workloadb",
            "-p",
            "memcached.hosts=127.0.0.1",
            NULL
        };

        // Replace the current process with ./bin/ycsb, passing in args
        execv("./bin/ycsb", args);
        perror("ycsb exec failed");
        exit(EXIT_FAILURE);
    } else if (pid_ycsb < 0) {
        perror("fork ycsb failed");
        exit(EXIT_FAILURE);
    }

    // 3. 等待YCSB Client结束
    while (waitpid(pid_ycsb, &status, 0) != -1) {
        perror("waitpid failed");
    } 

    // 4. 解析latency结果
    FILE *in_file = fopen("latency_output.txt", "r");
    if (!in_file) {
        perror("latency_output.txt not found");
        exit(EXIT_FAILURE);
    }

    char line[256];
    printf("\n--- Latency Results ---\n");
    while (fgets(line, sizeof(line), in_file)) {
        if (strstr(line, "AverageLatency") || strstr(line, "PercentileLatency")) {
            printf("%s", line);
        }
    }
    fclose(in_file);

    // 5. 结束memcached服务进程
    if (kill(pid_memcached, SIGTERM) == -1) {
        perror("kill memcached failed");
    } else {
        waitpid(pid_memcached, NULL, 0);
        printf("\nmemcached stopped.\n");
    }

    return 0;
}
