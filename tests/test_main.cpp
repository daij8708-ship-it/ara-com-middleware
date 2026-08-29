/**
 * @file    test_main.cpp
 * @brief   GTest 测试入口。
 *
 */

#include <gtest/gtest.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
