#ifndef WHATEVERPAD_RADIO_H
#define WHATEVERPAD_RADIO_H

typedef enum { RADIO_NOTHING, RADIO_OPEN, RADIO_CLOSE } radio_action;
#define RADIO_LINGER_MS 30000L

typedef struct {
    int vpad_ok;
    int open;
    int trying;
    int pair_requested;
    int busy;
    long now, last_busy;
} radio_input;

radio_action radio_decide(radio_input in);

#define RADIO_LINK_SILENT_MS 20000L
#define RADIO_LINK_CHECK_TRIES 3
typedef enum { RADIO_LINK_NOTHING, RADIO_LINK_CHECK, RADIO_LINK_GIVE_UP } radio_link_action;

radio_link_action radio_link_check(long now, long last_acl, long last_check, int unanswered);
int radio_rssi_reply_gone(const unsigned char *ev, int len, int outstanding, unsigned handle);
int radio_link_unanswered_after(int unanswered, int status);

typedef struct {
    unsigned op;
    unsigned char p[8];
    int len;
} radio_cmd;

typedef struct {
    int dead;
    int nhandles;
    unsigned handles[4];
    int inquiring;
    int le_scanning;
    int le_create_pending;
    int ps_saved;
    unsigned ps_orig_iv, ps_orig_win;
    int mask_conn;
} radio_goodbye_in;

#define RADIO_CMD_MAX 9

int radio_goodbye(const radio_goodbye_in *in, radio_cmd *out, int max);

#endif
