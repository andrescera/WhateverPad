#include "radio.h"

radio_action radio_decide(radio_input in)
{
    if (in.trying) return RADIO_NOTHING;
    if (!in.open) return in.pair_requested && in.vpad_ok ? RADIO_OPEN : RADIO_NOTHING;
    if (in.busy || in.pair_requested) return RADIO_NOTHING;
    return in.now - in.last_busy >= RADIO_LINGER_MS ? RADIO_CLOSE : RADIO_NOTHING;
}

radio_link_action radio_link_check(long now, long last_acl, long last_check, int unanswered)
{
    if (now - last_acl < RADIO_LINK_SILENT_MS || now - last_check < RADIO_LINK_SILENT_MS)
        return RADIO_LINK_NOTHING;
    return unanswered >= RADIO_LINK_CHECK_TRIES ? RADIO_LINK_GIVE_UP : RADIO_LINK_CHECK;
}

int radio_rssi_reply_gone(const unsigned char *ev, int len, int outstanding, unsigned handle)
{
    return outstanding && len >= 8 && ev[0] == 0x0E && ev[1] + 2 == len &&
           ev[3] == 0x05 && ev[4] == 0x14 && ev[5] == 0x02 &&
           ((unsigned)ev[6] | (unsigned)ev[7] << 8) == handle;
}

int radio_link_unanswered_after(int unanswered, int status)
{
    return status == 0 ? 0 : unanswered + 1;
}

int radio_goodbye(const radio_goodbye_in *in, radio_cmd *out, int max)
{
    int i, n = 0;

    if (in->dead) return 0;
    for (i = 0; i < in->nhandles && n < max; i++)
        out[n++] = (radio_cmd){ 0x0406,
            { (unsigned char)in->handles[i], (unsigned char)(in->handles[i] >> 8), 0x13 }, 3 };
    if (in->inquiring && n < max)
        out[n++] = (radio_cmd){ 0x0402, { 0 }, 0 };
    if (in->le_scanning && n < max)
        out[n++] = (radio_cmd){ 0x200C, { 0, 0 }, 2 };
    if (in->le_create_pending && n < max)
        out[n++] = (radio_cmd){ 0x200E, { 0 }, 0 };
    if (in->ps_saved && n < max)
        out[n++] = (radio_cmd){ 0x0C1C,
            { (unsigned char)in->ps_orig_iv, (unsigned char)(in->ps_orig_iv >> 8),
              (unsigned char)in->ps_orig_win, (unsigned char)(in->ps_orig_win >> 8) }, 4 };
    if (in->mask_conn && n < max)
        out[n++] = (radio_cmd){ 0x0C01, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F }, 8 };
    return n;
}
