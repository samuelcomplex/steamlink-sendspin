// slcec: send HDMI-CEC commands from the Steam Link, e.g. volume and power to an AV receiver.
//
//   slcec volup [N] | voldown [N] | mute | standby | on | key HEX | raw OPCODE [BYTES...]
//   slcec status          ask the receiver for its volume and mute state (prints "VOL MUTED")
//   slcec monitor [SEC]   print every CEC message seen on the bus
//
// The Steam Link has no /dev/cec. CEC goes through Marvell's PE (presentation engine) over its
// RPC, reached through libPEAgent.so in /home/steam/lib. Valve's cecmanager owns the CEC session
// (logical address allocation, incoming messages); this tool only opens its own PE handle and
// posts frames, the same call libDMPCECSrv.so makes, so cecmanager keeps running untouched.
//
// Frame layout handed to MV_PE_VOutCECSendMessage(hPE, 1, &msg), from libDMPCECSrv.so:
//   byte 0 initiator, byte 1 destination, u16 opcode at 2, operands from byte 4, 32 bytes total.
//
// Receiving: MV_PE_RegisterEventCallBack(hPE, 0x3b, cb, ctx, 0) delivers CEC events as
// cb(0x3b, info, ctx), info = {u32 kind, u32 status, ...}: kind 1 = a frame we sent
// (status: 0 = acknowledged), kind 2 = a frame received, with the frame from byte 8 in the
// same layout as above.

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LIBDIR "/home/steam/lib"

typedef struct {
    uint8_t src;
    uint8_t dst;
    uint16_t opcode;
    uint8_t operands[28];
} cec_msg;

typedef int (*osal_init_fn)(void);
typedef int (*osal_exit_fn)(void);
typedef int (*pe_init_fn)(void **handle);
typedef int (*pe_remove_fn)(void *handle);
typedef int (*pe_send_fn)(void *handle, int vout, cec_msg *msg);
typedef int (*pe_event_cb)(unsigned code, void *info, void *ctx);
typedef int (*pe_reg_fn)(void *handle, unsigned code, pe_event_cb cb, void *ctx, unsigned flags);
typedef int (*pe_unreg_fn)(void *handle, unsigned code, pe_event_cb cb);

#define CEC_EVENT 0x3b

static volatile int got_status = -1;   // last Report Audio Status operand, -1 = none yet
static volatile int monitoring;

static int on_cec_event(unsigned code, void *info, void *ctx)
{
    (void)ctx;
    if (code != CEC_EVENT || !info) return 0;
    const uint32_t *w = (const uint32_t *)info;
    if (w[0] == 2) {
        const cec_msg *m = (const cec_msg *)((const uint8_t *)info + 8);
        if (m->opcode == 0x7a) got_status = m->operands[0];
        if (monitoring) {
            fprintf(stderr, "rx %x->%x opcode 0x%02x:", m->src, m->dst, m->opcode);
            for (int i = 0; i < 6; i++) fprintf(stderr, " %02x", m->operands[i]);
            fprintf(stderr, "\n");
        }
    } else if (w[0] == 1 && monitoring) {
        fprintf(stderr, "tx opcode 0x%02x: %s\n", w[1], w[2] == 0 ? "acknowledged" : "not acknowledged");
    }
    return 0;
}

static pe_send_fn pe_send;
static void *pe;
static int src = 4, dst = 5, verbose;

static int send_frame(uint16_t opcode, const uint8_t *ops, int n)
{
    cec_msg m;
    memset(&m, 0, sizeof m);
    m.src = (uint8_t)src;
    m.dst = (uint8_t)dst;
    m.opcode = opcode;
    if (n > (int)sizeof m.operands) n = sizeof m.operands;
    if (n > 0) memcpy(m.operands, ops, n);
    int rc = pe_send(pe, 1, &m);
    if (verbose || rc < 0)
        fprintf(stderr, "slcec: %x->%x opcode 0x%02x: rc 0x%08x\n", src, dst, opcode, rc);
    return rc < 0 ? 1 : 0;
}

// A remote-control key: User Control Pressed, short hold, User Control Released.
static int press(uint8_t key)
{
    int err = send_frame(0x44, &key, 1);
    usleep(120 * 1000);
    err |= send_frame(0x45, NULL, 0);
    usleep(150 * 1000);
    return err;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: slcec [-d DEST] [-s SRC] [-v] COMMAND\n"
        "  volup [N]        volume up N steps (default 1)\n"
        "  voldown [N]      volume down N steps\n"
        "  mute             toggle mute\n"
        "  standby          put DEST in standby\n"
        "  on               power on DEST (Power On Function key)\n"
        "  power            power toggle key\n"
        "  key HEX          press any CEC user-control key code\n"
        "  raw OPCODE [B..] send any frame, hex bytes\n"
        "  status           print the receiver's volume (0-100) and mute: \"VOL MUTED\"\n"
        "  monitor [SEC]    print CEC messages on the bus for SEC seconds (default 30)\n"
        "DEST defaults to 5 (audio system / AV receiver), SRC to 4 (the Steam Link).\n");
}

static void *load(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, LIBDIR "/%s", name);
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    return h;
}

int main(int argc, char **argv)
{
    // The Marvell libraries find each other through LD_LIBRARY_PATH; set it and start over.
    const char *llp = getenv("LD_LIBRARY_PATH");
    if (!llp || !strstr(llp, LIBDIR)) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s%s%s", LIBDIR, llp && *llp ? ":" : "", llp ? llp : "");
        setenv("LD_LIBRARY_PATH", buf, 1);
        execv("/proc/self/exe", argv);
    }

    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dst = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) src = (int)strtol(argv[++i], NULL, 16);
        else { usage(); return 2; }
    }
    if (i >= argc) { usage(); return 2; }
    const char *cmd = argv[i++];

    void *peagent = load("libPEAgent.so");
    if (!peagent) { fprintf(stderr, "slcec: %s\n", dlerror()); return 1; }
    osal_init_fn osal_init = (osal_init_fn)dlsym(RTLD_DEFAULT, "MV_OSAL_Init");
    osal_exit_fn osal_exit = (osal_exit_fn)dlsym(RTLD_DEFAULT, "MV_OSAL_Exit");
    pe_init_fn pe_init = (pe_init_fn)dlsym(RTLD_DEFAULT, "MV_PE_Init");
    pe_remove_fn pe_remove = (pe_remove_fn)dlsym(RTLD_DEFAULT, "MV_PE_Remove");
    pe_send = (pe_send_fn)dlsym(RTLD_DEFAULT, "MV_PE_VOutCECSendMessage");
    pe_reg_fn pe_reg = (pe_reg_fn)dlsym(RTLD_DEFAULT, "MV_PE_RegisterEventCallBack");
    pe_unreg_fn pe_unreg = (pe_unreg_fn)dlsym(RTLD_DEFAULT, "MV_PE_UnRegisterEventCallBack");
    if (!osal_init) { load("libOSAL.so"); osal_init = (osal_init_fn)dlsym(RTLD_DEFAULT, "MV_OSAL_Init");
                      osal_exit = (osal_exit_fn)dlsym(RTLD_DEFAULT, "MV_OSAL_Exit"); }
    if (!osal_init || !pe_init || !pe_send) {
        fprintf(stderr, "slcec: Marvell PE functions not found\n");
        return 1;
    }

    // The Marvell libraries are chatty on stdout; keep that out of our output unless -v.
    int saved = dup(1);
    if (!verbose) { fflush(stdout); freopen("/dev/null", "w", stdout); }
    osal_init();
    int rc = pe_init(&pe);
    if (rc != 0 || !pe) {
        fprintf(stderr, "slcec: MV_PE_Init failed: 0x%08x\n", rc);
        return 1;
    }

    int listening = 0;
    if (!strcmp(cmd, "status") || !strcmp(cmd, "monitor")) {
        if (!pe_reg) { fprintf(stderr, "slcec: no event callback in this firmware\n"); return 1; }
        int r = pe_reg(pe, CEC_EVENT, on_cec_event, NULL, 0);
        if (verbose) fprintf(stderr, "slcec: register event callback: 0x%08x\n", r);
        listening = 1;
    }

    int n = i < argc ? atoi(argv[i]) : 1;
    if (n < 1) n = 1;
    if (n > 50) n = 50;
    int err = 0;
    if (!strcmp(cmd, "volup")) { while (n--) err |= press(0x41); }
    else if (!strcmp(cmd, "voldown")) { while (n--) err |= press(0x42); }
    else if (!strcmp(cmd, "mute")) err = press(0x43);
    else if (!strcmp(cmd, "power")) err = press(0x40);
    else if (!strcmp(cmd, "on")) err = press(0x6d);
    else if (!strcmp(cmd, "standby")) err = send_frame(0x36, NULL, 0);
    else if (!strcmp(cmd, "key") && i < argc) err = press((uint8_t)strtol(argv[i], NULL, 16));
    else if (!strcmp(cmd, "raw") && i < argc) {
        uint16_t op = (uint16_t)strtol(argv[i++], NULL, 16);
        uint8_t ops[28];
        int k = 0;
        for (; i < argc && k < 28; i++) ops[k++] = (uint8_t)strtol(argv[i], NULL, 16);
        err = send_frame(op, ops, k);
    } else if (!strcmp(cmd, "status")) {
        err = 1;
        for (int attempt = 0; attempt < 3 && got_status < 0; attempt++) {
            send_frame(0x71, NULL, 0);   // Give Audio Status
            for (int t = 0; t < 40 && got_status < 0; t++) usleep(25 * 1000);
        }
        if (got_status >= 0) {
            int vol = got_status & 0x7f;
            fflush(stdout);
            dup2(saved, 1);
            if (vol == 0x7f) printf("unknown %d\n", (got_status >> 7) & 1);
            else printf("%d %d\n", vol, (got_status >> 7) & 1);
            fflush(stdout);
            if (!verbose) freopen("/dev/null", "w", stdout);
            err = 0;
        } else {
            fprintf(stderr, "slcec: no audio status from %x\n", dst);
        }
    } else if (!strcmp(cmd, "monitor")) {
        int secs = i < argc ? atoi(argv[i]) : 30;
        monitoring = 1;
        fprintf(stderr, "slcec: listening for %d s; use the receiver's remote now\n", secs);
        send_frame(0x71, NULL, 0);
        for (int t = 0; t < secs * 10; t++) usleep(100 * 1000);
    } else { usage(); err = 2; }

    usleep(100 * 1000);  // let the PE put the last frame on the wire
    if (listening && pe_unreg) pe_unreg(pe, CEC_EVENT, on_cec_event);
    if (pe_remove) pe_remove(pe);
    if (osal_exit) osal_exit();
    fflush(stdout);
    dup2(saved, 1);
    return err;
}
