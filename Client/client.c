// client.c
#define _CRT_SECURE_NO_WARNINGS
#include "game_protocol.h"
#include <conio.h>
#include <windows.h>

#define TRACK_WIDTH 100   // 콘솔 트랙 폭 고정

static int g_my_id = -1;        // 서버가 배정해주는 내 플레이어 번호(0~3)
static int g_map_size = 0;      // 서버에서 받은 실제 맵 크기
static int g_waiting_order_input = 0;
static int g_order_rolls[MAX_PLAYERS] = { 0 };
static int g_rerolling = 0;
static int g_turn_order[MAX_PLAYERS] = { -1, -1, -1, -1 };
static int g_turn_order_shown = 0;
static int g_showing_result = 0;
static int g_game_started = 0;
static int g_just_started = 0;
static int g_in_minigame = 0;
static int g_minigame_phase = 0;   // 0=READY, 1=GO
static int g_minigame_count = 0;
static int g_minigame_players[MAX_PLAYERS];
static int g_minigame_type = 0;   // MG_REACTION / MG_MASH
static DWORD g_mash_end_time = 0;
static int g_mashing = 0;

static void cls() { system("cls"); }

typedef struct {    // 서버 메시지 매핑
    const char* key;
    WORD color;
    const char* text;
} SysMsg;

// ---------------- 콘솔 색상 ----------------

static HANDLE g_hConsole;

enum {
    COL_DEFAULT = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
    COL_TITLE = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY, // 네온 시안
    COL_SYSTEM = FOREGROUND_GREEN | FOREGROUND_INTENSITY,                   // 네온 그린
    COL_WARN = FOREGROUND_RED | FOREGROUND_INTENSITY,                     // 경고 레드
    COL_ACCENT = FOREGROUND_BLUE | FOREGROUND_INTENSITY,                   // 블루
    COL_DIM = FOREGROUND_GREEN | FOREGROUND_BLUE                          // 흐린 시안
};

static void SetCol(WORD col) {
    SetConsoleTextAttribute(g_hConsole, col);
}

static SysMsg SYS_MSG_TABLE[] = {
    { "SYS:ORDER_SYNC", COL_SYSTEM, "[ PROTOCOL SYNC ] 순서 동기화\nSPACE를 입력하여 진행" },
    { "SYS:REACTION_EARLY", COL_WARN, "[ INPUT VIOLATION ]\n신호 이전 입력 감지\n0.4초 입력 차단" },
    { "SYS:RACE_WON",   COL_ACCENT, "[ SYSTEM OVERRIDE ] 우선권 확보\nPROGRESS +2" },
    { "SYS:RACE_LOST",  COL_WARN,   "[ LATENCY ERROR ] 반응 지연" },
    { "SYS:MASH_WON",   COL_ACCENT, "[ SYSTEM OVERRIDE ] 우선권 확보\nPROGRESS +2" },
    { "SYS:MASH_LOST",  COL_WARN,   "[ LATENCY ERROR ] 반응 지연" },
    { "SYS:MASH_DRAW", COL_DIM, "[ SYSTEM NEUTRALIZED ]\n반응 동점\n효과 없음" },
};

// ---------------- UI ----------------

static void DrawBanner() {
    SetCol(COL_TITLE);
    printf("=========================================\n");
    printf("  D I C E   R A C E   O N L I N E\n");
    printf("    [ NETWORK PROTOCOL v1.0 ]\n");
    printf("=========================================\n\n");
    SetCol(COL_DEFAULT);
}

static void DrawTrack(int real_map_size, int positions[MAX_PLAYERS]) {

    if (real_map_size <= 0) {
        printf("[TRACK] 맵 정보 수신 대기 중...\n\n");
        return;
    }

    int draw_width = TRACK_WIDTH;

    printf("[TRACK] 0");
    for (int i = 0; i < draw_width - 1; i++) printf("-");
    printf("|%d\n", real_map_size);   // 실제 맵 크기 표시

    for (int p = 0; p < MAX_PLAYERS; p++) {

        int pos = positions[p];
        if (pos < 0) pos = 0;
        if (pos > real_map_size) pos = real_map_size;

        // 실제 위치 → 고정 폭 위치 비율 변환
        int draw_pos = (pos * draw_width) / real_map_size;

        if (draw_pos < 0) draw_pos = 0;
        if (draw_pos >= draw_width) draw_pos = draw_width - 1;

        printf("P%d %s |", p + 1, (p == g_my_id ? "(YOU)" : "     "));

        for (int x = 0; x < draw_width; x++) {
            if (x == draw_pos) printf(">");
            else printf(".");
        }
        printf("|\n");
    }
    printf("\n");
}

static void PrintPositions(int positions[MAX_PLAYERS]) {
    printf("[현재 위치]\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        printf(" P%d: %d\n", i + 1, positions[i]);
    }
    printf("\n");
}

static void FlushKeyBuffer() {
    while (_kbhit()) {
        _getch();
    }
}

static int IsOrderComplete() {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_order_rolls[i] <= 0)
            return 0;
    }
    return 1;
}

static void DrawFinalTurnOrder() {
    printf("[턴 순서 확정]\n\n");

    for (int i = 0; i < MAX_PLAYERS; i++) {
        int pid = g_turn_order[i];
        if (pid >= 0) {
            printf("%d번: P%d %s\n",
                i + 1,
                pid + 1,
                pid == g_my_id ? "<- YOU" : "");
        }
    }

    printf("\n");
}

// 미니게임 참가자인지 판별
static int IsMinigameParticipant() {
    for (int i = 0; i < g_minigame_count; i++)
        if (g_minigame_players[i] == g_my_id)
            return 1;
    return 0;
}

static void PrintSystemMessage(const char* msg) {
    for (int i = 0; i < _countof(SYS_MSG_TABLE); i++) {
        if (strcmp(msg, SYS_MSG_TABLE[i].key) == 0) {
            SetCol(SYS_MSG_TABLE[i].color);
            printf("%s\n", SYS_MSG_TABLE[i].text);
            SetCol(COL_DEFAULT);
            return;
        }
    }

    // fallback
    SetCol(COL_DIM);
    printf("%s\n", msg);
    SetCol(COL_DEFAULT);
}

// ---------------- main ----------------

int main() {
    WSADATA wsa;

    g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup 실패\n");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) err_quit("socket()");

    struct sockaddr_in srv;
    srv.sin_family = AF_INET;
    srv.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &srv.sin_addr);

    if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) {
        err_quit("connect()");
    }

    cls();
    SetCol(COL_DEFAULT);
    DrawBanner();
    printf("서버에 접속했습니다.\n\n");

    int last_positions[MAX_PLAYERS] = { 0 };

    while (1) {
        // ===== 연타 미니게임 입력 처리 (recv와 병행) =====
        if (g_mashing) {

            if (GetTickCount() > g_mash_end_time) {
                g_mashing = 0;
            }
            else if (_kbhit()) {
                int ch = _getch();
                if (ch == ' ' || ch == '\r') {
                    GamePacket inp = { 0 };
                    inp.type = PKT_MINIGAME_INPUT;
                    send(sock, (char*)&inp, sizeof(inp), 0);
                }
            }
        }

        GamePacket pkt;
        int ret = recv(sock, (char*)&pkt, sizeof(pkt), 0);
        if (ret <= 0) {
            printf("서버와 연결 종료\n");
            break;
        }

        int ch;
        switch (pkt.type) {

        case PKT_ASSIGN_ID:
            g_my_id = pkt.player_id;
            SetCol(COL_ACCENT);
            printf("당신은 %dP입니다.\n\n", g_my_id + 1);
            SetCol(COL_DEFAULT);
            break;

        case PKT_MAP_REQ: {
            char buf[64];
            int size;

            while (1) {
                printf("[맵 설정] 맵 크기 입력 (50 ~ 200): ");

                // 문자열로 한 줄 입력
                if (!fgets(buf, sizeof(buf), stdin)) {
                    continue;
                }

                // 개행 제거
                buf[strcspn(buf, "\n")] = 0;

                // 숫자로 변환
                char* end;
                size = (int)strtol(buf, &end, 10);

                // 숫자가 아닌 입력 처리
                if (end == buf || *end != '\0') {
                    printf("숫자만 입력해 주세요.\n\n");
                    continue;
                }

                // 범위 검사
                if (size < 50 || size > 200) {
                    printf("50 ~ 200 사이의 값을 입력해 주세요.\n\n");
                    continue;
                }

                break;
            }

            GamePacket res = { 0 };
            res.type = PKT_MAP_REQ;
            res.map_size = size;
            send(sock, (char*)&res, sizeof(res), 0);
            break;
        }

        case PKT_ORDER_REQ:
        {
            if (g_waiting_order_input)
                break;

            g_waiting_order_input = 1;

            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();

            if (pkt.message[0] != '\0') {
                // 서버가 보낸 메시지가 있으면 그것만 사용
                PrintSystemMessage(pkt.message);
            }
            else {
                // 서버 메시지가 없을 때만 기본 문구 출력
                printf("스페이스바 또는 Enter를 눌러 진행\n");
            }

            int ch;
            do {
                ch = _getch();
            } while (ch != ' ' && ch != '\r');

            GamePacket ord = { 0 };
            ord.type = PKT_ORDER_REQ;
            send(sock, (char*)&ord, sizeof(ord), 0);

            break;
        }

        case PKT_ORDER_RESULT:
        {
            // 시스템 메시지 (동점 안내)
            if (pkt.player_id == -1) {
                cls();
                SetCol(COL_DEFAULT);
                DrawBanner();

                printf("[안내]\n%s\n\n", pkt.message);

                // 재굴림 단계 진입
                g_rerolling = 1;
                break;
            }

            // 플레이어 주사위 결과 수신
            if (pkt.player_id >= 0 && pkt.player_id < MAX_PLAYERS) {
                g_order_rolls[pkt.player_id] = pkt.dice1;

                // 내 결과면 입력 대기 해제
                if (pkt.player_id == g_my_id) {
                    g_waiting_order_input = 0;
                }
            }

            // 화면 출력
            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();
            printf("[순서 주사위 결과]\n\n");

            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (g_order_rolls[p] > 0)
                    printf("P%d : %d\n", p + 1, g_order_rolls[p]);
                else
                    printf("P%d : (동기화 대상)\n", p + 1);
            }

            printf("\n");

            // 모든 값 확정되었으면 마무리
            if (IsOrderComplete()) {
                g_rerolling = 0;  // 재굴림 종료

                printf("---------------------------------\n");
                printf("순서가 확정되었습니다.\n\n");

                DrawFinalTurnOrder();

                printf("잠시 후 게임이 시작됩니다...\n");
                printf("---------------------------------\n");

                Sleep(2000);
            }

            break;
        }

        case PKT_GAME_START:
            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();
            DrawFinalTurnOrder();
            DrawTrack(g_map_size, last_positions);
            PrintPositions(last_positions);

            SetCol(COL_DIM);
            printf("---------------------------------\n");
            printf(" SYSTEM BOOTING...\n");
            printf(" 시스템 초기화 중...\n\n");

            SetCol(COL_SYSTEM);
            printf(" GAME PROTOCOL START\n");
            printf(" 게임을 시작합니다\n");

            SetCol(COL_DIM);
            printf("---------------------------------\n");

            SetCol(COL_DEFAULT);
            Sleep(1500);
            g_game_started = 1;
            g_just_started = 1;
            break;

        case PKT_UPDATE:
            if (pkt.map_size > 0) g_map_size = pkt.map_size;
            memcpy(last_positions, pkt.positions, sizeof(last_positions));

            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();

            if (pkt.dice1 == 0 && pkt.dice2 == 0) {
                DrawTrack(g_map_size, last_positions);
                PrintPositions(last_positions);
                break;
            }

            // 주사위 결과
            printf("[ EXECUTION LOG ]\n\n");
            printf(" DICE   : %d + %d\n", pkt.dice1, pkt.dice2);
            SetCol(COL_ACCENT);
            printf(" STATUS : %s\n", pkt.jokbo);
            SetCol(COL_DEFAULT);

            // ---------- EVENT ----------
            if (pkt.message[0] == '\0') {
                SetCol(COL_DIM);
                printf(" EVENT  : NONE\n\n");
            }
            else {
                if (strstr(pkt.message, "ACCELERATION") ||
                    strstr(pkt.message, "OPTIMIZATION") ||
                    strstr(pkt.message, "STABILIZATION")) {
                    SetCol(COL_SYSTEM);   // 버프
                }
                else if (strstr(pkt.message, "DELAY") ||
                    strstr(pkt.message, "ERROR") ||
                    strstr(pkt.message, "LOSS")) {
                    SetCol(COL_WARN);     // 디버프
                }
                else {
                    SetCol(COL_DIM);      // 중립
                }

                printf(" EVENT  : %s\n\n", pkt.message);
            }

            SetCol(COL_DEFAULT);


            DrawTrack(g_map_size, last_positions);
            PrintPositions(last_positions);

            g_showing_result = 1;   // 결과 표시 중
            break;

        case PKT_YOUR_TURN:
            FlushKeyBuffer();

            if (pkt.map_size > 0) g_map_size = pkt.map_size;
            memcpy(last_positions, pkt.positions, sizeof(last_positions));
            memcpy(g_turn_order, pkt.turn_order, sizeof(g_turn_order));

            // 직전에 결과 화면이 떠있으면 그대로 위에 턴 문구만 이어 붙이기
            if (g_showing_result) {
                printf("=================================\n");
                SetCol(COL_ACCENT);
                printf(" >>> CONTROL GRANTED <<<\n");
                SetCol(COL_DEFAULT);
                printf(" 당신의 턴입니다\n\n");
                SetCol(COL_SYSTEM);
                printf(" [ SPACE ] EXECUTE ROLL\n");
                SetCol(COL_DEFAULT);
                printf(" SPACE를 입력하여 진행하십시오\n");
                printf("=================================\n");
            }
            else {
                cls();
                SetCol(COL_DEFAULT);
                DrawBanner();
                DrawTrack(g_map_size, last_positions);
                PrintPositions(last_positions);

                printf("=================================\n");
                SetCol(COL_ACCENT);
                printf(" >>> CONTROL GRANTED <<<\n");
                SetCol(COL_DEFAULT);
                printf(" 당신의 턴입니다\n\n");
                SetCol(COL_SYSTEM);
                printf(" [ SPACE ] EXECUTE ROLL\n");
                SetCol(COL_DEFAULT);
                printf(" SPACE를 입력하여 진행하십시오\n");
                printf("=================================\n");
            }

            // 여기서 결과 화면 잠금 해제 (다음부터 정상 렌더링)
            g_showing_result = 0;

            while ((ch = _getch()) != ' ' && ch != '\r');

            GamePacket go = { 0 };
            go.type = PKT_YOUR_TURN;
            send(sock, (char*)&go, sizeof(go), 0);
            break;

        case PKT_WAIT:
            FlushKeyBuffer();

            if (pkt.map_size > 0) g_map_size = pkt.map_size;
            memcpy(last_positions, pkt.positions, sizeof(last_positions));
            memcpy(g_turn_order, pkt.turn_order, sizeof(g_turn_order));

            // 방금 결과 화면이 떠있으면(UPDATE 직후) 덮어쓰지 말고 안내만 추가
            if (g_showing_result) {
                SetCol(COL_DIM);
                printf("---------------------------------\n");
                printf(" PLAYER P%d EXECUTING...\n", pkt.player_id + 1);
                printf(" %dP가 행동 중입니다...\n", pkt.player_id + 1);
                printf("---------------------------------\n");
                SetCol(COL_DEFAULT);

                g_just_started = 0;
                break;
            }

            // 결과 화면이 아니면 정상적으로 화면 렌더
            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();
            DrawTrack(g_map_size, last_positions);
            PrintPositions(last_positions);

            SetCol(COL_DIM);
            printf("---------------------------------\n");

            if (g_just_started) {
                printf(" SYSTEM SYNC IN PROGRESS...\n");
                printf(" 초기 동기화 중...\n");
            }
            else {
                printf(" PLAYER P%d EXECUTING...\n", pkt.player_id + 1);
                printf(" %dP가 행동 중입니다...\n", pkt.player_id + 1);
            }

            printf("---------------------------------\n");
            SetCol(COL_DEFAULT);

            g_just_started = 0;
            break;

            // ---------- 미니게임 ----------
        case PKT_MINIGAME_START:
        {
            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();

            g_in_minigame = 1;
            g_minigame_phase = pkt.minigame_phase;
            g_minigame_type = pkt.minigame_type;
            g_minigame_count = pkt.minigame_count;
            memcpy(g_minigame_players,
                pkt.minigame_players,
                sizeof(int) * pkt.minigame_count);

            SetCol(COL_SYSTEM);
            printf("=================================\n");
            printf("  SYSTEM RACE CONDITION DETECTED\n");
            printf("=================================\n\n");
            SetCol(COL_DEFAULT);

            printf("미니게임 참가자:\n");
            for (int i = 0; i < g_minigame_count; i++) {
                int pid = g_minigame_players[i];
                printf(" - P%d %s\n",
                    pid + 1,
                    pid == g_my_id ? "(YOU)" : "");
            }
            printf("\n");

            // ---------- READY ----------
            if (pkt.minigame_phase == 0) {
                if (IsMinigameParticipant()) {
                    SetCol(COL_SYSTEM);
                    printf(">>> READY <<<\n");

                    if (g_minigame_type == MG_MASH)
                        printf("GO 신호 후 SPACE를 연타하세요\n");
                    else
                        printf("GO 신호에 맞춰 SPACE를 입력하세요\n");

                    SetCol(COL_DEFAULT);
                }
                else {
                    SetCol(COL_DIM);
                    printf("[ 관전 중... ]\n");
                    SetCol(COL_DEFAULT);
                }
                break;
            }

            // ---------- GO ----------
            if (pkt.minigame_phase == 1) {

                if (!IsMinigameParticipant()) {
                    SetCol(COL_DIM);
                    printf("[ 관전 중... ]\n");
                    SetCol(COL_DEFAULT);
                    break;
                }

                SetCol(COL_ACCENT);
                printf("\n>>> GO! <<<\n");
                SetCol(COL_DEFAULT);

                FlushKeyBuffer();

                // ===== 반응속도 =====
                if (g_minigame_type == MG_REACTION) {

                    int ch;
                    while ((ch = _getch()) != ' ' && ch != '\r');

                    GamePacket inp = { 0 };
                    inp.type = PKT_MINIGAME_INPUT;
                    send(sock, (char*)&inp, sizeof(inp), 0);
                }

                // ===== 연타 =====
                else if (g_minigame_type == MG_MASH) {
                    g_mashing = 1;
                    g_mash_end_time = GetTickCount() + 3000;
                }
            }

            break;
        }

        case PKT_MINIGAME_UPDATE:
        {
            if (pkt.minigame_type != MG_MASH)
                break;

            cls();
            DrawBanner();

            SetCol(COL_SYSTEM);
            printf("=== MASHING... ===\n\n");
            SetCol(COL_DEFAULT);

            for (int i = 0; i < pkt.minigame_count; i++) {
                int pid = pkt.minigame_players[i];
                int score = pkt.positions[pid];

                if (pid == g_my_id)
                    SetCol(COL_ACCENT);
                else
                    SetCol(COL_DIM);

                printf("P%d : %d\n", pid + 1, score);
                SetCol(COL_DEFAULT);
            }
            break;
        }

        case PKT_MINIGAME_RESULT:
        {
            cls();
            DrawBanner();

            SetCol(COL_SYSTEM);
            printf("=================================\n");
            printf("  MINIGAME RESULT\n");
            printf("=================================\n\n");
            SetCol(COL_DEFAULT);

            if (pkt.player_id == g_my_id) {
                SetCol(COL_ACCENT);
                printf(">>> YOU WON <<<\n\n");
                PrintSystemMessage(pkt.message);
            }
            else if (IsMinigameParticipant()) {
                SetCol(COL_WARN);
                printf(">>> YOU LOST <<<\n");
                printf("Winner : P%d\n\n", pkt.player_id + 1);
                PrintSystemMessage(pkt.message);
            }
            else {
                SetCol(COL_DIM);
                printf("Winner : P%d (관전 중)\n", pkt.player_id + 1);
            }

            SetCol(COL_DEFAULT);

            if (g_minigame_type == MG_REACTION)
                printf("\nReaction Time : %d ms\n", pkt.value);
            else if (g_minigame_type == MG_MASH)
                printf("\nMash Count : %d\n", pkt.value);

            Sleep(4000);
            g_mashing = 0;
            g_in_minigame = 0;
            break;
        }

        case PKT_GAME_OVER:
            if (pkt.map_size > 0)
                g_map_size = pkt.map_size;

            cls();
            SetCol(COL_DEFAULT);
            DrawBanner();
            SetCol(COL_SYSTEM);
            printf("=================================\n");
            printf(" SYSTEM OVERRIDE COMPLETE\n");
            printf(" 시스템 장악 완료\n");
            printf("=================================\n\n");
            SetCol(COL_ACCENT);
            printf(" WINNER : P%d\n", pkt.player_id + 1);
            printf(" 최종 승자 : P%d\n\n", pkt.player_id + 1);
            SetCol(COL_DEFAULT);

            DrawTrack(g_map_size, pkt.positions);
            PrintPositions(pkt.positions);

            printf("아무 키나 누르면 종료합니다.\n");
            while ((ch = _getch()) != ' ' && ch != '\r');
            closesocket(sock);
            WSACleanup();
            return 0;

        default:
            printf("알 수 없는 패킷 수신: %d\n", pkt.type);
            break;
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
