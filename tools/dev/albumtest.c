/* Offline checks of the album (runtime/hle_album.c) without the game: pictures saved, listed and
 * loaded back as the Vault asks for them, in a made-up album folder (SSBU_DATA_ROOT, which
 * tests/test_album.py sets to a fresh folder).
 *
 *   python -c "import sys; sys.path.insert(0,'tools'); import cc, subprocess; subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.CFLAGS, 'tools/dev/albumtest.c', '/Febuild/albumtest.exe'], check=True)"
 *   set SSBU_DATA_ROOT=<an empty folder> & build\albumtest.exe
 *
 * Exit code 0 when every check passes. */
#include "../../runtime/hle_album.c"

const char *rt_data_dir = ".";

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u8 *picture(int w, int h, int seed) {
    u8 *p = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u8 *q = p + ((size_t)y * w + x) * 4;
            q[0] = (u8)(x + seed); q[1] = (u8)(y * 3); q[2] = (u8)(x ^ y); q[3] = 255;
        }
    return p;
}

static int exists(const char *name) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", album_dir(), name);
    return file_size(path) != ~0ull;
}

static void base_name(const AlbumEntry *e, char *out, size_t n) {
    snprintf(out, n, "%04d%02d%02d_%02d%02d%02d_%02d", e->date.year, e->date.month, e->date.day, e->date.hour, e->date.minute, e->date.second,
             e->date.id);
}

int main(void) {
    AlbumEntry list[8], e1, e2, e3;
    char name[64], path[640];
    CHECK(album_list(list, 8) == 0, "an album not made yet is empty");

    /* a picture with its application data */
    u8 *pic = picture(ALBUM_W, ALBUM_H, 7);
    u8 user[5] = {1, 2, 3, 4, 5};
    CHECK(album_save(pic, (u64)ALBUM_W * ALBUM_H * 4, user, sizeof user, "test", &e1) == 0, "saved");
    base_name(&e1, name, sizeof name);
    snprintf(path, sizeof path, "%s.bmp", name);
    CHECK(exists(path), "%s written", path);
    snprintf(path, sizeof path, "%s.userdata.bin", name);
    CHECK(exists(path), "%s written", path);
    CHECK(e1.content == CONTENT_SCREENSHOT && e1.one == 1 && e1.size == 54 + (u64)ALBUM_W * ALBUM_H * 4 && e1.hash == entry_hash(&e1.date, 0),
          "the entry (size %llu)", (unsigned long long)e1.size);
    CHECK(album_list(list, 8) == 1 && !memcmp(&list[0], &e1, sizeof e1), "listed as the entry it was saved as");

    /* loaded back whole */
    size_t big = (size_t)ALBUM_W * ALBUM_H * 4;
    u8 *buf = malloc(big), work[64], got_user[16];
    i32 w = -1, h = -1;
    u64 user_size = 0, attr[2] = {9, 9};
    memset(work, 0xAA, sizeof work);
    memset(got_user, 0, sizeof got_user);
    CHECK(album_load(&w, &h, NULL, &user_size, got_user, sizeof got_user, buf, big, work, sizeof work, &list[0], 0) == 0, "loaded");
    CHECK(w == ALBUM_W && h == ALBUM_H && !memcmp(buf, pic, big), "the same picture (%dx%d)", w, h);
    CHECK(user_size == 5 && !memcmp(got_user, user, 5), "its application data");
    CHECK(work[0] == 0 && work[63] == 0, "the work buffer cleared");
    u8 cut[2] = {0, 0};
    CHECK(album_load(&w, &h, NULL, &user_size, cut, 2, buf, big, NULL, 0, &list[0], 0) == 0 && user_size == 5 && cut[0] == 1 && cut[1] == 2,
          "application data cut to the caller's room, its whole size said");

    /* the thumbnail: 320x180, each pixel the mean of a 4x4 block */
    size_t tsize = (size_t)THUMB_W * THUMB_H * 4;
    u8 *thumb = malloc(tsize);
    CHECK(album_load(&w, &h, attr, NULL, NULL, 0, thumb, tsize, NULL, 0, &list[0], 1) == 0 && w == THUMB_W && h == THUMB_H, "thumbnail");
    CHECK(attr[0] == 0 && attr[1] == 0, "the attribute");
    {
        unsigned sum = 0;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) sum += pic[((size_t)(4 * 10 + y) * ALBUM_W + 4 * 20 + x) * 4 + 2];
        CHECK(thumb[((size_t)10 * THUMB_W + 20) * 4 + 2] == (sum + 8) / 16, "thumbnail pixel %d, wanted %u", thumb[((size_t)10 * THUMB_W + 20) * 4 + 2], (sum + 8) / 16);
    }

    /* more in the same second: the next numbers; the newest when asked for fewer */
    CHECK(album_save(pic, (u64)ALBUM_W * ALBUM_H * 4, NULL, 0, "test", &e2) == 0, "second");
    u8 *hd = picture(1920, 1080, 3);
    CHECK(album_save(hd, 1920ull * 1080 * 4, NULL, 0, "test", &e3) == 0, "a 1920x1080 picture");
    CHECK(album_list(list, 8) == 3 && !memcmp(&list[0], &e1, sizeof e1) && !memcmp(&list[1], &e2, sizeof e2) && !memcmp(&list[2], &e3, sizeof e3),
          "three, oldest first");
    CHECK(date_cmp(&e1.date, &e2.date) < 0 && date_cmp(&e2.date, &e3.date) < 0, "each its own name");
    CHECK(album_list(list, 2) == 2 && !memcmp(&list[0], &e2, sizeof e2) && !memcmp(&list[1], &e3, sizeof e3), "max 2: the newest two");
    CHECK(album_load(&w, &h, NULL, &user_size, NULL, 0, buf, big, NULL, 0, &e3, 0) == 0 && w == ALBUM_W && h == ALBUM_H && user_size == 0,
          "a 1920x1080 picture loads as 1280x720, no application data");

    /* refusals */
    CHECK(album_save(pic, 1234, NULL, 0, "test", NULL) != 0 && album_list(list, 8) == 3, "not a picture's size: nothing saved");
    AlbumEntry bad = e1;
    bad.hash ^= 1;
    CHECK(album_load(&w, &h, NULL, NULL, NULL, 0, buf, big, NULL, 0, &bad, 0) == 0xd6d6 && w == 0 && h == 0, "a wrong hash: not found, outputs cleared");
    bad = e1;
    bad.content = CONTENT_EXTRA_MOVIE;
    CHECK(album_load(&w, &h, NULL, NULL, NULL, 0, buf, big, NULL, 0, &bad, 0) == 0xd6d6, "not a screenshot");
    CHECK(album_load(&w, &h, NULL, NULL, NULL, 0, buf, big - 1, NULL, 0, &e1, 0) == 0xded6, "the buffer too cut");
    CHECK(album_load(&w, &h, NULL, NULL, NULL, 0, buf, big, NULL, 0, NULL, 0) == 0xd6d6, "no entry");

    /* what is not the album's is passed over; a broken picture is refused */
    FILE *f;
    snprintf(path, sizeof path, "%s/notes.bmp", album_dir());
    if ((f = fopen(path, "wb")) != NULL) { fputs("x", f); fclose(f); }
    snprintf(path, sizeof path, "%s/20240101_120000_00.txt", album_dir());
    if ((f = fopen(path, "wb")) != NULL) { fputs("x", f); fclose(f); }
    snprintf(path, sizeof path, "%s/20240101_120000_00.bmp", album_dir());
    if ((f = fopen(path, "wb")) != NULL) { fputs("not a picture", f); fclose(f); }
    int n = album_list(list, 8);
    CHECK(n == 4 && list[0].date.year == 2024 && !memcmp(&list[1], &e1, sizeof e1), "foreign names passed over, the old one first (%d)", n);
    CHECK(album_load(&w, &h, NULL, NULL, NULL, 0, buf, big, NULL, 0, &list[0], 0) == 0xd6d6, "a broken picture refused");

    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures != 0;
}
