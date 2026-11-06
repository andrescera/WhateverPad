#ifndef WHATEVERPAD_RADIO_H
#define WHATEVERPAD_RADIO_H

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
