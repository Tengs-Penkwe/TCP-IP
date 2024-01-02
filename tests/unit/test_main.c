#include "unity.h"

void setUp(void) {
    // Code to run before each test
}

void tearDown(void) {
    // Code to run after each test
}

extern void all_hton16_tests(void);
extern void all_hton6_tests(void);

extern void all_maccmp_tests(void);
extern void all_tomac_tests(void);
extern void all_frommac_tests(void);
extern void all_mem2mac_tests(void);


int main(void) {
    UNITY_BEGIN();
    // RUN_TEST(test_ArpInit);
    // RUN_TEST(test_ArpRegisterAndLookup);
    // RUN_TEST(test_ArpDestroy);

    // RUN_TEST(test_IcmpInit);
    // RUN_TEST(test_IcmpMarshalUnmarshal);
    
    RUN_TEST(all_hton16_tests);
    RUN_TEST(all_hton6_tests);
    
    RUN_TEST(all_maccmp_tests);
    RUN_TEST(all_tomac_tests);
    RUN_TEST(all_frommac_tests);
    RUN_TEST(all_mem2mac_tests);
    return UNITY_END();
}