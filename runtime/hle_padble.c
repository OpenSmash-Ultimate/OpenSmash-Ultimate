/* Switch 2 controllers over Bluetooth LE, on Windows (padble.h): the Pro Controller 2 and the Joy-Con 2s.
 *
 * Neither SDL nor Windows reads them over Bluetooth: they speak a GATT service of their own, which wants a start-up of
 * commands before it sends a single report. This file talks it through WinRT's Bluetooth LE classes, which every Windows
 * 10 / 11 has: an advertisement watcher finds them by Nintendo's manufacturer data, BluetoothLEDevice opens one from its
 * address, its characteristics are read uncached (that read is what connects it), and then: commands out as writes
 * without response, their replies and the 63-byte input reports in as notifications. WinRT is called through its ABI
 * by vtable slot (Windows' C++ projection and the SDK's winrt headers are not needed: this build has neither), its
 * classes found through combase.dll, loaded at run time. WinRT raises its events on its own threads: those only queue
 * them, and this file's thread runs them, so everything below the queue runs on one thread.
 *
 * The protocol is switch2_proto.h, from seedds/mkw-neo (MIT) as adriadam10/mkw-recomp carries it, and this file follows
 * mkw-recomp's switch2.c / switch2_win.c (MIT) in what it does with it: the identity read, pairing when the controller is
 * in Sync (its own 0x15 commands: a button press then wakes it toward this PC), the sticks' calibration, the features
 * the console turns on, the report-rate descriptor, Windows 11's throughput-optimized connection (a Pro Controller 2 was
 * noticeably late on the default interval there), and the player lights written every second (the link drops after
 * about ten seconds without a write from the host). What is this port's own: the pads as hle_pad.c's ports (no virtual
 * SDL gamepad), a lone Joy-Con turned to how it is held sideways, and HD rumble - the game's own amplitudes and
 * frequencies of both bands, each motor, played in 5 ms samples as the console plays them (switch2_proto.h's rumble
 * log) instead of a two-motor rumble at fixed frequencies.
 *
 * controllers.ini [general] switch2_bluetooth = 0 (or SSBU_SWITCH2=0) leaves Bluetooth alone; 2 also logs every
 * advertising device once. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "padble.h"

#ifndef _WIN32
int padble_start(int trace) { (void)trace; return 0; }
void padble_poll(PadBleState out[PADBLE_PORTS]) { memset(out, 0, sizeof(PadBleState) * PADBLE_PORTS); }
void padble_player(int port, int slot) { (void)port; (void)slot; }
void padble_vibrate(int port, const float motors[2][4]) { (void)port; (void)motors; }
#else
#include "switch2_proto.h"

/* ---- WinRT's ABI, by vtable slot ----------------------------------------------------------------------------------
 * Every WinRT interface is IInspectable's six slots (QueryInterface, AddRef, Release, GetIids, GetRuntimeClassName,
 * GetTrustLevel) and then its own methods, in the order of its metadata. The slots and IIDs below are the Windows
 * metadata's, as mingw-w64's generated headers (windows.devices.bluetooth*.h, windows.foundation*.h,
 * windows.storage.streams.h, windows.security.cryptography.h, asyncinfo.h) and microsoft/windows-rs give them. */
typedef void *RtHString;
#define VT(o) (*(void *const *const *)(o))
#define S_GET(o, slot, out) ((HRESULT (STDMETHODCALLTYPE *)(void *, void *))VT(o)[slot])((o), (void *)(out))
#define S_GET2(o, slot, a, out) ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, void *))VT(o)[slot])((o), (void *)(a), (void *)(out))
#define S_NONE(o, slot) ((HRESULT (STDMETHODCALLTYPE *)(void *))VT(o)[slot])((o))
#define S_I32(o, slot, v) ((HRESULT (STDMETHODCALLTYPE *)(void *, INT32))VT(o)[slot])((o), (INT32)(v))
#define S_I32_GET(o, slot, v, out) ((HRESULT (STDMETHODCALLTYPE *)(void *, INT32, void *))VT(o)[slot])((o), (INT32)(v), (void *)(out))
#define S_U32_GET(o, slot, v, out) ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, void *))VT(o)[slot])((o), (UINT32)(v), (void *)(out))
#define S_U64_GET(o, slot, v, out) ((HRESULT (STDMETHODCALLTYPE *)(void *, UINT64, void *))VT(o)[slot])((o), (UINT64)(v), (void *)(out))
#define S_GUID_GET(o, slot, g, out) ((HRESULT (STDMETHODCALLTYPE *)(void *, GUID, void *))VT(o)[slot])((o), (g), (void *)(out))
#define S_TOKEN(o, slot, t) ((HRESULT (STDMETHODCALLTYPE *)(void *, INT64))VT(o)[slot])((o), (INT64)(t))

static HRESULT qi(void *o, const GUID *iid, void **out) {
    *out = NULL;
    return o ? ((HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, void **))VT(o)[0])(o, iid, out) : E_POINTER;
}
static void release(void *o) { if (o) ((ULONG (STDMETHODCALLTYPE *)(void *))VT(o)[2])(o); }

enum {   /* slots */
    ASYNCINFO_STATUS = 7, ASYNCINFO_CANCEL = 9,               /* IAsyncInfo */
    ASYNCOP_RESULTS = 8,                                      /* IAsyncOperation<T>::GetResults */
    VEC_GETAT = 6, VEC_SIZE = 7,                              /* IVector<T>, IVectorView<T> */
    CLOSABLE_CLOSE = 6,                                       /* IClosable */
    BUFFER_LENGTH = 7, BYTEACCESS_BUFFER = 3,                 /* IBuffer, IBufferByteAccess */
    CRYPTO_FROM_BYTES = 9,                                    /* ICryptographicBufferStatics::CreateFromByteArray */
    ADAPTERS_DEFAULT = 8, ADAPTER_ADDRESS = 7, ADAPTER_LE = 9, /* IBluetoothAdapterStatics, IBluetoothAdapter */
    LEDEVS_FROM_ADDRESS = 7,                                  /* IBluetoothLEDeviceStatics */
    LEDEV_STATUS = 9, LEDEV_ADD_STATUS = 16, LEDEV_REMOVE_STATUS = 17,   /* IBluetoothLEDevice */
    LEDEV3_SERVICES_CACHE = 9,                                /* IBluetoothLEDevice3::GetGattServicesWithCacheModeAsync */
    LEDEV6_PARAMS = 6, LEDEV6_PREFER = 8,                     /* IBluetoothLEDevice6 */
    PREFS_THROUGHPUT = 7, PREFREQ_STATUS = 6,                 /* ...PreferredConnectionParametersStatics, ...Request */
    CONN_TIMEOUT = 6, CONN_LATENCY = 7, CONN_INTERVAL = 8,    /* IBluetoothLEConnectionParameters */
    RESULT_STATUS = 6, RESULT_LIST = 8,                       /* Gatt{DeviceServices,Characteristics,Descriptors}Result */
    SVC3_CHARS_CACHE = 12,                                    /* IGattDeviceService3::GetCharacteristicsWithCacheModeAsync */
    CHAR_UUID = 11, CHAR_WRITE_OPTION = 17, CHAR_WRITE_CCCD = 19, CHAR_ADD_CHANGED = 20, CHAR_REMOVE_CHANGED = 21,   /* IGattCharacteristic */
    CHAR3_DESCS_UUID = 8,                                     /* IGattCharacteristic3::GetDescriptorsForUuidAsync */
    DESC_WRITE = 12,                                          /* IGattDescriptor::WriteValueAsync */
    CHANGED_VALUE = 6,                                        /* IGattValueChangedEventArgs::get_CharacteristicValue */
    WATCHER_SCANMODE = 12, WATCHER_START = 17, WATCHER_ADD_RECEIVED = 19,   /* IBluetoothLEAdvertisementWatcher */
    RECEIVED_ADDRESS = 7, RECEIVED_ADVERT = 10,               /* IBluetoothLEAdvertisementReceivedEventArgs */
    ADVERT_MANUFACTURER = 11,                                 /* IBluetoothLEAdvertisement::get_ManufacturerData */
    MFR_COMPANY = 6, MFR_DATA = 8,                            /* IBluetoothLEManufacturerData */
};
enum { CACHE_UNCACHED = 1, GATT_SUCCESS = 0, WRITE_NO_RESPONSE = 1, CCCD_NOTIFY = 1, CONN_DISCONNECTED = 0, SCAN_ACTIVE = 1,
       ASYNC_STARTED = 0, ASYNC_COMPLETED = 1, RO_MULTITHREADED = 1 };

static const GUID IID_Unknown_ = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const GUID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_Closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
static const GUID IID_ByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
static const GUID IID_CryptoStatics = {0x320b7e22, 0x3cb0, 0x4cdf, {0x86, 0x63, 0x1d, 0x28, 0x91, 0x00, 0x65, 0xeb}};
static const GUID IID_AdapterStatics = {0x8b02fb6a, 0xac4c, 0x4741, {0x86, 0x61, 0x8e, 0xab, 0x7d, 0x17, 0xea, 0x9f}};
static const GUID IID_LEDeviceStatics = {0xc8cf1a19, 0xf0b6, 0x4bf0, {0x86, 0x89, 0x41, 0x30, 0x3d, 0xe2, 0xd9, 0xf4}};
static const GUID IID_LEDevice3 = {0xaee9e493, 0x44ac, 0x40dc, {0xaf, 0x33, 0xb2, 0xc1, 0x3c, 0x01, 0xca, 0x46}};
static const GUID IID_LEDevice6 = {0xca7190ef, 0x0cae, 0x573c, {0xa1, 0xca, 0xe1, 0xfc, 0x5b, 0xfc, 0x39, 0xe2}};
static const GUID IID_PrefsStatics = {0x0e3e8edc, 0x2751, 0x55aa, {0xa8, 0x38, 0x8f, 0xae, 0xee, 0x81, 0x8d, 0x72}};
static const GUID IID_Service3 = {0xb293a950, 0x0c53, 0x437c, {0xa9, 0xb3, 0x5c, 0x32, 0x10, 0xc6, 0xe5, 0x69}};
static const GUID IID_Char3 = {0x3f3c663e, 0x93d4, 0x406b, {0xb8, 0x17, 0xdb, 0x81, 0xf8, 0xed, 0x53, 0xb3}};
static const GUID IID_Watcher = {0xa6ac336f, 0xf3d3, 0x4297, {0x8d, 0x6c, 0xc8, 0x1e, 0xa6, 0x62, 0x3f, 0x40}};
/* the event handlers' parameterized interfaces: TypedEventHandler<watcher, received args>, <characteristic, value
 * changed args>, <BluetoothLEDevice, IInspectable> */
static const GUID IID_OnReceived = {0x90eb4eca, 0xd465, 0x5ea0, {0xa6, 0x1c, 0x03, 0x3c, 0x8c, 0x5e, 0xce, 0xf2}};
static const GUID IID_OnValue = {0xc1f420f6, 0x6292, 0x5760, {0xa2, 0xc9, 0x9d, 0xdf, 0x98, 0x68, 0x3c, 0xfc}};
static const GUID IID_OnStatus = {0xa90661e2, 0x372e, 0x5d1e, {0xbb, 0xbb, 0xb8, 0xa2, 0xce, 0x0e, 0x7c, 0x4d}};

static const wchar_t CLS_ADAPTER[] = L"Windows.Devices.Bluetooth.BluetoothAdapter";
static const wchar_t CLS_LEDEVICE[] = L"Windows.Devices.Bluetooth.BluetoothLEDevice";
static const wchar_t CLS_WATCHER[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
static const wchar_t CLS_CRYPTO[] = L"Windows.Security.Cryptography.CryptographicBuffer";
static const wchar_t CLS_PREFS[] = L"Windows.Devices.Bluetooth.BluetoothLEPreferredConnectionParameters";

static struct {   /* combase.dll */
    HRESULT (WINAPI *Initialize)(int);
    HRESULT (WINAPI *GetActivationFactory)(RtHString, const GUID *, void **);
    HRESULT (WINAPI *ActivateInstance)(RtHString, void **);
    HRESULT (WINAPI *CreateString)(const WCHAR *, UINT32, RtHString *);
    HRESULT (WINAPI *DeleteString)(RtHString);
} ro;

static int load_ro(void) {
    HMODULE m = LoadLibraryW(L"combase.dll");
    if (!m) return 0;
    ro.Initialize = (HRESULT (WINAPI *)(int))(void *)GetProcAddress(m, "RoInitialize");
    ro.GetActivationFactory = (HRESULT (WINAPI *)(RtHString, const GUID *, void **))(void *)GetProcAddress(m, "RoGetActivationFactory");
    ro.ActivateInstance = (HRESULT (WINAPI *)(RtHString, void **))(void *)GetProcAddress(m, "RoActivateInstance");
    ro.CreateString = (HRESULT (WINAPI *)(const WCHAR *, UINT32, RtHString *))(void *)GetProcAddress(m, "WindowsCreateString");
    ro.DeleteString = (HRESULT (WINAPI *)(RtHString))(void *)GetProcAddress(m, "WindowsDeleteString");
    return ro.Initialize && ro.GetActivationFactory && ro.ActivateInstance && ro.CreateString && ro.DeleteString;
}

static HRESULT factory(const wchar_t *cls, const GUID *iid, void **out) {
    RtHString s = NULL;
    *out = NULL;
    HRESULT hr = ro.CreateString(cls, (UINT32)wcslen(cls), &s);
    if (SUCCEEDED(hr)) hr = ro.GetActivationFactory(s, iid, out);
    if (s) ro.DeleteString(s);
    return hr;
}

static HRESULT activate(const wchar_t *cls, const GUID *iid, void **out) {
    RtHString s = NULL;
    void *inspectable = NULL;
    *out = NULL;
    HRESULT hr = ro.CreateString(cls, (UINT32)wcslen(cls), &s);
    if (SUCCEEDED(hr)) hr = ro.ActivateInstance(s, &inspectable);
    if (s) ro.DeleteString(s);
    if (SUCCEEDED(hr)) hr = qi(inspectable, iid, out);
    release(inspectable);
    return hr;
}

static uint64_t now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}

/* Waits up to ms for an IAsyncOperation: 1 when it completed. */
static int wait_async(void *op, int ms) {
    void *info = NULL;
    INT32 st = ASYNC_STARTED;
    if (!op || FAILED(qi(op, &IID_AsyncInfo, &info))) return 0;
    for (uint64_t end = now_ns() + (uint64_t)ms * 1000000; st == ASYNC_STARTED && now_ns() < end;) {
        if (FAILED(S_GET(info, ASYNCINFO_STATUS, &st))) break;
        if (st == ASYNC_STARTED) Sleep(5);
    }
    if (st == ASYNC_STARTED) S_NONE(info, ASYNCINFO_CANCEL);
    release(info);
    return st == ASYNC_COMPLETED;
}
/* An IAsyncOperation's result (an interface), waited for up to ms; the operation released. NULL when none. */
static void *async_result(void *op, int ms) {
    void *r = NULL;
    if (op && wait_async(op, ms)) S_GET(op, ASYNCOP_RESULTS, &r);
    release(op);
    return r;
}
/* The same for IAsyncOperation<GattCommunicationStatus>: 1 when it came back Success. */
static int async_ok(void *op, int ms) {
    INT32 st = -1;
    if (op && wait_async(op, ms)) S_GET(op, ASYNCOP_RESULTS, &st);
    release(op);
    return st == GATT_SUCCESS;
}

static void close_obj(void *o) {
    void *c = NULL;
    if (o && SUCCEEDED(qi(o, &IID_Closable, &c))) S_NONE(c, CLOSABLE_CLOSE), release(c);
}

static int parse_guid(const char *s, GUID *g) {
    unsigned d[11];
    if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7], &d[8], &d[9],
               &d[10]) != 11)
        return 0;
    g->Data1 = d[0], g->Data2 = (unsigned short)d[1], g->Data3 = (unsigned short)d[2];
    for (int i = 0; i < 8; i++) g->Data4[i] = (unsigned char)d[3 + i];
    return 1;
}

/* ---- event handlers: one COM object shape for every TypedEventHandler<sender, args> --------------------------------- */
typedef struct Handler Handler;
typedef void (*HandlerFn)(Handler *h, void *sender, void *args);
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(Handler *, const GUID *, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(Handler *);
    ULONG (STDMETHODCALLTYPE *Release)(Handler *);
    HRESULT (STDMETHODCALLTYPE *Invoke)(Handler *, void *, void *);
} HandlerVtbl;
struct Handler {
    const HandlerVtbl *lpVtbl;
    const GUID *iid;
    HandlerFn fn;
    volatile LONG refs;
    int link, is_input;   /* which link and characteristic it reports for */
};
static HRESULT STDMETHODCALLTYPE h_query(Handler *h, const GUID *riid, void **out) {
    if (IsEqualGUID(riid, &IID_Unknown_) || IsEqualGUID(riid, &IID_Agile) || IsEqualGUID(riid, h->iid)) {
        InterlockedIncrement(&h->refs);
        *out = h;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE h_addref(Handler *h) { return (ULONG)InterlockedIncrement(&h->refs); }
static ULONG STDMETHODCALLTYPE h_release(Handler *h) {
    LONG n = InterlockedDecrement(&h->refs);
    if (!n) free(h);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE h_invoke(Handler *h, void *sender, void *args) { h->fn(h, sender, args); return S_OK; }
static const HandlerVtbl handler_vtbl = {h_query, h_addref, h_release, h_invoke};
static Handler *handler(const GUID *iid, HandlerFn fn, int link, int is_input) {
    Handler *h = calloc(1, sizeof *h);
    if (h) h->lpVtbl = &handler_vtbl, h->iid = iid, h->fn = fn, h->refs = 1, h->link = link, h->is_input = is_input;
    return h;
}

/* ---- the queue from WinRT's threads to this file's ------------------------------------------------------------- */
enum { EV_ADVERT, EV_VALUE, EV_GONE, QUEUE_LEN = 256 };
typedef struct { int kind, link, is_input; uint64_t addr, at; uint16_t n; uint8_t d[128]; } Event;
static Event queue[QUEUE_LEN];
static int q_head, q_count;
static CRITICAL_SECTION q_lock;
static HANDLE q_ready;

static void post(const Event *e) {
    EnterCriticalSection(&q_lock);
    if (q_count < QUEUE_LEN) queue[(q_head + q_count++) % QUEUE_LEN] = *e;   /* (full: dropped, as a lost notification) */
    LeaveCriticalSection(&q_lock);
    SetEvent(q_ready);
}

static void buffer_bytes(void *b, Event *e) {   /* an IBuffer's bytes into e->d (at most its size) */
    UINT32 n = 0;
    BYTE *p = NULL;
    void *acc = NULL;
    e->n = 0;
    if (!b || FAILED(S_GET(b, BUFFER_LENGTH, &n)) || FAILED(qi(b, &IID_ByteAccess, &acc))) return;
    if (SUCCEEDED(S_GET(acc, BYTEACCESS_BUFFER, &p)) && p) {
        e->n = (uint16_t)(n < sizeof e->d ? n : sizeof e->d);
        memcpy(e->d, p, e->n);
    }
    release(acc);
}

static void on_advert(Handler *h, void *sender, void *args) {
    (void)h; (void)sender;
    void *adv = NULL, *list = NULL;
    Event e = {.kind = EV_ADVERT};
    UINT32 n = 0;
    if (FAILED(S_GET(args, RECEIVED_ADDRESS, &e.addr)) || FAILED(S_GET(args, RECEIVED_ADVERT, &adv))) return;
    if (SUCCEEDED(S_GET(adv, ADVERT_MANUFACTURER, &list)) && list && SUCCEEDED(S_GET(list, VEC_SIZE, &n)))
        for (UINT32 i = 0; i < n; i++) {
            void *m = NULL, *b = NULL;
            UINT16 company = 0;
            if (FAILED(S_U32_GET(list, VEC_GETAT, i, &m)) || !m) continue;
            if (SUCCEEDED(S_GET(m, MFR_COMPANY, &company)) && SUCCEEDED(S_GET(m, MFR_DATA, &b)) && b) {
                buffer_bytes(b, &e);
                e.is_input = company;   /* (the company id rides in is_input) */
                release(b);
                post(&e);
            }
            release(m);
        }
    release(list);
    release(adv);
}

static void on_value(Handler *h, void *sender, void *args) {
    (void)sender;
    void *b = NULL;
    Event e = {.kind = EV_VALUE, .link = h->link, .is_input = h->is_input, .at = now_ns()};
    if (FAILED(S_GET(args, CHANGED_VALUE, &b)) || !b) return;
    buffer_bytes(b, &e);
    release(b);
    post(&e);
}

static void on_status(Handler *h, void *sender, void *args) {
    (void)args;
    INT32 st = 1;
    S_GET(sender, LEDEV_STATUS, &st);
    if (st == CONN_DISCONNECTED) post(&(Event){.kind = EV_GONE, .link = h->link});
}

/* ---- pads: a Pro Controller 2, two Joy-Con 2s together, or one Joy-Con alone (sideways) --------------------------- */
typedef struct Link Link;
typedef struct Pad Pad;
typedef struct Sess {   /* one connected controller */
    Link *link;
    uint16_t pid;
    int use;
    Pad *pad;
    s2_cal cal[2];
    char serial[15];
    uint8_t want, reply[0x50 + S2_REPLY_DATA];   /* (a command awaiting its reply; the reply) */
    size_t reply_n;
    int got;
    uint8_t vseq;
    int quiet;                 /* silent rumble packets still to send after the motors stopped */
    s2_rumble_cursor cur;
    s2_hold hold;              /* a pair's split or two Joy-Cons' join, held */
    uint64_t next_led;
    uint64_t rate_t0;          /* (the reports a second over the first 5 s, in the log) */
    uint32_t rate_n;
    int rate_done;
} Sess;
struct Pad {
    int used, solo;            /* solo: split by the player (a Joy-Con alone that does not join the next one that comes) */
    unsigned gen;
    s2_pad_state st;           /* the controllers' last reports and calibration (switch2_proto.h) */
    Sess *pro, *left, *right;
    int player;                /* the player hle_pad.c made of it (padble_player), -1 none */
    s2_rumble_log log;         /* the game's vibration for it, with the times it was set (padble_vibrate) */
};
static Pad pads[PADBLE_PORTS];
static SRWLOCK pads_lock = SRWLOCK_INIT;   /* (this file's thread writes reports and pads; the input thread reads them and writes logs) */
static volatile LONG next_gen = 1;

static const char *kind(uint16_t pid) {
    return pid == S2_PID_PRO ? "Pro Controller 2" : pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)";
}
static s2_dev *dev_of(Pad *p, uint16_t pid) { return pid == S2_PID_PRO ? &p->st.pro : pid == S2_PID_JOYCON_L ? &p->st.left : &p->st.right; }
static Sess **sess_of(Pad *p, uint16_t pid) { return pid == S2_PID_PRO ? &p->pro : pid == S2_PID_JOYCON_L ? &p->left : &p->right; }
static int port_of(const Pad *p) { return (int)(p - pads); }

/* ---- links: one controller's Bluetooth device and its characteristics -------------------------------------------- */
enum { MAX_SVC = 8 };
struct Link {
    int id;
    uint64_t addr;
    void *dev;                             /* IBluetoothLEDevice */
    void *svc[MAX_SVC];                    /* IGattDeviceService */
    int nsvc;
    void *cmd, *rsp, *input, *vib;         /* IGattCharacteristic */
    INT64 t_status, t_rsp, t_input;        /* (event registration tokens) */
    void *fast;                            /* the preferred connection parameters request, held for the link's life */
    uint16_t pid;
    int use, gone;                         /* gone: disconnected while its handshake ran */
    Sess *s;
    Link *next;
};
static Link *busy, *links;
static int next_id = 1, trace;
static void *bytes_to_buffer;   /* ICryptographicBufferStatics */
static uint8_t host[6];         /* the adapter's address, little endian (what the controllers wake toward) */

static int write_value(void *c, const uint8_t *d, size_t n) {
    void *b = NULL, *op = NULL;
    if (!c || FAILED(((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, BYTE *, void **))VT(bytes_to_buffer)[CRYPTO_FROM_BYTES])(
                  bytes_to_buffer, (UINT32)n, (BYTE *)d, &b)) || !b)
        return 0;
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, void *, INT32, void **))VT(c)[CHAR_WRITE_OPTION])(c, b, WRITE_NO_RESPONSE, &op);
    release(b);
    release(op);   /* (without response: nothing to wait for) */
    return SUCCEEDED(hr);
}

/* Notifications of c on, each one queued for link id. */
static int subscribe(void *c, int id, int is_input, INT64 *tok) {
    Handler *h = handler(&IID_OnValue, on_value, id, is_input);
    void *op = NULL;
    if (!h) return 0;
    HRESULT hr = S_GET2(c, CHAR_ADD_CHANGED, h, tok);
    h_release(h);
    if (FAILED(hr) || FAILED(S_I32_GET(c, CHAR_WRITE_CCCD, CCCD_NOTIFY, &op))) return 0;
    return async_ok(op, 5000);
}

/* The input characteristic's report-rate descriptor written, when the controller has one (switch2_proto.h). */
static void write_rate_desc(Link *l) {
    static const uint8_t pay[] = {S2_RATE_PAYLOAD};
    void *c3 = NULL, *r = NULL, *v = NULL, *d = NULL, *b = NULL, *op = NULL;
    UINT32 n = 0;
    GUID g;
    const char *what = "not found";
    parse_guid(S2_UUID_RATE_DESC, &g);
    if (SUCCEEDED(qi(l->input, &IID_Char3, &c3)) && SUCCEEDED(S_GUID_GET(c3, CHAR3_DESCS_UUID, g, &op)))
        r = async_result(op, 5000);
    if (r && SUCCEEDED(S_GET(r, RESULT_LIST, &v)) && v && SUCCEEDED(S_GET(v, VEC_SIZE, &n)) && n &&
        SUCCEEDED(S_U32_GET(v, VEC_GETAT, 0, &d)) && d) {
        what = "refused";
        op = NULL;
        if (SUCCEEDED(((HRESULT (STDMETHODCALLTYPE *)(void *, UINT32, BYTE *, void **))VT(bytes_to_buffer)[CRYPTO_FROM_BYTES])(
                bytes_to_buffer, sizeof pay, (BYTE *)pay, &b)) && b &&
            SUCCEEDED(S_GET2(d, DESC_WRITE, b, &op)) && async_ok(op, 5000))
            what = "written";
    }
    if (trace) fprintf(stderr, "[input] Switch 2 controller %012llX: report-rate descriptor %s\n", (unsigned long long)l->addr, what);
    release(b); release(d); release(v); release(r); release(c3);
}

/* The link's connection interval in the log (Windows 11: IBluetoothLEDevice6) */
static void log_conn(void *dev, uint64_t addr, const char *when) {
    void *d6 = NULL, *p = NULL;
    if (FAILED(qi(dev, &IID_LEDevice6, &d6))) return;
    if (SUCCEEDED(S_GET(d6, LEDEV6_PARAMS, &p)) && p) {
        UINT16 iv = 0, lat = 0, to = 0;
        S_GET(p, CONN_TIMEOUT, &to); S_GET(p, CONN_LATENCY, &lat); S_GET(p, CONN_INTERVAL, &iv);
        fprintf(stderr, "[input] Switch 2 controller %012llX %s: connection interval %.2f ms, latency %u, timeout %u ms\n",
                (unsigned long long)addr, when, iv * 1.25, lat, to * 10u);
        release(p);
    }
    release(d6);
}

/* Asks Windows 11 for its shortest connection interval; the request object to keep, NULL when refused or older. */
static void *prefer_fast(void *dev, uint64_t addr) {
    void *d6 = NULL, *st = NULL, *pp = NULL, *req = NULL;
    INT32 status = 0;
    if (FAILED(qi(dev, &IID_LEDevice6, &d6))) return NULL;   /* (Windows 10: its default interval) */
    if (SUCCEEDED(factory(CLS_PREFS, &IID_PrefsStatics, &st)) && SUCCEEDED(S_GET(st, PREFS_THROUGHPUT, &pp)) && pp &&
        SUCCEEDED(S_GET2(d6, LEDEV6_PREFER, pp, &req)) && req)
        S_GET(req, PREFREQ_STATUS, &status);
    fprintf(stderr, "[input] Switch 2 controller %012llX: throughput-optimized connection %s\n", (unsigned long long)addr,
            status == 1 ? "asked for" : status == 2 ? "refused (device not available)" : status == 3 ? "refused (access denied)" : "not asked (call failed)");
    release(pp); release(st); release(d6);
    if (status != 1 && req) release(req), req = NULL;
    return req;
}

static void release_char(void **c, INT64 *tok) {
    if (!*c) return;
    if (tok && *tok) S_TOKEN(*c, CHAR_REMOVE_CHANGED, *tok);
    release(*c);
    *c = NULL;
}

/* Everything of the link released and closed: Windows drops the connection once nothing holds the device. */
static void free_link(Link *l) {
    release_char(&l->rsp, &l->t_rsp);
    release_char(&l->input, &l->t_input);
    release_char(&l->cmd, NULL);
    release_char(&l->vib, NULL);
    for (int i = 0; i < l->nsvc; i++) close_obj(l->svc[i]), release(l->svc[i]);
    if (l->fast) close_obj(l->fast), release(l->fast);
    if (l->dev) {
        if (l->t_status) S_TOKEN(l->dev, LEDEV_REMOVE_STATUS, l->t_status);
        close_obj(l->dev);
        release(l->dev);
    }
    free(l);
}

/* ---- the session: commands, one in flight (the controller has one reply channel), replies pumped meanwhile ------- */
static void pump(int ms);

static int command(Sess *s, uint8_t cmd, uint8_t sub, const uint8_t *payload, size_t n) {
    uint8_t f[8 + 0xFF];
    size_t len = s2_command(f, sizeof f, cmd, sub, payload, n);
    s->want = cmd, s->got = 0;
    if (!len || !write_value(s->link->cmd, f, len)) return s->want = 0, 0;
    for (uint64_t end = now_ns() + 2000000000ull; !s->got && now_ns() < end;) pump(10);
    if (!s->got) fprintf(stderr, "[input] %s: command 0x%02x timed out\n", kind(s->pid), cmd);
    s->want = 0;
    return s->got;
}

static void on_reply(Sess *s, const uint8_t *d, size_t n) {
    if (!s->want || !s2_reply_is(d, n, s->want)) return;
    s->reply_n = n < sizeof s->reply ? n : sizeof s->reply;
    memcpy(s->reply, d, s->reply_n);
    s->got = 1;
}

static int read_memory(Sess *s, uint8_t len, uint32_t addr, uint8_t *out) {
    uint8_t p[8];
    s2_memory_payload(p, len, addr);
    if (!command(s, S2_CMD_MEMORY, S2_SUB_MEMORY_READ, p, 8)) return 0;
    if (s->reply_n < (size_t)S2_REPLY_DATA + len || s->reply[8] != len) return 0;
    memcpy(out, s->reply + S2_REPLY_DATA, len);
    return 1;
}

static void read_cal(Sess *s, int slot) {   /* the player's own calibration when set, else the factory's */
    uint8_t d[0x0B];
    if (read_memory(s, sizeof d, slot ? S2_ADDR_USER_STICK2 : S2_ADDR_USER_STICK1, d) && s2_cal_parse(d, sizeof d, &s->cal[slot])) return;
    if (read_memory(s, sizeof d, slot ? S2_ADDR_FACTORY_STICK2 : S2_ADDR_FACTORY_STICK1, d)) s2_cal_parse(d, sizeof d, &s->cal[slot]);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint8_t random_byte(void) {   /* (the pairing keys: any will do - nothing uses them afterwards, switch2_proto.h) */
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    rng ^= (uint64_t)t.QuadPart;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint8_t)(rng >> 24);
}

/* The console's pairing: this PC's address and two keys of its choosing. From then on a button press wakes the
 * controller toward this PC. A failed step leaves it playing, but needing Sync again next time. */
static void bond(Sess *s) {
    uint8_t h[S2_PAIR_HOST_LEN], k[16], key[S2_PAIR_KEY_LEN], zero = 0;
    s2_pair_host_payload(h, host);
    int ok = command(s, S2_CMD_PAIR, S2_SUB_PAIR_HOST, h, sizeof h);
    for (int i = 0; ok && i < 2; i++) {
        for (int j = 0; j < 16; j++) k[j] = random_byte();
        s2_pair_key_payload(key, k);
        ok = command(s, S2_CMD_PAIR, i ? S2_SUB_PAIR_KEY2 : S2_SUB_PAIR_KEY1, key, sizeof key);
    }
    ok = ok && command(s, S2_CMD_PAIR, S2_SUB_PAIR_DONE, &zero, 1);
    fprintf(stderr, "[input] %s %s\n", kind(s->pid), ok ? "paired with this PC: from now on a press of any of its buttons connects it"
                                                         : "not paired (no answer): it plays now, but needs Sync again next time");
}

static void set_leds(Sess *s) {
    Pad *p = s->pad;
    int who = p->player >= 0 ? p->player : port_of(p);
    uint8_t pl[4] = {s2_led_pattern[who & 7], 0, 0, 0}, f[12];
    size_t n = s2_command(f, sizeof f, S2_CMD_LEDS, S2_SUB_LEDS_SET, pl, 4);
    if (n) write_value(s->link->cmd, f, n);
}

/* A free pad (its port), NULL when all are taken. Under pads_lock. */
static Pad *pad_new(void) {
    for (int i = 0; i < PADBLE_PORTS; i++)
        if (!pads[i].used) {
            Pad *p = &pads[i];
            memset(p, 0, sizeof *p);
            p->used = 1;
            p->player = -1;
            p->gen = (unsigned)InterlockedIncrement(&next_gen);
            return p;
        }
    return NULL;
}

/* A Joy-Con joins a pad that holds the other one alone (not one split by its player); anything else starts a pad. */
static Pad *pad_for(uint16_t pid) {
    if (pid != S2_PID_PRO)
        for (int i = 0; i < PADBLE_PORTS; i++) {
            Pad *p = &pads[i];
            if (p->used && !p->solo && !p->pro && ((pid == S2_PID_JOYCON_L && p->right && !p->left) || (pid == S2_PID_JOYCON_R && p->left && !p->right)))
                return p;
        }
    return pad_new();
}

/* s into pad p (under pads_lock): its device's calibration and a fresh report state. */
static void pad_put(Pad *p, Sess *s) {
    *sess_of(p, s->pid) = s;
    s2_dev *d = dev_of(p, s->pid);
    memset(d, 0, sizeof *d);
    d->have = 1, d->pid = s->pid, d->cal[0] = s->cal[0], d->cal[1] = s->cal[1];
    s->pad = p;
    memset(&s->cur, 0, sizeof s->cur);
    memset(&s->hold, 0, sizeof s->hold);
}
/* s out of its pad (under pads_lock); the pad freed when it holds nothing more, else made another (gen). */
static void pad_take(Sess *s) {
    Pad *p = s->pad;
    if (!p) return;
    memset(dev_of(p, s->pid), 0, sizeof(s2_dev));
    *sess_of(p, s->pid) = NULL;
    s->pad = NULL;
    if (!p->pro && !p->left && !p->right) p->used = 0;
    else p->gen = (unsigned)InterlockedIncrement(&next_gen);
}

static Sess *handshake(Link *l) {
    Sess *s = calloc(1, sizeof *s);
    uint8_t info[0x40];
    uint16_t vid = 0, real = 0;
    if (!s) return NULL;
    s->link = l, s->pid = l->pid, s->use = l->use;
    s2_cal_default(&s->cal[0]);
    s2_cal_default(&s->cal[1]);
    l->s = s;
    if (!read_memory(s, sizeof info, S2_ADDR_INFO, info) || !s2_info_parse(info, sizeof info, &vid, &real, s->serial) || vid != S2_VENDOR_ID) {
        fprintf(stderr, "[input] %s: it did not answer the identity read\n", kind(s->pid));
        return NULL;
    }
    s->pid = real;
    if (s->use == S2_ADV_PAIR) bond(s);
    read_cal(s, 0);
    if (s->pid == S2_PID_PRO) read_cal(s, 1);
    uint8_t feat[4] = {S2_FEATURES, 0, 0, 0};
    command(s, S2_CMD_FEATURE, S2_SUB_FEATURE_SET, feat, 4);
    if (!command(s, S2_CMD_FEATURE, S2_SUB_FEATURE_ENABLE, feat, 4)) {
        fprintf(stderr, "[input] %s: enabling its reports was refused\n", kind(s->pid));
        return NULL;
    }
    AcquireSRWLockExclusive(&pads_lock);
    Pad *p = pad_for(s->pid);
    int joined = p && (p->left || p->right);
    if (p) {
        pad_put(p, s);
        if (joined) p->gen = (unsigned)InterlockedIncrement(&next_gen);   /* (a lone Joy-Con became a pair: another pad) */
    }
    ReleaseSRWLockExclusive(&pads_lock);
    if (!p) { fprintf(stderr, "[input] %s: all %d Bluetooth ports are taken\n", kind(s->pid), PADBLE_PORTS); return NULL; }
    set_leds(s);
    s->next_led = now_ns() + 1000000000ull;
    write_rate_desc(l);
    if (!subscribe(l->input, l->id, 1, &l->t_input)) fprintf(stderr, "[input] %s: its reports could not be subscribed\n", kind(s->pid));
    fprintf(stderr, "[input] %s %.14s connected over Bluetooth (port %d)%s\n", kind(s->pid), s->serial, port_of(p) + 1,
            joined ? ", with its other Joy-Con: one player" : s->pid != S2_PID_PRO ? ", alone: held sideways (connect the other Joy-Con to make a pair)" : "");
    return s;
}

static void sess_close(Sess *s) {
    if (!s) return;
    if (s->pad) {
        fprintf(stderr, "[input] %s %.14s disconnected\n", kind(s->pid), s->serial);
        AcquireSRWLockExclusive(&pads_lock);
        pad_take(s);
        ReleaseSRWLockExclusive(&pads_lock);
    }
    free(s);
}

/* SL + SR held on a Joy-Con of a pair: it leaves the pair, both held sideways; L on a lone left one and R on a lone right
 * one held together: they are a pair again (switch2_proto.h's gestures, S2_GESTURE_NS). Under pads_lock. */
static void gestures(Sess *s, uint64_t now) {
    Pad *p = s->pad;
    if (!p || s->pid == S2_PID_PRO) return;
    int right = s->pid == S2_PID_JOYCON_R, paired = p->left && p->right;
    int g = s2_gesture(dev_of(p, s->pid)->r.buttons, right, paired);
    if (!s2_hold_update(&s->hold, g, now) || s->hold.done) return;
    if (g == S2_GESTURE_SPLIT) {
        Pad *q = pad_new();
        if (!q) return;
        s->hold.done = 1;
        s2_cal cal0 = s->cal[0], cal1 = s->cal[1];
        pad_take(s);   /* (the pair: the other Joy-Con alone, another pad) */
        s->cal[0] = cal0, s->cal[1] = cal1;
        p->solo = 1;
        pad_put(q, s);
        q->solo = 1;
        fprintf(stderr, "[input] Joy-Con 2 pair split: each is a player of its own, held sideways (ports %d and %d)\n", port_of(p) + 1, port_of(q) + 1);
        return;
    }
    if (g == S2_GESTURE_JOIN) {   /* the other side's lone Joy-Con, holding its own for as long */
        for (int i = 0; i < PADBLE_PORTS; i++) {
            Pad *o = &pads[i];
            Sess *t = right ? o->left : o->right;
            if (!o->used || o == p || o->pro || !t || (right ? o->right : o->left)) continue;
            int tg = s2_gesture(dev_of(o, t->pid)->r.buttons, !right, 0);
            if (tg != S2_GESTURE_JOIN || !s2_hold_update(&t->hold, tg, now)) continue;
            s->hold.done = t->hold.done = 1;
            pad_take(t);   /* (frees o) */
            pad_put(p, t);
            p->solo = 0;
            p->gen = (unsigned)InterlockedIncrement(&next_gen);
            fprintf(stderr, "[input] Joy-Con 2s joined: one player again (port %d)\n", port_of(p) + 1);
            return;
        }
    }
}

static void on_report(Sess *s, const uint8_t *d, size_t n, uint64_t at) {
    if (!s->pad) return;
    AcquireSRWLockExclusive(&pads_lock);
    s2_dev *dev = dev_of(s->pad, s->pid);
    int ok = s2_report_parse(d, n, &dev->r);
    if (ok) gestures(s, at);
    ReleaseSRWLockExclusive(&pads_lock);
    if (!ok) return;
    if (!s->rate_t0) s->rate_t0 = at;
    else if (!s->rate_done && at - s->rate_t0 >= 5000000000ull) {
        s->rate_done = 1;
        fprintf(stderr, "[input] %s: %.1f reports a second over its first 5 s\n", kind(s->pid), s->rate_n / ((at - s->rate_t0) / 1e9));
        log_conn(s->link->dev, s->link->addr, "5 s into its reports");
    }
    s->rate_n++;
}

/* HD rumble: three 5 ms samples a packet every ~15 ms from the game's values (the pad's log), each controller playing
 * its share of the two motors (s2_rumble_pick); after the motors stop, three silent packets (a write without response
 * can be lost), then nothing. */
static void rumble(Sess *s, uint64_t now) {
    Pad *p = s->pad;
    if (!p || !s->link->vib) return;
    s2_haptic a[3], b[3];
    int on = 0;
    AcquireSRWLockShared(&pads_lock);
    s2_dev *dev = dev_of(p, s->pid);
    unsigned motors = s2_rumble_motors(&p->st, dev);
    s2_rumble_resync(&s->cur, now, 5000000ull);
    for (int i = 0; i < 3; i++) {
        s2_rumble_slot(&p->log, &s->cur, motors);
        s2_rumble_pick(&p->st, dev, s->cur.now, &a[i], &b[i]);
        on |= s2_haptic_on(&a[i]) || s2_haptic_on(&b[i]);
    }
    ReleaseSRWLockShared(&pads_lock);
    if (!on) {
        if (!s->quiet) return;
        s->quiet--;
    } else s->quiet = 3;
    uint8_t out[33];
    size_t n = s2_rumble_packet3(out, a, b, s->vseq++);
    write_value(s->link->vib, out, n);
}

static void tick(void) {
    uint64_t now = now_ns();
    for (Link *l = links; l; l = l->next) {
        Sess *s = l->s;
        if (!s || !s->pad || l == busy) continue;
        rumble(s, now);
        if (now >= s->next_led) set_leds(s), s->next_led = now + 1000000000ull;
    }
}

/* ---- links found, connected, dropped --------------------------------------------------------------------------- */
static void drop(Link *l) {
    Link **p = &links;
    while (*p && *p != l) p = &(*p)->next;
    if (*p) *p = l->next;
    sess_close(l->s);
    l->s = NULL;
    free_link(l);
}
static Link *link_by_id(int id) { for (Link *l = links; l; l = l->next) if (l->id == id) return l; return busy && busy->id == id ? busy : NULL; }
static Link *link_by_addr(uint64_t a) { for (Link *l = links; l; l = l->next) if (l->addr == a) return l; return busy && busy->addr == a ? busy : NULL; }

enum { MAX_PENDING = 8 };
static struct { uint64_t addr; uint16_t pid; int use; } pending[MAX_PENDING];
static int npending;
static struct { uint64_t addr, until; } failed[MAX_PENDING];

static int first_seen(uint64_t addr) {
    static uint64_t seen[64];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == addr) return 0;
    if (n < 64) seen[n++] = addr;
    return 1;
}

static void advert(const Event *e) {
    uint16_t company = (uint16_t)e->is_input;
    s2_advert a;
    int ours = company == S2_COMPANY_ID && s2_advert_parse(e->d, e->n, &a);
    if (trace && first_seen(e->addr))
        fprintf(stderr, "[input] Switch 2 trace: %012llX company 0x%04x, %d bytes%s\n", (unsigned long long)e->addr, company, e->n,
                company == S2_COMPANY_ID ? (ours ? ", a Switch 2 controller" : ", Nintendo, not a Switch 2 controller") : "");
    if (!ours) return;
    int use = s2_advert_use(&a, host);
    if (use == S2_ADV_IGNORE || link_by_addr(e->addr)) return;
    for (int i = 0; i < npending; i++) if (pending[i].addr == e->addr) return;
    for (int i = 0; i < MAX_PENDING; i++) if (failed[i].addr == e->addr && now_ns() < failed[i].until) return;
    if (npending == MAX_PENDING) return;
    pending[npending].addr = e->addr, pending[npending].pid = a.pid, pending[npending].use = use;
    npending++;
}

static void pump(int ms) {
    WaitForSingleObject(q_ready, (DWORD)ms);
    for (;;) {
        Event e;
        EnterCriticalSection(&q_lock);
        int have = q_count > 0;
        if (have) e = queue[q_head], q_head = (q_head + 1) % QUEUE_LEN, q_count--;
        LeaveCriticalSection(&q_lock);
        if (!have) return;
        if (e.kind == EV_ADVERT) { advert(&e); continue; }
        Link *l = link_by_id(e.link);
        if (!l) continue;
        if (e.kind == EV_GONE) {
            if (l == busy) l->gone = 1;
            else drop(l);
        } else if (l->s) {
            if (e.is_input) on_report(l->s, e.d, e.n, e.at);
            else on_reply(l->s, e.d, e.n);
        }
    }
}

static void give_up(uint64_t addr, const char *why) {
    fprintf(stderr, "[input] Switch 2 controller %012llX: %s\n", (unsigned long long)addr, why);
    int slot = 0;
    for (int i = 1; i < MAX_PENDING; i++) if (failed[i].until < failed[slot].until) slot = i;
    failed[slot].addr = addr, failed[slot].until = now_ns() + 10000000000ull;
}

/* The characteristics of one service that are ours, kept in l; 1 when it had any. want: command, reply, input, then
 * the three vibration channels. */
static int find_chars(Link *l, void *svc, const GUID want[6]) {
    void *s3 = NULL, *op = NULL, *r = NULL, *v = NULL;
    INT32 st = -1;
    UINT32 n = 0;
    int any = 0;
    if (FAILED(qi(svc, &IID_Service3, &s3))) return 0;
    if (SUCCEEDED(S_I32_GET(s3, SVC3_CHARS_CACHE, CACHE_UNCACHED, &op))) r = async_result(op, 10000);
    if (r && SUCCEEDED(S_GET(r, RESULT_STATUS, &st)) && st == GATT_SUCCESS && SUCCEEDED(S_GET(r, RESULT_LIST, &v)) && v &&
        SUCCEEDED(S_GET(v, VEC_SIZE, &n)))
        for (UINT32 i = 0; i < n; i++) {
            void *c = NULL, **dst = NULL;
            GUID u;
            if (FAILED(S_U32_GET(v, VEC_GETAT, i, &c)) || !c) continue;
            if (SUCCEEDED(S_GET(c, CHAR_UUID, &u)))
                dst = IsEqualGUID(&u, &want[0]) ? &l->cmd : IsEqualGUID(&u, &want[1]) ? &l->rsp : IsEqualGUID(&u, &want[2]) ? &l->input
                    : IsEqualGUID(&u, &want[3]) || IsEqualGUID(&u, &want[4]) || IsEqualGUID(&u, &want[5]) ? &l->vib : NULL;
            if (dst && !*dst) *dst = c, any = 1;
            else release(c);
        }
    release(v); release(r); release(s3);
    return any;
}

/* Its GATT services, read from the controller (this is what connects it), and our characteristics in them. */
static int find_services(Link *l, const GUID want[6]) {
    void *d3 = NULL, *op = NULL, *r = NULL, *v = NULL;
    INT32 st = -1;
    UINT32 n = 0;
    if (FAILED(qi(l->dev, &IID_LEDevice3, &d3))) return 0;
    if (SUCCEEDED(S_I32_GET(d3, LEDEV3_SERVICES_CACHE, CACHE_UNCACHED, &op))) r = async_result(op, 20000);
    if (r && SUCCEEDED(S_GET(r, RESULT_STATUS, &st)) && st == GATT_SUCCESS && SUCCEEDED(S_GET(r, RESULT_LIST, &v)) && v &&
        SUCCEEDED(S_GET(v, VEC_SIZE, &n)))
        for (UINT32 i = 0; i < n; i++) {
            void *s = NULL;
            if (FAILED(S_U32_GET(v, VEC_GETAT, i, &s)) || !s) continue;
            if (l->nsvc < MAX_SVC && find_chars(l, s, want)) l->svc[l->nsvc++] = s;
            else release(s);
        }
    release(v); release(r); release(d3);
    return st == GATT_SUCCESS;
}

static void connect_one(void *devs, const GUID want[6], uint64_t addr, uint16_t pid, int use) {
    fprintf(stderr, "[input] Switch 2 controller %012llX (%s) %s, connecting\n", (unsigned long long)addr, kind(pid),
            use == S2_ADV_PAIR ? "in Sync" : "woke toward this PC");
    Link *l = calloc(1, sizeof *l);
    void *op = NULL;
    if (!l) { give_up(addr, "out of memory"); return; }
    l->id = next_id++, l->addr = addr, l->pid = pid, l->use = use;
    if (SUCCEEDED(S_U64_GET(devs, LEDEVS_FROM_ADDRESS, addr, &op))) l->dev = async_result(op, 10000);
    if (!l->dev) { free_link(l); give_up(addr, "Windows did not open it"); return; }
    Handler *h = handler(&IID_OnStatus, on_status, l->id, 0);
    if (h) S_GET2(l->dev, LEDEV_ADD_STATUS, h, &l->t_status), h_release(h);
    if (!find_services(l, want)) { free_link(l); give_up(addr, "its services were not read (the connection failed)"); return; }
    if (!l->cmd || !l->rsp || !l->input) { free_link(l); give_up(addr, "not the GATT service of a Switch 2 controller"); return; }
    if (!subscribe(l->rsp, l->id, 0, &l->t_rsp)) { free_link(l); give_up(addr, "no replies"); return; }
    l->fast = prefer_fast(l->dev, addr);
    busy = l;
    Sess *s = handshake(l);
    busy = NULL;
    if (s && !l->gone) {
        l->next = links, links = l;
        log_conn(l->dev, addr, "after the handshake");
        return;
    }
    sess_close(l->s);
    l->s = NULL;
    free_link(l);
    give_up(addr, s ? "it disconnected during the handshake" : "the handshake failed");
}

/* The default adapter's address (host), and whether it does LE: 1 yes, -1 no, 0 no adapter. */
static int find_adapter(void) {
    void *st = NULL, *op = NULL, *a = NULL;
    UINT64 addr = 0;
    BYTE le = 0;   /* (boolean) */
    if (FAILED(factory(CLS_ADAPTER, &IID_AdapterStatics, &st))) return 0;
    if (SUCCEEDED(S_GET(st, ADAPTERS_DEFAULT, &op))) a = async_result(op, 5000);
    release(st);
    if (!a) return 0;
    S_GET(a, ADAPTER_ADDRESS, &addr);
    S_GET(a, ADAPTER_LE, &le);
    release(a);
    for (int i = 0; i < 6; i++) host[i] = (uint8_t)(addr >> 8 * i);
    return le ? 1 : -1;
}

static DWORD WINAPI bt_main(LPVOID unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"switch2 bluetooth");
    static const char *const uuids[6] = {S2_UUID_CMD_WR, S2_UUID_CMD_RSP, S2_UUID_INPUT, S2_UUID_VIB_PRO, S2_UUID_VIB_JOYCON_L, S2_UUID_VIB_JOYCON_R};
    GUID want[6];
    for (int i = 0; i < 6; i++) parse_guid(uuids[i], &want[i]);
    if (!load_ro() || FAILED(ro.Initialize(RO_MULTITHREADED))) { fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: off (no WinRT)\n"); return 0; }
    int le = find_adapter();
    if (!le) { fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: off (no Bluetooth adapter, or it is off)\n"); return 0; }
    if (le < 0) { fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: off (the adapter does not do Bluetooth LE)\n"); return 0; }
    void *devs = NULL, *watcher = NULL;
    if (FAILED(factory(CLS_CRYPTO, &IID_CryptoStatics, &bytes_to_buffer)) || FAILED(factory(CLS_LEDEVICE, &IID_LEDeviceStatics, &devs)) ||
        FAILED(activate(CLS_WATCHER, &IID_Watcher, &watcher))) {
        fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: off (Windows' Bluetooth LE classes are missing)\n");
        return 0;
    }
    S_I32(watcher, WATCHER_SCANMODE, SCAN_ACTIVE);   /* (active: the scan response too) */
    INT64 tok = 0;
    Handler *h = handler(&IID_OnReceived, on_advert, 0, 0);
    if (!h || FAILED(S_GET2(watcher, WATCHER_ADD_RECEIVED, h, &tok)) || FAILED(S_NONE(watcher, WATCHER_START))) {
        fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: off (scanning refused: is Bluetooth on?)\n");
        return 0;
    }
    h_release(h);
    fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: looking for them (adapter %02X:%02X:%02X:%02X:%02X:%02X); "
                    "hold Sync on one until its lights sweep to pair it\n", host[5], host[4], host[3], host[2], host[1], host[0]);
    for (uint64_t next_tick = 0;;) {
        pump(5);
        uint64_t now = now_ns();
        if (now >= next_tick) tick(), next_tick = now + 15000000ull;
        if (!npending) continue;
        uint64_t addr = pending[0].addr;
        uint16_t pid = pending[0].pid;
        int use = pending[0].use;
        memmove(pending, pending + 1, sizeof *pending * (size_t)--npending);
        connect_one(devs, want, addr, pid, use);
    }
}

/* ---- the input thread's side (padble.h) ----------------------------------------------------------------------- */
int padble_start(int trace_on) {
    trace = trace_on;
    InitializeCriticalSection(&q_lock);
    q_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, bt_main, NULL, 0, NULL);
    if (!t) { fprintf(stderr, "[input] Switch 2 controllers over Bluetooth: off (no thread)\n"); return 0; }
    CloseHandle(t);
    return 1;
}

/* A Joy-Con held sideways, its stick on the player's left: the left one turned a quarter left, the right one a quarter
 * right. Its buttons by where they are then: the face button on the right is A, the bottom one B, the top one X, the
 * left one Y; SL is L and SR is R. Sticks in the console's terms (+y up). */
static u32 sideways(const s2_dev *d, int right, i32 st[2]) {
    u32 b = d->r.buttons, m = 0;
    int32_t raw[2];
    s2_joy_stick(d, right, raw);
    if (!right) {   /* left: up -> left, left -> down, down -> right, right -> up */
        st[0] = -raw[1]; st[1] = raw[0];
        if (b & S2_DOWN) m |= PB_A; if (b & S2_LEFT) m |= PB_B; if (b & S2_RIGHT) m |= PB_X; if (b & S2_UP) m |= PB_Y;
        if (b & S2_SL_L) m |= PB_L; if (b & S2_SR_L) m |= PB_R;
        if (b & S2_MINUS) m |= PB_PLUS; if (b & S2_LSTICK) m |= PB_LSTICK; if (b & S2_CAPTURE) m |= PB_CAPTURE;
        if (b & S2_L) m |= PB_ZL; if (b & S2_ZL) m |= PB_ZL;
    } else {        /* right: up -> right, right -> down, down -> left, left -> up */
        st[0] = raw[1]; st[1] = -raw[0];
        if (b & S2_X) m |= PB_A; if (b & S2_A) m |= PB_B; if (b & S2_Y) m |= PB_X; if (b & S2_B) m |= PB_Y;
        if (b & S2_SL_R) m |= PB_L; if (b & S2_SR_R) m |= PB_R;
        if (b & S2_PLUS) m |= PB_PLUS; if (b & S2_RSTICK) m |= PB_LSTICK; if (b & S2_HOME) m |= PB_HOME; if (b & S2_C) m |= PB_C;
        if (b & S2_R) m |= PB_ZR; if (b & S2_ZR) m |= PB_ZR;
    }
    return m;
}

static u32 extras(u32 b) {   /* the buttons the console's NpadButtons have no bit for */
    return (b & S2_HOME ? PB_HOME : 0) | (b & S2_CAPTURE ? PB_CAPTURE : 0) | (b & S2_C ? PB_C : 0) | (b & S2_GL ? PB_GL : 0) | (b & S2_GR ? PB_GR : 0);
}

void padble_poll(PadBleState out[PADBLE_PORTS]) {
    memset(out, 0, sizeof(PadBleState) * PADBLE_PORTS);
    AcquireSRWLockShared(&pads_lock);
    for (int i = 0; i < PADBLE_PORTS; i++) {
        const Pad *p = &pads[i];
        PadBleState *o = &out[i];
        if (!p->used || !(p->pro || p->left || p->right)) continue;
        const Sess *first = p->pro ? p->pro : p->left ? p->left : p->right;
        o->connected = 1;
        o->gen = p->gen;
        snprintf(o->serial, sizeof o->serial, "%.14s", first->serial);
        snprintf(o->path, sizeof o->path, "ble:%012llX", (unsigned long long)first->link->addr);
        i32 st[4] = {0};
        uint64_t m = 0;
        if (p->pro || (p->left && p->right)) {
            int32_t s4[4];
            s2_pad_input(&p->st, &m, s4);
            st[0] = s4[0], st[1] = s4[1], st[2] = s4[2], st[3] = s4[3];
            m = (m & 0xFFFF) | extras((p->pro ? p->st.pro.r.buttons : p->st.left.r.buttons | p->st.right.r.buttons));
            o->kind = p->pro ? PADBLE_PRO : PADBLE_PAIR;
            snprintf(o->name, sizeof o->name, "%s", p->pro ? "Pro Controller 2 (Bluetooth)" : "Joy-Con 2 pair (Bluetooth)");
            snprintf(o->id, sizeof o->id, "%s", p->pro ? "switch2pro" : "joycon2pair");
        } else {
            int right = !p->left;
            m = sideways(right ? &p->st.right : &p->st.left, right, st);
            o->kind = right ? PADBLE_RIGHT : PADBLE_LEFT;
            snprintf(o->name, sizeof o->name, "%s", right ? "Joy-Con 2 (R), sideways" : "Joy-Con 2 (L), sideways");
            snprintf(o->id, sizeof o->id, "%s", right ? "joycon2r" : "joycon2l");
        }
        o->buttons = (u32)m;
        o->lx = st[0], o->ly = st[1] == -32768 ? 32767 : -st[1];   /* (SDL's terms: +y down) */
        o->rx = st[2], o->ry = st[3] == -32768 ? 32767 : -st[3];
    }
    ReleaseSRWLockShared(&pads_lock);
}

void padble_player(int port, int slot) {
    if (port < 0 || port >= PADBLE_PORTS) return;
    AcquireSRWLockExclusive(&pads_lock);
    if (pads[port].used && pads[port].player != slot) {
        pads[port].player = slot;
        for (int k = 0; k < 3; k++) {   /* the lights at the next tick */
            Sess *s = k == 0 ? pads[port].pro : k == 1 ? pads[port].left : pads[port].right;
            if (s) s->next_led = 0;
        }
    }
    ReleaseSRWLockExclusive(&pads_lock);
}

void padble_vibrate(int port, const float motors[2][4]) {
    if (port < 0 || port >= PADBLE_PORTS) return;
    s2_haptic m[2];
    for (int k = 0; k < 2; k++) {
        float low = motors[k][0], high = motors[k][2];
        low = low > 0 ? low : 0; high = high > 0 ? high : 0;
        s2_rumble_cap(&low, &high);   /* (the console's limit on both bands together) */
        m[k] = s2_haptic_from(low, motors[k][1] > 0 ? motors[k][1] : 160.f, high, motors[k][3] > 0 ? motors[k][3] : 320.f);
    }
    uint64_t now = now_ns();
    AcquireSRWLockExclusive(&pads_lock);
    if (pads[port].used) s2_rumble_log_add(&pads[port].log, now, m);
    ReleaseSRWLockExclusive(&pads_lock);
}
#endif
