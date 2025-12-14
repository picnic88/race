#pragma once
#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include "game_protocol.h"

// 주사위 2개 굴리고, 족보 문자열 채움 (jokbo_out는 최소 50바이트 권장)
void RollTwoDice(int* d1, int* d2, char* jokbo_out);

// 맵 이벤트 처리: pos를 직접 수정할 수 있음. 이벤트 설명 문자열 반환(정적 문자열)
const char* CheckMapEvent(int* pos, int map_size);

// order_rolls(각 플레이어의 값)를 내림차순으로 정렬해서 turn_order에 플레이어 인덱스를 채움
// 예: turn_order[0] = 첫 턴 플레이어 인덱스
void SortTurnOrder(int* turn_order, const int* order_rolls, int n);

#endif
