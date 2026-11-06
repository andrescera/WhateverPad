#include "radio.h"

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
