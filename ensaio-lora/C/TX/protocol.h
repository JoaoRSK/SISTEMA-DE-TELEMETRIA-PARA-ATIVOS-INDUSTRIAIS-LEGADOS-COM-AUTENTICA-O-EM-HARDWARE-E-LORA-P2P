#ifndef TCC_LORA_TEST_PROTOCOL_H
#define TCC_LORA_TEST_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TEST_FRAME_SIZE 61u
#define TEST_DATA_OFFSET 10u
#define TEST_PACKETS 100u
static inline void test_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t test_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint32_t test_next(uint32_t *x) {
    *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5;
    return *x;
}
static inline void test_make_frame(uint8_t frame[TEST_FRAME_SIZE], uint32_t run, uint16_t seq) {
    frame[0] = 'T'; frame[1] = 'C'; frame[2] = 'C'; frame[3] = 'T';
    test_put32(&frame[4], run);
    frame[8] = (uint8_t)(seq >> 8); frame[9] = (uint8_t)seq;
    uint32_t state = 0x6B3D2E91u ^ run ^ ((uint32_t)seq * 0x9E3779B9u);
    if (!state) state = 0xACE1u;
    for (size_t i = TEST_DATA_OFFSET; i < TEST_FRAME_SIZE; i++)
        frame[i] = (uint8_t)test_next(&state);
}
static inline bool test_parse_frame(const uint8_t *frame, size_t len,
                                    uint32_t *run, uint16_t *seq) {
    if (len != TEST_FRAME_SIZE || frame[0] != 'T' || frame[1] != 'C' ||
        frame[2] != 'C' || frame[3] != 'T') return false;
    *run = test_get32(&frame[4]);
    *seq = ((uint16_t)frame[8] << 8) | frame[9];
    if (*seq < 1u || *seq > TEST_PACKETS) return false;
    uint8_t expected[TEST_FRAME_SIZE];
    test_make_frame(expected, *run, *seq);
    for (size_t i = TEST_DATA_OFFSET; i < TEST_FRAME_SIZE; i++)
        if (frame[i] != expected[i]) return false;
    return true;
}
#endif
