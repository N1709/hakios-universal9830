/*
 * Calls, step 3 (2026-11-13, A): the call's voice on the AP. The S5123's own voice engine does not take the RTP stream
 * (the caller's packets, dport 50010/50011, arrive at the AP as ordinary data on the IMS PDN: "SBD RX ch 11"), so the
 * AP plays the part of the voice engine, as any VoIP app does:
 *   in   RTP (AMR-NB, PT 100, RFC 4867 bandwidth-efficient: our SDP answer has no octet-align) -> opencore-amr decoder
 *        -> 8 kHz -> 48 kHz stereo -> /dev/redwood-audio (the speaker driver's raw input, sys/redwood_audio.h)
 *   out  /dev/redwood-mic (48 kHz mono, docs/devices/x1slte/audio.md) -> 8 kHz -> AMR-NB 12.2 -> RTP to the SDP's c=/m=
 * opencore-amr 0.1.6 (Apache-2.0), built static for iOS: work/third_party/amr-build/out (see tools/cellular/build-ims.sh).
 *
 * (B, 2026-11-15) the payload types are the negotiated ones (Jio offers AMR as 100 or 112: with 112 every packet was
 * dropped here before -- silence one way), each call has its own RTP port (ims-call.h), DTMF goes out as RFC 4733
 * telephone-events from the send thread, and a local ringback tone plays while an outgoing call rings without early
 * media (media_tone).
 */
#include <math.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <net/if_dl.h>
#include <opencore-amrnb/interf_dec.h>
#include <opencore-amrnb/interf_enc.h>

static const int amr_bits[16] = { 95, 103, 118, 134, 148, 159, 204, 244, 39, 0, 0, 0, 0, 0, 0, 0 };

/* the jitter buffer: decoded 8 kHz samples, played out at a steady 48 kHz by media_play (the speaker driver repeats
 * its ring when nothing new comes, so bursts written as they arrived stuttered: the user, 2026-11-14 00:04) */
#define FIFO_N 8000                                         /* 1 s at 8 kHz */
static int16_t fifo[FIFO_N];
static volatile unsigned fifo_w, fifo_r;
static pthread_mutex_t fifo_mx = PTHREAD_MUTEX_INITIALIZER;

static struct {
    volatile int run;
    pthread_t play;
    unsigned underruns, concealed;
    float mic_gain;
    volatile int muted;
    volatile int route;                 /* 1 earpiece (default), 0 speakers: kept on the kernel every 0.5 s */
    int sock, spk, mic;
    struct sockaddr_in6 peer;
    pthread_t rx, tx;
    unsigned rx_pkts, rx_frames, tx_pkts, rx_bad, tx_err, mic_bytes; int tx_errno;
    int pt, dtmf_pt, lport;             /* negotiated AMR / telephone-event payload types; our RTP port */
    unsigned rx_other;                  /* packets of other payload types (CN, events): counted, not played */
} med = { .sock = -1, .spk = -1, .mic = -1, .pt = 100, .dtmf_pt = 101 };

/* DTMF digits waiting to go out (ims-call.h); the send thread turns each into RFC 4733 events */
static char dtmf_q[64];
static volatile unsigned dtmf_w, dtmf_r;
static void media_dtmf(const char *digits)
{
    for (const char *d = digits; *d; d++) {
        char c = *d;
        if (c >= 'a' && c <= 'd') c = (char)(c - 32);
        if (!((c >= '0' && c <= '9') || c == '*' || c == '#' || (c >= 'A' && c <= 'D'))) continue;
        if (dtmf_w - dtmf_r >= sizeof(dtmf_q)) break;
        dtmf_q[dtmf_w % sizeof(dtmf_q)] = c; dtmf_w++;
    }
}
static int dtmf_code(char c) { return c >= '0' && c <= '9' ? c - '0' : c == '*' ? 10 : c == '#' ? 11 : c - 'A' + 12; }

/* bit reader / writer, MSB first */
static unsigned getbits(const uint8_t *b, size_t *pos, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++, (*pos)++) v = (v << 1) | ((b[*pos >> 3] >> (7 - (*pos & 7))) & 1);
    return v;
}
static void putbits(uint8_t *b, size_t *pos, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--, (*pos)++) {
        if ((v >> i) & 1) b[*pos >> 3] |= (uint8_t)(0x80 >> (*pos & 7));
    }
}


/* (A, 2026-11-14, quality) a windowed-sinc low-pass at 3.6 kHz for 48 kHz: the anti-alias filter before 48 -> 8 kHz
 * (was the mean of 6) and the interpolation filter after 8 -> 48 kHz (was linear), 72 taps (12 per phase), Hamming */
#define FIR_N 72
static float fir[FIR_N];
static void fir_init(void)
{
    double sum = 0;
    for (int i = 0; i < FIR_N; i++) {
        double x = i - (FIR_N - 1) / 2.0, fc = 3600.0 / 48000.0;
        double h = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        h *= 0.54 - 0.46 * cos(2 * M_PI * i / (FIR_N - 1));
        fir[i] = (float)h; sum += h;
    }
    for (int i = 0; i < FIR_N; i++) fir[i] = (float)(fir[i] / sum);
}
static int16_t sat16(float v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lrintf(v); }
static void rt_thread(int pri)
{   /* real-time priority: the audio threads may not wait behind the rest of the phone */
    struct sched_param sp = { .sched_priority = pri };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
}

static void *media_rx(void *arg)
{
    (void)arg;
    void *dec = Decoder_Interface_init();
    uint8_t pkt[1500];
    int16_t pcm[160];
    int have_seq = 0; uint16_t next_seq = 0;
    rt_thread(46);
    while (med.run) {
        struct pollfd p = { .fd = med.sock, .events = POLLIN };
        if (poll(&p, 1, 200) <= 0) continue;
        ssize_t n = recv(med.sock, pkt, sizeof(pkt), 0);
        if (n < 13 || (pkt[0] >> 6) != 2) { med.rx_bad++; continue; }
        if ((pkt[1] & 0x7f) != med.pt) { med.rx_other++; continue; }   /* (telephone-event, comfort noise) */
        med.rx_pkts++;
        uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
        /* (A, 2026-11-20) a new stream: an outgoing call's early media (the network's ringback, 183) is followed at
         * the answer by the other phone's voice with a new SSRC and its own sequence numbers. When they started
         * lower, every voice packet counted as "late" and was dropped: no voice on outgoing calls (in 410 packets /
         * 151 frames), incoming fine. A new SSRC, or a jump far back, starts the sequence again. */
        static uint32_t rx_ssrc;
        uint32_t ssrc = ((uint32_t)pkt[8] << 24) | ((uint32_t)pkt[9] << 16) | ((uint32_t)pkt[10] << 8) | pkt[11];
        if (have_seq && (ssrc != rx_ssrc || (int16_t)(seq - next_seq) < -100)) have_seq = 0;
        rx_ssrc = ssrc;
        if (have_seq) {
            int16_t gap = (int16_t)(seq - next_seq);
            if (gap < 0) continue;                          /* (late or repeated: already concealed) */
            /* lost packets (1..5): the decoder conceals them (a bad-frame call per missing 20 ms), not silence */
            for (int g = 0; g < gap && g < 5; g++) {
                uint8_t nd[1] = { (uint8_t)(15 << 3) };
                Decoder_Interface_Decode(dec, nd, pcm, 1);
                med.concealed++;
                pthread_mutex_lock(&fifo_mx);
                for (int s2 = 0; s2 < 160; s2++) fifo[(fifo_w + (unsigned)s2) % FIFO_N] = pcm[s2];
                fifo_w += 160;
                pthread_mutex_unlock(&fifo_mx);
            }
        }
        have_seq = 1; next_seq = (uint16_t)(seq + 1);
        size_t h = 12 + 4 * (size_t)(pkt[0] & 0x0f);
        if (pkt[0] & 0x10) {                                /* header extension */
            if ((size_t)n < h + 4) continue;
            h += 4 + 4 * (size_t)((pkt[h + 2] << 8) | pkt[h + 3]);
        }
        if ((size_t)n <= h) continue;
        const uint8_t *pl = pkt + h;
        size_t plbits = ((size_t)n - h) * 8, pos = 4;      /* CMR */
        int ft[16], q[16], nf = 0, f;
        do {
            if (pos + 6 > plbits) break;
            f = (int)getbits(pl, &pos, 1); ft[nf] = (int)getbits(pl, &pos, 4); q[nf] = (int)getbits(pl, &pos, 1); nf++;
        } while (f && nf < 16);
        for (int i = 0; i < nf; i++) {
            uint8_t st[40] = { 0 };
            int bits = amr_bits[ft[i]];
            if (pos + (size_t)bits > plbits) break;
            st[0] = (uint8_t)((ft[i] << 3) | (q[i] << 2));
            size_t w = 8;
            for (int k = 0; k < bits; k++) putbits(st, &w, getbits(pl, &pos, 1), 1);
            Decoder_Interface_Decode(dec, st, pcm, ft[i] == 15 || !q[i]);
            med.rx_frames++;
            pthread_mutex_lock(&fifo_mx);
            if (fifo_w - fifo_r > FIFO_N - 160) fifo_r = fifo_w - 1600;    /* (far behind: keep the last 200 ms) */
            for (int s = 0; s < 160; s++) fifo[(fifo_w + (unsigned)s) % FIFO_N] = pcm[s];
            fifo_w += 160;
            pthread_mutex_unlock(&fifo_mx);
        }
    }
    Decoder_Interface_exit(dec);
    return NULL;
}


/* every 10 ms: 80 samples at 8 kHz from the FIFO (silence when it is empty), up to 48 kHz stereo, to the speaker; the
 * clock is the AP's, kept against the elapsed time so it never drifts. Playout starts once 60 ms are buffered. */
static void *media_play(void *arg)
{
    (void)arg;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long played = 0;                               /* 8 kHz samples */
    int primed = 0;
    int16_t pcm[80], out[80 * 6 * 2];
    float hist[FIR_N / 6] = { 0 };                          /* the last 12 input samples (8 kHz) */
    rt_thread(47);
    while (med.run) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        unsigned long due = (unsigned long)((now.tv_sec - t0.tv_sec) * 8000L + (now.tv_nsec - t0.tv_nsec) / 125000L);
        if (due < played + 80) { usleep(2000); continue; }
        pthread_mutex_lock(&fifo_mx);
        unsigned have = fifo_w - fifo_r;
        if (!primed && have >= 480) primed = 1;
        if (primed && have >= 80) {
            for (int s = 0; s < 80; s++) pcm[s] = fifo[(fifo_r + (unsigned)s) % FIFO_N];
            fifo_r += 80;
        } else {
            if (primed) { med.underruns++; primed = 0; }    /* refill 60 ms before going on */
            memset(pcm, 0, sizeof(pcm));
        }
        pthread_mutex_unlock(&fifo_mx);
        for (int s = 0; s < 80; s++) {                      /* 8 -> 48 kHz: polyphase FIR, gain 6 */
            memmove(hist + 1, hist, sizeof(hist) - sizeof(hist[0])); hist[0] = pcm[s];
            for (int k = 0; k < 6; k++) {
                float a = 0;
                for (int j = 0; j < FIR_N / 6; j++) a += hist[j] * fir[k + 6 * j];
                out[2 * (6 * s + k)] = out[2 * (6 * s + k) + 1] = sat16(6 * a);
            }
        }
        if (med.spk >= 0) (void)!write(med.spk, out, sizeof(out));
        played += 80;
        if (played % 4000 == 0) {                           /* every 0.5 s: our route, whatever iOS's own sounds set */
            int r = med.route;
            sysctlbyname("kern.redwood.snd_route", NULL, NULL, &r, sizeof(r));
        }
        if (played % 40000 == 0)                            /* every 5 s */
            {printf("call: media (AP): in %u pkts, out %u pkts (%u failed: %s), mic %u bytes, %u underruns, %u concealed, mic gain %.1f, buffered %u ms\n",
                med.rx_pkts, med.tx_pkts, med.tx_err, med.tx_err ? strerror(med.tx_errno) : "-", med.mic_bytes, med.underruns,
                med.concealed, med.mic_gain, (fifo_w - fifo_r) / 8); fflush(stdout);}
        if (due > played + 800) played = due;               /* (stalled over 100 ms: skip ahead, do not burst) */
    }
    return NULL;
}

static void *media_tx(void *arg)
{
    (void)arg;
    void *enc = Encoder_Interface_init(0);
    int16_t in[960], pcm[160], prev[FIR_N] = { 0 };
    float dec8[160], gain = 2;
    size_t have = 0;
    rt_thread(45);
    uint16_t seq = (uint16_t)arc4random();
    uint32_t ts = arc4random(), ssrc = arc4random();
    int first = 1;
    unsigned long mic_try = 0;
    int ev = -1, ev_phase = 0, ev_n = 0; uint32_t ev_ts = 0;   /* the RFC 4733 event going out: 1 tone, 2 end, 3 gap */
    while (med.run) {
        /* the mic may be held by iOS's own audio for a moment (the call's audio session opens its input: "mic NOT open",
         * 2026-11-14 04:46): ask again every 200 ms; meanwhile silence goes out at 20 ms, so the stream never stops */
        if (med.mic < 0 && (mic_try++ % 10) == 0) {
            med.mic = open("/dev/redwood-mic", O_RDONLY | O_NONBLOCK);
            if (med.mic >= 0) printf("%s call: media (AP): mic open\n", ts_now());
        }
        ssize_t n = med.mic >= 0 ? read(med.mic, (uint8_t *)in + have, sizeof(in) - have) : -1;
        if (n > 0) { have += (size_t)n; med.mic_bytes += (unsigned)n; }
        if (med.mic < 0) {                                  /* no mic: send silence at 20 ms */
            usleep(20000); memset(in, 0, sizeof(in)); have = sizeof(in);
        } else if (have < sizeof(in)) {
            usleep(4000);
            continue;
        }
        have = 0;
        float peak = 0;
        for (int s = 0; s < 160; s++) {                     /* 48 -> 8 kHz: FIR low-pass, every 6th */
            float a = 0;
            for (int j = 0; j < FIR_N; j++) {
                int idx = 6 * s + 5 - j;
                a += (idx >= 0 ? in[idx] : prev[FIR_N + idx]) * fir[j];
            }
            dec8[s] = a; if (fabsf(a) > peak) peak = fabsf(a);
        }
        memcpy(prev, in + 960 - FIR_N, sizeof(prev));
        /* AGC: speech peaks toward 12000, gain 1..6, fast down, slow up; quiet frames (noise) do not raise it */
        if (peak > 400) {
            float want = 12000.0f / peak; if (want > 6) want = 6; if (want < 1) want = 1;
            gain += (want - gain) * (want < gain ? 0.5f : 0.02f);
        }
        med.mic_gain = gain;
        for (int s = 0; s < 160; s++) pcm[s] = med.muted ? 0 : sat16(dec8[s] * gain);
        uint8_t st[64];
        int len = Encoder_Interface_Encode(enc, MR122, pcm, st, 0);
        if (len < 1) continue;
        /* DTMF: 100 ms of event packets (every 20 ms, same timestamp, growing duration, marker on the first), the end
         * packet three times, then 60 ms of voice before the next digit. The voice clock (ts) runs on meanwhile. */
        if (ev_phase == 0 && dtmf_r != dtmf_w && med.dtmf_pt > 0) {
            ev = dtmf_code(dtmf_q[dtmf_r % sizeof(dtmf_q)]); dtmf_r++; ev_phase = 1; ev_n = 0; ev_ts = ts;
        }
        if (ev_phase == 1 || ev_phase == 2) {
            uint8_t e[16];
            int end = ev_phase == 2;
            unsigned dur = end ? 800 : 160 * (unsigned)(ev_n + 1);
            e[0] = 0x80; e[1] = (uint8_t)(med.dtmf_pt | (ev_phase == 1 && ev_n == 0 ? 0x80 : 0));
            e[2] = (uint8_t)(seq >> 8); e[3] = (uint8_t)seq; seq++;
            e[4] = (uint8_t)(ev_ts >> 24); e[5] = (uint8_t)(ev_ts >> 16); e[6] = (uint8_t)(ev_ts >> 8); e[7] = (uint8_t)ev_ts;
            e[8] = (uint8_t)(ssrc >> 24); e[9] = (uint8_t)(ssrc >> 16); e[10] = (uint8_t)(ssrc >> 8); e[11] = (uint8_t)ssrc;
            e[12] = (uint8_t)ev; e[13] = (uint8_t)((end ? 0x80 : 0) | 10); e[14] = (uint8_t)(dur >> 8); e[15] = (uint8_t)dur;
            if (sendto(med.sock, e, sizeof(e), 0, (struct sockaddr *)&med.peer, sizeof(med.peer)) > 0) med.tx_pkts++;
            ts += 160; ev_n++;
            if (ev_phase == 1 && ev_n == 5) { ev_phase = 2; ev_n = 0; }
            else if (ev_phase == 2 && ev_n == 3) { ev_phase = 3; ev_n = 0; }
            continue;
        }
        if (ev_phase == 3 && ++ev_n >= 3) ev_phase = 0;
        int ftv = (st[0] >> 3) & 15, bits = amr_bits[ftv];
        uint8_t pkt[12 + 40] = { 0 };
        pkt[0] = 0x80; pkt[1] = (uint8_t)(med.pt | (first ? 0x80 : 0)); first = 0;
        pkt[2] = (uint8_t)(seq >> 8); pkt[3] = (uint8_t)seq; seq++;
        pkt[4] = (uint8_t)(ts >> 24); pkt[5] = (uint8_t)(ts >> 16); pkt[6] = (uint8_t)(ts >> 8); pkt[7] = (uint8_t)ts; ts += 160;
        pkt[8] = (uint8_t)(ssrc >> 24); pkt[9] = (uint8_t)(ssrc >> 16); pkt[10] = (uint8_t)(ssrc >> 8); pkt[11] = (uint8_t)ssrc;
        size_t pos = 0, rp = 8;
        putbits(pkt + 12, &pos, 15, 4);                     /* CMR: no request */
        putbits(pkt + 12, &pos, 0, 1); putbits(pkt + 12, &pos, (unsigned)ftv, 4); putbits(pkt + 12, &pos, 1, 1);
        for (int k = 0; k < bits; k++) putbits(pkt + 12, &pos, getbits(st, &rp, 1), 1);
        size_t plen = 12 + (pos + 7) / 8;
        if (sendto(med.sock, pkt, plen, 0, (struct sockaddr *)&med.peer, sizeof(med.peer)) > 0) med.tx_pkts++;
        else { med.tx_err++; med.tx_errno = errno; }
    }
    Encoder_Interface_exit(enc);
    return NULL;
}


/* a host route to the media peer through the IMS interface: redwood-cellulard routes only the P-CSCFs there, and
 * sendto() to the media gateway failed "No route to host" (A, 2026-11-14 03:33). The routing-socket ABI is macOS's
 * (net/route.h, not in the iOS SDK), as in live-modem.c tun_route6. */
struct rw_rt_metrics {uint32_t rmx_locks,rmx_mtu,rmx_hopcount;int32_t rmx_expire;uint32_t rmx_recvpipe,rmx_sendpipe,rmx_ssthresh,rmx_rtt,rmx_rttvar,rmx_pksent,rmx_state,rmx_filler[3];};
struct rw_rt_msghdr {u_short rtm_msglen;u_char rtm_version,rtm_type;u_short rtm_index;int rtm_flags,rtm_addrs;pid_t rtm_pid;int rtm_seq,rtm_errno,rtm_use;uint32_t rtm_inits;struct rw_rt_metrics rtm_rmx;};
static void media_route(const struct in6_addr *dst, const char *ifn)
{
    int rs = socket(PF_ROUTE, SOCK_RAW, 0);
    if (rs < 0) { printf("call: media: route socket: %s\n", strerror(errno)); return; }
    struct {struct rw_rt_msghdr h; struct sockaddr_in6 dst; struct sockaddr_dl gw; struct sockaddr_in6 mask;} m;
    memset(&m, 0, sizeof(m));
    m.h.rtm_msglen = sizeof(m); m.h.rtm_version = 5; m.h.rtm_type = 1;                  /* RTM_VERSION, RTM_ADD */
    m.h.rtm_flags = 0x1 | 0x800 | 0x4;                                                  /* RTF_UP|RTF_STATIC|RTF_HOST */
    m.h.rtm_addrs = 0x1 | 0x2 | 0x4; m.h.rtm_seq = 7;                                   /* DST, GATEWAY, NETMASK */
    m.dst.sin6_len = sizeof(m.dst); m.dst.sin6_family = AF_INET6; m.dst.sin6_addr = *dst;
    m.gw.sdl_len = sizeof(m.gw); m.gw.sdl_family = AF_LINK; m.gw.sdl_index = (u_short)if_nametoindex(ifn);
    m.mask.sin6_len = sizeof(m.mask); m.mask.sin6_family = AF_INET6; memset(&m.mask.sin6_addr, 0xff, 16);
    if (write(rs, &m, sizeof(m)) < 0 && errno != EEXIST) printf("call: media: route via %s: %s\n", ifn, strerror(errno));
    else printf("call: media: host route to the peer via %s\n", ifn);
    close(rs);
}

/* rport: the SDP's m= port; remote: its c= address; sock: a UDP socket bound to our RTP port lport; pt / dtmf_pt: the
 * negotiated payload types (dtmf_pt 0: none) */
static void media_tone(int on);
static int media_start(int sock, const char *remote, unsigned rport, int pt, int dtmf_pt, int lport)
{
    media_tone(0);
    med.pt = pt; med.dtmf_pt = dtmf_pt; med.lport = lport; med.rx_other = 0; dtmf_r = dtmf_w;
    memset(&med.peer, 0, sizeof(med.peer));
    med.peer.sin6_len = sizeof(med.peer); med.peer.sin6_family = AF_INET6; med.peer.sin6_port = htons((uint16_t)rport);
    if (inet_pton(AF_INET6, remote, &med.peer.sin6_addr) != 1) return -1;
    med.sock = sock;
    media_route(&med.peer.sin6_addr, cfg.ifn);
    med.spk = open("/dev/redwood-audio", O_WRONLY | O_NONBLOCK);
    med.mic = open("/dev/redwood-mic", O_RDONLY | O_NONBLOCK);
    med.rx_pkts = med.rx_frames = med.tx_pkts = med.rx_bad = med.tx_err = med.mic_bytes = 0; med.tx_errno = 0;
    {   /* the earpiece (top amplifier only) unless the call screen says speaker; iOS's own audio leaves the device to us */
        int one = 1, r = med.route;
        sysctlbyname("kern.redwood.snd_call", NULL, NULL, &one, sizeof(one));
        sysctlbyname("kern.redwood.snd_route", NULL, NULL, &r, sizeof(r));
    }
    /* (route and mute belong to the whole call session, not to one media start: a hold/swap restarts the media and must
     * not undo the user's Speaker or Mute -- ims-call.h resets them when the first call of a session begins) */
    fifo_w = fifo_r = 0; med.underruns = med.concealed = 0; med.mic_gain = 2;
    fir_init();
    med.run = 1;
    pthread_create(&med.play, NULL, media_play, NULL);
    pthread_create(&med.rx, NULL, media_rx, NULL);
    pthread_create(&med.tx, NULL, media_tx, NULL);
    printf("call: media (AP): RTP :%d <-> [%s]:%u, AMR PT %d, DTMF PT %d, speaker %s, mic %s\n", lport, remote, rport, pt, dtmf_pt,
        med.spk >= 0 ? "open" : strerror(errno), med.mic >= 0 ? "open" : "NOT open");
    return 0;
}

static void media_stop(void)
{
    if (!med.run) return;
    med.run = 0;
    pthread_join(med.rx, NULL); pthread_join(med.tx, NULL); pthread_join(med.play, NULL);
    printf("call: media (AP): stopped: in %u packets / %u frames (%u bad, %u other types), out %u packets (%u failed: %s), mic %u bytes, %u underruns\n",
        med.rx_pkts, med.rx_frames, med.rx_bad, med.rx_other, med.tx_pkts, med.tx_err, strerror(med.tx_errno), med.mic_bytes, med.underruns);
    if (med.spk >= 0) close(med.spk);
    if (med.mic >= 0) close(med.mic);
    if (med.sock >= 0) close(med.sock);
    med.spk = med.mic = med.sock = -1;
    {int zero = 0; sysctlbyname("kern.redwood.snd_call", NULL, NULL, &zero, sizeof(zero));
     sysctlbyname("kern.redwood.snd_route", NULL, NULL, &zero, sizeof(zero));}
}


/*
 * The ringback tone (B, 2026-11-15): an outgoing call that rings at the other end without early media (180 without
 * SDP) is silent on a VoLTE phone unless the phone plays the tone itself, as a baseband does. On the call's route,
 * 48 kHz stereo to /dev/redwood-audio. India (IMSI 404/405/406): 400 Hz modulated by 25 Hz, 0.4 on 0.2 off 0.4 on
 * 2.0 off; elsewhere 440 + 480 Hz, 2 s on 4 s off.
 */
static struct { volatile int run; pthread_t th; int fd; int india; } tone = { .fd = -1 };
static void *tone_thread(void *arg)
{
    (void)arg;
    int16_t out[480 * 2];
    unsigned long n = 0;                                    /* 48 kHz samples played */
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    rt_thread(44);
    while (tone.run) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        unsigned long due = (unsigned long)((now.tv_sec - t0.tv_sec) * 48000L + (now.tv_nsec - t0.tv_nsec) / 20833L);
        if (due < n + 480) { usleep(3000); continue; }
        for (int i = 0; i < 480; i++, n++) {
            double t = n / 48000.0, v = 0;
            if (tone.india) {
                double c = fmod(t, 3.0);
                if (c < 0.4 || (c >= 0.6 && c < 1.0)) v = sin(2 * M_PI * 400 * t) * (0.5 + 0.5 * sin(2 * M_PI * 25 * t));
            } else if (fmod(t, 6.0) < 2.0) v = 0.5 * (sin(2 * M_PI * 440 * t) + sin(2 * M_PI * 480 * t));
            out[2 * i] = out[2 * i + 1] = (int16_t)(v * 6000);
        }
        if (tone.fd >= 0) (void)!write(tone.fd, out, sizeof(out));
        if (n % 24000 < 480) { int r = med.route; sysctlbyname("kern.redwood.snd_route", NULL, NULL, &r, sizeof(r)); }
        if (due > n + 4800) n = due;
    }
    return NULL;
}
static void media_tone(int on)
{
    if (on && !tone.run) {
        if (med.run) return;                                /* (the call's own audio has the speaker) */
        tone.india = !strncmp(cfg.imsi, "404", 3) || !strncmp(cfg.imsi, "405", 3) || !strncmp(cfg.imsi, "406", 3);
        tone.fd = open("/dev/redwood-audio", O_WRONLY | O_NONBLOCK);
        int one = 1, r = med.route;
        sysctlbyname("kern.redwood.snd_call", NULL, NULL, &one, sizeof(one));
        sysctlbyname("kern.redwood.snd_route", NULL, NULL, &r, sizeof(r));
        tone.run = 1;
        pthread_create(&tone.th, NULL, tone_thread, NULL);
        printf("%s call: ringback tone on (%s)\n", ts_now(), tone.india ? "India" : "default");
    } else if (!on && tone.run) {
        tone.run = 0;
        pthread_join(tone.th, NULL);
        if (tone.fd >= 0) close(tone.fd);
        tone.fd = -1;
        if (!med.run) { int zero = 0; sysctlbyname("kern.redwood.snd_call", NULL, NULL, &zero, sizeof(zero)); sysctlbyname("kern.redwood.snd_route", NULL, NULL, &zero, sizeof(zero)); }
        printf("%s call: ringback tone off\n", ts_now());
    }
}
