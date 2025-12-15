#include "game_logic.h"
#include <stdio.h>

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void RollTwoDice(int* d1, int* d2, char* jokbo_out) {
    if (!d1 || !d2 || !jokbo_out) return;

    *d1 = (rand() % 6) + 1;
    *d2 = (rand() % 6) + 1;

    // 간단 족보(원하는대로 늘려도 됨)
    if (*d1 == *d2) {   // 더블
        sprintf(jokbo_out, "[DOUBLE_%d]", *d1);
        return;
    }

    int sum = *d1 + *d2;
    if (sum == 11)
        strcpy(jokbo_out, "[ELEVATE_11]");
    else if (sum == 2)
        strcpy(jokbo_out, "[FAULT_02]");
    else if (sum == 12)
        strcpy(jokbo_out, "[OVERDRIVE]");
    else
        sprintf(jokbo_out, "[NORMAL_%02d]", sum);
}

void SortTurnOrder(int* turn_order, const int* order_rolls, int n) {
    if (!turn_order || !order_rolls || n <= 0) return;

    // 초기: 0..n-1
    for (int i = 0; i < n; i++) turn_order[i] = i;

    // 내림차순 정렬 (값이 큰 사람이 먼저)
    // 동점이면 "플레이어 인덱스 작은 쪽" 우선(결정적)
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int a = turn_order[i];
            int b = turn_order[j];

            if (order_rolls[b] > order_rolls[a] ||
                (order_rolls[b] == order_rolls[a] && b < a)) {
                int tmp = turn_order[i];
                turn_order[i] = turn_order[j];
                turn_order[j] = tmp;
            }
        }
    }
}

// 이벤트는 "칸 번호" 기반으로 간단히
// pos는 0..map_size 범위를 벗어나지 않도록 보정
// 반환 문자열은 서버에서 sprintf로 message에 붙여 쓰므로 짧게 유지
const char* CheckMapEvent(int* pos, int map_size) {
    static const char* EVT_NONE = "";
    static const char* EVT_BOOST = "[ EVENT ] SYSTEM ACCELERATION +2 | 시스템 가속";
    static const char* EVT_TRAP = "[ EVENT ] COMPUTE DELAY -2 | 연산 지연";
    static const char* EVT_WARP = "[ EVENT ] PROXY OPTIMIZATION +3 ~ +5 | 프록시 최적화";
    static const char* EVT_SLIP = "[ EVENT ] CRITICAL ERROR -2 ~ -4 | 치명적 오류";
    static const char* EVT_LUCKY = "[ EVENT ] PACKET STABILIZATION +1 | 패킷 안정화";
    static const char* EVT_CURSE = "[ EVENT ] DATA LOSS -1 | 데이터 손실";

    if (!pos) return EVT_NONE;
    if (map_size <= 0) return EVT_NONE;

    // 승리 지점에서 이벤트로 더 움직이지 않도록(원하면 제거 가능)
    if (*pos >= map_size) {
        *pos = map_size;
        return EVT_NONE;
    }

    // 0 기반인지 1 기반인지 섞이지 않게: 현재 pos는 "칸 수"처럼 쓰고 있으니
    // 이벤트 칸은 5, 10, 15... 같은 "절대 위치"로 처리
    int p = *pos;

    // 확률형 작은 양념(역전용): 뒤쳐질수록 살짝 유리하게 하고 싶으면 여기서 조정 가능
    // 지금은 공정하게 고정 이벤트 + 약간의 랜덤 이벤트만
    // (서버 상태값을 안 쓰므로 여기서 pos만 수정)

    // 고정 이벤트 칸
    if (p == 5 || p == 17 || p == 26) {
        *pos = clamp_int(p + 2, 0, map_size);
        return EVT_BOOST;
    }
    if (p == 9 || p == 21) {
        *pos = clamp_int(p - 2, 0, map_size);
        return EVT_TRAP;
    }
    if (p == 13) {
        // 앞으로 3~5칸 순간이동
        int jump = 3 + (rand() % 3); // 3,4,5
        *pos = clamp_int(p + jump, 0, map_size);
        return EVT_WARP;
    }
    if (p == 18) {
        // 뒤로 2~4칸
        int back = 2 + (rand() % 3); // 2,3,4
        *pos = clamp_int(p - back, 0, map_size);
        return EVT_SLIP;
    }

    // 랜덤 이벤트(가끔)
    // 12% 확률로 +/-1
    int r = rand() % 100;
    if (r < 6) {
        *pos = clamp_int(p + 1, 0, map_size);
        return EVT_LUCKY;
    }
    if (r < 12) {
        *pos = clamp_int(p - 1, 0, map_size);
        return EVT_CURSE;
    }

    return EVT_NONE;
}
