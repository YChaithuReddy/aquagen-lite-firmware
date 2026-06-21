#include "dns_server.h"
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "dns_portal";
static TaskHandle_t s_task = NULL;
static volatile int s_sock = -1;

#define DNS_PORT 53
// 192.168.4.1 — the SoftAP gateway every query resolves to.
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

// Minimal captive-portal DNS responder: every A query is answered with 192.168.4.1.
static void dns_task(void *arg)
{
    uint8_t buf[512];
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(DNS_PORT),
    };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket failed"); s_task = NULL; vTaskDelete(NULL); return; }
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "bind :53 failed"); close(sock); s_task = NULL; vTaskDelete(NULL); return;
    }
    s_sock = sock;
    ESP_LOGI(TAG, "captive DNS up on :53 → 192.168.4.1");

    while (1) {
        struct sockaddr_in cli; socklen_t clen = sizeof(cli);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &clen);
        if (len < 0) break;                 // socket closed on stop
        if (len < 12) continue;             // smaller than a DNS header → ignore

        // Turn the query into an authoritative answer in place.
        buf[2] = 0x81;                       // QR=1, Opcode=0, AA=1
        buf[3] = 0x80;                       // RA=1, RCODE=0 (no error)
        buf[6] = 0x00; buf[7] = 0x01;        // ANCOUNT = 1
        buf[8] = 0x00; buf[9] = 0x00;        // NSCOUNT = 0
        buf[10] = 0x00; buf[11] = 0x00;      // ARCOUNT = 0

        // Walk past the question's QNAME (sequence of length-prefixed labels, 0-terminated).
        int p = 12;
        while (p < len && buf[p] != 0) {
            if (buf[p] & 0xC0) { p = -1; break; }   // compressed name in a question → bail
            p += buf[p] + 1;
        }
        if (p < 0 || p >= len) continue;
        p += 1 + 4;                          // null label + QTYPE(2) + QCLASS(2) → end of question
        if (p + 16 > (int)sizeof(buf)) continue;

        // Answer RR: name = pointer to the question (0xC00C), type A, class IN, TTL 60, 4-byte IP.
        buf[p++] = 0xC0; buf[p++] = 0x0C;
        buf[p++] = 0x00; buf[p++] = 0x01;    // TYPE A
        buf[p++] = 0x00; buf[p++] = 0x01;    // CLASS IN
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C;  // TTL = 60s
        buf[p++] = 0x00; buf[p++] = 0x04;    // RDLENGTH = 4
        buf[p++] = AP_IP[0]; buf[p++] = AP_IP[1]; buf[p++] = AP_IP[2]; buf[p++] = AP_IP[3];

        sendto(sock, buf, p, 0, (struct sockaddr *)&cli, clen);
    }

    close(sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (s_task) return;
    xTaskCreate(dns_task, "dns_portal", 3072, NULL, 5, &s_task);
}

void dns_server_stop(void)
{
    int sock = s_sock;
    if (sock >= 0) { shutdown(sock, 0); close(sock); s_sock = -1; }  // unblocks recvfrom
}
