/* GameCube controllers straight from USB on macOS (padusb.h, as hle_padusb.c on Windows): the Wii
 * U GameCube adapter (WUP-028, USB 057e:0337), read with libusb on a thread of its own.
 *
 * On a Mac the adapter is held by GCAdapterDriver (a DriverKit extension: without one macOS's HID
 * driver takes it and it sends nothing), which publishes no HID device, so SDL never sees it; the
 * driver leaves it to libusb, as Dolphin reads it. libusb-1.0 is loaded at run time (Homebrew's,
 * or one in the app's Frameworks), so a Mac without it just has no adapter.
 *
 * The protocol, as Dolphin's InputCommon/GCAdapter.cpp speaks it: 0x13 on the OUT endpoint
 * starts the reports; each is 37 bytes on the IN endpoint, 0x21 then 9 bytes a port:
 *   [status] bit 4 wired, bit 5 wireless   [buttons] A B X Y LEFT RIGHT DOWN UP   [buttons] START Z R L
 *   [stick x] [stick y] [c-stick x] [c-stick y] [L] [R]   (0-255, +y up)
 * and 0x11 with a motor byte a port (0 stop, 1 rumble, 2 stop hard) drives the rumble. Sticks and
 * triggers are scaled as hle_padusb.c scales them: a stick's centre where it is when the
 * controller is plugged in, its reach 80 (or as far as it goes); a trigger's rest where it is
 * then, its top 140 past that. SSBU_GC_USB=0 leaves the adapter alone. */
#include <dlfcn.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "padusb.h"

enum { VID = 0x057e, PID = 0x0337, REPORT = 37, EP_IN = 0x81, EP_OUT = 0x02 };   /* (the adapter's two endpoints) */
enum { ERR_TIMEOUT = -7, ERR_PIPE = -9 };   /* LIBUSB_ERROR_* */

/* The part of libusb-1.0's API used here (libusb.h: stable since 1.0.0). */
typedef struct UsbDeviceDescriptor {
    uint8_t bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
} UsbDeviceDescriptor;

static struct {
    int (*init)(void **ctx);
    ssize_t (*get_device_list)(void *ctx, void ***list);
    void (*free_device_list)(void **list, int unref);
    int (*get_device_descriptor)(void *dev, UsbDeviceDescriptor *desc);
    int (*open)(void *dev, void **handle);
    void (*close)(void *handle);
    int (*claim_interface)(void *handle, int n);
    int (*release_interface)(void *handle, int n);
    int (*clear_halt)(void *handle, unsigned char endpoint);
    int (*interrupt_transfer)(void *handle, unsigned char endpoint, unsigned char *data, int length, int *done, unsigned timeout);
    const char *(*error_name)(int err);
} usb;
#define USB_FUNCS(X) X(init) X(get_device_list) X(free_device_list) X(get_device_descriptor) X(open) X(close) \
    X(claim_interface) X(release_interface) X(clear_halt) X(interrupt_transfer) X(error_name)

static void *ctx;

/* ---- the ports, shared with the input thread (ports 0-3: the adapter's) -------------------- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static PadUsbState ports[PADUSB_PORTS];
static int rumble_want[4];

void padusb_poll(PadUsbState out[PADUSB_PORTS]) {
    pthread_mutex_lock(&lock);
    memcpy(out, ports, sizeof ports);
    pthread_mutex_unlock(&lock);
}

void padusb_rumble(int port, int on) {
    if (port < 0 || port >= 4) return;
    pthread_mutex_lock(&lock);
    rumble_want[port] = on;
    pthread_mutex_unlock(&lock);
}

/* ---- the adapter thread -------------------------------------------------------------------- */
typedef struct Axis { int centre, reach; } Axis;

static i32 stick_value(Axis *a, int v) {
    int dv = v - a->centre, mag = dv < 0 ? -dv : dv;
    if (mag > a->reach) a->reach = mag;   /* a stick that reaches further than thought */
    long r = (long)dv * 32767 / a->reach;
    return (i32)(r > 32767 ? 32767 : r < -32767 ? -32767 : r);
}

/* The first adapter there is, opened with its interface claimed and its reports started; NULL
 * when there is none (or it can't be had: said once until it comes back). */
static void *adapter_find(void) {
    static int warned;
    void **list, *h = NULL;
    ssize_t n = usb.get_device_list(ctx, &list);
    int seen = 0, err = 0;
    const char *step = "";
    for (ssize_t i = 0; i < n && !h; i++) {
        UsbDeviceDescriptor d;
        if (usb.get_device_descriptor(list[i], &d) || d.idVendor != VID || d.idProduct != PID) continue;
        seen = 1;
        if ((err = usb.open(list[i], &h))) { h = NULL; step = "open"; continue; }
        if ((err = usb.claim_interface(h, 0))) { usb.close(h); h = NULL; step = "claim"; continue; }
        usb.clear_halt(h, EP_IN);   /* (a read cut short last time can leave the pipe stalled) */
        unsigned char start = 0x13;
        int done = 0;
        if ((err = usb.interrupt_transfer(h, EP_OUT, &start, 1, &done, 100))) {
            usb.release_interface(h, 0); usb.close(h); h = NULL; step = "start";
        }
    }
    if (n >= 0) usb.free_device_list(list, 1);
    if (h) { fprintf(stderr, "[input] GameCube adapter connected (libusb)\n"); warned = 0; }
    else if (seen && !warned) {
        fprintf(stderr, "[input] GameCube adapter found, but its %s failed (%s): is GCAdapterDriver on, "
                        "in System Settings > General > Login Items & Extensions?\n", step, usb.error_name(err));
        warned = 1;
    } else if (!seen) warned = 0;
    return h;
}

static void adapter_close(void *h) {
    fprintf(stderr, "[input] GameCube adapter disconnected\n");
    usb.release_interface(h, 0);
    usb.close(h);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 4; i++) ports[i].connected = 0;
    pthread_mutex_unlock(&lock);
}

static void adapter_report(const unsigned char *p, Axis axes[4][4], int trigger_rest[4][2], int trigger_top[4][2]) {
    static const struct { int off; unsigned char bit; u32 pu; } BUTTONS[] = {
        {1, 0x01, PU_A}, {1, 0x02, PU_B}, {1, 0x04, PU_X}, {1, 0x08, PU_Y}, {1, 0x10, PU_LEFT}, {1, 0x20, PU_RIGHT},
        {1, 0x40, PU_DOWN}, {1, 0x80, PU_UP}, {2, 0x01, PU_START}, {2, 0x02, PU_Z}, {2, 0x04, PU_R}, {2, 0x08, PU_L},
    };
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 4; i++) {
        const unsigned char *s = p + 1 + 9 * i;
        PadUsbState *o = &ports[i];
        int was = o->connected;
        o->connected = (s[0] & 0x30) != 0;   /* 0x10 a cable, 0x20 a WaveBird */
        if (!o->connected) continue;
        if (!was) {   /* a controller plugged in: its sticks' centres and triggers' rests as they are now */
            for (int k = 0; k < 4; k++) axes[i][k] = (Axis){s[3 + k] > 80 && s[3 + k] < 176 ? s[3 + k] : 128, 80};
            for (int t = 0; t < 2; t++) {
                trigger_rest[i][t] = s[7 + t] < 70 ? s[7 + t] : 30;
                trigger_top[i][t] = trigger_rest[i][t] + 140;
            }
        }
        o->buttons = 0;
        for (int b = 0; b < (int)(sizeof BUTTONS / sizeof *BUTTONS); b++)
            if (s[BUTTONS[b].off] & BUTTONS[b].bit) o->buttons |= BUTTONS[b].pu;
        o->lx = stick_value(&axes[i][0], s[3]);
        o->ly = -stick_value(&axes[i][1], s[4]);   /* (+y down, as SDL's) */
        o->rx = stick_value(&axes[i][2], s[5]);
        o->ry = -stick_value(&axes[i][3], s[6]);
        for (int t = 0; t < 2; t++) {
            int v = s[7 + t], rest = trigger_rest[i][t];
            if (v > trigger_top[i][t]) trigger_top[i][t] = v;
            int val = v <= rest ? 0 : (v - rest) * 32767 / (trigger_top[i][t] - rest);
            *(t ? &o->rt : &o->lt) = val > 32767 ? 32767 : val;
        }
    }
    pthread_mutex_unlock(&lock);
}

static void *adapter_thread(void *unused) {
    (void)unused;
    pthread_setname_np("input usb");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   /* (as Windows' reader is raised: reports on time) */
    Axis axes[4][4];
    int trigger_rest[4][2], trigger_top[4][2], sent[4] = {0};
    void *h = NULL;
    int stalls = 0;
    for (;;) {
        if (!h) {
            if (!(h = adapter_find())) { sleep(1); continue; }   /* (looked for again every second) */
            memset(sent, 0, sizeof sent);
            stalls = 0;
        }
        unsigned char buf[REPORT];   /* (exactly the report: a bigger read overflows, and stalls the pipe) */
        int done = 0, err = usb.interrupt_transfer(h, EP_IN, buf, REPORT, &done, 100);
        if (err == ERR_PIPE && ++stalls < 10) { usb.clear_halt(h, EP_IN); continue; }
        if (err && err != ERR_TIMEOUT) { adapter_close(h); h = NULL; continue; }
        if (!err && done == REPORT && buf[0] == 0x21) {
            stalls = 0;
            adapter_report(buf, axes, trigger_rest, trigger_top);
        }
        /* rumble as the game last asked, when a motor changes (without the second cable it rumbles nothing) */
        int want[4], changed = 0;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < 4; i++) { want[i] = rumble_want[i] && ports[i].connected; changed |= want[i] != sent[i]; }
        pthread_mutex_unlock(&lock);
        if (changed) {
            unsigned char packet[5] = {0x11};
            for (int i = 0; i < 4; i++) packet[1 + i] = want[i] ? 1 : sent[i] ? 2 : 0;
            usb.interrupt_transfer(h, EP_OUT, packet, sizeof packet, &done, 20);
            memcpy(sent, want, sizeof want);
        }
    }
    return NULL;
}

const char *padusb_start(void) {
    const char *env = getenv("SSBU_GC_USB");
    if (env && !strcmp(env, "0")) return NULL;
    static const char *const PATHS[] = {"@executable_path/../Frameworks/libusb-1.0.0.dylib", "/opt/homebrew/lib/libusb-1.0.0.dylib",
                                        "/usr/local/lib/libusb-1.0.0.dylib", "libusb-1.0.0.dylib"};
    void *m = NULL;
    for (unsigned i = 0; i < sizeof PATHS / sizeof *PATHS && !m; i++) m = dlopen(PATHS[i], RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        fprintf(stderr, "[input] no libusb (brew install libusb): a Wii U GameCube adapter can't be read\n");
        return NULL;
    }
    const char *missing = NULL;
#define USB_LOAD(f) if (!(*(void **)&usb.f = dlsym(m, "libusb_" #f)) && !missing) missing = "libusb_" #f;
    USB_FUNCS(USB_LOAD)
    if (missing) { fprintf(stderr, "[input] libusb has no %s: no GameCube adapter\n", missing); return NULL; }
    int err = usb.init(&ctx);
    if (err) { fprintf(stderr, "[input] libusb failed to start (%s): no GameCube adapter\n", usb.error_name(err)); return NULL; }
    for (int i = 0; i < 4; i++) {
        snprintf(ports[i].name, sizeof ports[i].name, "Nintendo GameCube Controller");
        snprintf(ports[i].id, sizeof ports[i].id, "gcadapter-%d", i + 1);
        snprintf(ports[i].path, sizeof ports[i].path, "usb 057e:0337#%d", i + 1);
    }
    pthread_t t;
    if (pthread_create(&t, NULL, adapter_thread, NULL)) return NULL;
    pthread_detach(t);
    return "0x057e/0x0337";
}
