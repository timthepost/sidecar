/* A sidecar tool to watch a file while keeping an eye on system
 * resources. Built to watch Runa's inference layer bus traffic
 * while monitoring system resource usage. Meant to be run in a small
 * window next to a terminal, hence the name "side car."
 * LICENSE: MIT Or Compatible. Public Domain if that works better for you.
 * Just have fun with it!
 *
 * s = swap
 * i = iowait over sampling time
 *
 * Still very much a work-in-progress.
 *
 * Usage: sidecar [optional file to watch "tail -f" style]
 * Then watch pretty graphs update while debug messages roll by.
 *
 * Copyright 2025 Tim Post <timthepost@protonmail.com>
 *
 * License: MIT
 */

/* Coming soon:
 * Tasteful colorization of output.
 * Command line switches for max debug lines, graph height, refresh,
 * and other constants.
 * Perhaps support a few keypress events? '[' and ']' to adjust the
 * sampling rate? Still considering it.
 * May support direct splinter integration (via build flag?)
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>

#define HISTORY_HEIGHT 10
#define REFRESH 500000
#define HISTORY_DIVISOR 4
#define MAXW 512
#define MAX_DEBUG_LINES 12

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stats;

typedef struct {
    double load_1min;
    double load_5min;
    double load_15min;
    int running_processes;
    int total_processes;
    int last_pid;
} loadavg_t;

typedef struct {
    int battery_percent;    // Battery percentage (0-100, -1 if no battery)
    unsigned int on_ac;     // 1 if AC adapter connected, 0 if not
} power_status_t;

static int term_cols = 80;
static int term_rows = 24;
static int graph_width = 50;

static double hist_cpu[MAXW] = {0};
static double hist_mem[MAXW] = {0};

// aggregate % of CPU time spent in iowait over sampling interval
// useful to show a system thrashing
static double last_io_pct = 0.0;

// window changed between ticks
static volatile sig_atomic_t resize_pending = 0;

// debug log buffer
static char *debug_lines[MAX_DEBUG_LINES];
static int debug_line_count = 0;

// file handle for tail mode
static FILE *dbg_fp = NULL;

void handle_winch(int sig) {
    (void)sig;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_cols  = ws.ws_col;
        term_rows  = ws.ws_row;
        graph_width = term_cols - 12;
        if (graph_width > MAXW) graph_width = MAXW;
        if (graph_width < 20)   graph_width = 20;
    }
    resize_pending = 1;
}

void install_winch_handler() {
    struct sigaction sa;
    sa.sa_handler = handle_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGWINCH, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }
}

// a ring buffer where the oldest is freed off the
// top as the newest is tacked to the bottom.
void debug_log_append(const char *line) {
    if (debug_line_count >= MAX_DEBUG_LINES) {
        free(debug_lines[0]);
        memmove(&debug_lines[0], &debug_lines[1],
                (MAX_DEBUG_LINES-1)*sizeof(char*));
        debug_line_count--;
    }
    debug_lines[debug_line_count++] = strdup(line);
}

// "tail -f"-like file tail 
int init_debug_file(const char *path) {
    dbg_fp = fopen(path, "r");
    if (!dbg_fp) {
        fprintf(stderr, "Unable to load debug file %s\n", path);
        perror("file");
        return -1;
    }
    setvbuf(dbg_fp, NULL, _IONBF, 0);
    fseek(dbg_fp, 0, SEEK_END); // jump to end for tail -f behavior
    return 0;
}

int read_debug_file() {
    if (!dbg_fp) return 0;
    char line[1024];
    int updated = 0;
    while (fgets(line, sizeof(line), dbg_fp)) {
        line[strcspn(line, "\r\n")] = 0;
        debug_log_append(line);
        updated = 1;
    }
    clearerr(dbg_fp); // crucial for tail -f behavior (always EOF!)
    return updated;
}

void get_cpu_stats(cpu_stats *s) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) exit(1);
    fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &s->user, &s->nice, &s->system, &s->idle,
           &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(f);
}

int parse_loadavg(loadavg_t *loadavg) {
    FILE *file;
    char buffer[256];
    
    if (!loadavg) {
        return -1;
    }

    file = fopen("/proc/loadavg", "r");
    if (!file) {
        perror("fopen /proc/loadavg");
        return -1;
    }

    if (!fgets(buffer, sizeof(buffer), file)) {
        perror("fgets");
        fclose(file);
        return -1;
    }

    fclose(file);
    
    // Parse the format: "0.08 0.03 0.05 2/278 1234"
    int parsed = sscanf(buffer, "%lf %lf %lf %d/%d %d",
            &loadavg->load_1min,
            &loadavg->load_5min, 
            &loadavg->load_15min,
            // I don't use these yet, but should/could.
            &loadavg->running_processes,
            &loadavg->total_processes,
            // this is probably not so useful to show.
            &loadavg->last_pid);
    if (parsed != 6) {
        fprintf(stderr, "Failed parsing /proc/loadavg (%d/6 ok)\n", parsed);
        return -1;
    }
    return 0;
}

double get_cpu_usage(cpu_stats *prev) {
    cpu_stats cur;
    get_cpu_stats(&cur);
    unsigned long long prev_idle = prev->idle + prev->iowait;
    unsigned long long idle = cur.idle + cur.iowait;
    unsigned long long prev_non = (prev->user + prev->nice + prev->system +
                                   prev->irq + prev->softirq + prev->steal);
    unsigned long long non = (cur.user + cur.nice + cur.system +
                              cur.irq + cur.softirq + cur.steal);
    unsigned long long prev_total = prev_idle + prev_non;
    unsigned long long total = idle + non;
    unsigned long long diff_total = total - prev_total;
    unsigned long long diff_idle = idle - prev_idle;
    unsigned long long diff_iowait = cur.iowait - prev->iowait;

    *prev = cur;

    if (diff_total > 0) {
        last_io_pct = (double)diff_iowait / diff_total * 100.0;
        return (double)(diff_total - diff_idle) / diff_total * 100.0;
    } else {
        last_io_pct = 0.0;
        return 0.0;
    }
}

double get_mem_usage(double *swap_pct) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) exit(1);
    char key[32]; unsigned long val; char unit[8];
    unsigned long mem_total=1, mem_free=0, buffers=0, cached=0;
    unsigned long swap_total=1, swap_free=0;
    while (fscanf(f, "%31s %lu %7s\n", key, &val, unit) == 3) {
        if (!strcmp(key, "MemTotal:")) mem_total = val;
        else if (!strcmp(key, "MemFree:")) mem_free = val;
        else if (!strcmp(key, "Buffers:")) buffers = val;
        else if (!strcmp(key, "Cached:")) cached = val;
        else if (!strcmp(key, "SwapTotal:")) swap_total = val;
        else if (!strcmp(key, "SwapFree:")) swap_free = val;
    }
    fclose(f);
    unsigned long used = mem_total - mem_free - buffers - cached;
    *swap_pct = (swap_total > 0) ?
                (double)(swap_total - swap_free) / swap_total * 100.0 : 0.0;
    return (double)used / mem_total * 100.0;
}

static int read_sys_int(const char *path) {
    FILE *file;
    int value = -1;
    
    file = fopen(path, "r");
    if (file) {
        fscanf(file, "%d", &value);
        fclose(file);
    }
    
    return value;
}

// Check if a power supply device exists
static int device_exists(const char *device_path) {
    return access(device_path, F_OK) == 0;
}

int parse_power_status(power_status_t *power) {
    DIR *dir;
    struct dirent *entry;
    char path[512];
    char type_path[512];
    char value_path[512];
    FILE *file;
    char type_buf[32];
    int found_battery = 0;
    int found_ac = 0;

    if (!power) {
        return -1;
    }

    // Initialize defaults
    power->battery_percent = -1;
    power->on_ac = 0;

    dir = opendir("/sys/class/power_supply");
    if (!dir) {
        perror("opendir /sys/class/power_supply");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        // Build path to the power supply device
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s", entry->d_name);
        snprintf(type_path, sizeof(type_path), "%s/type", path);

        // Read the type of this power supply
        file = fopen(type_path, "r");
        if (!file) {
            continue;
        }

        if (!fgets(type_buf, sizeof(type_buf), file)) {
            fclose(file);
            continue;
        }
        fclose(file);
        type_buf[strcspn(type_buf, "\n")] = '\0';

        if (strcmp(type_buf, "Battery") == 0 && !found_battery) {
            // Found a battery, get its capacity
            snprintf(value_path, sizeof(value_path), "%s/capacity", path);
            power->battery_percent = read_sys_int(value_path);
            if (power->battery_percent >= 0) {
                found_battery = 1;
            }
        }
        else if ((strcmp(type_buf, "Mains") == 0 || 
                  strcmp(type_buf, "ADP1") == 0 ||
                  strstr(entry->d_name, "ADP") != NULL ||
                  strstr(entry->d_name, "AC") != NULL) && !found_ac) {
            // Found AC adapter, check if it's online
            snprintf(value_path, sizeof(value_path), "%s/online", path);
            int online = read_sys_int(value_path);
            if (online > 0) {
                power->on_ac = 1;
                found_ac = 1;
            }
        }
    }

    closedir(dir);

    // If we didn't find AC status but found a battery, check battery status
    if (!found_ac && found_battery) {
        // Look for charging status as backup AC detection
        dir = opendir("/sys/class/power_supply");
        if (dir) {
            rewinddir(dir);
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') {
                    continue;
                }

                snprintf(path, sizeof(path), "/sys/class/power_supply/%s", entry->d_name);
                snprintf(type_path, sizeof(type_path), "%s/type", path);

                file = fopen(type_path, "r");
                if (!file) {
                    continue;
                }

                if (fgets(type_buf, sizeof(type_buf), file)) {
                    type_buf[strcspn(type_buf, "\n")] = '\0';
                    if (strcmp(type_buf, "Battery") == 0) {
                        snprintf(value_path, sizeof(value_path), "%s/status", path);
                        file = freopen(value_path, "r", file);
                        if (file && fgets(type_buf, sizeof(type_buf), file)) {
                            type_buf[strcspn(type_buf, "\n")] = '\0';
                            if (strcmp(type_buf, "Charging") == 0) {
                                power->on_ac = 1;
                            }
                        }
                        fclose(file);
                        break;
                    }
                }
                fclose(file);
            }
            closedir(dir);
        }
    }
    return 0;
}

void draw_bar(const char *label, double percent) {
    int filled = (int)(percent / 100.0 * graph_width);
    printf("┌> ");
    for (int i = 0; i < graph_width; i++) {
        if (i < filled) printf("■");
        else printf(" ");
    }
    printf("%-3s\n└> %-5.1f%%\n", label, percent);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (init_debug_file(argv[1]) != 0) {
            dbg_fp = NULL; // disable debug if file can't be opened
        }
    }

    install_winch_handler();
    handle_winch(0);

    cpu_stats prev;
    loadavg_t loads;
    power_status_t power;
    int hist_counter = 0;

    get_cpu_stats(&prev);
    parse_loadavg(&loads);
    parse_power_status(&power);
    printf("\033[2J");

    while (1) {
        double swap_pct;
        double cpu = get_cpu_usage(&prev);
        double mem = get_mem_usage(&swap_pct);
        parse_loadavg(&loads);
        parse_power_status(&power);

        int new_debug = 0;
        if (dbg_fp) {
            new_debug = read_debug_file();
        }

        if (hist_counter == 0) {
            memmove(hist_cpu, hist_cpu+1, (MAXW-1)*sizeof(double));
            memmove(hist_mem, hist_mem+1, (MAXW-1)*sizeof(double));
            hist_cpu[MAXW-1] = cpu;
            hist_mem[MAXW-1] = mem;
        }
        hist_counter = (hist_counter + 1) % HISTORY_DIVISOR;

        if (resize_pending || new_debug) {
            printf("\033[2J");
            resize_pending = 0;
        }
        printf("\033[H");

        printf("History (CPU=█, RAM=░)\n");
        for (int row=HISTORY_HEIGHT; row>=0; row--) {
            for (int col=MAXW-graph_width; col<MAXW; col++) {
                int c = (int)(hist_cpu[col] / 100.0 * HISTORY_HEIGHT);
                int m = (int)(hist_mem[col] / 100.0 * HISTORY_HEIGHT);
                if (c >= row && m >= row) printf("▓");
                else if (c >= row) printf("█");
                else if (m >= row) printf("░");
                else printf(" ");
            }
            printf("\n");
        }

        printf("\n");
        draw_bar("cpu", cpu);
        printf(" > s=%-.1f%% | i=%-.1f%% | 1=%-.2f | 5=%-.2f | 15=%-.2f\n",
            swap_pct, last_io_pct, loads.load_1min, loads.load_5min, loads.load_15min);
        printf(" > [%d/%d] :: (%d%% %s\n",
            loads.running_processes,
            loads.total_processes,
            power.battery_percent == -1 ? 0 : power.battery_percent, 
            power.on_ac == 1 ? "on ac)  " : "on batt)");
        draw_bar("mem", mem);

        if (dbg_fp) {
            int used_above_debug = 1 + HISTORY_HEIGHT + 1 + (2*2) + 1;
            int used_below_debug = 2;
            int available = term_rows - used_above_debug - used_below_debug;
            int max_debug_rows = (available > 0 ? available : 0);
            // TODO: util to truncate long names here
            // can use the graph_width var for that
            printf(" > tail: %s\n", argv[1]);
            // TODO: we also have to worry about length in debug lines, as they
            // could overflow the debug window length assumptions.
            int start = debug_line_count > max_debug_rows ?
                        debug_line_count - max_debug_rows : 0;
            for (int i = start; i < debug_line_count; i++) {
                printf("%.*s\n", term_cols > 1 ? term_cols - 1 : 1, debug_lines[i]);
            }
        }

        fflush(stdout);
        struct timespec ts = {0, REFRESH * 1000};
        nanosleep(&ts, NULL);
    }
}
