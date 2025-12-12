// server_main.c
#include "game_protocol.h"
#include "game_logic.h"
#include <time.h>

enum { ST_WAIT, ST_MAP, ST_ORDER, ST_GAME }; // 서버 상태

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) err_quit("socket()");

    BOOL opt = TRUE;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in srv_addr = { AF_INET, htons(PORT), htonl(INADDR_ANY) };
    if (bind(listen_sock, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) == SOCKET_ERROR) err_quit("bind()");
    listen(listen_sock, SOMAXCONN);

    SOCKET clients[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) clients[i] = INVALID_SOCKET;

    int positions[MAX_PLAYERS] = { 0 };
    int order_rolls[MAX_PLAYERS] = { 0 }; // 순서 정하기 주사위 값
    int turn_order[MAX_PLAYERS] = { 0 };  // 결정된 턴 순서

    int client_cnt = 0;
    int state = ST_WAIT;
    int map_size = 30;
    int order_cnt = 0;
    int turn_idx = 0;

    srand((unsigned int)time(NULL));
    printf("=== [SERVER] Dice Race (Max 4 Players) ===\n");

    while (1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listen_sock, &rset);
        SOCKET maxfd = listen_sock;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (clients[i] != INVALID_SOCKET) {
                FD_SET(clients[i], &rset);
                if (clients[i] > maxfd) maxfd = clients[i];
            }
        }

        if (select(0, &rset, NULL, NULL, NULL) == SOCKET_ERROR) break;

        // 1. 접속 처리
        if (FD_ISSET(listen_sock, &rset)) {
            SOCKET cs = accept(listen_sock, NULL, NULL);
            if (client_cnt < MAX_PLAYERS) {
                clients[client_cnt] = cs;
                printf(">> Player %d Connected\n", client_cnt + 1);
                client_cnt++;

                if (client_cnt == MAX_PLAYERS) {
                    state = ST_MAP;
                    GamePacket pkt = { 0 };
                    pkt.type = PKT_MAP_REQ;
                    send(clients[0], (char*)&pkt, sizeof(pkt), 0); // P1에게 맵 선택 요청
                    printf(">> 4명 접속 완료. 맵 크기 요청 중...\n");
                }
            }
            else {
                closesocket(cs);
            }
        }

        // 2. 데이터 처리
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (clients[i] != INVALID_SOCKET && FD_ISSET(clients[i], &rset)) {
                GamePacket pkt;
                int ret = recv(clients[i], (char*)&pkt, sizeof(pkt), 0);
                if (ret <= 0) {
                    closesocket(clients[i]);
                    clients[i] = INVALID_SOCKET;
                    continue;
                }

                if (state == ST_MAP && i == 0) { // 맵 선택 응답 (P1)
                    map_size = pkt.map_size;
                    printf(">> 맵 크기 결정: %d칸\n", map_size);

                    state = ST_ORDER;
                    GamePacket req = { 0 };
                    req.type = PKT_ORDER_REQ;
                    req.map_size = map_size;
                    for (int j = 0; j < MAX_PLAYERS; j++) send(clients[j], (char*)&req, sizeof(req), 0);
                }
                else if (state == ST_ORDER) { // 순서 정하기 주사위
                    order_rolls[i] = (rand() % 12) + 1; // 1~100 난수 (동점 방지)
                    order_cnt++;
                    printf(">> P%d 순서 주사위: %d\n", i + 1, order_rolls[i]);

                    if (order_cnt == MAX_PLAYERS) {
                        SortTurnOrder(turn_order, order_rolls, MAX_PLAYERS);

                        state = ST_GAME;
                        GamePacket start = { 0 };
                        start.type = PKT_YOUR_TURN;
                        start.map_size = map_size;
                        memcpy(start.turn_order, turn_order, sizeof(turn_order)); // 순서 정보 공유

                        send(clients[turn_order[0]], (char*)&start, sizeof(start), 0); // 첫 턴

                        start.type = PKT_WAIT;
                        for (int j = 1; j < MAX_PLAYERS; j++)
                            send(clients[turn_order[j]], (char*)&start, sizeof(start), 0); // 나머지 대기
                    }
                }
                else if (state == ST_GAME) { // 게임 진행
                    if (i == turn_order[turn_idx]) {
                        int d1, d2;
                        char jokbo[50];
                        RollTwoDice(&d1, &d2, jokbo);

                        positions[i] += (d1 + d2);
                        const char* evt = CheckMapEvent(&positions[i], map_size);

                        int win = 0;
                        if (positions[i] >= map_size) {
                            positions[i] = map_size;
                            win = 1;
                        }

                        GamePacket res = { 0 };
                        res.type = win ? PKT_GAME_OVER : PKT_UPDATE;
                        res.player_id = i;
                        res.dice1 = d1;
                        res.dice2 = d2;
                        res.map_size = map_size;
                        strcpy(res.jokbo, jokbo);
                        memcpy(res.positions, positions, sizeof(positions));
                        sprintf(res.message, "%s %s", jokbo, evt);

                        for (int j = 0; j < MAX_PLAYERS; j++)
                            send(clients[j], (char*)&res, sizeof(res), 0);

                        if (!win) {
                            turn_idx = (turn_idx + 1) % MAX_PLAYERS;
                            GamePacket next = { 0 };
                            next.type = PKT_YOUR_TURN;
                            send(clients[turn_order[turn_idx]], (char*)&next, sizeof(next), 0);
                        }
                        else {
                            printf(">> 게임 종료. 승자: P%d\n", i + 1);
                        }
                    }
                }
            }
        }
    }
    WSACleanup();
    return 0;
}