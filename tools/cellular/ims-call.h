/*
 * ims-call.h (B, 2026-11-15): VoLTE calls -- the SIP side of a phone's calls, for several calls at once.
 * Included by redwood-ims.c (it replaces the one-call code of 2026-11-13/14; that history is in git).
 *
 * What a phone's call handling must cover, and where it is here:
 *   - incoming: 100/180, answer (200 + SDP answer in the offer's own payload types), decline (603, or 486 while another
 *     call is up so the caller hears busy), caller cancel (487; "completed elsewhere" reported as such), no answer
 *     (480 after 60 s), the P-CSCF resending the INVITE on a new connection
 *   - outgoing: INVITE, 100/18x/2xx/3xx-6xx, early media (18x with SDP: network tones, caller tunes) or a local ringback
 *     tone (180 without SDP), PRACK for reliable 18x, CANCEL while ringing, a 2xx after our CANCEL (ACK + BYE), no
 *     answer timeouts, number formatting from the SIM's country
 *   - in a call: hold / resume (re-INVITE sendonly / sendrecv) with 491 glare retry, being held by the other side
 *     (their re-INVITE: answered recvonly / inactive), session refresh (re-INVITE / UPDATE answered with SDP; and our
 *     own refresh when we are the refresher, so long calls are not dropped at Session-Expires), DTMF (RFC 4733),
 *     mute, BYE either way, 481 for dialogs we do not know
 *   - several calls: call waiting (a second call rings while one is up), hold-and-answer, end-and-answer, swap; only
 *     the one call not on hold has the speaker and mic; each call has its own RTP port
 *   - the phone's call screen sees every call's state through a snapshot file (level-triggered: nothing is lost when
 *     either side restarts) and acts through command files -- see "The call screen" below
 *
 * Not here (yet): conference (merge), emergency registration (urn:service:sos: emergency numbers go out as ordinary
 * calls), USSD, video, Wi-Fi calling.
 */

#define CALL_MAX 4
#define CALL_RTP_BASE 50010             /* call slot i: RTP port 50010 + 2i */
#define CALL_RING_S 60                  /* incoming: 480 after this long unanswered */
#define CALL_TRYING_S 32                /* outgoing: no answer at all (no 1xx past 100) */
#define CALL_ALERT_S 90                 /* outgoing: ringing at the far end this long: give up */
#define CALL_KEEP_S 40                  /* an ended call stays in the snapshot (and absorbs late messages) this long */
#define IMS_PORT_C 50060                /* port-s is port-c + 2 */

enum { CS_FREE, CS_IN_RING, CS_IN_ANS, CS_UP, CS_OUT_TRY, CS_OUT_ALERT, CS_GONE };
static const char *const cs_name[] = { "free", "ringing", "connecting", "active", "dialing", "alerting", "ended" };

struct sc {
    int st, out, slot;
    unsigned id;                        /* the call's number for the call screen (unique across restarts) */
    time_t at, answered, gone_at, alert_at;
    int fd;                             /* incoming: the connection the INVITE came on (its responses go there) */
    char cid[200];
    char local[400], remote[400];       /* our side / the other side, as name-addr with their tags */
    char target[400];                   /* the other side's Contact URI (requests in the dialog go to it) */
    char route[1200];                   /* "Route: ...\r\n" lines for our requests in the dialog */
    char inv_via[1600], inv_cseq[80];   /* incoming INVITE: Via block and CSeq (its responses) */
    char inv_rr[1200], inv_se[80];      /* incoming INVITE: Record-Route lines (echoed in 18x/2xx), Session-Expires */
    char ruri[200];                     /* outgoing: our INVITE's Request-URI (CANCEL, ACK of a failure) */
    char branch[40];                    /* our INVITE's branch (CANCEL, ACK of a failure) */
    unsigned lcseq;                     /* our next CSeq in the dialog */
    char tag[80];                       /* outgoing: the call screen's tag (until it knows the id) */
    char number[64];                    /* the other party ("" = withheld) */
    int answer_req;                     /* the call screen said answer */
    /* media */
    char rc[96]; unsigned rport;        /* the other side's c= / m= */
    int pt, dtmf_pt;                    /* negotiated payload types */
    int early;                          /* outgoing: 18x had SDP (network audio before the answer) */
    unsigned sess, ver;                 /* our SDP o= session id and version */
    int lhold;                          /* we have the call on hold (confirmed by the network) */
    int want_hold;                      /* what the call screen wants (a re-INVITE makes lhold follow it) */
    int rhold;                          /* the other side has us on hold (their SDP sendonly / inactive) */
    /* our re-INVITE / UPDATE in progress */
    int ri;                             /* 0 none, 1 re-INVITE (hold/resume/refresh), 2 UPDATE (refresh) */
    unsigned ri_cseq; time_t ri_at, ri_retry;
    /* session timer */
    int se, we_refresh; time_t refreshed;
    int peer_update;                    /* the other side allows UPDATE */
    int heard;                          /* outgoing: the network answered (100 Trying or more) */
    unsigned rseq_acked;
    char why[96];                       /* why it ended */
    char reason_tag[24];                /* ended reason, one word, for the call screen */
};
static struct sc calls[CALL_MAX];
static unsigned call_seq;
static char call_ip6[64];
static int call_ports, call_sock = -1, call_portc;
static const char *call_user;
static int call_media_slot = -1;        /* the call that has the speaker and mic now */
static int call_session;                /* a call session (one or more calls) is in progress */

static void calls_snapshot(int force);
static void call_media_sync(void);

/* (A, 2026-11-15) message building never runs past the buffer: MSG_CAT appends only while there is room, and a
 * message that did not fit (o >= sizeof(m)) is not sent -- a cut-off SIP message is worse than none, and sending
 * the would-be length would read past the buffer */
#define MSG_CAT(m, o, ...) do { if ((o) >= 0 && (o) < (int)sizeof(m)) (o) += snprintf((m) + (o), sizeof(m) - (size_t)(o), __VA_ARGS__); } while (0)
#define MSG_FITS(m, o) ((o) > 0 && (o) < (int)sizeof(m))

/* ---------------------------------------------------------------- small SIP helpers */

/* every line of a header, as "Name: value\r\n" lines, in order */
static void hdr_lines(const char *r, const char *name, char *out, size_t cap)
{
    size_t o = 0, L = strlen(name);
    out[0] = 0;
    for (const char *q = r; (q = strstr(q, "\r\n"));) {
        q += 2;
        if (!strncasecmp(q, name, L) && q[L] == ':') {
            const char *e = strstr(q, "\r\n");
            if (!e) break;
            o += (size_t)snprintf(out + o, cap > o ? cap - o : 0, "%.*s\r\n", (int)(e - q), q);
            if (o >= cap) { out[cap - 1] = 0; return; }
        }
        if (q[0] == '\r' && q[1] == '\n') break;            /* end of headers */
    }
}
/* the Record-Route values as Route lines: in order (UAS) or reversed (UAC) */
static void route_from_rr(const char *r, int reverse, char *out, size_t cap)
{
    const char *v[12]; int len[12], n = 0;
    for (const char *q = r; (q = strstr(q, "\r\n")) && n < 12;) {
        q += 2;
        if (q[0] == '\r' && q[1] == '\n') break;
        if (!strncasecmp(q, "Record-Route:", 13)) {
            const char *s = q + 13, *e = strstr(q, "\r\n");
            while (*s == ' ') s++;
            if (!e) break;
            v[n] = s; len[n] = (int)(e - s); n++;
        }
    }
    size_t o = 0; out[0] = 0;
    for (int k = 0; k < n; k++) {
        int i = reverse ? n - 1 - k : k;
        o += (size_t)snprintf(out + o, cap > o ? cap - o : 0, "Route: %.*s\r\n", len[i], v[i]);
    }
}
/* the URI of a name-addr ("Name" <uri>;params -> uri) */
static void uri_of(const char *na, char *out, size_t cap)
{
    const char *a = strchr(na, '<'), *e = a ? strchr(a, '>') : NULL;
    if (a && e) snprintf(out, cap, "%.*s", (int)(e - a - 1), a + 1);
    else { snprintf(out, cap, "%s", na); char *sc = strchr(out, ';'); if (sc) *sc = 0; }
}
/* the user part of a sip:/tel: URI in a header value: the phone number */
static void call_number(const char *v, char *out, size_t cap)
{
    const char *p = strstr(v, "sip:"); if (!p) p = strstr(v, "tel:");
    out[0] = 0; if (!p) return; p += 4;
    size_t o = 0;
    while (*p && *p != '@' && *p != ';' && *p != '>' && o + 1 < cap) out[o++] = *p++;
    out[o] = 0;
    if (strstr(out, "anonymous") || strstr(out, "unavailable") || strstr(out, "restricted")) out[0] = 0;
}
static int sip_code(const char *r) { return atoi(r + 8); }
static int sip_cseq_n(const char *r) { char c[80]; return hdr(r, "CSeq", c, sizeof(c)) ? atoi(c) : -1; }
static int sip_cseq_is(const char *r, const char *method) { char c[80]; return hdr(r, "CSeq", c, sizeof(c)) && strstr(c, method) != NULL; }
static int sip_has_body(const char *r) { return strstr(r, "\r\n\r\nv=0") != NULL; }

/* send on a connection; if it is gone, on our registration connection (the P-CSCF's connections can die under a call) */
static void sip_send(int fd, const char *m, size_t n, const char *what)
{
    if (fd >= 0 && send(fd, m, n, 0) == (ssize_t)n) { printf("%s call: --> %s\n", ts_now(), what); return; }
    if (call_sock >= 0 && fd != call_sock && send(call_sock, m, n, 0) == (ssize_t)n) {
        printf("%s call: --> %s (on the registration connection)\n", ts_now(), what); return;
    }
    printf("%s call: --> %s FAILED (%s)\n", ts_now(), what, strerror(errno));
}

/* ---------------------------------------------------------------- SDP */

/* the other side's SDP: address, port, AMR and telephone-event payload types, direction */
static int sdp_parse(struct sc *c, const char *r, int *dir)
{
    const char *b = strstr(r, "\r\n\r\n"); if (!b) return -1;
    char rc[96] = "", line[200]; unsigned rport = 0; int pt = -1, dpt = 0, d = 0;
    const char *ma = strstr(b, "\nm=audio ");
    const char *cc = strstr(b, "\nc=IN IP6 ");
    if (cc) sscanf(cc + 10, "%95s", rc);
    if (ma) sscanf(ma + 9, "%u", &rport);
    for (const char *q = b; (q = strstr(q, "\na=")); q++) {
        const char *e = strpbrk(q + 1, "\r\n"); size_t L = e ? (size_t)(e - q - 1) : strlen(q + 1);
        if (L >= sizeof(line)) L = sizeof(line) - 1;
        memcpy(line, q + 1, L); line[L] = 0;
        int n; char enc[40];
        if (sscanf(line, "a=rtpmap:%d %39[^/]", &n, enc) == 2) {
            if (!strcasecmp(enc, "AMR") && pt < 0) pt = n;
            else if (!strcasecmp(enc, "telephone-event") && !dpt) dpt = n;
        } else if (!strcmp(line, "a=sendonly")) d = 1;
        else if (!strcmp(line, "a=recvonly")) d = 2;
        else if (!strcmp(line, "a=inactive")) d = 3;
    }
    if (rc[0] && !strcmp(rc, "::")) d = 3;                  /* (old-style hold: no address) */
    if (dir) *dir = d;
    if (rc[0]) snprintf(c->rc, sizeof(c->rc), "%s", rc);
    if (rport) c->rport = rport;
    if (pt >= 0) c->pt = pt;
    if (dpt) c->dtmf_pt = dpt;
    if (pt < 0) printf("%s call: SDP without AMR (codecs offered: only others) -- AMR %d kept\n", ts_now(), c->pt);
    return 0;
}
/* our SDP; dir: 0 sendrecv, 1 sendonly, 2 recvonly, 3 inactive */
static void sdp_ours(struct sc *c, char *sdp, size_t cap, int dir)
{
    static const char *const ds[] = { "sendrecv", "sendonly", "recvonly", "inactive" };
    snprintf(sdp, cap,
        "v=0\r\no=- %u %u IN IP6 %s\r\ns=-\r\nc=IN IP6 %s\r\nt=0 0\r\n"
        "m=audio %d RTP/AVP %d %d\r\nb=AS:41\r\n"
        "a=rtpmap:%d AMR/8000\r\na=fmtp:%d mode-change-capability=2;max-red=0\r\n"
        "a=rtpmap:%d telephone-event/8000\r\na=fmtp:%d 0-15\r\na=ptime:20\r\na=maxptime:80\r\na=%s\r\n",
        c->sess, c->ver, call_ip6, call_ip6, CALL_RTP_BASE + 2 * c->slot, c->pt, c->dtmf_pt ? c->dtmf_pt : 101,
        c->pt, c->pt, c->dtmf_pt ? c->dtmf_pt : 101, c->dtmf_pt ? c->dtmf_pt : 101, ds[dir & 3]);
}
/* the direction we answer an offer with, from theirs and our own hold */
static int sdp_answer_dir(int theirs, int lhold)
{
    /* theirs sendonly (they hold us) -> we recvonly (or inactive if we hold too); theirs recvonly -> we sendonly;
     * theirs inactive -> inactive */
    if (theirs == 3) return 3;
    if (theirs == 1) return lhold ? 3 : 2;
    if (theirs == 2) return lhold ? 3 : 1;
    return lhold ? 1 : 0;
}

/* ---------------------------------------------------------------- slots */

static struct sc *call_by_cid(const char *cid)
{
    for (int i = 0; i < CALL_MAX; i++) if (calls[i].st != CS_FREE && !strcmp(calls[i].cid, cid)) return &calls[i];
    return NULL;
}
static struct sc *call_by_id(unsigned id)
{
    for (int i = 0; i < CALL_MAX; i++) if (calls[i].st != CS_FREE && calls[i].id == id) return &calls[i];
    return NULL;
}
static int calls_live(void)
{
    int n = 0;
    for (int i = 0; i < CALL_MAX; i++) if (calls[i].st != CS_FREE && calls[i].st != CS_GONE) n++;
    return n;
}
static struct sc *call_new(void)
{
    int best = -1;
    for (int i = 0; i < CALL_MAX; i++) if (calls[i].st == CS_FREE) { best = i; break; }
    if (best < 0)                       /* an ended call still kept: the oldest goes */
        for (int i = 0; i < CALL_MAX; i++) if (calls[i].st == CS_GONE && (best < 0 || calls[i].gone_at < calls[best].gone_at)) best = i;
    if (best < 0) return NULL;
    struct sc *c = &calls[best];
    memset(c, 0, sizeof(*c));
    c->slot = best; c->fd = -1; c->at = time(NULL); c->id = ++call_seq; c->pt = 100; c->dtmf_pt = 101;
    c->sess = arc4random() & 0x7fffffff; c->ver = c->sess; c->lcseq = 1;
    if (!call_session) {                /* the first call of a session: earpiece, not muted */
        call_session = 1; med.route = 1; med.muted = 0;
        notify_post("com.redwood.call.started");
    }
    return c;
}
static void call_gone(struct sc *c, const char *tag, const char *why)
{
    if (c->st == CS_GONE || c->st == CS_FREE) return;
    printf("%s call %u: ended (%s) after %lds\n", ts_now(), c->id, why, (long)(time(NULL) - c->at));
    snprintf(c->why, sizeof(c->why), "%s", why);
    snprintf(c->reason_tag, sizeof(c->reason_tag), "%s", tag);
    c->st = CS_GONE; c->gone_at = time(NULL); c->ri = 0;
    call_media_sync();
    calls_snapshot(1);
}

/* ---------------------------------------------------------------- messages of ours */

/* a response to an incoming INVITE (or, with r, to another request: its own Via/From/To/CSeq) */
static void call_respond(struct sc *c, const char *status, const char *body, const char *extra)
{
    char m[6000]; int o;
    int code = atoi(status);
    o = snprintf(m, sizeof(m), "SIP/2.0 %s\r\n%s", status, c->inv_via);
    /* Record-Route goes back in every dialog-making response (18x/2xx: RFC 3261 12.1.1) */
    if (code > 100 && code < 300) MSG_CAT(m, o, "%s", c->inv_rr);
    MSG_CAT(m, o, "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n",
        c->remote, c->local, c->cid, c->inv_cseq);
    if (code != 100) {
        MSG_CAT(m, o,
            "Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
            "Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,OPTIONS,MESSAGE,INFO\r\nSupported: replaces,timer\r\n",
            call_user, call_ip6, call_ports);
    }
    if (extra) MSG_CAT(m, o, "%s", extra);
    if (body) MSG_CAT(m, o, "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
    else MSG_CAT(m, o, "Content-Length: 0\r\n\r\n");
    char what[60]; snprintf(what, sizeof(what), "%.40s (call %u)", status, c->id);
    if (!MSG_FITS(m, o)) { printf("%s call: --> %s NOT SENT (too long)\n", ts_now(), what); return; }
    sip_send(c->fd, m, (size_t)o, what);
}
/* a plain response to a request r (its Via/From/To/Call-ID/CSeq) */
static void sip_reply(int fd, const char *r, const char *status, const char *extra, const char *sdp)
{
    char via[1600], fr[400], to[400], ci[200], cq[80], m[6000];
    hdr_lines(r, "Via", via, sizeof(via));
    hdr(r, "From", fr, sizeof(fr)); hdr(r, "To", to, sizeof(to)); hdr(r, "Call-ID", ci, sizeof(ci)); hdr(r, "CSeq", cq, sizeof(cq));
    int o = snprintf(m, sizeof(m), "SIP/2.0 %s\r\n%sFrom: %s\r\nTo: %s%s\r\nCall-ID: %s\r\nCSeq: %s\r\n%s",
        status, via, fr, to, strstr(to, "tag=") ? "" : ";tag=rwx1", ci, cq, extra ? extra : "");
    if (sdp) MSG_CAT(m, o,
        "Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
        "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s", call_user, call_ip6, call_ports, strlen(sdp), sdp);
    else MSG_CAT(m, o, "Content-Length: 0\r\n\r\n");
    char what[80]; snprintf(what, sizeof(what), "%.30s to %.10s", status, r);
    if (!MSG_FITS(m, o)) { printf("%s call: --> %s NOT SENT (too long)\n", ts_now(), what); return; }
    sip_send(fd, m, (size_t)o, what);
}
/* a request of ours in the dialog (BYE, ACK, PRACK, re-INVITE, UPDATE), on our registration connection */
static void call_request_out(struct sc *c, const char *method, unsigned cseq, const char *extra, const char *sdp, char *branch_out)
{
    char m[6000], uri[400], br[40];
    snprintf(br, sizeof(br), "z9hG4bK%08x%04x", arc4random(), arc4random() & 0xffff);
    if (branch_out) strcpy(branch_out, br);
    uri_of(c->target[0] ? c->target : c->remote, uri, sizeof(uri));
    int o = snprintf(m, sizeof(m), "%s %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\n%sMax-Forwards: 70\r\n"
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %u %s\r\n",
        method, uri, call_ip6, call_portc, br, c->route[0] ? c->route : sms_route, c->local, c->remote, c->cid, cseq, method);
    if (strcmp(method, "ACK") && strcmp(method, "BYE"))
        MSG_CAT(m, o,
            "Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n",
            call_user, call_ip6, call_ports);
    if (extra) MSG_CAT(m, o, "%s", extra);
    if (sdp) MSG_CAT(m, o, "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s", strlen(sdp), sdp);
    else MSG_CAT(m, o, "Content-Length: 0\r\n\r\n");
    char what[60]; snprintf(what, sizeof(what), "%s (call %u)", method, c->id);
    if (!MSG_FITS(m, o)) { printf("%s call: --> %s NOT SENT (too long)\n", ts_now(), what); return; }
    sip_send(call_sock, m, (size_t)o, what);
}
/* CANCEL of our INVITE (same branch, CSeq number of the INVITE) */
static void call_cancel(struct sc *c, const char *reason)
{
    char m[3000], uri[400], to[400];
    uri_of(c->target[0] ? c->target : c->remote, uri, sizeof(uri));
    /* the CANCEL's To has no tag only if no 1xx gave one; it must equal the INVITE's To (RFC 3261 9.1) */
    snprintf(to, sizeof(to), "%s", c->remote); char *t = strstr(to, ";tag="); if (t) *t = 0;
    /* (the Request-URI must be the INVITE's: our INVITE went to the tel URI kept in target before any Contact) */
    int o = snprintf(m, sizeof(m), "CANCEL %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\nMax-Forwards: 70\r\n%s"
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 CANCEL\r\n%sContent-Length: 0\r\n\r\n",
        c->ruri[0] ? c->ruri : uri, call_ip6, call_portc, c->branch, sms_route, c->local, to, c->cid, reason ? reason : "");
    char what[40]; snprintf(what, sizeof(what), "CANCEL (call %u)", c->id);
    if (!MSG_FITS(m, o)) { printf("%s call: --> %s NOT SENT (too long)\n", ts_now(), what); return; }
    sip_send(call_sock, m, (size_t)o, what);
}
static void call_bye(struct sc *c)
{
    call_request_out(c, "BYE", c->lcseq++, NULL, NULL, NULL);
}

/* ---------------------------------------------------------------- media: who has the speaker */

/* the call that should be heard now: an answered call not on hold (the newest), else an outgoing call's early media */
static void call_media_sync(void)
{
    int want = -1, ringback = 0;
    for (int i = 0; i < CALL_MAX; i++) {
        struct sc *c = &calls[i];
        if ((c->st == CS_UP || c->st == CS_IN_ANS) && !c->lhold && !c->want_hold && c->rport &&
            (want < 0 || calls[want].st != CS_UP || c->answered > calls[want].answered)) want = i;
    }
    if (want < 0)
        for (int i = 0; i < CALL_MAX; i++) {
            struct sc *c = &calls[i];
            if (c->st == CS_OUT_ALERT && c->early && c->rport) want = i;
            else if (c->st == CS_OUT_ALERT && !c->early) ringback = 1;
        }
    if (want >= 0 && med.run && call_media_slot == want) {
        /* the same call: follow a moved media address */
        struct sc *c = &calls[want]; struct in6_addr a;
        if (inet_pton(AF_INET6, c->rc, &a) == 1 && (memcmp(&a, &med.peer.sin6_addr, 16) || ntohs(med.peer.sin6_port) != c->rport || med.pt != c->pt)) {
            printf("%s call %u: media moves to [%s]:%u PT %d\n", ts_now(), c->id, c->rc, c->rport, c->pt);
            media_stop();
        } else { media_tone(0); return; }
    }
    if (med.run && call_media_slot != want) { media_stop(); call_media_slot = -1; }
    if (want >= 0 && !med.run) {
        struct sc *c = &calls[want];
        int s = bound(SOCK_DGRAM, CALL_RTP_BASE + 2 * c->slot);
        if (s < 0) printf("%s call %u: media: RTP socket %d: %s\n", ts_now(), c->id, CALL_RTP_BASE + 2 * c->slot, strerror(errno));
        else if (media_start(s, c->rc, c->rport, c->pt, c->dtmf_pt, CALL_RTP_BASE + 2 * c->slot) == 0) call_media_slot = want;
        else close(s);
    }
    media_tone(want < 0 && ringback);
    if (!calls_live()) {
        if (med.run) { media_stop(); call_media_slot = -1; }
        media_tone(0);
        call_session = 0;
    }
}

/* ---------------------------------------------------------------- the call screen */
/*
 * Shared with redwood-telephonyd (tools/cellular/telephonyd), the phone's telephony provider, in
 * /var/tmp/redwood-cell/call-ui/ (mobile may read; it writes only cmd.*):
 *   calls    ours, rewritten whole on every change and at least every 2 s:
 *              "ims PID TIME registered 0|1"
 *              "call ID in|out STATE HOLD NUMBER TAG REASON"   one line per call (ended ones for CALL_KEEP_S)
 *            STATE ringing | dialing | alerting | connecting | active | ended; HOLD 0, 1 we hold, 2 they hold, 3 both;
 *            NUMBER "-" when withheld; TAG "-" for incoming; REASON "-" or one word (busy, declined, cancelled, ...)
 *   cmd.SEQ  theirs, one command each, taken in SEQ order: "dial TAG NUMBER", "answer ID", "end ID", "hold ID 0|1",
 *            "dtmf ID DIGITS", "mute 0|1", "route speaker|earpiece"
 *   alive    theirs, touched every few seconds: without it incoming calls are answered 486 (no call screen to ring)
 */
#define CALL_UI_DIR "/var/tmp/redwood-cell/call-ui"
static int call_ui_alive(void)
{
    struct stat st;
    return !stat(CALL_UI_DIR "/alive", &st) && time(NULL) - st.st_mtime < 20;
}
static int call_registered = 1;
static void calls_snapshot(int force)
{
    static time_t last; static char prev[4096];
    char b[4096]; size_t o = 0;
    time_t now = time(NULL);
    for (int i = 0; i < CALL_MAX; i++) {
        struct sc *c = &calls[i];
        if (c->st == CS_FREE) continue;
        int hold = (c->lhold ? 1 : 0) | (c->rhold ? 2 : 0);
        if (o >= sizeof(b)) break;
        o += (size_t)snprintf(b + o, sizeof(b) - o, "call %u %s %s %d %s %s %s\n", c->id, c->out ? "out" : "in", cs_name[c->st], hold,
            c->number[0] ? c->number : "-", c->tag[0] ? c->tag : "-", c->st == CS_GONE && c->reason_tag[0] ? c->reason_tag : "-");
    }
    if (o >= sizeof(b)) b[sizeof(b) - 1] = 0;
    if (!force && !strcmp(b, prev) && now - last < 2) return;
    int changed = strcmp(b, prev) != 0;
    snprintf(prev, sizeof(prev), "%s", b); last = now;
    mkdir(CALL_UI_DIR, 0755); chown(CALL_UI_DIR, 501, 501);
    static int mcc_done;
    if (!mcc_done) {                    /* the SIM's country (MCC only) for the call screen's number formatting */
        FILE *m = fopen(CALL_UI_DIR "/mcc", "w"); if (m) { fprintf(m, "%.3s\n", cfg.imsi); fclose(m); chmod(CALL_UI_DIR "/mcc", 0644); mcc_done = 1; }
    }
    FILE *f = fopen(CALL_UI_DIR "/calls.tmp", "w"); if (!f) return;
    fprintf(f, "ims %d %ld registered %d\n%s", getpid(), (long)now, call_registered, b);
    fclose(f); chmod(CALL_UI_DIR "/calls.tmp", 0644); rename(CALL_UI_DIR "/calls.tmp", CALL_UI_DIR "/calls");
    if (changed) {
        printf("%s call: screen <- %s", ts_now(), b[0] ? "" : "(no calls)\n");
        /* (numbers are not logged) */
        for (int i = 0; i < CALL_MAX; i++) if (calls[i].st != CS_FREE)
            printf("%s[%u %s %s hold %d%s%s]", i ? " " : "", calls[i].id, calls[i].out ? "out" : "in", cs_name[calls[i].st],
                (calls[i].lhold ? 1 : 0) | (calls[i].rhold ? 2 : 0), calls[i].reason_tag[0] ? " " : "", calls[i].reason_tag);
        if (b[0]) printf("\n");
        FILE *g = fopen("/var/tmp/redwood-cell/call", "w");   /* (old single-line status, for other tools) */
        if (g) { fprintf(g, "%s %ld\n", calls_live() ? "active" : "idle", (long)now); fclose(g); }
    }
}

/* ---------------------------------------------------------------- hold / resume / refresh (our re-INVITE) */

static void call_reinvite_out(struct sc *c, int refresh_only)
{
    if (c->st != CS_UP || c->ri) return;
    char sdp[900], extra[200] = "";
    c->ver++;
    int dir = c->want_hold ? (c->rhold ? 3 : 1) : (c->rhold ? 2 : 0);
    sdp_ours(c, sdp, sizeof(sdp), dir);
    if (c->se) snprintf(extra, sizeof(extra), "Session-Expires: %d;refresher=uac\r\nSupported: timer\r\n", c->se);
    c->ri = 1; c->ri_cseq = c->lcseq++; c->ri_at = time(NULL);
    call_request_out(c, "INVITE", c->ri_cseq, extra, sdp, NULL);
    printf("%s call %u: re-INVITE (%s)\n", ts_now(), c->id, refresh_only ? "session refresh" : c->want_hold ? "hold" : "resume");
}
static void call_update_out(struct sc *c)
{
    if (c->st != CS_UP || c->ri) return;
    char extra[200];
    snprintf(extra, sizeof(extra), "Session-Expires: %d;refresher=uac\r\nSupported: timer\r\n", c->se ? c->se : 1800);
    c->ri = 2; c->ri_cseq = c->lcseq++; c->ri_at = time(NULL);
    call_request_out(c, "UPDATE", c->ri_cseq, extra, NULL, NULL);
    printf("%s call %u: UPDATE (session refresh)\n", ts_now(), c->id);
}
/* the session timer from a 2xx (ours or theirs): who refreshes, how often */
static void call_session_timer(struct sc *c, const char *r, int we_are_uac)
{
    char se[80];
    if (!hdr(r, "Session-Expires", se, sizeof(se)) && !hdr(r, "x", se, sizeof(se))) { c->se = 0; c->we_refresh = 0; return; }
    c->se = atoi(se);
    if (c->se < 90) c->se = 90;
    int uac_refreshes = !strstr(se, "refresher=uas");
    c->we_refresh = we_are_uac ? uac_refreshes : !uac_refreshes;
    c->refreshed = time(NULL);
    char al[300]; if (hdr(r, "Allow", al, sizeof(al))) c->peer_update = strstr(al, "UPDATE") != NULL;
    printf("%s call %u: session timer %d s, refreshed by %s\n", ts_now(), c->id, c->se, c->we_refresh ? "us" : "the network");
}

/* ---------------------------------------------------------------- incoming requests */

/* a new or repeated INVITE; returns 1 when handled */
static int call_invite(int fd, const char *r)
{
    char ci[200], to[400];
    if (!hdr(r, "Call-ID", ci, sizeof(ci))) return 0;
    struct sc *c = call_by_cid(ci);
    hdr(r, "To", to, sizeof(to));
    if (c && strstr(to, "tag=")) {                           /* a re-INVITE in the dialog */
        if (c->st == CS_GONE) { sip_reply(fd, r, "481 Call/Transaction Does Not Exist", NULL, NULL); return 1; }
        if (c->ri == 1) {                                    /* glare: ours is out too (RFC 3261 14.1) */
            sip_reply(fd, r, "491 Request Pending", NULL, NULL); return 1;
        }
        int theirs = 0;
        if (sip_has_body(r)) sdp_parse(c, r, &theirs); else theirs = c->rhold ? 1 : 0;
        int was = c->rhold; c->rhold = theirs == 1 || theirs == 3;
        if (was != c->rhold) printf("%s call %u: the other side %s\n", ts_now(), c->id, c->rhold ? "put us on hold" : "resumed");
        char sdp[900], extra[200] = "", se[80];
        c->ver++;
        sdp_ours(c, sdp, sizeof(sdp), sdp_answer_dir(theirs, c->lhold));
        if (hdr(r, "Session-Expires", se, sizeof(se))) {
            snprintf(extra, sizeof(extra), "Session-Expires: %s\r\nRequire: timer\r\n", se);
            call_session_timer(c, r, 0);
        }
        char ct[400]; if (hdr(r, "Contact", ct, sizeof(ct))) uri_of(ct, c->target, sizeof(c->target));
        sip_reply(fd, r, "200 OK", extra, sdp);
        c->refreshed = time(NULL);
        call_media_sync(); calls_snapshot(1);
        return 1;
    }
    if (c) {                                                 /* the same INVITE again (the P-CSCF's new connection) */
        c->fd = fd;
        printf("%s call %u: INVITE again on a new connection: carried on there\n", ts_now(), c->id);
        hdr_lines(r, "Via", c->inv_via, sizeof(c->inv_via));
        if (c->st == CS_IN_RING) { call_respond(c, "100 Trying", NULL, NULL); call_respond(c, "180 Ringing", NULL, NULL); }
        else if (c->st == CS_IN_ANS || c->st == CS_UP) { char sdp[900]; sdp_ours(c, sdp, sizeof(sdp), 0); call_respond(c, "200 OK", sdp, NULL); }
        else if (c->st == CS_GONE) sip_reply(fd, r, "487 Request Terminated", NULL, NULL);
        return 1;
    }
    if (strstr(to, "tag=")) { sip_reply(fd, r, "481 Call/Transaction Does Not Exist", NULL, NULL); return 1; }   /* (a dialog we do not know) */
    /* a new call */
    if (!call_ui_alive()) {                                  /* no call screen to ring on */
        sip_reply(fd, r, "100 Trying", NULL, NULL);
        sip_reply(fd, r, "486 Busy Here", NULL, NULL);
        printf("%s call: INCOMING while the call screen is not running: 486\n", ts_now());
        return 1;
    }
    if (calls_live() >= 2 || !(c = call_new())) {
        sip_reply(fd, r, "486 Busy Here", NULL, NULL);
        printf("%s call: INCOMING with %d calls already: 486\n", ts_now(), calls_live());
        return 1;
    }
    c->fd = fd; c->st = CS_IN_RING;
    snprintf(c->cid, sizeof(c->cid), "%s", ci);
    hdr_lines(r, "Via", c->inv_via, sizeof(c->inv_via));
    route_from_rr(r, 0, c->route, sizeof(c->route));
    hdr_lines(r, "Record-Route", c->inv_rr, sizeof(c->inv_rr));
    hdr(r, "Session-Expires", c->inv_se, sizeof(c->inv_se));
    hdr(r, "From", c->remote, sizeof(c->remote));
    snprintf(c->local, sizeof(c->local), "%s;tag=rw%08x", to, arc4random());
    hdr(r, "CSeq", c->inv_cseq, sizeof(c->inv_cseq));
    char ct[400]; if (hdr(r, "Contact", ct, sizeof(ct))) uri_of(ct, c->target, sizeof(c->target));
    char al[300]; if (hdr(r, "Allow", al, sizeof(al))) c->peer_update = strstr(al, "UPDATE") != NULL;
    /* the number: P-Asserted-Identity (the network's), else From; withheld when Privacy says id or the URI is anonymous */
    char pai[400], pv[80];
    if (hdr(r, "P-Asserted-Identity", pai, sizeof(pai))) {
        /* (two values possible: sip and tel; take the first with a number) */
        call_number(pai, c->number, sizeof(c->number));
    }
    if (!c->number[0]) call_number(c->remote, c->number, sizeof(c->number));
    if (hdr(r, "Privacy", pv, sizeof(pv)) && (strstr(pv, "id") || strstr(pv, "user")) && strstr(c->remote, "anonymous")) c->number[0] = 0;
    if (sip_has_body(r)) sdp_parse(c, r, NULL);
    else printf("%s call %u: INVITE without an offer (the offer will come in the ACK)\n", ts_now(), c->id);
    printf("%s call %u: INCOMING%s, AMR PT %d, media port %u%s\n", ts_now(), c->id, c->number[0] ? "" : " (number withheld)", c->pt, c->rport,
        calls_live() > 1 ? " -- call waiting" : "");
    {   /* root-only, for working out network behaviour */
        /* (one file, the latest: one per call grew without end and kept every caller's number) */
        int f = open("/var/root/ims-invite-last.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600); if (f >= 0) { (void)!write(f, r, strlen(r)); close(f); }
    }
    call_respond(c, "100 Trying", NULL, NULL);
    call_respond(c, "180 Ringing", NULL, NULL);
    calls_snapshot(1);
    return 1;
}

/* other requests in a call's dialog; returns 1 when it was a call's */
static int call_request(int fd, const char *r)
{
    char ci[200];
    if (!hdr(r, "Call-ID", ci, sizeof(ci))) return 0;
    struct sc *c = call_by_cid(ci);
    int is_ack = !strncmp(r, "ACK ", 4), is_bye = !strncmp(r, "BYE ", 4), is_cancel = !strncmp(r, "CANCEL ", 7);
    int is_update = !strncmp(r, "UPDATE ", 7), is_info = !strncmp(r, "INFO ", 5), is_prack = !strncmp(r, "PRACK ", 6);
    int is_options = !strncmp(r, "OPTIONS ", 8), is_refer = !strncmp(r, "REFER ", 6), is_notify = !strncmp(r, "NOTIFY ", 7);
    if (!c) {
        if (is_bye || is_cancel || is_update || is_info || is_prack || is_refer) {   /* (MESSAGE/NOTIFY/OPTIONS are not calls') */
            sip_reply(fd, r, "481 Call/Transaction Does Not Exist", NULL, NULL); return 1;
        }
        return is_ack;                                       /* (an ACK for something of the old code's: nothing to do) */
    }
    if (is_ack) {
        if (c->st == CS_IN_ANS) {
            if (sip_has_body(r)) sdp_parse(c, r, NULL);      /* (the offer-in-ACK case) */
            c->st = CS_UP;
            printf("%s call %u: ACK -- the call is up\n", ts_now(), c->id);
            call_media_sync(); calls_snapshot(1);
        }
        return 1;
    }
    if (is_cancel) {
        sip_reply(fd, r, "200 OK", NULL, NULL);
        if (c->st == CS_IN_RING) {
            call_respond(c, "487 Request Terminated", NULL, NULL);
            char rs[200] = "";
            hdr(r, "Reason", rs, sizeof(rs));
            /* answered or declined on another of the user's devices (RFC 3326 cause 200 / 603) */
            if (strstr(rs, "cause=200") || strstr(rs, "elsewhere")) call_gone(c, "answered-elsewhere", "answered elsewhere");
            else if (strstr(rs, "cause=603")) call_gone(c, "declined-elsewhere", "declined elsewhere");
            else call_gone(c, "missed", "the caller hung up before an answer");
        }
        return 1;
    }
    if (is_bye) {
        sip_reply(fd, r, "200 OK", NULL, NULL);
        if (c->st != CS_GONE) call_gone(c, "remote", "the other side hung up");
        return 1;
    }
    if (is_update) {
        if (c->st == CS_GONE) { sip_reply(fd, r, "481 Call/Transaction Does Not Exist", NULL, NULL); return 1; }
        char extra[200] = "", se[80], sdp[900];
        if (hdr(r, "Session-Expires", se, sizeof(se))) { snprintf(extra, sizeof(extra), "Session-Expires: %s\r\nRequire: timer\r\n", se); call_session_timer(c, r, 0); }
        if (sip_has_body(r)) {
            int theirs = 0; sdp_parse(c, r, &theirs);
            c->rhold = theirs == 1 || theirs == 3; c->ver++;
            sdp_ours(c, sdp, sizeof(sdp), sdp_answer_dir(theirs, c->lhold));
            sip_reply(fd, r, "200 OK", extra, sdp);
            call_media_sync(); calls_snapshot(1);
        } else sip_reply(fd, r, "200 OK", extra, NULL);
        c->refreshed = time(NULL);
        return 1;
    }
    if (is_refer) { sip_reply(fd, r, "603 Decline", NULL, NULL); return 1; }   /* (no transfer) */
    if (is_info || is_prack || is_options || is_notify) { sip_reply(fd, r, "200 OK", NULL, NULL); return 1; }
    sip_reply(fd, r, "501 Not Implemented", NULL, NULL);
    return 1;
}

/* ---------------------------------------------------------------- outgoing calls */

/* E.164 country code from the IMSI's MCC; NULL when unknown (numbers then go out as dialled) */
static const char *mcc_country_code(const char *imsi)
{
    static const struct {int mcc;const char *cc;} t[]={
        {404,"91"},{405,"91"},{406,"91"},{310,"1"},{311,"1"},{312,"1"},{313,"1"},{314,"1"},{315,"1"},{316,"1"},{302,"1"},
        {234,"44"},{235,"44"},{262,"49"},{208,"33"},{222,"39"},{214,"34"},{505,"61"},{530,"64"},{440,"81"},{441,"81"},
        {450,"82"},{460,"86"},{454,"852"},{525,"65"},{502,"60"},{510,"62"},{515,"63"},{520,"66"},{452,"84"},{470,"880"},
        {410,"92"},{413,"94"},{429,"977"},{424,"971"},{420,"966"},{427,"974"},{419,"965"},{422,"968"},{426,"973"},
        {602,"20"},{655,"27"},{621,"234"},{639,"254"},{724,"55"},{722,"54"},{334,"52"},{730,"56"},{732,"57"},{716,"51"},
        {250,"7"},{255,"380"},{260,"48"},{286,"90"},{204,"31"},{206,"32"},{228,"41"},{232,"43"},{238,"45"},{240,"46"},
        {242,"47"},{244,"358"},{268,"351"},{272,"353"},{202,"30"},{425,"972"},{416,"962"},{415,"961"},{418,"964"},{432,"98"}};
    if (!imsi || strlen(imsi)<3) return NULL;
    int mcc=(imsi[0]-'0')*100+(imsi[1]-'0')*10+(imsi[2]-'0');
    for (size_t i=0;i<sizeof(t)/sizeof(t[0]);i++) if (t[i].mcc==mcc) return t[i].cc;
    return NULL;
}
static void call_dial(const char *tag, const char *number)
{
    struct sc *c;
    for (int i = 0; i < CALL_MAX; i++) if (calls[i].st != CS_FREE && calls[i].out && !strcmp(calls[i].tag, tag)) return;   /* (same dial again) */
    int full = calls_live() >= 2;
    if (!(c = call_new())) { printf("%s call: dial refused: no slot\n", ts_now()); return; }
    c->out = 1; c->st = CS_OUT_TRY; c->lcseq = 2;
    snprintf(c->tag, sizeof(c->tag), "%s", tag);
    if (full) { call_gone(c, "failed", "two calls up already"); return; }
    if (!call_registered) { call_gone(c, "network", "VoLTE is not registered"); return; }
    char num[48]; size_t n = 0;
    int clir = 0;
    const char *q = number;
    if (!strncmp(q, "*31#", 4)) { clir = 1; q += 4; }        /* withhold my number for this call */
    else if (!strncmp(q, "#31#", 4)) { clir = -1; q += 4; }  /* show it for this call */
    for (; *q && n + 1 < sizeof(num); q++) if ((*q >= '0' && *q <= '9') || (*q == '+' && !n) || *q == '*' || *q == '#') num[n++] = *q;
    num[n] = 0;
    snprintf(c->number, sizeof(c->number), "%s", num);
    /* the number as the network wants it: the SIM's MCC gives the country code; 00.. and +.. are international, a
     * leading 0 is the trunk prefix, a national number gets +CC, short codes (under 7 digits, * or #) stay local
     * (phone-context = the SIM's IMS domain) */
    char tel[120]; const char *cc = mcc_country_code(cfg.imsi);
    if (num[0] == '+') snprintf(tel, sizeof(tel), "tel:%s", num);
    else if (!strncmp(num, "00", 2)) snprintf(tel, sizeof(tel), "tel:+%s", num + 2);
    else if (cc && !strchr(num, '*') && !strchr(num, '#') && strlen(num) >= 7)
        snprintf(tel, sizeof(tel), "tel:+%s%s", cc, num[0] == '0' ? num + 1 : num);
    else snprintf(tel, sizeof(tel), "tel:%s;phone-context=%s", num, cfg.realm);
    snprintf(c->branch, sizeof(c->branch), "z9hG4bK%08x%04x", arc4random(), arc4random() & 0xffff);
    snprintf(c->cid, sizeof(c->cid), "%08x%08x@[%s]", arc4random(), arc4random(), cfg.ip6);
    snprintf(c->local, sizeof(c->local), "<%s>;tag=%08x", sms_impu[0] ? sms_impu : "sip:anonymous@anonymous.invalid", arc4random());
    snprintf(c->remote, sizeof(c->remote), "<%s>", tel);
    snprintf(c->ruri, sizeof(c->ruri), "%s", tel);
    char sdp[900]; sdp_ours(c, sdp, sizeof(sdp), 0);
    char m[6000];
    int o = snprintf(m, sizeof(m),
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n%s"
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 INVITE\r\n"
        "Contact: <sip:%s@[%s]:%d;transport=tcp>;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
        "Accept-Contact: *;+g.3gpp.icsi-ref=\"urn%%3Aurn-7%%3A3gpp-service.ims.icsi.mmtel\"\r\n"
        "P-Preferred-Service: urn:urn-7:3gpp-service.ims.icsi.mmtel\r\n"
        "P-Preferred-Identity: <%s>\r\n"
        "P-Early-Media: supported\r\n"
        "%s"
        "Allow: INVITE,ACK,CANCEL,BYE,UPDATE,PRACK,OPTIONS,MESSAGE,INFO\r\nSupported: 100rel,replaces,timer\r\n"
        "Session-Expires: 1800\r\nMin-SE: 90\r\n"
        "Content-Type: application/sdp\r\nContent-Length: %zu\r\n\r\n%s",
        tel, call_ip6, call_portc, c->branch, sms_route, c->local, c->remote, c->cid,
        call_user, call_ip6, call_ports, sms_impu[0] ? sms_impu : "sip:anonymous@anonymous.invalid",
        clir > 0 ? "Privacy: id\r\n" : clir < 0 ? "Privacy: none\r\n" : "", strlen(sdp), sdp);
    printf("%s call %u: OUTGOING%s\n", ts_now(), c->id, clir > 0 ? " (number withheld)" : "");
    if (!MSG_FITS(m, o)) { printf("%s call %u: INVITE too long, not sent\n", ts_now(), c->id); call_gone(c, "failed", "INVITE too long"); return; }
    if (send(call_sock, m, (size_t)o, 0) != o) { printf("%s call %u: INVITE send failed: %s\n", ts_now(), c->id, strerror(errno)); call_gone(c, "failed", "could not send"); return; }
    printf("%s call: --> INVITE (call %u)\n", ts_now(), c->id);
    calls_snapshot(1);
}

/* the ACK of a failure response to our INVITE: same branch, to the INVITE's Request-URI (RFC 3261 17.1.1.3) */
static void call_ack_failure(struct sc *c, const char *r)
{
    char to[400], m[3000];
    if (!hdr(r, "To", to, sizeof(to))) snprintf(to, sizeof(to), "%s", c->remote);
    int o = snprintf(m, sizeof(m), "ACK %s SIP/2.0\r\nVia: SIP/2.0/TCP [%s]:%d;branch=%s;rport\r\nMax-Forwards: 70\r\n%s"
        "From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n",
        c->ruri, call_ip6, call_portc, c->branch, sms_route, c->local, to, c->cid);
    if (!MSG_FITS(m, o)) { printf("%s call: --> ACK (of a failure) NOT SENT (too long)\n", ts_now()); return; }
    sip_send(call_sock, m, (size_t)o, "ACK (of a failure)");
}
/* the end reason of a failure code, for the call screen and the log */
static const char *fail_tag(int code)
{
    switch (code) {
    case 486: case 600: return "busy";
    case 603: return "declined";
    case 404: case 484: case 604: return "invalid-number";
    case 480: return "unavailable";
    case 408: case 504: return "no-answer";
    case 487: return "cancelled";
    case 403: return "forbidden";
    case 488: case 606: return "incompatible";
    case 503: case 500: return "network";
    default: return code >= 300 && code < 400 ? "redirected" : "failed";
    }
}

/* a response to something of ours in a call; returns 1 when it was a call's */
static int call_response(const char *r)
{
    char ci[200];
    if (!hdr(r, "Call-ID", ci, sizeof(ci))) return 0;
    struct sc *c = call_by_cid(ci);
    int code = sip_code(r), cseq = sip_cseq_n(r);
    if (!c) {
        /* an answer to an INVITE of ours already given up (the callee picked up just as we cancelled): ACK + BYE */
        if (code >= 200 && code < 300 && sip_cseq_is(r, "INVITE")) {
            struct sc tmp; memset(&tmp, 0, sizeof(tmp));
            hdr(r, "From", tmp.local, sizeof(tmp.local)); hdr(r, "To", tmp.remote, sizeof(tmp.remote));
            snprintf(tmp.cid, sizeof(tmp.cid), "%s", ci);
            char ct[400]; if (hdr(r, "Contact", ct, sizeof(ct))) uri_of(ct, tmp.target, sizeof(tmp.target));
            route_from_rr(r, 1, tmp.route, sizeof(tmp.route));
            call_request_out(&tmp, "ACK", (unsigned)cseq, NULL, NULL, NULL);
            call_request_out(&tmp, "BYE", (unsigned)cseq + 1, NULL, NULL, NULL);
            printf("%s call: an answer to a call already ended: ACK + BYE\n", ts_now());
            return 1;
        }
        return 0;
    }
    if (sip_cseq_is(r, "CANCEL") || sip_cseq_is(r, "BYE") || sip_cseq_is(r, "PRACK") || sip_cseq_is(r, "ACK")) {
        if (code == 481 && sip_cseq_is(r, "PRACK") && c->st == CS_OUT_ALERT) printf("%s call %u: 481 to our PRACK (ignored)\n", ts_now(), c->id);
        return 1;
    }
    if (sip_cseq_is(r, "UPDATE")) {
        if (c->ri == 2 && (unsigned)cseq == c->ri_cseq && code >= 200) {
            c->ri = 0;
            if (code < 300) { call_session_timer(c, r, 1); c->refreshed = time(NULL); }
            else if (code == 481 || code == 408) call_gone(c, "failed", "the network lost the call (refresh refused)");
            else c->peer_update = 0;                         /* (try a re-INVITE next time) */
        }
        return 1;
    }
    /* INVITE responses: our first INVITE (CSeq 1, outgoing) or a re-INVITE (hold/resume/refresh) */
    if (c->ri == 1 && (unsigned)cseq == c->ri_cseq) {
        if (code < 200) return 1;
        if (code < 300) {
            call_request_out(c, "ACK", (unsigned)cseq, NULL, NULL, NULL);
            c->ri = 0;
            if (sip_has_body(r)) { int theirs = 0; sdp_parse(c, r, &theirs); }
            c->lhold = c->want_hold;
            call_session_timer(c, r, 1);
            c->refreshed = time(NULL);
            printf("%s call %u: %s\n", ts_now(), c->id, c->lhold ? "on hold" : "resumed");
        } else {
            call_request_out(c, "ACK", (unsigned)cseq, NULL, NULL, NULL);   /* (non-2xx in the dialog: ACK too) */
            c->ri = 0;
            if (code == 491) { c->ri_retry = time(NULL) + 2 + (time_t)(arc4random() % 3); printf("%s call %u: 491 (glare): trying again\n", ts_now(), c->id); }
            else if (code == 481 || code == 408) { call_gone(c, "failed", "the network lost the call"); return 1; }
            else { printf("%s call %u: re-INVITE refused %d: hold state stays\n", ts_now(), c->id, code); c->want_hold = c->lhold; }
        }
        call_media_sync(); calls_snapshot(1);
        return 1;
    }
    if (!c->out || cseq != 1) return 1;                      /* (a stray answer) */
    char to[400];
    if (c->st == CS_GONE) {                                  /* after our CANCEL: a late 2xx -> ACK + BYE, a 487 -> ACK */
        if (code >= 200 && code < 300) {
            if (hdr(r, "To", to, sizeof(to))) snprintf(c->remote, sizeof(c->remote), "%s", to);
            char ct[400]; if (hdr(r, "Contact", ct, sizeof(ct))) uri_of(ct, c->target, sizeof(c->target));
            route_from_rr(r, 1, c->route, sizeof(c->route));
            call_request_out(c, "ACK", 1, NULL, NULL, NULL); call_bye(c);
            printf("%s call %u: answered after we gave up: ACK + BYE\n", ts_now(), c->id);
        } else if (code >= 300) call_ack_failure(c, r);
        return 1;
    }
    printf("%s call %u: <-- %d\n", ts_now(), c->id, code);
    c->heard = 1;
    if (code == 100) return 1;
    if (hdr(r, "To", to, sizeof(to)) && strstr(to, "tag=")) snprintf(c->remote, sizeof(c->remote), "%s", to);
    if (code > 100 && code < 300) {
        char ct[400]; if (hdr(r, "Contact", ct, sizeof(ct))) uri_of(ct, c->target, sizeof(c->target));
        char rr[1200]; route_from_rr(r, 1, rr, sizeof(rr)); if (rr[0]) snprintf(c->route, sizeof(c->route), "%s", rr);
        if (sip_has_body(r)) {
            sdp_parse(c, r, NULL);
            if (code < 200) {
                char pem[80] = "";
                hdr(r, "P-Early-Media", pem, sizeof(pem));
                /* early media unless the network says it is not there (P-Early-Media: inactive) */
                c->early = !strstr(pem, "inactive");
                if (c->early) printf("%s call %u: early media (network audio before the answer)\n", ts_now(), c->id);
            }
        }
        char req[120], rs[40];
        if (code < 200 && hdr(r, "Require", req, sizeof(req)) && strstr(req, "100rel") && hdr(r, "RSeq", rs, sizeof(rs))) {
            unsigned rseq = (unsigned)atoi(rs);
            if (rseq > c->rseq_acked) { char x[80]; snprintf(x, sizeof(x), "RAck: %u 1 INVITE\r\n", rseq); call_request_out(c, "PRACK", c->lcseq++, x, NULL, NULL); c->rseq_acked = rseq; }
        }
    }
    if (code > 100 && code < 200) {
        if (c->st == CS_OUT_TRY) { c->st = CS_OUT_ALERT; c->alert_at = time(NULL); }
        call_media_sync(); calls_snapshot(1);
    } else if (code >= 200 && code < 300) {
        call_request_out(c, "ACK", 1, NULL, NULL, NULL);
        if (c->st != CS_UP) {
            c->st = CS_UP; c->answered = time(NULL); c->early = 0;
            call_session_timer(c, r, 1);
            printf("%s call %u: answered\n", ts_now(), c->id);
        }
        call_media_sync(); calls_snapshot(1);
    } else if (code >= 300) {
        call_ack_failure(c, r);
        char why[64]; snprintf(why, sizeof(why), "%d from the network", code);
        call_gone(c, fail_tag(code), why);
    }
    return 1;
}

/* ---------------------------------------------------------------- user actions */

static void call_end_one(struct sc *c, const char *why)
{
    switch (c->st) {
    case CS_OUT_TRY: case CS_OUT_ALERT: call_cancel(c, NULL); call_gone(c, "local", why); break;
    case CS_IN_RING: call_respond(c, calls_live() > 1 ? "486 Busy Here" : "603 Decline", NULL, NULL); call_gone(c, "declined", why); break;
    case CS_IN_ANS: case CS_UP: call_bye(c); call_gone(c, "local", why); break;
    default: break;
    }
}
static void call_answer(struct sc *c)
{
    if (c->st != CS_IN_RING) return;
    /* answering while another call is up and not held: the call screen asks hold (or end) first; if it did not, the
     * other call is put on hold here so the two never share the speaker */
    for (int i = 0; i < CALL_MAX; i++) {
        struct sc *o = &calls[i];
        if (o != c && o->st == CS_UP && !o->want_hold) { o->want_hold = 1; call_reinvite_out(o, 0); }
    }
    char sdp[900], extra[200] = "";
    sdp_ours(c, sdp, sizeof(sdp), 0);
    /* session timer (RFC 4028 9): the INVITE's interval echoed; the caller's side stays refresher unless it named us */
    if (c->inv_se[0]) {
        int uas = strstr(c->inv_se, "refresher=uas") != NULL;
        snprintf(extra, sizeof(extra), "Session-Expires: %d;refresher=%s\r\nRequire: timer\r\n", atoi(c->inv_se) < 90 ? 90 : atoi(c->inv_se), uas ? "uas" : "uac");
        c->se = atoi(c->inv_se) < 90 ? 90 : atoi(c->inv_se); c->we_refresh = uas;
    }
    call_respond(c, "200 OK", sdp, extra[0] ? extra : NULL);
    c->st = CS_IN_ANS; c->answered = time(NULL); c->refreshed = c->answered;
    printf("%s call %u: answered here\n", ts_now(), c->id);
    call_media_sync(); calls_snapshot(1);
}

/* the call screen's commands, in their order */
static int cmd_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void call_ui_poll(void)
{
    /* the call screen's Speaker button (libredwood-calls in InCallService posts these) */
    static int tsp = -1, tea = -1;
    if (tsp < 0) { notify_register_check("com.redwood.call.speaker", &tsp); notify_register_check("com.redwood.call.earpiece", &tea); }
    int ch = 0;
    if (tsp >= 0 && notify_check(tsp, &ch) == 0 && ch && call_session) { med.route = 0; printf("%s call: screen -> speaker\n", ts_now()); }
    ch = 0;
    if (tea >= 0 && notify_check(tea, &ch) == 0 && ch && call_session) { med.route = 1; printf("%s call: screen -> earpiece\n", ts_now()); }
    DIR *d = opendir(CALL_UI_DIR); if (!d) return;
    char *names[64]; int nn = 0; struct dirent *e;
    while ((e = readdir(d)) && nn < 64) if (!strncmp(e->d_name, "cmd.", 4) && !strstr(e->d_name, ".tmp")) names[nn++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)nn, sizeof(names[0]), cmd_cmp);
    for (int k = 0; k < nn; k++) {
        char fn[300], line[200] = ""; struct stat cst;
        snprintf(fn, sizeof(fn), CALL_UI_DIR "/%s", names[k]); free(names[k]);
        int stale = !stat(fn, &cst) && time(NULL) - cst.st_mtime > 15;
        FILE *f = fopen(fn, "r"); if (f) { if (!fgets(line, sizeof(line), f)) line[0] = 0; fclose(f); }
        unlink(fn);
        line[strcspn(line, "\r\n")] = 0;
        char verb[20] = ""; unsigned id = 0;
        if (sscanf(line, "%19s", verb) != 1) continue;
        if (!strcmp(verb, "dial")) {
            char tag[80], num[64];
            if (sscanf(line, "%*s %79s %63s", tag, num) != 2) continue;
            if (stale) { printf("%s call: screen -> dial (stale, dropped)\n", ts_now()); continue; }
            printf("%s call: screen -> dial\n", ts_now());
            call_dial(tag, num);
            continue;
        }
        if (!strcmp(verb, "mute")) { unsigned on = 0; sscanf(line, "%*s %u", &on); med.muted = (int)on; printf("%s call: screen -> mute %u\n", ts_now(), on); continue; }
        if (!strcmp(verb, "route")) { char w[20] = ""; sscanf(line, "%*s %19s", w); med.route = strcmp(w, "speaker") != 0; printf("%s call: screen -> route %s\n", ts_now(), w); continue; }
        if (!strcmp(verb, "cancel")) {                      /* (an end for a dial not yet named: by its tag) */
            char tg[80] = ""; sscanf(line, "%*s %79s", tg);
            for (int i = 0; i < CALL_MAX; i++) if (calls[i].out && calls[i].st != CS_FREE && !strcmp(calls[i].tag, tg)) call_end_one(&calls[i], "ended here");
            continue;
        }
        if (sscanf(line, "%*s %u", &id) != 1) continue;
        struct sc *c = call_by_id(id);
        printf("%s call: screen -> %s %u%s\n", ts_now(), verb, id, c ? "" : " (no such call)");
        if (!c) continue;
        if (!strcmp(verb, "answer")) call_answer(c);
        else if (!strcmp(verb, "end")) call_end_one(c, "ended here");
        else if (!strcmp(verb, "hold")) {
            unsigned on = 0; sscanf(line, "%*s %*u %u", &on);
            c->want_hold = (int)on;
            if (c->st == CS_UP && c->want_hold != c->lhold) call_reinvite_out(c, 0);
            call_media_sync(); calls_snapshot(1);
        } else if (!strcmp(verb, "dtmf")) {
            char dg[40] = ""; sscanf(line, "%*s %*u %39s", dg);
            if (call_media_slot == c->slot && med.run) media_dtmf(dg);
        }
    }
}

/* ---------------------------------------------------------------- timers, once a second */

static void call_tick(void)
{
    call_ui_poll();
    time_t now = time(NULL);
    for (int i = 0; i < CALL_MAX; i++) {
        struct sc *c = &calls[i];
        switch (c->st) {
        case CS_IN_RING:
            if (now - c->at >= CALL_RING_S) { call_respond(c, "480 Temporarily Unavailable", NULL, NULL); call_gone(c, "missed", "not answered"); }
            break;
        case CS_IN_ANS:
            /* our 200 not confirmed by an ACK: the network resends it for 32 s; voice already flowing counts as up */
            if (call_media_slot == c->slot && med.rx_pkts > 0) { c->st = CS_UP; printf("%s call %u: up (voice flows; no ACK yet)\n", ts_now(), c->id); calls_snapshot(1); }
            else if (now - c->answered >= 32) { call_bye(c); call_gone(c, "failed", "no ACK and no voice in 32 s after our 200"); }
            break;
        case CS_OUT_TRY:
            /* 32 s is for no response at all (RFC 3261 timer B); after 100 Trying the network is working on it -- its
             * 18x can come late (Jio held them ~20 s, 2026-11-15) -- so a heard call waits like a ringing one */
            if (!c->heard && now - c->at >= CALL_TRYING_S) { call_cancel(c, NULL); call_gone(c, "no-answer", "no response from the network"); }
            else if (c->heard && now - c->at >= CALL_ALERT_S) { call_cancel(c, NULL); call_gone(c, "no-answer", "not answered"); }
            break;
        case CS_OUT_ALERT:
            if (now - c->alert_at >= CALL_ALERT_S) { call_cancel(c, NULL); call_gone(c, "no-answer", "not answered"); }
            break;
        case CS_UP:
            if (c->ri && now - c->ri_at > 32) { printf("%s call %u: no answer to our %s\n", ts_now(), c->id, c->ri == 1 ? "re-INVITE" : "UPDATE"); c->ri = 0; c->want_hold = c->lhold; calls_snapshot(1); }
            if (!c->ri && c->ri_retry && now >= c->ri_retry) { c->ri_retry = 0; call_reinvite_out(c, 0); }
            if (!c->ri && c->want_hold != c->lhold && !c->ri_retry) call_reinvite_out(c, 0);
            if (!c->ri && c->se && c->we_refresh && now - c->refreshed >= c->se / 2) {
                if (c->peer_update) call_update_out(c); else call_reinvite_out(c, 1);
            }
            /* the network refreshes: if it stops (expiry passed with no refresh), the call is gone at its side */
            if (c->se && !c->we_refresh && now - c->refreshed > c->se + 32 && now - c->refreshed < c->se + 34)
                printf("%s call %u: the network's session refresh is overdue (call kept)\n", ts_now(), c->id);
            break;
        case CS_GONE:
            if (now - c->gone_at >= CALL_KEEP_S) { c->st = CS_FREE; calls_snapshot(1); }
            break;
        }
    }
    call_media_sync();
    calls_snapshot(0);
}

/* leaving (the registration is gone, or a stop): every call closed at the network as far as we still can, and on
 * the call screen, so neither side keeps a call nobody has */
static void call_abort_all(const char *why)
{
    for (int i = 0; i < CALL_MAX; i++) {
        struct sc *c = &calls[i];
        switch (c->st) {
        case CS_OUT_TRY: case CS_OUT_ALERT: call_cancel(c, NULL); break;
        case CS_IN_RING: call_respond(c, "480 Temporarily Unavailable", NULL, NULL); break;
        case CS_IN_ANS: case CS_UP: call_bye(c); break;
        default: continue;
        }
        call_gone(c, "failed", why);
    }
    call_registered = 0;
    calls_snapshot(1);
}

/* a P-CSCF connection closed: calls whose INVITE came on it answer on the registration connection from now on */
static void call_fd_closed(int fd)
{
    for (int i = 0; i < CALL_MAX; i++) if (calls[i].st != CS_FREE && calls[i].fd == fd) calls[i].fd = call_sock;
}
