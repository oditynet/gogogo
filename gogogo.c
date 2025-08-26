#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <mntent.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <grp.h>
#include <pwd.h>

#define LOG_FILE "/var/log/gogogo.log"
#define CONFIG_DIR "/etc/gogogo/"
#define FSTAB_FILE "/etc/fstab"
#define INITLEVEL_FILE "/etc/gogogo/initlevel"
#define RC1_DIR "/etc/gogogo/rc1/"
#define RC2_DIR "/etc/gogogo/rc2/"
#define RC3_DIR "/etc/gogogo/rc3/"

#define GREEN "\033[32m"
#define BLUE "\033[34m"
#define YELLOW "\033[33m"
#define RED "\033[31m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define RESET "\033[0m"

typedef enum {
    SERVICE_STOPPED,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_STOPPING,
    SERVICE_FAILED
} ServiceState;

typedef enum {
    RESTART_NEVER,
    RESTART_ALWAYS,
    RESTART_ON_FAILURE
} RestartPolicy;

typedef struct Service {
    char name[50];
    char cmd[256];
    char dependencies[10][50];
    int dep_count;
    RestartPolicy restart;
    pid_t pid;
    ServiceState state;
    int respawn_count;
    int max_respawn;
    int runlevel;
    pthread_t monitor_thread;
    struct Service *next;
} Service;

Service *services = NULL;
int running = 1;
int current_runlevel = 1;
int log_fd = -1;
int in_chroot = 0;


void log_message(const char *format, ...) {
    char buffer[512];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, sizeof(buffer), "[%Y-%m-%d %H:%M:%S] ", tm_info);
    
    int len = strlen(buffer);
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + len, sizeof(buffer) - len, format, args);
    va_end(args);
    
    write(log_fd, buffer, strlen(buffer));
    write(log_fd, "\n", 1);
}

void print_status(const char *color, const char *symbol, const char *message) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s[%s]%s %s", color, symbol, RESET, message);
    printf("%s\n", buffer);
    log_message("[%s] %s", symbol, message);
}

void initialize_system_devices() {
    print_status(BLUE, "*", "Initializing system devices");
    
    // Запускаем скрипт инициализации устройств
    if (system("/etc/gogogo/rc.devices start") == 0) {
        print_status(GREEN, "+", "Devices initialized");
    } else {
        print_status(RED, "!", "Device initialization failed");
    }
}

void shutdown_system_devices() {
    print_status(BLUE, "*", "Shutting down devices");
    
    // Останавливаем device services
    system("/etc/init.d/rc.devices stop");
    print_status(GREEN, "+", "Devices stopped");
}


void print_status_format(const char *color, const char *symbol, const char *format, ...) {
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    print_status(color, symbol, message);
}

int read_runlevel() {
    FILE *f = fopen(INITLEVEL_FILE, "r");
    if (!f) {
        print_status(YELLOW, "!", "No initlevel file, using default runlevel 1");
        return 1;
    }
    
    char level[2];
    fgets(level, sizeof(level), f);
    fclose(f);
    
    int rl = atoi(level);
    if (rl < 1 || rl > 3) rl = 1;
    
    return rl;
}

void mount_virtual_filesystems() {
    print_status(BLUE, "*", "Mounting virtual filesystems");
    
    // Массив структур для файловых систем
    struct {
        const char *fstype;
        const char *mountpoint;
        const char *options;
    } vfs[] = {
        {"proc", "/proc", "defaults"},
        {"sysfs", "/sys", "defaults"},
        {"devtmpfs", "/dev", "defaults"},
        {"tmpfs", "/run", "defaults"},
        {"tmpfs", "/dev/shm", "defaults"},
        {NULL, NULL, NULL}
    };
    
    for (int i = 0; vfs[i].fstype != NULL; i++) {
        const char *fstype = vfs[i].fstype;
        const char *mountpoint = vfs[i].mountpoint;
        const char *options = vfs[i].options;
        
        // Создаем директорию если не существует
        struct stat st;
        if (stat(mountpoint, &st) != 0) {
            mkdir(mountpoint, 0755);
        }
        
        // Монтируем используя отдельные аргументы
        char cmd[512];
        if (strcmp(fstype, "devtmpfs") == 0) {
            // Для devtmpfs монтируем без устройства
            snprintf(cmd, sizeof(cmd), "mount -t %s -o %s %s %s", 
                    fstype, options, fstype, mountpoint);
        } else {
            // Для остальных - стандартный mount
            snprintf(cmd, sizeof(cmd), "mount -t %s -o %s none %s", 
                    fstype, options, mountpoint);
        }
        
        if (system(cmd) == 0) {
            print_status(GREEN, "+", mountpoint);
        } else {
            print_status(RED, "!", mountpoint);
        }
    }
}
void mount_fstab_filesystems() {
    print_status(BLUE, "*", "Mounting filesystems from " FSTAB_FILE);
    
    FILE *fstab = fopen(FSTAB_FILE, "r");
    if (!fstab) {
        print_status(RED, "!", "Failed to open " FSTAB_FILE);
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fstab)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char device[64], mountpoint[64], fstype[32], options[128];
        int dump, pass;
        
        if (sscanf(line, "%63s %63s %31s %127s %d %d", 
                  device, mountpoint, fstype, options, &dump, &pass) >= 4) {
            
            struct stat st;
            if (stat(mountpoint, &st) != 0) {
                mkdir(mountpoint, 0755);
            }
            
            if (strcmp(mountpoint, "/") == 0) {
                if (mount(device, "/", fstype, MS_REMOUNT | MS_RDONLY, options) == 0) {
                    print_status(GREEN, "+", "/ (remounted ro)");
                }
                continue;
            }
            
            char cmd[512];
            if (strcmp(fstype, "swap") == 0) {
                snprintf(cmd, sizeof(cmd), "swapon %s", device);
            } else {
                snprintf(cmd, sizeof(cmd), "mount -t %s -o %s %s %s", 
                        fstype, options, device, mountpoint);
            }
            
            if (system(cmd) == 0) {
                print_status(GREEN, "+", mountpoint);
            }
        }
    }
    
    fclose(fstab);
}

void initialize_dbus() {
    print_status(BLUE, "*", "Starting DBus");
    
    pid_t pid = fork();
    if (pid == 0) {
        // Дочерний процесс для dbus - запускаем как демон
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        // Создаем сессию и запускаем в фоне
        setsid();
        execl("/usr/bin/dbus-daemon", "dbus-daemon", "--system", "--fork", NULL);
        exit(1);
    } else if (pid > 0) {
        // Родительский процесс - не ждем завершения
        sleep(2); // Даем время dbus запуститься
        print_status(GREEN, "+", "dbus");
    }
}
void set_system_time() {
    print_status(BLUE, "*", "Setting system time from hardware clock");
    
    if (system("hwclock --hctosys --localtime") == 0) {
        print_status(GREEN, "+", "system time");
    }
}

void setup_console() {
    print_status(BLUE, "*", "Setting up console");
    
    if (system("setfont /usr/share/kbd/consolefonts/ter-116n.psf.gz") == 0) {
        print_status(GREEN, "+", "console font");
    }
    
    if (system("loadkeys ru") == 0) {
        print_status(GREEN, "+", "keyboard layout");
    }
}

void setup_tty() {
    print_status(BLUE, "*", "Setting up TTY");
    
    for (int i = 1; i <= 6; i++) {
        char tty[16], cmd[256];
        snprintf(tty, sizeof(tty), "tty%d", i);
        snprintf(cmd, sizeof(cmd), "/sbin/agetty -a root %s linux &", tty);
        
        if (system(cmd) == 0) {
            print_status_format(GREEN, "+", "tty%d", i);
        }
    }
}

void enter_chroot() {
    print_status(BLUE, "*", "Entering chroot environment");
    
    if (chroot("/") == 0) {
        chdir("/");
        in_chroot = 1;
        print_status(GREEN, "+", "chroot");
    } else {
        print_status(RED, "!", "chroot failed");
    }
}

void* service_monitor(void *arg) {
    Service *service = (Service*)arg;
    
    while (running && service->runlevel <= current_runlevel) {
        if (service->state == SERVICE_RUNNING) {
            int status;
            pid_t result = waitpid(service->pid, &status, WNOHANG);
            
            if (result > 0) {
                service->state = SERVICE_STOPPED;
                int exit_status = WEXITSTATUS(status);
                
                log_message("Service %s exited with status %d", service->name, exit_status);
                
                int should_restart = 0;
                if (service->restart == RESTART_ALWAYS) {
                    should_restart = 1;
                }
                else if (service->restart == RESTART_ON_FAILURE && exit_status != 0) {
                    should_restart = 1;
                }
                
                if (should_restart && service->respawn_count < service->max_respawn) {
                    service->respawn_count++;
                    log_message("Restarting %s (attempt %d/%d)", 
                               service->name, service->respawn_count, service->max_respawn);
                    
                    pid_t pid = fork();
                    if (pid == 0) {
                        setsid();
                        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
                        open("/dev/null", O_RDONLY); open("/dev/null", O_WRONLY); open("/dev/null", O_WRONLY);
                        
                        char *args[] = {"/bin/sh", "-c", service->cmd, NULL};
                        execv("/bin/sh", args);
                        exit(EXIT_FAILURE);
                    }
                    else if (pid > 0) {
                        service->pid = pid;
                        service->state = SERVICE_RUNNING;
                        print_status(GREEN, "+", service->name);
                    }
                }
            }
        }
        sleep(2);
    }
    return NULL;
}

Service* find_service(const char *name) {
    Service *current = services;
    while (current) {
        if (strcmp(current->name, name) == 0) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int are_dependencies_met(Service *service) {
    for (int i = 0; i < service->dep_count; i++) {
        Service *dep = find_service(service->dependencies[i]);
        if (!dep || dep->state != SERVICE_RUNNING) {
            return 0;
        }
    }
    return 1;
}

void start_service(Service *service) {
    if (service->state != SERVICE_STOPPED) return;
    if (!are_dependencies_met(service)) return;
    if (service->runlevel > current_runlevel) return;

    service->state = SERVICE_STARTING;
    print_status(BLUE, "~", service->name);
    
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
        open("/dev/null", O_RDONLY); open("/dev/null", O_WRONLY); open("/dev/null", O_WRONLY);
        
        char *args[] = {"/bin/sh", "-c", service->cmd, NULL};
        execv("/bin/sh", args);
        exit(EXIT_FAILURE);
    }
    else if (pid > 0) {
        service->pid = pid;
        service->state = SERVICE_RUNNING;
        print_status(GREEN, "+", service->name);
        
        pthread_create(&service->monitor_thread, NULL, service_monitor, service);
    }
    else {
        service->state = SERVICE_FAILED;
        print_status(RED, "!", service->name);
    }
}

void load_services_from_dir(const char *dirpath, int runlevel) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        print_status_format(YELLOW, "!", "No services directory for runlevel %d", runlevel);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char config_path[256];
            snprintf(config_path, sizeof(config_path), "%s%s", dirpath, entry->d_name);
            
            FILE *f = fopen(config_path, "r");
            if (!f) continue;
            
            Service *service = malloc(sizeof(Service));
            memset(service, 0, sizeof(Service));
            service->runlevel = runlevel;
            service->max_respawn = 5;
            
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "NAME=", 5) == 0) {
                    strncpy(service->name, line + 5, sizeof(service->name));
                    service->name[strcspn(service->name, "\n")] = 0;
                }
                else if (strncmp(line, "CMD=", 4) == 0) {
                    strncpy(service->cmd, line + 4, sizeof(service->cmd));
                    service->cmd[strcspn(service->cmd, "\n")] = 0;
                }
                else if (strncmp(line, "RESTART=", 8) == 0) {
                    char *value = line + 8;
                    value[strcspn(value, "\n")] = 0;
                    if (strcmp(value, "always") == 0) service->restart = RESTART_ALWAYS;
                    else if (strcmp(value, "on-failure") == 0) service->restart = RESTART_ON_FAILURE;
                    else service->restart = RESTART_NEVER;
                }
                else if (strncmp(line, "DEPENDS=", 8) == 0) {
                    char *value = line + 8;
                    value[strcspn(value, "\n")] = 0;
                    char *token = strtok(value, " ");
                    while (token && service->dep_count < 10) {
                        strcpy(service->dependencies[service->dep_count++], token);
                        token = strtok(NULL, " ");
                    }
                }
            }
            fclose(f);
            
            service->next = services;
            services = service;
            print_status(CYAN, "L", service->name);
        }
    }
    closedir(dir);
}

void start_services_for_runlevel(int runlevel) {
    print_status_format(MAGENTA, "=", "Starting services for runlevel %d", runlevel);
    
    int changed;
    do {
        changed = 0;
        Service *current = services;
        while (current) {
            if (current->runlevel == runlevel && 
                current->state == SERVICE_STOPPED && 
                are_dependencies_met(current)) {
                start_service(current);
                changed = 1;
            }
            current = current->next;
        }
        if (changed) sleep(1);
    } while (changed);
}

void runlevel_1() {
    print_status(MAGENTA, "=", "Entering runlevel 1 (Single User)");
    
    mount_virtual_filesystems();
    mount_fstab_filesystems();
    enter_chroot();
    initialize_system_devices();
    //mount_fstab_filesystems();
    //initialize_udev();
    initialize_dbus();
    //set_system_time();
    //setup_console();
    //enter_chroot();
    setup_tty();
}

void runlevel_2() {
    print_status(MAGENTA, "=", "Entering runlevel 2 (Multi User)");
    
    load_services_from_dir(RC2_DIR, 2);
    start_services_for_runlevel(2);
}

void runlevel_3() {
    print_status(MAGENTA, "=", "Entering runlevel 3 (Graphical)");
    
    load_services_from_dir(RC3_DIR, 3);
    start_services_for_runlevel(3);
    
/*    if (system("which lightdm >/dev/null 2>&1") == 0) {
        system("lightdm &");
    } else if (system("which gdm >/dev/null 2>&1") == 0) {
        system("gdm &");
    } else if (system("which sddm >/dev/null 2>&1") == 0) {
        system("sddm &");
    }*/
}

void stop_all_services() {
    print_status(BLUE, "*", "Stopping all services");
    
    Service *current = services;
    while (current) {
        if (current->state == SERVICE_RUNNING) {
            print_status(BLUE, "~", current->name);
            kill(current->pid, SIGTERM);
            waitpid(current->pid, NULL, 0);
            current->state = SERVICE_STOPPED;
            print_status(GREEN, "-", current->name);
        }
        current = current->next;
    }
}

void unmount_all_filesystems() {
    print_status(BLUE, "*", "Unmounting filesystems");
    
    if (in_chroot) {
        chroot("..");
        chdir("/");
        in_chroot = 0;
    }
    
    system("umount -a -r");
    system("swapoff -a");
    
    print_status(GREEN, "+", "Filesystems unmounted");
}

void poweroff_system() {
    print_status(MAGENTA, "=", "Shutting down system");
    
    stop_all_services();
    unmount_all_filesystems();
    
    print_status(GREEN, "+", "Powering off via BIOS");
    
    // Прямой вызов BIOS для выключения
    sync(); // Синхронизируем диски
    reboot(RB_POWER_OFF); // Выключаем компьютер
}

void list_services() {
    printf("\n%s=== Running Services ===%s\n", CYAN, RESET);
    
    Service *current = services;
    int count = 0;
    
    while (current) {
        if (current->state == SERVICE_RUNNING) {
            printf("%s[+]%s %s (PID: %d)\n", GREEN, RESET, current->name, current->pid);
            count++;
        }
        current = current->next;
    }
    
    if (count == 0) {
        printf("%s[!]%s No services running\n", YELLOW, RESET);
    }
}

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        print_status(BLUE, "*", "Received shutdown signal");
        running = 0;
        poweroff_system();
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "list") == 0) {
            list_services();
            return 0;
        }
        else if (strcmp(argv[1], "poweroff") == 0) {
            poweroff_system();
            return 0;
        }
    }
    
    log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    
    printf("\n%s=== Gogogo Init System ===%s\n", MAGENTA, RESET);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    current_runlevel = read_runlevel();
    
    runlevel_1();
    if (current_runlevel >= 2) runlevel_2();
    if (current_runlevel >= 3) runlevel_3();
    
    print_status(GREEN, "+", "System initialization complete");
    
    while (running) {
        sleep(1);
    }
    
    close(log_fd);
    return 0;
}
