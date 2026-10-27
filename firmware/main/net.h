/* Wi-Fi(STA) + mDNS + TCP 서버 — deck.py 프로토콜을 네트워크로도 받는다 */
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DECK_TCP_PORT   5555
#define DECK_HOSTNAME   "deck"       /* mDNS: deck.local, 서비스 _deck._tcp */

/* 수신 공용 버퍼(USB+TCP)가 만들어진 뒤 호출. 내부에서 Wi-Fi/mDNS/서버 태스크 시작 */
void net_start(void);
/* TCP 클라이언트가 붙어 있으면 그쪽으로 송신 (논블로킹, 못 보내면 버림) */
void net_send(const char *buf, int len);
bool net_connected(void);

#ifdef __cplusplus
}
#endif
#endif
