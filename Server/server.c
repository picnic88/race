// server.c
#include "game_protocol.h"
#include "game_logic.h"
#include <time.h>

enum {
    ST_WAIT,
    ST_MAP,
    ST_ORDER,
    ST_GAME,
    ST_MINIGAME
};

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

// 같은 위치에 있는 플레이어 수 반환 (미니게임 트리거)
static int CheckPositionCollision(
    int positions[],
    int target_pos,
    int collided_players[],
    int n
) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (positions[i] == target_pos) {
            collided_players[cnt++] = i;
        }
    }
    return cnt;
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

    static int mg_active = 0;
    static int mg_type = MG_REACTION;   // 현재 미니게임 타입
    static int mg_players[MAX_PLAYERS];
    static int mg_count = 0;
    static int mg_winner = -1;
    static int mg_reaction[MAX_PLAYERS];
    static int mg_received[MAX_PLAYERS];
    static DWORD mg_go_time = 0;
    static DWORD mg_penalty_until[MAX_PLAYERS]; // 반응속도 선입력 페널티 입력 금지 해제 시각

    static DWORD mg_end_time = 0;       // 연타 종료 시각
    static int mg_score[MAX_PLAYERS];   // 연타 점수
    static DWORD mg_last_input[MAX_PLAYERS]; // 연타 스팸 방지

    srand((unsigned int)time(NULL));
    printf("=== [SERVER] Dice Race ===\n");

    while (1) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listen_sock, &rset);

        for (int i = 0; i < MAX_PLAYERS; i++)
            if (clients[i] != INVALID_SOCKET)
                FD_SET(clients[i], &rset);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000; // 20ms

        int sel = select(0, &rset, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;

        // ===== 연타 미니게임 실시간 점수 브로드캐스트 =====
        static DWORD last_mash_broadcast = 0;

        if (state == ST_MINIGAME &&
            mg_active &&
            mg_type == MG_MASH &&
            mg_go_time != 0)
        {
            DWORD now = GetTickCount();

            if (now - last_mash_broadcast >= 100) { // 100ms
                last_mash_broadcast = now;

                GamePacket up = { 0 };
                up.type = PKT_MINIGAME_UPDATE;
                up.minigame_type = MG_MASH;
                up.minigame_count = mg_count;
                memcpy(up.minigame_players, mg_players,
                    sizeof(int) * mg_count);

                for (int k = 0; k < mg_count; k++) {
                    int pid = mg_players[k];
                    up.positions[pid] = mg_score[pid];
                }

                for (int j = 0; j < MAX_PLAYERS; j++)
                    if (clients[j] != INVALID_SOCKET)
                        send(clients[j], (char*)&up, sizeof(up), 0);
            }
        }

        // ===== 연타 미니게임 타임아웃 처리 =====
        if (state == ST_MINIGAME &&
            mg_active &&
            mg_type == MG_MASH &&
            mg_go_time != 0 &&
            GetTickCount() > mg_end_time)
        {
            int best = -1;
            int best_score = -1;
            int tie = 0;

            for (int k = 0; k < mg_count; k++) {
                int pid = mg_players[k];
                int score = mg_score[pid];

                if (score > best_score) {
                    best_score = score;
                    best = pid;
                    tie = 0;
                }
                else if (score == best_score) {
                    tie = 1;   // 동점 발생
                }
            }

            // ===== 동점이면 무효 =====
            if (tie || best_score <= 0) {

                mg_active = 0;
                mg_winner = -1;

                // 결과 패킷 (무효)
                for (int k = 0; k < mg_count; k++) {
                    int pid = mg_players[k];

                    GamePacket r = { 0 };
                    r.type = PKT_MINIGAME_RESULT;
                    r.player_id = -1;          // 승자 없음
                    r.value = mg_score[pid];
                    strcpy(r.message, "SYS:MASH_DRAW");

                    send(clients[pid], (char*)&r, sizeof(r), 0);
                }

                // 이동 없음 / 보상 없음
            }
            else {
                // ===== 단독 승자 =====
                mg_winner = best;

                for (int k = 0; k < mg_count; k++) {
                    int pid = mg_players[k];

                    GamePacket r = { 0 };
                    r.type = PKT_MINIGAME_RESULT;
                    r.player_id = mg_winner;
                    r.value = mg_score[pid];
                    strcpy(r.message,
                        pid == mg_winner
                        ? "SYS:MASH_WON"
                        : "SYS:MASH_LOST");

                    send(clients[pid], (char*)&r, sizeof(r), 0);
                }

                // 보상
                positions[mg_winner] += 2;
                BroadcastUpdate(clients, positions, map_size);
            }

            // 다음 턴
            state = ST_GAME;
            turn_idx = (turn_idx + 1) % MAX_PLAYERS;
            int next = turn_order[turn_idx];
            SendTurnPacket(clients, next, positions, map_size, turn_order);
            for (int j = 0; j < MAX_PLAYERS; j++)
                if (j != next)
                    SendWaitPacket(clients, j, next, positions, map_size, turn_order);

            mg_active = 0;

            // 결과 전송
            for (int k = 0; k < mg_count; k++) {
                int pid = mg_players[k];

                GamePacket r = { 0 };
                r.type = PKT_MINIGAME_RESULT;
                r.player_id = mg_winner;
                r.value = mg_score[pid];
                strcpy(r.message,
                    pid == mg_winner
                    ? "SYS:MASH_WON"
                    : "SYS:MASH_LOST");

                send(clients[pid], (char*)&r, sizeof(r), 0);
            }

            // 보상
            positions[mg_winner] += 2;
            BroadcastUpdate(clients, positions, map_size);

            // 다음 턴
            state = ST_GAME;
            turn_idx = (turn_idx + 1) % MAX_PLAYERS;
            int next = turn_order[turn_idx];
            SendTurnPacket(clients, next, positions, map_size, turn_order);
            for (int j = 0; j < MAX_PLAYERS; j++)
                if (j != next)
                    SendWaitPacket(clients, j, next, positions, map_size, turn_order);
        }

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
                strcpy(req.message, "SYS:ORDER_SYNC");

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

                int collided[MAX_PLAYERS];
                int collide_cnt = CheckPositionCollision(
                    positions,
                    positions[i],
                    collided,
                    MAX_PLAYERS
                );

                if (collide_cnt >= 2) {
                    // 일단 이동 결과(위치/주사위/이벤트)를 먼저 뿌려서 화면 동기화
                    GamePacket upd = { 0 };
                    upd.type = PKT_UPDATE;
                    upd.player_id = i;
                    upd.dice1 = d1;
                    upd.dice2 = d2;
                    upd.map_size = map_size;
                    strcpy(upd.jokbo, jokbo);
                    memcpy(upd.positions, positions, sizeof(positions));
                    sprintf(upd.message, "%s %s", jokbo, evt);

                    for (int j = 0; j < MAX_PLAYERS; j++)
                        send(clients[j], (char*)&upd, sizeof(upd), 0);

                    // 미니게임 타입 선택
                    mg_type = (rand() % 2) ? MG_REACTION : MG_MASH;

                    // 미니게임 진입 시 초기화
                    state = ST_MINIGAME;
                    mg_active = 1;
                    mg_winner = -1;
                    mg_go_time = 0;
                    mg_count = collide_cnt;
                    memcpy(mg_players, collided, sizeof(int)* collide_cnt);

                    for (int k = 0; k < mg_count; k++) {
                        int pid = mg_players[k];
                        mg_received[pid] = 0;
                        mg_reaction[pid] = -1;
                        mg_score[pid] = 0;
                        mg_last_input[pid] = 0;
                        mg_penalty_until[pid] = 0;
                    }

                    // READY 패킷(전체 공지)
                    GamePacket mg = { 0 };
                    mg.type = PKT_MINIGAME_START;
                    mg.minigame_phase = 0; // READY
                    mg.minigame_type = mg_type;
                    mg.minigame_count = mg_count;
                    memcpy(mg.minigame_players, mg_players,
                        sizeof(int)* mg_count);

                    strcpy(mg.message,
                        mg_type == MG_MASH
                        ? "[ MASH MODE ] 3초 동안 SPACE 연타"
                        : "[ REACTION MODE ] GO 신호 후 SPACE 입력");

                    for (int j = 0; j < MAX_PLAYERS; j++)
                        send(clients[j], (char*)&mg, sizeof(mg), 0);

                    // 랜덤 딜레이 후 GO 패킷(참가자에게만)
                    Sleep(4000 + rand() % 1200);

                    mg_go_time = GetTickCount();
                    if (mg_type == MG_MASH)
                        mg_end_time = mg_go_time + 3000;

                    GamePacket go = { 0 };
                    go.type = PKT_MINIGAME_START;
                    go.minigame_phase = 1; // GO
                    go.minigame_type = mg_type;
                    go.minigame_count = mg_count;
                    memcpy(go.minigame_players, mg_players,
                        sizeof(int) * mg_count);
                    strcpy(go.message, ">>> GO <<<");

                    for (int k = 0; k < mg_count; k++) {
                        int pid = mg_players[k];
                        send(clients[pid], (char*)&go, sizeof(go), 0);
                    }

                    break; // 여기서 GAME 처리 종료하고 ST_MINIGAME으로 넘어감
                }

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

            // ---------- MINIGAME ----------
            else if (state == ST_MINIGAME) {

                if (!mg_active) continue;
                if (pkt.type != PKT_MINIGAME_INPUT) continue;

                // 참가자 체크 (공통)
                int is_participant = 0;
                for (int k = 0; k < mg_count; k++) {
                    if (mg_players[k] == i) {
                        is_participant = 1;
                        break;
                    }
                }
                if (!is_participant) continue;

                // GO 이전 입력 무시
                if (mg_go_time == 0) continue;

                // ===== 미니게임 타입 분기 =====
                if (mg_type == MG_REACTION) {   // 반응속도 측정

                    DWORD now = GetTickCount();

                    // ===== 패널티 중이면 입력 무시 =====
                    if (mg_penalty_until[i] > now)
                        continue;

                    // ===== GO 이전 입력 =====
                    if (mg_go_time == 0) {

                        // 0.4초 입력 금지
                        mg_penalty_until[i] = now + 400;

                        // 경고 패킷 전송 (해당 플레이어에게만)
                        GamePacket warn = { 0 };
                        warn.type = PKT_MINIGAME_RESULT;   // 또는 전용 WARNING 패킷 만들어도 됨
                        warn.player_id = -1;
                        strcpy(warn.message, "SYS:REACTION_EARLY");

                        send(clients[i], (char*)&warn, sizeof(warn), 0);
                        continue;
                    }

                    // ===== 이미 입력했으면 무시 =====
                    if (mg_received[i])
                        continue;

                    // ===== 정상 입력 =====
                    mg_received[i] = 1;
                    mg_reaction[i] = now - mg_go_time;

                    // 전원 입력 확인
                    int done = 1;
                    for (int k = 0; k < mg_count; k++) {
                        int pid = mg_players[k];
                        if (!mg_received[pid]) {
                            done = 0;
                            break;
                        }
                    }
                    if (!done) continue;

                    // 승자 결정
                    int best = -1;
                    for (int k = 0; k < mg_count; k++) {
                        int pid = mg_players[k];
                        if (best == -1 || mg_reaction[pid] < mg_reaction[best])
                            best = pid;
                    }

                    mg_winner = best;
                    mg_active = 0;

                    // 결과 패킷
                    for (int k = 0; k < mg_count; k++) {
                        int pid = mg_players[k];

                        GamePacket r = { 0 };
                        r.type = PKT_MINIGAME_RESULT;
                        r.player_id = mg_winner;
                        r.value = mg_reaction[pid];
                        strcpy(r.message,
                            pid == mg_winner ? "SYS:RACE_WON" : "SYS:RACE_LOST");

                        send(clients[pid], (char*)&r, sizeof(r), 0);
                    }

                    // 보상
                    positions[mg_winner] += 2;
                    BroadcastUpdate(clients, positions, map_size);

                    // 다음 턴
                    state = ST_GAME;
                    turn_idx = (turn_idx + 1) % MAX_PLAYERS;
                    int next = turn_order[turn_idx];
                    SendTurnPacket(clients, next, positions, map_size, turn_order);
                }

                else if (mg_type == MG_MASH) {  // 연타 측정

                    DWORD now = GetTickCount();

                    // 시간 초과면 무시 (종료는 외부에서)
                    if (now > mg_end_time)
                        continue;

                    // 연타 스팸 방지 (30ms)
                    if (now - mg_last_input[i] < 30)
                        continue;

                    mg_last_input[i] = now;
                    mg_score[i]++;
                }
            }

        }
    }

    WSACleanup();
    return 0;
}
