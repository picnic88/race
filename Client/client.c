// client.c
#define _CRT_SECURE_NO_WARNINGS
#include "game_protocol.h"
#include <conio.h>
#include <windows.h>

static int g_my_id = -1;     // 서버가 배정해주는 내 플레이어 번호(0~3)
static int g_map_size = 30;  // 서버에서 받은 맵 크기

static void cls() { system("cls"); }

static void DrawBanner() {
    printf("=========================================\n");
    printf("            D I C E   R A C E            \n");
    printf("=========================================\n\n");
}

static void DrawTrack(int map_size, int positions[MAX_PLAYERS]) {
    if (map_size < 5) map_size = 5;
    if (map_size > 80) map_size = 80; // 콘솔 폭 제한

    printf("[TRACK] 0");
    for (int i = 0; i < map_size - 1; i++) printf("-");
    printf("|%d\n", map_size);

    for (int p = 0; p < MAX_PLAYERS; p++) {
        int pos = positions[p];
        if (pos < 0) pos = 0;
        if (pos > map_size) pos = map_size;

        printf("P%d %s ", p + 1, (p == g_my_id ? "(YOU)" : "     "));
        printf("|");
        for (int x = 0; x < map_size; x++) {
            if (x == pos) printf(">");
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

int main() {
    WSADATA wsa;
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
    DrawBanner();
    printf("서버에 접속했습니다.\n\n");

    int last_positions[MAX_PLAYERS] = { 0 };

    while (1) {
        GamePacket pkt;
        int ret = recv(sock, (char*)&pkt, sizeof(pkt), 0);
        if (ret <= 0) {
            printf("서버와 연결 종료\n");
            break;
        }

        switch (pkt.type) {

        case PKT_ASSIGN_ID:
            g_my_id = pkt.player_id;
            printf("당신은 %dP입니다!\n\n", g_my_id + 1);
            break;

        case PKT_MAP_REQ: {
            int size;
            printf("[맵 설정] 맵 크기 입력 (추천 20~40): ");
            scanf("%d", &size);

            GamePacket res = { 0 };
            res.type = PKT_MAP_REQ;
            res.map_size = size;
            send(sock, (char*)&res, sizeof(res), 0);
            break;
        }

        case PKT_ORDER_REQ:
            cls();
            DrawBanner();
            printf("[순서 결정] Enter 키를 누르면 순서 주사위를 굴립니다...\n");
            _getch();

            GamePacket ord = { 0 };
            ord.type = PKT_ORDER_REQ;
            send(sock, (char*)&ord, sizeof(ord), 0);
            break;

        case PKT_YOUR_TURN:
            if (pkt.map_size > 0) {
                g_map_size = pkt.map_size;
                memcpy(last_positions, pkt.positions, sizeof(last_positions));
            }

            printf("\n=================================\n");
            printf(">>> 당신의 턴입니다! <<<\n");
            printf("Enter 키를 눌러 주사위를 굴리세요...\n");
            printf("=================================\n");

            _getch();

            GamePacket go = { 0 };
            go.type = PKT_YOUR_TURN;
            send(sock, (char*)&go, sizeof(go), 0);
            break;


        case PKT_WAIT:
            if (pkt.player_id >= 0 && pkt.player_id < MAX_PLAYERS) {
                printf("\n---------------------------------\n");
                printf("P%d의 턴입니다. 대기 중...\n", pkt.player_id + 1);
                printf("---------------------------------\n");
            }
            else {
                printf("\n---------------------------------\n");
                printf("다른 플레이어의 턴입니다. 대기 중...\n");
                printf("---------------------------------\n");
            }
            break;

        case PKT_UPDATE:
            if (pkt.map_size > 0) g_map_size = pkt.map_size;
            memcpy(last_positions, pkt.positions, sizeof(last_positions));

            cls();
            DrawBanner();

            printf("P%d 결과\n", pkt.player_id + 1);
            printf("주사위: %d + %d\n", pkt.dice1, pkt.dice2);
            printf("족보: %s\n", pkt.jokbo);
            printf("이벤트: %s\n\n", pkt.message);

            DrawTrack(g_map_size, pkt.positions);
            PrintPositions(pkt.positions);
            break;

        case PKT_GAME_OVER:
            if (pkt.map_size > 0) g_map_size = pkt.map_size;

            cls();
            DrawBanner();
            printf("게임 종료!\n");
            printf("승자: P%d\n\n", pkt.player_id + 1);

            DrawTrack(g_map_size, pkt.positions);
            PrintPositions(pkt.positions);

            printf("아무 키나 누르면 종료합니다.\n");
            _getch();
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
