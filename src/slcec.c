// slcec: send HDMI-CEC commands from the Steam Link, e.g. volume and power to an AV receiver.
//
//   slcec volup [N] | voldown [N] | mute | standby | on | key HEX | raw OPCODE [BYTES...]
//
// The Steam Link has no /dev/cec. CEC goes through Marvell's PE (presentation engine) over its
// RPC, reached through libPEAgent.so in /home/steam/lib. Valve's cecmanager owns the CEC session
// (logical address allocation, incoming messages); this tool only opens its own PE handle and
// posts frames, the same call libDMPCECSrv.so makes, so cecmanager keeps running untouched.
//
// Frame layout handed to MV_PE_VOutCECSendMessage(hPE, 1, &msg), from libDMPCECSrv.so:
//   byte 0 initiator, byte 1 destination, u16 opcode at 2, operands from byte 4, 32 bytes total.

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
    } else { usage(); err = 2; }

    usleep(100 * 1000);  // let the PE put the last frame on the wire
    if (pe_remove) pe_remove(pe);
    if (osal_exit) osal_exit();
    fflush(stdout);
    dup2(saved, 1);
    return err;
}
