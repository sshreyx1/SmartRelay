/*
 * test_capture_id.c - common/capture_id.h: format -> parse round trip, and the exact strings the
 * Python mirror (common/capture_id.py, host/tests/test_capture_id.py) must also produce.
 */
#include <stdio.h>
#include <string.h>

#include "capture_id.h"

static int fails;

static void expect_str(const char* p_got, const char* p_want)
{
    if (0 != strcmp(p_got, p_want))
    {
        printf("FAIL: got '%s', want '%s'\n", p_got, p_want);
        fails++;
    }
}

static void expect_parse(const char* p_str, int ok_flag, unsigned node, unsigned long cap_id)
{
    uint8_t got_node = 0;
    uint32_t value = 0;
    int got = capture_id_parse(p_str, &got_node, &value);
    if ((got != ok_flag) || (ok_flag && ((got_node != node) || (value != cap_id))))
    {
        printf("FAIL: parse('%s') = %d N%u %lu, want %d N%u %lu\n", p_str, got, got_node, (unsigned long)value, ok_flag, node, cap_id);
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
    for (unsigned idx = 0; idx < sizeof(ids) / sizeof(ids[0]); idx++)
    {
        for (unsigned node = 0; node < 256; node += 51)
        {
            capture_id_format(buf, sizeof(buf), (uint8_t)node, ids[idx]);
            expect_parse(buf, 1, node, ids[idx]);
            capture_id_file_stem(buf, sizeof(buf), (uint8_t)node, ids[idx]);
            expect_parse(buf, 1, node, ids[idx]);
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
