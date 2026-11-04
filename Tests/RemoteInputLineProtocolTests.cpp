// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "RemoteInputLineProtocol.h"

TEST(ParseRemoteInputLine, AcceptsCallbackNames)
{
    for (const std::string_view name : { "SimEject", "SimEject_Press", "SimEject_Release", "A", "_", "abc_XYZ_019" })
    {
        const RemoteInputLine line = ParseRemoteInputLine(name);
        EXPECT_EQ(line.kind, RemoteInputLineKind::Command) << name;
        EXPECT_EQ(line.text, name);
    }
}

TEST(ParseRemoteInputLine, RemovesCarriageReturnOfCrLf)
{
    const RemoteInputLine line = ParseRemoteInputLine("SimEject\r");

    EXPECT_EQ(line.kind, RemoteInputLineKind::Command);
    EXPECT_EQ(line.text, "SimEject");
}

TEST(ParseRemoteInputLine, RemovesOnlyOneCarriageReturn)
{
    const RemoteInputLine line = ParseRemoteInputLine("SimEject\r\r");

    EXPECT_EQ(line.kind, RemoteInputLineKind::Invalid);
    EXPECT_EQ(line.text, "SimEject\r");
}

TEST(ParseRemoteInputLine, TreatsBareLineEndingsAsEmpty)
{
    EXPECT_EQ(ParseRemoteInputLine("").kind, RemoteInputLineKind::Empty);
    EXPECT_EQ(ParseRemoteInputLine("\r").kind, RemoteInputLineKind::Empty);
}

TEST(ParseRemoteInputLine, RejectsCharactersOutsideCallbackNames)
{
    const std::string with_nul("Sim\0Eject", 9);
    for (const std::string_view text : { std::string_view("Sim Eject"), std::string_view(" SimEject"),
        std::string_view("SimEject "), std::string_view("Sim-Eject"), std::string_view("SimEject!"),
        std::string_view("Sim\tEject"), std::string_view(with_nul), std::string_view("S\xC3\xAEm"),
        std::string_view("\xEF\xBB\xBFSimEject") })
    {
        const RemoteInputLine line = ParseRemoteInputLine(text);
        EXPECT_EQ(line.kind, RemoteInputLineKind::Invalid) << text;
        EXPECT_EQ(line.text, text);
    }
}

TEST(RemoveByteOrderMark, RemovesAUtf8ByteOrderMark)
{
    EXPECT_EQ(RemoveByteOrderMark("\xEF\xBB\xBFSimEject"), "SimEject");
    EXPECT_EQ(RemoveByteOrderMark("\xEF\xBB\xBF"), "");
}

TEST(RemoveByteOrderMark, LeavesEverythingElseAlone)
{
    // Only one mark, only at the start, and only all three bytes of it.
    for (const std::string_view text : { "SimEject", "", "Sim\xEF\xBB\xBF", "\xEF\xBBSimEject", "\xBB\xBFSimEject" })
    {
        EXPECT_EQ(RemoveByteOrderMark(text), text) << text;
    }
    EXPECT_EQ(RemoveByteOrderMark("\xEF\xBB\xBF\xEF\xBB\xBFSimEject"), "\xEF\xBB\xBFSimEject");
}

TEST(SplitRemoteInputCommand, SeparatesPressAndReleaseSuffixes)
{
    const RemoteInputCallbackRequest press = SplitRemoteInputCommand("SimEject_Press");
    EXPECT_EQ(press.callback, "SimEject");
    EXPECT_EQ(press.action, RemoteInputAction::Press);

    const RemoteInputCallbackRequest release = SplitRemoteInputCommand("SimEject_Release");
    EXPECT_EQ(release.callback, "SimEject");
    EXPECT_EQ(release.action, RemoteInputAction::Release);

    const RemoteInputCallbackRequest both = SplitRemoteInputCommand("SimEject");
    EXPECT_EQ(both.callback, "SimEject");
    EXPECT_EQ(both.action, RemoteInputAction::PressAndRelease);
}

TEST(SplitRemoteInputCommand, TakesOnlyTheLastSuffix)
{
    const RemoteInputCallbackRequest request = SplitRemoteInputCommand("Odd_Press_Release");
    EXPECT_EQ(request.callback, "Odd_Press");
    EXPECT_EQ(request.action, RemoteInputAction::Release);
}

TEST(SplitRemoteInputCommand, TreatsASuffixOnItsOwnAsAName)
{
    for (const std::string_view command : { "_Press", "_Release", "Press", "SimEject_press" })
    {
        const RemoteInputCallbackRequest request = SplitRemoteInputCommand(command);
        EXPECT_EQ(request.callback, command);
        EXPECT_EQ(request.action, RemoteInputAction::PressAndRelease);
    }
}

TEST(MakeRemoteInputCommand, IsTheInverseOfSplit)
{
    for (const std::string_view command : { "SimEject", "SimEject_Press", "SimEject_Release" })
    {
        const RemoteInputCallbackRequest request = SplitRemoteInputCommand(command);
        EXPECT_EQ(MakeRemoteInputCommand(request.callback, request.action), command);
    }
}

TEST(RemoteInputActionName, NamesEveryAction)
{
    EXPECT_EQ(RemoteInputActionName(RemoteInputAction::PressAndRelease), "press and release");
    EXPECT_EQ(RemoteInputActionName(RemoteInputAction::Press), "press");
    EXPECT_EQ(RemoteInputActionName(RemoteInputAction::Release), "release");
}

TEST(IsRemoteInputNameCharacter, AllowsExactlyLettersDigitsAndUnderscore)
{
    std::string allowed;
    for (int value = -128; value <= 127; ++value)
    {
        const char character = static_cast<char>(value);
        if (IsRemoteInputNameCharacter(character))
        {
            allowed += character;
        }
    }
    EXPECT_EQ(allowed, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz");
}
