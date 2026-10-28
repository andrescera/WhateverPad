#include "event_reads.h"
#include "host_int.h"

int event_reads_wanted(const host_t *h, long now, int idle_reads)
{
    int i;

    if (h->yield_at && now - h->yield_at < T_YIELD) return 0;
    if (now < h->pair_until || h->inquiring || h->pair_pending_s ||
        h->repage_at || h->defer_at ||
        now < h->full_reads_until) return HOST_EV_READS;
    for (i = 0; i < HOST_MAX_PADS; i++)
        if (h->links[i].used && (!h->links[i].ready || h->links[i].disconnecting))
            return HOST_EV_READS;
    return idle_reads;
}
