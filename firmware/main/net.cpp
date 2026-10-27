/*
 * net.cpp — Wi-Fi STA(휴대폰 핫스팟) + mDNS(deck.local) + TCP 서버(5555)
 * 받은 바이트는 deck_rx_push() 로 USB 와 같은 수신 버퍼에 넣는다.
 * 보낼 바이트는 net_send() — deck_printf 가 USB 와 함께 호출.
 */
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "lwip/sockets.h"

#include "net.h"
#include "deck_io.h"
#include "wifi_secret.h"     /* WIFI_SSID / WIFI_PASS — wifi_secret.h.example 참고 */

static const char *TAG = "net";
static volatile int client_fd = -1;
static volatile bool wifi_up = false;
static int retry_count = 0;

bool net_connected(void) { return client_fd >= 0; }

void net_send(const char *buf, int len)
{
    int fd = client_fd;
    if (fd < 0) return;
    int r = send(fd, buf, len, MSG_DONTWAIT);
    (void)r;   /* 못 보내면 버림 (LVGL 태스크 블로킹 방지) */
}

/* ---------- Wi-Fi 이벤트 ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_up = false;
        retry_count++;
        ESP_LOGW(TAG, "disconnected, retry %d", retry_count);
        esp_wifi_connect();                 /* 드라이버 자체가 수 초 간격으로 재시도 */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        retry_count = 0;
        wifi_up = true;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", ip);
        /* 화면 토스트로 IP 표시 — 수신 버퍼에 T: 줄을 직접 넣는다 */
        char line[96];
        int n = snprintf(line, sizeof(line), "T:Wi-Fi|%s|%s:%d  (deck.local)\n", WIFI_SSID, ip, DECK_TCP_PORT);
        deck_rx_push((const uint8_t *)line, n);
    }
}

static void wifi_init_sta(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, DECK_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_PASS[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));     /* 지연 최소화 (전원은 차량이라 무관) */
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(DECK_HOSTNAME);
    mdns_instance_name_set("Deck Display");
    mdns_service_add(NULL, "_deck", "_tcp", DECK_TCP_PORT, NULL, 0);
}

/* ---------- TCP 서버: 클라이언트 1개, 새로 오면 이전 연결 끊음 ---------- */
static void tcp_server_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DECK_TCP_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "bind/listen failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "tcp server on :%d", DECK_TCP_PORT);

    static uint8_t rx[1024];
    for (;;) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int fd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int ka = 1, idle = 5, intvl = 3, cnt = 3;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

        int old = client_fd;
        client_fd = fd;
        if (old >= 0) { shutdown(old, SHUT_RDWR); close(old); }
        ESP_LOGI(TAG, "client %s connected", inet_ntoa(caddr.sin_addr));
        deck_rx_push((const uint8_t *)"T:Wi-Fi|앱 연결됨|\n", strlen("T:Wi-Fi|앱 연결됨|\n"));

        for (;;) {
            int n = recv(fd, rx, sizeof(rx), 0);
            if (n <= 0) break;                       /* 끊김 */
            deck_rx_push(rx, n);
        }
        ESP_LOGI(TAG, "client disconnected");
        if (client_fd == fd) client_fd = -1;
        close(fd);
    }
}

void net_start(void)
{
    wifi_init_sta();
    mdns_start();
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_srv", 4096, NULL, 3, NULL, 1);
}
