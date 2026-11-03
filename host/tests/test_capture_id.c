/*
 * test_capture_id.c - common/capture_id.h: format -> parse round trip, and the exact strings the
 * Python mirror (common/capture_id.py, host/tests/test_capture_id.py) must also produce.
 */
#include <stdio.h>
#include <string.h>

#include "capture_id.h"

static int fails;

static void expect_str(const char* got, const char* want)
{
    if (strcmp(got, want) != 0)
    {
        printf("FAIL: got '%s', want '%s'\n", got, want);
        fails++;
    }
}

static void expect_parse(const char* s, int ok, unsigned node, unsigned long id)
{
    uint8_t n = 0;
    uint32_t v = 0;
    int got = capture_id_parse(s, &n, &v);
    if (got != ok || (ok && (n != node || v != id)))
    {
        printf("FAIL: parse('%s') = %d N%u %lu, want %d N%u %lu\n", s, got, n, (unsigned long)v, ok, node, id);
        fails++;
    }
}

int main(void)
{
    char buf[CAPTURE_ID_STR_MAX];
    capture_id_format(buf, sizeof(buf), 2, 42);
    expect_str(buf, "N2-000042");
    capture_id_file_stem(buf, sizeof(buf), 2, 42);
    expect_str(buf, "N2_000042");
    capture_id_format(buf, sizeof(buf), 255, 4294967295u);
    expect_str(buf, "N255-4294967295");
    capture_id_format(buf, sizeof(buf), 5, 1234567);
    expect_str(buf, "N5-1234567");

    const uint32_t ids[] = {0, 1, 42, 999999, 1000000, 4294967295u};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
    {
        for (unsigned node = 0; node < 256; node += 51)
        {
            capture_id_format(buf, sizeof(buf), (uint8_t)node, ids[i]);
            expect_parse(buf, 1, node, ids[i]);
            capture_id_file_stem(buf, sizeof(buf), (uint8_t)node, ids[i]);
            expect_parse(buf, 1, node, ids[i]);
        }
    }
    expect_parse("N2_000042_full.scl", 1, 2, 42);
    expect_parse("n3-000007", 1, 3, 7);
    expect_parse("N2-42", 0, 0, 0);    /* fewer than 6 digits */
    expect_parse("N256-000001", 0, 0, 0);
    expect_parse("img_00.jpg", 0, 0, 0);
    expect_parse("N-000001", 0, 0, 0);
    expect_parse("", 0, 0, 0);
    printf("capture_id: %s\n", fails ? "FAILED" : "all tests passed");
    return fails ? 1 : 0;
}
