// server.c
#include "game_protocol.h"
#include "game_logic.h"
#include <time.h>

enum { ST_WAIT, ST_MAP, ST_ORDER, ST_GAME };

// ------------------ 유틸 ------------------

static void BroadcastUpdate(SOCKET clients[], int positions[], int map_size) {
    GamePacket st = { 0 };
    st.type = PKT_UPDATE;
    st.map_size = map_size;
    memcpy(st.positions, positions, sizeof(int) * MAX_PLAYERS);

    for (int j = 0; j < MAX_PLAYERS; j++) {
        if (clients[j] != INVALID_SOCKET)
            send(clients[j], (char*)&st, sizeof(st), 0);
    }
}

static void SendTurnPacket(
    SOCKET clients[],
    int current_player,
    int positions[],
    int map_size,
    int turn_order[]
) {
    GamePacket p = { 0 };
    p.type = PKT_YOUR_TURN;
    p.player_id = current_player;
    p.map_size = map_size;
    memcpy(p.positions, positions, sizeof(int) * MAX_PLAYERS);
    memcpy(p.turn_order, turn_order, sizeof(int) * MAX_PLAYERS);

    send(clients[current_player], (char*)&p, sizeof(p), 0);
}

static void SendWaitPacket(
    SOCKET clients[],
    int to_player,
    int current_player,
    int positions[],
    int map_size,
    int turn_order[]
) {
    GamePacket p = { 0 };
    p.type = PKT_WAIT;
    p.player_id = current_player;
    p.map_size = map_size;
    memcpy(p.positions, positions, sizeof(int) * MAX_PLAYERS);
    memcpy(p.turn_order, turn_order, sizeof(int) * MAX_PLAYERS);

    send(clients[to_player], (char*)&p, sizeof(p), 0);
}

// ------------------ main ------------------

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) err_quit("socket()");

    BOOL opt = TRUE;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        err_quit("bind()");
    listen(listen_sock, SOMAXCONN);

    SOCKET clients[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) clients[i] = INVALID_SOCKET;

    int positions[MAX_PLAYERS] = { 0 };
    int order_rolls[MAX_PLAYERS] = { 0 };
    int turn_order[MAX_PLAYERS] = { 0 };
    int received_order[MAX_PLAYERS] = { 0 };

    int client_cnt = 0;
    int state = ST_WAIT;
    int map_size = 30;
    int order_cnt = 0;
    int turn_idx = 0;

    srand((unsigned int)time(NULL));
    printf("=== [SERVER] Dice Race ===\n");

    while (1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listen_sock, &rset);

        for (int i = 0; i < MAX_PLAYERS; i++)
            if (clients[i] != INVALID_SOCKET)
                FD_SET(clients[i], &rset);

        if (select(0, &rset, NULL, NULL, NULL) == SOCKET_ERROR) break;

        // ---------- 접속 ----------
        if (FD_ISSET(listen_sock, &rset)) {
            SOCKET cs = accept(listen_sock, NULL, NULL);
            if (client_cnt < MAX_PLAYERS) {
                clients[client_cnt] = cs;

                GamePacket hello = { 0 };
                hello.type = PKT_ASSIGN_ID;
                hello.player_id = client_cnt;
                send(cs, (char*)&hello, sizeof(hello), 0);

                printf(">> Player %d Connected\n", client_cnt + 1);
                client_cnt++;

                if (client_cnt == MAX_PLAYERS) {
                    state = ST_MAP;
                    GamePacket req = { 0 };
                    req.type = PKT_MAP_REQ;
                    send(clients[0], (char*)&req, sizeof(req), 0);
                }
            }
            else {
                closesocket(cs);
            }
        }

        // ---------- 데이터 ----------
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (clients[i] == INVALID_SOCKET) continue;
            if (!FD_ISSET(clients[i], &rset)) continue;

            GamePacket pkt;
            int ret = recv(clients[i], (char*)&pkt, sizeof(pkt), 0);
            if (ret <= 0) {
                closesocket(clients[i]);
                clients[i] = INVALID_SOCKET;
                continue;
            }

            // MAP
            if (state == ST_MAP && i == 0 && pkt.type == PKT_MAP_REQ) {
                map_size = pkt.map_size;
                state = ST_ORDER;

                order_cnt = 0;
                memset(received_order, 0, sizeof(received_order));

                GamePacket req = { 0 };
                req.type = PKT_ORDER_REQ;
                req.map_size = map_size;

                for (int j = 0; j < MAX_PLAYERS; j++)
                    send(clients[j], (char*)&req, sizeof(req), 0);
            }

            // ORDER
            else if (state == ST_ORDER && pkt.type == PKT_ORDER_REQ) {
                if (received_order[i]) continue;
                received_order[i] = 1;

                order_rolls[i] = (rand() % 12) + 1;
                order_cnt++;

                if (order_cnt == MAX_PLAYERS) {
                    SortTurnOrder(turn_order, order_rolls, MAX_PLAYERS);
                    state = ST_GAME;
                    turn_idx = 0;

                    BroadcastUpdate(clients, positions, map_size);

                    int current = turn_order[turn_idx];
                    SendTurnPacket(clients, current, positions, map_size, turn_order);

                    for (int j = 0; j < MAX_PLAYERS; j++) {
                        if (j == current) continue;
                        SendWaitPacket(clients, j, current, positions, map_size, turn_order);
                    }
                }
            }

            // GAME
            else if (state == ST_GAME) {
                int current = turn_order[turn_idx];
                if (i != current) continue;
                if (pkt.type != PKT_YOUR_TURN) continue;

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
                    if (clients[j] != INVALID_SOCKET)
                        send(clients[j], (char*)&res, sizeof(res), 0);

                if (!win) {
                    turn_idx = (turn_idx + 1) % MAX_PLAYERS;
                    int next = turn_order[turn_idx];

                    SendTurnPacket(clients, next, positions, map_size, turn_order);

                    for (int j = 0; j < MAX_PLAYERS; j++) {
                        if (j == next) continue;
                        SendWaitPacket(clients, j, next, positions, map_size, turn_order);
                    }
                }
            }
        }
    }

    WSACleanup();
    return 0;
}
