// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include <string>

#include "gtest/gtest.h"

#include "RemoteInputLog.h"

TEST(QuoteForRemoteInputLog, QuotesPrintableText)
{
    EXPECT_EQ(QuoteForRemoteInputLog("Sim Eject!"), "\"Sim Eject!\"");
    EXPECT_EQ(QuoteForRemoteInputLog(""), "\"\"");
}

TEST(QuoteForRemoteInputLog, EscapesControlCharactersAndNonAscii)
{
    EXPECT_EQ(QuoteForRemoteInputLog("a\tb\r\n"), "\"a\\x09b\\x0D\\x0A\"");
    EXPECT_EQ(QuoteForRemoteInputLog(std::string("a\0b", 3)), "\"a\\x00b\"");
    EXPECT_EQ(QuoteForRemoteInputLog("\x1B[2J"), "\"\\x1B[2J\"");    // a terminal escape sequence
    EXPECT_EQ(QuoteForRemoteInputLog("caf\xC3\xA9"), "\"caf\\xC3\\xA9\"");   // UTF-8 for "cafe" with an accent
}

TEST(QuoteForRemoteInputLog, EscapesQuotesAndBackslashes)
{
    EXPECT_EQ(QuoteForRemoteInputLog("say \"hi\" \\"), "\"say \\x22hi\\x22 \\x5C\"");
}

TEST(QuoteForRemoteInputLog, ShortensLongText)
{
    const std::string quoted = QuoteForRemoteInputLog(std::string(1000, 'A'));
    EXPECT_EQ(quoted, "\"" + std::string(64, 'A') + "\" (first 64 of 1000 bytes)");
}

TEST(RemoteInputLogLevelName, NamesEveryLevel)
{
    EXPECT_EQ(RemoteInputLogLevelName(RemoteInputLogLevel::Verbose), "verbose");
    EXPECT_EQ(RemoteInputLogLevelName(RemoteInputLogLevel::Info), "info");
    EXPECT_EQ(RemoteInputLogLevelName(RemoteInputLogLevel::Warning), "warning");
    EXPECT_EQ(RemoteInputLogLevelName(RemoteInputLogLevel::Error), "error");
}
