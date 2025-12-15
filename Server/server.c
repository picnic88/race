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

    for (int j = 0; j < MAX_PLAYERS; j++)
        if (clients[j] != INVALID_SOCKET)
            send(clients[j], (char*)&st, sizeof(st), 0);
}

static void SendTurnPacket(
    SOCKET clients[], int current_player,
    int positions[], int map_size, int turn_order[]
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
    SOCKET clients[], int to_player, int current_player,
    int positions[], int map_size, int turn_order[]
) {
    GamePacket p = { 0 };
    p.type = PKT_WAIT;
    p.player_id = current_player;
    p.map_size = map_size;
    memcpy(p.positions, positions, sizeof(int) * MAX_PLAYERS);
    memcpy(p.turn_order, turn_order, sizeof(int) * MAX_PLAYERS);

    send(clients[to_player], (char*)&p, sizeof(p), 0);
}

// 이번 라운드 active 플레이어끼리만 동점 검사
static int CheckDuplicateRollsMasked(
    int rolls[], int active[], int need_reroll[], int n
) {
    int dup = 0;
    memset(need_reroll, 0, sizeof(int) * n);

    for (int i = 0; i < n; i++) {
        if (!active[i]) continue;
        for (int j = i + 1; j < n; j++) {
            if (!active[j]) continue;
            if (rolls[i] == rolls[j]) {
                need_reroll[i] = 1;
                need_reroll[j] = 1;
                dup = 1;
            }
        }
    }
    return dup;
}

static void BroadcastGameStart(SOCKET clients[], int map_size, int turn_order[]) {
    GamePacket p = { 0 };
    p.type = PKT_GAME_START;
    p.map_size = map_size;
    memcpy(p.turn_order, turn_order, sizeof(int) * MAX_PLAYERS);

    for (int i = 0; i < MAX_PLAYERS; i++)
        if (clients[i] != INVALID_SOCKET)
            send(clients[i], (char*)&p, sizeof(p), 0);
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

    int order_rolls[MAX_PLAYERS];
    int order_active[MAX_PLAYERS];
    int order_received[MAX_PLAYERS];
    int need_reroll[MAX_PLAYERS];
    int turn_order[MAX_PLAYERS];

    int client_cnt = 0;
    int state = ST_WAIT;
    int map_size = 100;
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

        if (select(0, &rset, NULL, NULL, NULL) == SOCKET_ERROR)
            break;

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

            // ---------- MAP ----------
            if (state == ST_MAP && i == 0 && pkt.type == PKT_MAP_REQ) {

                map_size = (pkt.map_size >= 50 && pkt.map_size <= 200)
                    ? pkt.map_size : 100;

                state = ST_ORDER;

                // 순서 정하기 최초 라운드: 전원 active
                for (int k = 0; k < MAX_PLAYERS; k++) {
                    order_active[k] = 1;
                    order_received[k] = 0;
                    order_rolls[k] = -1;
                }

                GamePacket req = { 0 };
                req.type = PKT_ORDER_REQ;
                strcpy(req.message,
                    "[ ORDER SYNC ]\n"
                    "SPACE를 입력하여 순서 결정");

                for (int j = 0; j < MAX_PLAYERS; j++)
                    send(clients[j], (char*)&req, sizeof(req), 0);
            }

            // ---------- ORDER ----------
            else if (state == ST_ORDER && pkt.type == PKT_ORDER_REQ) {

                if (!order_active[i]) continue;
                if (order_received[i]) continue;

                order_received[i] = 1;
                order_rolls[i] = (rand() % 12) + 1;

                printf(">> P%d 순서 주사위: %d\n", i + 1, order_rolls[i]);

                GamePacket res = { 0 };
                res.type = PKT_ORDER_RESULT;
                res.player_id = i;
                res.dice1 = order_rolls[i];
                sprintf(res.message,
                    "[ ORDER ROLL ] P%d = %d",
                    i + 1, order_rolls[i]);

                for (int j = 0; j < MAX_PLAYERS; j++)
                    send(clients[j], (char*)&res, sizeof(res), 0);

                int active_cnt = 0, received_cnt = 0;
                for (int k = 0; k < MAX_PLAYERS; k++) {
                    if (order_active[k]) {
                        active_cnt++;
                        if (order_received[k]) received_cnt++;
                    }
                }

                if (received_cnt < active_cnt) continue;

                // ----- 동점 검사 -----
                if (CheckDuplicateRollsMasked(order_rolls, order_active, need_reroll, MAX_PLAYERS)) {

                    // 메시지 생성
                    char reroll_msg[128] = "[ REROLL REQUIRED ]\n재굴림 대상: ";

                    int first = 1;
                    for (int k = 0; k < MAX_PLAYERS; k++) {
                        if (need_reroll[k]) {
                            char tmp[16];
                            sprintf(tmp, "%sP%d", first ? "" : ", ", k + 1);
                            strcat(reroll_msg, tmp);
                            first = 0;
                        }
                    }

                    // 모두에게 재굴림 대상 리스트 공지
                    GamePacket notice = { 0 };
                    notice.type = PKT_ORDER_RESULT;
                    notice.player_id = -1;
                    strcpy(notice.message, reroll_msg);

                    for (int j = 0; j < MAX_PLAYERS; j++)
                        send(clients[j], (char*)&notice, sizeof(notice), 0);

                    // 대상만 다시 굴리기
                    for (int k = 0; k < MAX_PLAYERS; k++) {
                        if (need_reroll[k]) {
                            order_active[k] = 1;
                            order_received[k] = 0;
                            order_rolls[k] = -1;   // 재굴림 대상은 리셋
                        }
                        else {
                            order_active[k] = 0;   // 비대상은 이번 라운드 비활성
                        }
                    }

                    // 재굴림 대상에게만 입력 요청
                    GamePacket req = { 0 };
                    req.type = PKT_ORDER_REQ;
                    strcpy(req.message,
                        "[ REROLL INPUT REQUIRED ]\n"
                        "당신은 재굴림 대상입니다\n"
                        "SPACE 또는 ENTER를 입력하세요");

                    for (int k = 0; k < MAX_PLAYERS; k++)
                        if (order_active[k])
                            send(clients[k], (char*)&req, sizeof(req), 0);

                    continue;
                }

                // ----- 순서 확정 -----
                SortTurnOrder(turn_order, order_rolls, MAX_PLAYERS);

                state = ST_GAME;
                turn_idx = 0;

                // 게임 시작 패킷
                BroadcastGameStart(clients, map_size, turn_order);

                // 상태 동기화용 (위치 0)
                BroadcastUpdate(clients, positions, map_size);

                // 첫 턴
                int cur = turn_order[turn_idx];
                SendTurnPacket(clients, cur, positions, map_size, turn_order);

                for (int j = 0; j < MAX_PLAYERS; j++)
                    if (j != cur)
                        SendWaitPacket(clients, j, cur, positions, map_size, turn_order);
            }

            // ---------- GAME ----------
            else if (state == ST_GAME) {

                int current = turn_order[turn_idx];
                if (i != current) continue;
                if (pkt.type != PKT_YOUR_TURN) continue;

                int d1, d2;
                char jokbo[50];
                RollTwoDice(&d1, &d2, jokbo);

                positions[i] += (d1 + d2);
                const char* evt = CheckMapEvent(&positions[i], map_size);

                int win = (positions[i] >= map_size);
                if (win) positions[i] = map_size;

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
                    int next = turn_order[turn_idx];

                    SendTurnPacket(clients, next, positions, map_size, turn_order);
                    for (int j = 0; j < MAX_PLAYERS; j++)
                        if (j != next)
                            SendWaitPacket(clients, j, next, positions, map_size, turn_order);
                }
            }
        }
    }

    WSACleanup();
    return 0;
}
