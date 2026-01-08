#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <termios.h>
#include <sys/ioctl.h>

#define MAX_MESSAGES 1000
#define INPUT_BUFFER 1024

typedef enum MessageType {
    MT_LOGIN = 0,
    MT_LOGOUT = 1,
    MT_MESSAGE_SEND = 2,
    MT_MESSAGE_RECV = 10,
    MT_DISCONNECT = 12,
    MT_SYSTEM = 13
} message_type_t;

typedef struct __attribute__((packed)) Message {
    uint32_t type;
    uint32_t timestamp;
    char username[32];
    char message[1024];
} message_t;

typedef struct Settings {
    int socket_fd;
    bool running;
    bool quiet;
    bool tui;
    char username[32];
    struct sockaddr_in server;
} settings_t;

static settings_t settings = {0};
static bool disconnected_by_server = false;

static const char* RED = "\033[31m";
static const char* GREEN = "\033[32m";
static const char* CYAN = "\033[36m";
static const char* YELLOW = "\033[33m";
static const char* MAGENTA = "\033[35m";
static const char* GRAY = "\033[90m";
static const char* RESET = "\033[0m";

// ----------------- Helpers -----------------
void print_help() {
    printf(
        "Usage: ./client [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet] [--tui] [--help]\n"
        "Options:\n"
        "  --help      Show this message\n"
        "  --port      Port to connect to (default 8080)\n"
        "  --ip        IP to connect to (default 127.0.0.1)\n"
        "  --domain    Domain name (cannot combine with --ip)\n"
        "  --quiet     Disable alerts/mentions\n"
        "  --tui       Enable Text User Interface\n"
    );
}

int process_args(int argc, char* argv[]) {
    const char* ip = "127.0.0.1";
    int port = 8080;
    char* domain = NULL;
    settings.quiet = false;
    settings.tui = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_help(); return -1; }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) { port = atoi(argv[++i]); }
        else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) { ip = argv[++i]; }
        else if (strcmp(argv[i], "--domain") == 0 && i + 1 < argc) { domain = argv[++i]; }
        else if (strcmp(argv[i], "--quiet") == 0) { settings.quiet = true; }
        else if (strcmp(argv[i], "--tui") == 0) { settings.tui = true; }
        else { fprintf(stderr, "Unknown argument: %s\n", argv[i]); return -1; }
    }

    if (domain && ip && strcmp(ip, "127.0.0.1") != 0) { fprintf(stderr, "--ip and --domain conflict\n"); return -1; }

    if (domain) {
        struct hostent* h = gethostbyname(domain);
        if (!h) { fprintf(stderr, "Cannot resolve domain\n"); return -1; }
        ip = inet_ntoa(*(struct in_addr*)h->h_addr_list[0]);
    }

    memset(&settings.server, 0, sizeof(settings.server));
    settings.server.sin_family = AF_INET;
    settings.server.sin_port = htons(port);
    if (!inet_aton(ip, &settings.server.sin_addr)) { fprintf(stderr, "Invalid IP\n"); return -1; }

    return 0;
}

char* get_username() {
    FILE* fp = popen("whoami", "r");
    if (!fp) { perror("whoami"); return NULL; }
    char buf[32]; if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return NULL; }
    pclose(fp);
    buf[strcspn(buf, "\r\n")] = 0;
    for (size_t i = 0; i < strlen(buf); i++) if (!isprint(buf[i])) return NULL;
    return strdup(buf);
}

ssize_t perform_full_read(void* buf, size_t n) {
    size_t total = 0; char* p = buf;
    while (total < n) {
        ssize_t r = read(settings.socket_fd, p + total, n - total);
        if (r == 0) return 0;
        if (r < 0) { perror("read"); return -1; }
        total += r;
    }
    return total;
}

void format_time(time_t t, char* buf, size_t sz) {
    struct tm* tm_info = localtime(&t);
    strftime(buf, sz, "%H:%M:%S", tm_info);
}

bool validate_message(const char* msg, size_t len) {
    if (len < 1 || len > 1023) return false;
    for (size_t i = 0; i < len; i++) if ((unsigned char)msg[i] < 9 || (unsigned char)msg[i] > 126) return false;
    return true;
}

bool send_message(const message_t* msg) {
    ssize_t total = 0;
    while (total < sizeof(message_t)) {
        ssize_t w = write(settings.socket_fd, (const char*)msg + total, sizeof(message_t) - total);
        if (w <= 0) { perror("write"); return false; }
        total += w;
    }
    return true;
}

// ----------------- Signal Handling -----------------
void handle_signal(int sig) {
    (void)sig;
    settings.running = false;
    if (settings.socket_fd > 0) {
        message_t logout = {0}; logout.type = htonl(MT_LOGOUT);
        write(settings.socket_fd, &logout, sizeof(logout));
    }
}

// ----------------- TUI Helpers -----------------
struct termios orig_term;
void reset_terminal() { tcsetattr(STDIN_FILENO, TCSANOW, &orig_term); }

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_term);
    atexit(reset_terminal);
    struct termios raw = orig_term;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void clear_screen() { printf("\033[2J\033[H"); fflush(stdout); }
int terminal_rows() { struct winsize w; ioctl(STDOUT_FILENO, TIOCGWINSZ, &w); return w.ws_row; }

typedef struct {
    message_t messages[MAX_MESSAGES];
    int count;
} History;

static History history = {0};

void add_to_history(message_t* msg) {
    if (history.count < MAX_MESSAGES) history.messages[history.count++] = *msg;
    else {
        memmove(&history.messages[0], &history.messages[1], (MAX_MESSAGES - 1) * sizeof(message_t));
        history.messages[MAX_MESSAGES - 1] = *msg;
    }
}

void draw_tui(const char* input) {
    clear_screen();
    int rows = terminal_rows() - 2;
    int start = (history.count > rows) ? history.count - rows : 0;

    for (int i = start; i < history.count; i++) {
        message_t* m = &history.messages[i];
        char timebuf[16]; format_time(ntohl(m->timestamp), timebuf, sizeof(timebuf));
        const char* user_color = (strcmp(m->username, settings.username) == 0) ? GREEN : CYAN;

        if (ntohl(m->type) == MT_MESSAGE_RECV) {
            const char* p = m->message;
            char pattern[40]; snprintf(pattern, sizeof(pattern), "@%s", settings.username);
            const char* mention;
            printf("[%s] %s%s%s: ", timebuf, user_color, m->username, RESET);
            while ((mention = strstr(p, pattern))) {
                fwrite(p, 1, mention - p, stdout);
                if (!settings.quiet) printf("\a"); // beep if not quiet
                printf("%s%.*s%s", RED, (int)strlen(pattern), mention, RESET);
                p = mention + strlen(pattern);
            }
            printf("%s\n", p);
        }
        else if (ntohl(m->type) == MT_SYSTEM) printf("%s[SYSTEM]: %s%s\n", YELLOW, m->message, RESET);
        else if (ntohl(m->type) == MT_DISCONNECT) printf("%s[DISCONNECT] %s%s\n", RED, m->message, RESET);
    }
    printf("\033[%d;1H> %s", rows + 1, input);
    fflush(stdout);
}

// ----------------- Receiving Thread -----------------
void* receive_thread(void* arg) {
    (void)arg;
    char dummy[1];
    while (settings.running) {
        message_t msg = {0};
        ssize_t r = perform_full_read(&msg, sizeof(msg));
        if (r <= 0) { settings.running = false; break; }
        if (settings.tui) { add_to_history(&msg); draw_tui(dummy); }
        else {
            uint32_t type = ntohl(msg.type);
            char timebuf[64]; format_time(ntohl(msg.timestamp), timebuf, sizeof(timebuf));
            switch(type) {
                case MT_MESSAGE_RECV:
                    printf("[%s] %s: ", timebuf, msg.username);
                    {
                        // highlight mentions in non-TUI mode
                        char pattern[40]; snprintf(pattern,sizeof(pattern),"@%s",settings.username);
                        const char* p = msg.message;
                        const char* mention;
                        while ((mention = strstr(p, pattern))) {
                            fwrite(p,1,mention-p,stdout);
                            if (!settings.quiet) printf("\a");
                            printf("%s%.*s%s", RED, (int)strlen(pattern), mention, RESET);
                            p = mention + strlen(pattern);
                        }
                        printf("%s\n",p);
                    }
                    break;
                case MT_SYSTEM: printf("%s[SYSTEM]: %s%s\n", GRAY, msg.message, RESET); break;
                case MT_DISCONNECT: printf("%s[DISCONNECT] %s%s\n", RED, msg.message, RESET);
                    disconnected_by_server = true; settings.running = false; break;
                default: fprintf(stderr, "Unknown message type %u\n", type); break;
            }
        }
    }
    return NULL;
}

// ----------------- Main -----------------
int main(int argc, char* argv[]) {
    struct sigaction sa = {0}; sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    if (process_args(argc, argv) != 0) return -1;

    char* username = get_username();
    if (!username) { fprintf(stderr, "Failed to get username\n"); return -1; }
    strncpy(settings.username, username, sizeof(settings.username) - 1);

    settings.socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (settings.socket_fd < 0) { perror("socket"); return -1; }
    if (connect(settings.socket_fd, (struct sockaddr*)&settings.server, sizeof(settings.server)) < 0) { perror("connect"); return -1; }

    settings.running = true;
    message_t login = {0}; login.type = htonl(MT_LOGIN);
    login.timestamp = htonl(time(NULL));
    strncpy(login.username, settings.username, sizeof(login.username) - 1);
    send_message(&login);

    if (settings.tui) enable_raw_mode();

    pthread_t recv_thread;
    pthread_create(&recv_thread, NULL, receive_thread, NULL);

    char* line = NULL; size_t cap = 0;
    char input[INPUT_BUFFER] = {0};
    size_t pos = 0;

    while (settings.running && !disconnected_by_server) {
        if (settings.tui) {
            char c; if (read(STDIN_FILENO, &c, 1) <= 0) continue;
            if (c == '\n') {
                input[pos] = 0;
                if (strlen(input) > 0) {
                    message_t msg = {0};
                    msg.type = htonl(MT_MESSAGE_SEND);
                    msg.timestamp = htonl(time(NULL));
                    strncpy(msg.username, settings.username, sizeof(msg.username) - 1);
                    strncpy(msg.message, input, sizeof(msg.message) - 1);
                    send_message(&msg);
                }
                pos = 0; input[0] = 0;
            } else if (c == 127) { if (pos > 0) pos--; input[pos] = 0; }
            else if (isprint(c)) { input[pos++] = c; input[pos] = 0; }
            draw_tui(input);
        } else {
            ssize_t len = getline(&line, &cap, stdin);
            if (len <= 0) { settings.running = false; break; }
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (!validate_message(line, len)) { fprintf(stderr, "Invalid characters in message\n"); continue; }

            message_t msg = {0};
            msg.type = htonl(MT_MESSAGE_SEND);
            msg.timestamp = htonl(time(NULL));
            strncpy(msg.username, settings.username, sizeof(msg.username) - 1);
            strncpy(msg.message, line, sizeof(msg.message) - 1);
            send_message(&msg);
        }
    }

    settings.running = false;
    pthread_join(recv_thread, NULL);

    if (!disconnected_by_server) { message_t logout = {0}; logout.type = htonl(MT_LOGOUT); write(settings.socket_fd, &logout, sizeof(logout)); }

    if (settings.tui) reset_terminal();
    free(username);
    close(settings.socket_fd);
    return 0;
}
