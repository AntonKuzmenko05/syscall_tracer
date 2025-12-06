/*TODO
Проблема: код не розрізняє, який саме процес зробив syscall!
nht,f ,elt ljlfnb
ctypedef struct {
    pid_t pid;
    syscall_stat_t stats[MAX_SYSCALLS];
} per_process_stats_t;

*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/reg.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>

#define MAX_SYSCALLS 512
#define MAX_NAME_LEN 64

// Структура для зберігання статистики системного виклику
typedef struct {
    long syscall_num;
    char name[MAX_NAME_LEN];
    unsigned long count;
    unsigned long total_time_ns; //в наносекундах
} syscall_stat_t;


syscall_stat_t syscall_stats[MAX_SYSCALLS];
int syscall_count = 0;

//Хеш-таблиця для O(1) пошуку
int syscall_index_map[MAX_SYSCALLS];

// Функція для читання TSC (Time Stamp Counter)
// static inline unsigned long long rdtsc(void) {
//     unsigned int lo, hi;
//     __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
//     return ((unsigned long long)hi << 32) | lo;
// }

// функція для вимірювання часу через clock_gettime
unsigned long long get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Таблиця системних викликів x86_64 linux
#include "syscall_table.h"

// Завантаження імен системних викликів через ausyscall --dump
void load_syscall_names(void) {
    FILE *fp;
    char line[256];
    int loaded_from_ausyscall = 0;
    
    // Ініціалізація map
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_index_map[i] = -1;
    }
    
    fp = popen("ausyscall --dump 2>/dev/null", "r");
    if (fp) {
        // Пропускаємо заголовок
        if (fgets(line, sizeof(line), fp) != NULL) {
            while (fgets(line, sizeof(line), fp) != NULL) {
                int num;
                char name[MAX_NAME_LEN];
                
                if (sscanf(line, "%d%s", &num, name) == 2) {
                    if (num >= 0 && num < MAX_SYSCALLS && syscall_count < MAX_SYSCALLS) {
                        syscall_stats[syscall_count].syscall_num = num;
                        strncpy(syscall_stats[syscall_count].name, name, MAX_NAME_LEN - 1);
                        syscall_stats[syscall_count].name[MAX_NAME_LEN - 1] = '\0';
                        syscall_stats[syscall_count].count = 0;
                        syscall_stats[syscall_count].total_time_ns = 0;
                        
                        syscall_index_map[num] = syscall_count;
                        syscall_count++;
                        loaded_from_ausyscall = 1;
                    }
                }
            }
        }
        pclose(fp);
    }
    
    // Якщо ausyscall не спрацював, використовуємо вбудовану таблицю
    if (!loaded_from_ausyscall) {
        fprintf(stderr, "Попередження: ausyscall недоступний. Використовується вбудована таблиця syscalls.\n");
        for (int i = 0; syscall_names[i].num != -1; i++) {
            int num = syscall_names[i].num;
            if (num >= 0 && num < MAX_SYSCALLS && syscall_count < MAX_SYSCALLS) {
                syscall_stats[syscall_count].syscall_num = num;
                strncpy(syscall_stats[syscall_count].name, syscall_names[i].name, MAX_NAME_LEN - 1);
                syscall_stats[syscall_count].name[MAX_NAME_LEN - 1] = '\0';
                syscall_stats[syscall_count].count = 0;
                syscall_stats[syscall_count].total_time_ns = 0;
                
                syscall_index_map[num] = syscall_count;
                syscall_count++;
            }
        }
    }
    
    printf("Завантажено %d системних викликів\n", syscall_count);
}

// Отримання індексу syscall в таблиці статистики
int get_syscall_index(long syscall_num) {
    if (syscall_num < 0 || syscall_num >= MAX_SYSCALLS) {
        return -1;
    }
    
    int idx = syscall_index_map[syscall_num];
    
    // Якщо syscall не знайдено в таблиці, створюємо новий запис
    if (idx == -1 && syscall_count < MAX_SYSCALLS) {
        idx = syscall_count;
        syscall_stats[idx].syscall_num = syscall_num;
        snprintf(syscall_stats[idx].name, MAX_NAME_LEN, "syscall_%ld", syscall_num);
        syscall_stats[idx].count = 0;
        syscall_stats[idx].total_time_ns = 0;
        
        syscall_index_map[syscall_num] = idx;
        syscall_count++;
    }
    
    return idx;
}

int compare_by_count(const void *a, const void *b) {
    syscall_stat_t *sa = (syscall_stat_t *)a;
    syscall_stat_t *sb = (syscall_stat_t *)b;
    return (sb->count > sa->count) - (sb->count < sa->count);
}

int compare_by_time(const void *a, const void *b) {
    syscall_stat_t *sa = (syscall_stat_t *)a;
    syscall_stat_t *sb = (syscall_stat_t *)b;
    return (sb->total_time_ns > sa->total_time_ns) - (sb->total_time_ns < sa->total_time_ns);
}

void print_report(void) {
    unsigned long total_syscalls = 0;
    syscall_stat_t *active_stats;
    int active_count = 0;
    
    // Підрахунок загальної кількості syscalls
    for (int i = 0; i < syscall_count; i++) {
        total_syscalls += syscall_stats[i].count;
        if (syscall_stats[i].count > 0) {
            active_count++;
        }
    }
    
    // Копіювання тільки активних syscalls
    active_stats = malloc(active_count * sizeof(syscall_stat_t));
    int j = 0;
    for (int i = 0; i < syscall_count; i++) {
        if (syscall_stats[i].count > 0) {
            active_stats[j++] = syscall_stats[i];
        }
    }
    
    printf("\n=== ЗВІТ ПРОФАЙЛІНГУ СИСТЕМНИХ ВИКЛИКІВ ===\n\n");
    printf("Загальна кількість системних викликів: %lu\n\n", total_syscalls);
   
    qsort(active_stats, active_count, sizeof(syscall_stat_t), compare_by_count);
    printf("Топ-5 найчастіше викликаних syscalls:\n");
    printf("%-5s %-30s %-15s %-10s\n", "№", "Назва", "Кількість", "Частка (%)");
    printf("─────────────────────────────────────────────────────────────────\n");
    
    for (int i = 0; i < 5 && i < active_count; i++) {
        double percent = (double)active_stats[i].count / total_syscalls * 100.0;
        printf("%-5d %-30s %-15lu %-10.2f\n", 
               i + 1, 
               active_stats[i].name, 
               active_stats[i].count,
               percent);
    }
    
    qsort(active_stats, active_count, sizeof(syscall_stat_t), compare_by_time);
    printf("\nТоп-5 syscalls за сумарним часом виконання:\n");
    printf("%-5s %-30s %-15s %-15s\n", "№", "Назва", "Час (мкс)", "Кількість");
    printf("─────────────────────────────────────────────────────────────────\n");
    
    for (int i = 0; i < 5 && i < active_count; i++) {
        double time_us = active_stats[i].total_time_ns / 1000.0;
        printf("%-5d %-30s %-15.2f %-15lu\n", 
               i + 1, 
               active_stats[i].name, 
               time_us,
               active_stats[i].count);
    }
    
    free(active_stats);
}

void trace_process(pid_t pid) {
    int status;
    struct user_regs_struct regs;
    int in_syscall = 0;
    unsigned long long start_time = 0;
    long current_syscall = -1;
    
    // Очікування на перший SIGTRAP
    waitpid(pid, &status, 0);
    
    // Встановлення опцій ptrace для відстеження fork/clone
    ptrace(PTRACE_SETOPTIONS, pid, 0, 
           PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | 
           PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE);
    
    while (1) {
        // Продовжити виконання до наступного syscall
        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1) {
            if (errno == ESRCH) {
                break; // Процес завершився
            }
            perror("ptrace PTRACE_SYSCALL");
            break;
        }
        
        // Очікування зупинки процесу
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            break;
        }
        
        // Перевірка, чи процес завершився
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            break;
        }
        
        // Обробка події fork/clone (новий дочірній процес)
        if (status >> 8 == (SIGTRAP | (PTRACE_EVENT_FORK << 8)) ||
            status >> 8 == (SIGTRAP | (PTRACE_EVENT_VFORK << 8)) ||
            status >> 8 == (SIGTRAP | (PTRACE_EVENT_CLONE << 8))) {
            
            unsigned long new_pid;
            ptrace(PTRACE_GETEVENTMSG, pid, 0, &new_pid);
            
            // Новий процес буде автоматично трасуватися
            continue;
        }
        
        // Перевірка, чи це зупинка на syscall
        if (!(WIFSTOPPED(status) && WSTOPSIG(status) == (SIGTRAP | 0x80))) {
            continue;
        }
        
        // Отримання регістрів
        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1) {
            perror("ptrace PTRACE_GETREGS");
            continue;
        }
        
        if (!in_syscall) {
            current_syscall = regs.orig_rax;
            start_time = get_time_ns();
            in_syscall = 1;
        } else {
            unsigned long long end_time = get_time_ns();
            unsigned long duration = end_time - start_time;
            
            int idx = get_syscall_index(current_syscall);
            if (idx != -1) {
                syscall_stats[idx].count++;
                syscall_stats[idx].total_time_ns += duration;
            }
            
            in_syscall = 0;
            current_syscall = -1;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Використання: %s <програма> [аргументи...]\n", argv[0]);
        return 1;
    }
    
    printf("Syscall Profiler - Профайлінг системних викликів\n");
    printf("=================================================\n\n");
    
    load_syscall_names();
    
    pid_t pid = fork();
    
    if (pid == -1) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) {
        // Дочірній процес
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1) {
            perror("ptrace PTRACE_TRACEME");
            return 1;
        }
        
        // Виконання цільової програми
        execvp(argv[1], &argv[1]);
        
        perror("execvp");
        return 1;
    } else {
        // Батьківський процес
        printf("Трасування процесу %d: %s\n\n", pid, argv[1]);
        
        trace_process(pid);

        print_report();
    }
    
    return 0;
}