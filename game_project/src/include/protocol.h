#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <sys/types.h>

#define LOBBY_FIFO "/tmp/race30_lobby.fifo"

#define MAX_NAME 32
#define MAX_PATH 128
#define MAX_TEXT 256

typedef enum {
    MSG_JOIN = 1,
    MSG_MOVE = 2,
    MSG_QUIT = 3
} MsgType;

typedef struct {
    MsgType type;
    pid_t   pid;
    int     move;                 // used for MOVE
    char    name[MAX_NAME];       // used for JOIN
    char    reply_fifo[MAX_PATH]; // client reply fifo path
} ClientMsg;

typedef struct {
    int  ok;                      // 1 success, 0 error
    char text[MAX_TEXT];          // server message
    char assigned_req_fifo[MAX_PATH]; // server assigns client-specific request fifo
} ServerMsg;

#endif
