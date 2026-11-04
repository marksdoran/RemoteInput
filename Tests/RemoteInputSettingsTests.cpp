// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "gtest/gtest.h"

#include "RemoteInputLog.h"
#include "RemoteInputSettings.h"
#include "TestLog.h"

TEST(RemoteInputSettings, DefaultsToDisabled)
{
    const RemoteInputSettings settings;
    EXPECT_EQ(settings.mode, RemoteInputMode::Disabled);
}

TEST(MakeRemoteInputSettings, ModeZeroDisablesWithoutComplaint)
{
    TestLog log;
    const RemoteInputSettings settings = MakeRemoteInputSettings(0, 0, "", log);

    EXPECT_EQ(settings.mode, RemoteInputMode::Disabled);
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Error), 0u) << log.Dump();
}

TEST(MakeRemoteInputSettings, AcceptsLocalHostOnly)
{
    TestLog log;
    const RemoteInputSettings settings = MakeRemoteInputSettings(1, 4242, "", log);

    EXPECT_EQ(settings.mode, RemoteInputMode::LocalHostOnly);
    EXPECT_EQ(settings.port, 4242);
    EXPECT_TRUE(settings.allowed_clients.empty());
}

TEST(MakeRemoteInputSettings, AcceptsLanWithAllowedClients)
{
    TestLog log;
    const RemoteInputSettings settings = MakeRemoteInputSettings(2, 4242, "192.168.1.42", log);

    EXPECT_EQ(settings.mode, RemoteInputMode::Lan);
    EXPECT_EQ(settings.port, 4242);
    EXPECT_EQ(settings.allowed_clients, "192.168.1.42");
}

TEST(MakeRemoteInputSettings, UnknownModeDisablesAndLogsError)
{
    for (const int mode : { -1, 3, 42 })
    {
        TestLog log;
        const RemoteInputSettings settings = MakeRemoteInputSettings(mode, 4242, "", log);

        EXPECT_EQ(settings.mode, RemoteInputMode::Disabled) << "mode " << mode;
        EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Error, "is not valid")) << log.Dump();
    }
}

TEST(MakeRemoteInputSettings, InvalidPortDisablesAndLogsError)
{
    for (const int port : { -1, 0, 65536, 100000 })
    {
        TestLog log;
        const RemoteInputSettings settings = MakeRemoteInputSettings(2, port, "192.168.1.42", log);

        EXPECT_EQ(settings.mode, RemoteInputMode::Disabled) << "port " << port;
        EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Error, "Port")) << log.Dump();
    }
}

TEST(MakeRemoteInputSettings, AcceptsLowestAndHighestPorts)
{
    TestLog log;
    EXPECT_EQ(MakeRemoteInputSettings(1, 1, "", log).port, 1);
    EXPECT_EQ(MakeRemoteInputSettings(1, 65535, "", log).port, 65535);
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Error), 0u) << log.Dump();
}

TEST(MakeRemoteInputSettings, PortIsNotCheckedWhenDisabled)
{
    TestLog log;
    MakeRemoteInputSettings(0, -5, "", log);
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Error), 0u) << log.Dump();
}

TEST(MakeRemoteInputSettings, IgnoresAllowedClientsOutsideLanMode)
{
    TestLog log;
    const RemoteInputSettings settings = MakeRemoteInputSettings(1, 4242, "192.168.1.42", log);

    EXPECT_EQ(settings.mode, RemoteInputMode::LocalHostOnly);
    EXPECT_TRUE(settings.allowed_clients.empty());
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "LAN mode only")) << log.Dump();
}

TEST(RemoteInputModeName, NamesEveryMode)
{
    EXPECT_EQ(RemoteInputModeName(RemoteInputMode::Disabled), "disabled");
    EXPECT_EQ(RemoteInputModeName(RemoteInputMode::LocalHostOnly), "local host only");
    EXPECT_EQ(RemoteInputModeName(RemoteInputMode::Lan), "LAN");
}
