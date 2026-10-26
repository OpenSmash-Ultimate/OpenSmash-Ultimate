/* Console features with no counterpart here: screenshot album, data delivery, friends,
 * amiibo, play reports, the shop, SSL - plus the two small library
 * pieces that unexpectedly consult a service (language codes, secure random bytes).
 *
 * Everything reports "nothing there" or "not available" through the API's normal failure
 * paths, so the game behaves as a console that is offline, with no amiibo reader and an
 * empty album. */
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_RAND_S
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

#define R_UNAVAILABLE RESULT(124, 300)
#define OK { c->x[0] = 0; }
#define FAIL { c->x[0] = R_UNAVAILABLE; }
#define NOTHING { (void)c; }

/* ---- nn::settings: language codes ------------------------------------------------ */
static const char *const LANGUAGES[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                        "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};

static u64 language_code(u64 language) {
    u64 code = 0;
    if (language < sizeof LANGUAGES / sizeof *LANGUAGES) memcpy(&code, LANGUAGES[language], strlen(LANGUAGES[language]));
    return code;
}
HLE(_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE) { c->x[0] = language_code((u32)c->x[0]); }
HLE(_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE) { c->x[0] = ld64(c->x[0]) == language_code(ld32(c->x[1])); }
HLE(_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE) { c->x[0] = ld64(c->x[1]) == language_code(ld32(c->x[0])); }

/* ---- nn::crypto: the secure random source ----------------------------------------- */
int test_fixed_random(u64 buffer, u64 size, const char *who, u64 caller);   /* (hle_fight_test.c) SSBU_FIXED_SEED */
HLE(_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm) {
    if (test_fixed_random(c->x[0], c->x[1], "nn::crypto::GenerateCryptographicallyRandomBytes", c->x[30] - 4)) return;
    u8 *p = P(u8, c->x[0]);
    for (u64 i = 0; i < c->x[1]; i++) {
        unsigned int r;
        rand_s(&r);
        p[i] = (u8)r;
    }
}

/* ---- nn::album: an empty album; screenshots are accepted and discarded ------------- */
HLE(_ZN2nn5album10InitializeEv) OK
HLE(_ZN2nn5album26GetAlbumScreenshotFileListEPiPNS0_14AlbumFileEntryEi) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn5album21GetExtraMovieFileListEPiPNS0_14AlbumFileEntryEi) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE) OK
HLE(_ZN2nn5album26SaveScreenshotWithUserDataEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionES2_m) OK
HLE(_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc) OK
HLE(_ZN2nn5album20DeleteExtraMovieFileERKNS0_14AlbumFileEntryE) OK
HLE(_ZN2nn5album14LoadAlbumImageEPiS1_PmPvmS3_mS3_mRKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album14LoadAlbumImageEPiS1_PvmS2_mRKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album23LoadAlbumThumbnailImageEPiS1_PNS0_18AlbumFileAttributeEPmPvmS5_mS5_mRKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album23LoadAlbumThumbnailImageEPiS1_PNS0_18AlbumFileAttributeEPvmS4_mRKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album23LoadAlbumThumbnailImageEPiS1_PmPvmS3_mS3_mRKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album20InitializeMovieMakerEPvm) FAIL
HLE(_ZN2nn5album25PrecheckToStartMovieMakerEm) FAIL
HLE(_ZN2nn5album20OpenAlbumMovieStreamEPNS0_17MovieStreamHandleERKNS0_14AlbumFileEntryE) FAIL
HLE(_ZN2nn5album20ReadAlbumMovieStreamEPmPvmNS0_17MovieStreamHandleEl) FAIL
HLE(_ZN2nn5album23GetAlbumMovieStreamSizeEPmNS0_17MovieStreamHandleE) FAIL
HLE(_ZN2nn5album21CloseAlbumMovieStreamENS0_17MovieStreamHandleE) NOTHING

/* ---- nn::bcat: no delivered data ------------------------------------------------- */
HLE(_ZN2nn4bcat10InitializeEv) NOTHING
HLE(_ZN2nn4bcat25MountDeliveryCacheStorageEv) FAIL
HLE(_ZN2nn4bcat27UnmountDeliveryCacheStorageEv) NOTHING
HLE(_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi) { st32(c->x[0], 0); c->x[0] = R_UNAVAILABLE; }
HLE(_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE) FAIL
HLE(_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressEPKc) FAIL
HLE(_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE) FAIL
HLE(_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi) { st32(c->x[1], 0); c->x[0] = R_UNAVAILABLE; }
HLE(_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv) NOTHING
HLE(_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev) NOTHING
HLE(_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE) FAIL
HLE(_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm) FAIL
HLE(_ZN2nn4bcat17DeliveryCacheFile5CloseEv) NOTHING
HLE(_ZN2nn4bcat17DeliveryCacheFileD1Ev) NOTHING

/* ---- nn::friends: nobody --------------------------------------------------------- */
HLE(_ZN2nn7friends10InitializeEv) NOTHING
/* The notification queue: its event exists (the game polls it) but nothing ever arrives. */
static u64 notification_event(Cpu *c) {
    static u64 event;   /* (published once it is made: another thread may take it and use it) */
    static const u8 zero[0x80];
    u64 e = __atomic_load_n(&event, __ATOMIC_ACQUIRE);
    if (!e) {
        e = rt_guest_copy(zero, sizeof zero);
        rt_callsym(c, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb", 3, e, 0, 0);
        __atomic_store_n(&event, e, __ATOMIC_RELEASE);
    }
    return e;
}
HLE(_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE) { notification_event(c); c->x[0] = 0; }
HLE(_ZN2nn7friends17NotificationQueue14GetSystemEventEv) { c->x[0] = notification_event(c); }
HLE(_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE) FAIL
HLE(_ZN2nn7friends17NotificationQueue8FinalizeEv) NOTHING
HLE(_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_) OK
/* The token a player hands the others in a local match, for their play history (which goes
 * nowhere here). Pia needs one for every local player before it sets up a room; its contents
 * are the console's own business, so this one just names the user. */
HLE(_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb) {
    u8 *key = P(u8, c->x[0]);
    memset(key, 0, 0x40);
    key[0] = 1;                          /* type */
    key[3] = (u8)(c->x[2] & 1);          /* local play */
    memcpy(key + 0x10, P(void, c->x[1]), 16);
    c->x[0] = 0;
}
HLE(_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi) FAIL
HLE(_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi) FAIL

/* ---- nn::nfp: no amiibo reader --------------------------------------------------- */
HLE(_ZN2nn3nfp10InitializeEv) NOTHING
HLE(_ZN2nn3nfp8FinalizeEv) NOTHING
HLE(_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeE) FAIL
HLE(_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp7RestoreERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE) FAIL
/* the availability, activate and deactivate events exist and never fire */
static void nfp_event(Cpu *c, u64 event) {
    rt_callsym(c, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb", 3, event, 0, 0);
    c->x[0] = 0;
}
HLE(_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE) { nfp_event(c, c->x[0]); }
HLE(_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE) { nfp_event(c, c->x[0]); }
HLE(_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE) { nfp_event(c, c->x[0]); }
HLE(_ZN2nn3nfp8GetStateEv) { c->x[0] = 1; }                                     /* initialized */
HLE(_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii) { st32(c->x[1], 0); c->x[0] = RESULT(115, 64); }   /* no device */
HLE(_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE) { c->x[0] = 5; }          /* unavailable */
HLE(_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE) { st32(c->x[0], 0); c->x[0] = 0; }
HLE(_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE) OK
HLE(_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE) OK
HLE(_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE) FAIL
HLE(_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj) FAIL
HLE(_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm) FAIL
HLE(_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm) FAIL
HLE(_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE) FAIL
HLE(_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE) FAIL
HLE(_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE) FAIL
HLE(_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE) FAIL
HLE(_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_) FAIL

/* ---- play reports, shop, profanity filter (local wireless: hle_net.c) ----------- */
HLE(_ZN2nn5prepo10InitializeEv) NOTHING
HLE(_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE) OK
HLE(_ZN2nn5prepo28RequestImmediateTransmissionEv) OK
HLE(_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm) NOTHING
HLE(_ZN2nn3ngc15ProfanityFilter10InitializeEPvm) OK
HLE(_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj) { if (c->x[1]) st32(c->x[1], 0); c->x[0] = 0; }
HLE(_ZN2nn3ngc15ProfanityFilterD1Ev) NOTHING

/* ---- SSL: initialised, never used (no servers; sockets are in hle_net.c) ------------- */
HLE(nnsslInitialize) OK
HLE(nnsslFinalize) OK
