#ifndef GAME_PROTOCOL_H
#define GAME_PROTOCOL_H

#define _CRT_SECURE_NO_WARNINGS 

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 9000          
#define BUFSIZE 1024       
#define MAX_PLAYERS 4      


typedef enum {
    PKT_WAIT,          
    PKT_MAP_REQ,       
    PKT_ORDER_REQ,     
    PKT_ORDER_RESULT,   
    PKT_YOUR_TURN,      
    PKT_UPDATE,         
    PKT_GAME_OVER      
} PacketType;


typedef struct {
    int type;                   
    int player_id;              
    int map_size;               
    int dice1;                  
    int dice2;                  
    char jokbo[50];             
    int positions[MAX_PLAYERS]; 
    int turn_order[MAX_PLAYERS];
    char message[256];         
} GamePacket;

/
static void err_display(const char* msg) {
    LPVOID lpMsgBuf;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, WSAGetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (char*)&lpMsgBuf, 0, NULL);
    printf("[%s] %s\n", msg, (char*)lpMsgBuf);
    LocalFree(lpMsgBuf);
}


static void err_quit(const char* msg) {
    LPVOID lpMsgBuf;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, WSAGetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (char*)&lpMsgBuf, 0, NULL);
    MessageBoxA(NULL, (const char*)lpMsgBuf, msg, MB_ICONERROR);
    LocalFree(lpMsgBuf);
    exit(1);
}

#endif