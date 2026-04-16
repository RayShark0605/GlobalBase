#include "GB_Utf8String.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    int totalCaseCount = 0;

    std::string FormatBytes(const std::string& text)
    {
        std::ostringstream stream;
        stream << '"';
        for (size_t i = 0; i < text.size(); i++)
        {
            const unsigned char byteValue = static_cast<unsigned char>(text[i]);
            if (byteValue >= 0x20 && byteValue <= 0x7E && byteValue != '\\' && byteValue != '"')
            {
                stream << static_cast<char>(byteValue);
            }
            else
            {
                stream << "\\x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byteValue) << std::dec;
            }
        }
        stream << '"';
        return stream.str();
    }

    std::string FormatCodePoint(const char32_t codePoint)
    {
        std::ostringstream stream;
        stream << "U+" << std::uppercase << std::hex << static_cast<uint32_t>(codePoint);
        return stream.str();
    }

    std::string MakeBytes(const std::initializer_list<uint32_t>& byteValues)
    {
        std::string result;
        result.reserve(byteValues.size());
        for (const uint32_t byteValue : byteValues)
        {
            result.push_back(static_cast<char>(byteValue & 0xFFu));
        }
        return result;
    }

    [[noreturn]] void Fail(const std::string& caseName, const std::string& detail)
    {
        std::cerr << "[FAILED] " << caseName << "\n" << detail << std::endl;
        std::exit(1);
    }

    void RequireTrue(const bool condition, const std::string& caseName, const std::string& detail)
    {
        totalCaseCount++;
        if (!condition)
        {
            Fail(caseName, detail);
        }
    }

    void RequireFalse(const bool condition, const std::string& caseName)
    {
        RequireTrue(!condition, caseName, "Expected false, but got true.");
    }

    void RequireEqualString(const std::string& actualValue, const std::string& expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue != expectedValue)
        {
            Fail(caseName, std::string("Expected: ") + FormatBytes(expectedValue) + "\nActual  : " + FormatBytes(actualValue));
        }
    }

    void RequireEqualBool(const bool actualValue, const bool expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue != expectedValue)
        {
            Fail(caseName, std::string("Expected: ") + (expectedValue ? "true" : "false") + "\nActual  : " + (actualValue ? "true" : "false"));
        }
    }

    void RequireEqualInt64(const int64_t actualValue, const int64_t expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue != expectedValue)
        {
            std::ostringstream stream;
            stream << "Expected: " << expectedValue << "\nActual  : " << actualValue;
            Fail(caseName, stream.str());
        }
    }

    void RequireEqualSize(const size_t actualValue, const size_t expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue != expectedValue)
        {
            std::ostringstream stream;
            stream << "Expected: " << expectedValue << "\nActual  : " << actualValue;
            Fail(caseName, stream.str());
        }
    }

    void RequireEqualInt(const int actualValue, const int expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue != expectedValue)
        {
            std::ostringstream stream;
            stream << "Expected: " << expectedValue << "\nActual  : " << actualValue;
            Fail(caseName, stream.str());
        }
    }

    void RequireEqualCodePoint(const char32_t actualValue, const char32_t expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue != expectedValue)
        {
            Fail(caseName, std::string("Expected: ") + FormatCodePoint(expectedValue) + "\nActual  : " + FormatCodePoint(actualValue));
        }
    }

    void RequireEqualStringVector(const std::vector<std::string>& actualValue, const std::vector<std::string>& expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue.size() != expectedValue.size())
        {
            std::ostringstream stream;
            stream << "Expected size: " << expectedValue.size() << "\nActual size  : " << actualValue.size();
            Fail(caseName, stream.str());
        }

        for (size_t i = 0; i < actualValue.size(); i++)
        {
            if (actualValue[i] != expectedValue[i])
            {
                std::ostringstream stream;
                stream << "Mismatch at index " << i << "\nExpected: " << FormatBytes(expectedValue[i]) << "\nActual  : " << FormatBytes(actualValue[i]);
                Fail(caseName, stream.str());
            }
        }
    }

    void RequireEqualCodePointVector(const std::vector<char32_t>& actualValue, const std::vector<char32_t>& expectedValue, const std::string& caseName)
    {
        totalCaseCount++;
        if (actualValue.size() != expectedValue.size())
        {
            std::ostringstream stream;
            stream << "Expected size: " << expectedValue.size() << "\nActual size  : " << actualValue.size();
            Fail(caseName, stream.str());
        }

        for (size_t i = 0; i < actualValue.size(); i++)
        {
            if (actualValue[i] != expectedValue[i])
            {
                std::ostringstream stream;
                stream << "Mismatch at index " << i << "\nExpected: " << FormatCodePoint(expectedValue[i]) << "\nActual  : " << FormatCodePoint(actualValue[i]);
                Fail(caseName, stream.str());
            }
        }
    }

    void ExpectThrow(const std::function<void()>& functionObject, const std::string& caseName)
    {
        totalCaseCount++;
        try
        {
            functionObject();
        }
        catch (const std::exception&)
        {
            return;
        }
        catch (...)
        {
            return;
        }

        Fail(caseName, "Expected an exception, but no exception was thrown.");
    }

    void TestMakeUtf8String()
    {
        RequireEqualString(GB_MakeUtf8String(static_cast<const char*>(nullptr)), "", "GB_MakeUtf8String(const char*) nullptr");
        RequireEqualString(GB_MakeUtf8String("abc"), "abc", "GB_MakeUtf8String(const char*) normal");
        RequireEqualString(GB_MakeUtf8String(U'A'), "A", "GB_MakeUtf8String(char32_t) ASCII");
        RequireEqualString(GB_MakeUtf8String(U'中'), GB_STR("中"), "GB_MakeUtf8String(char32_t) CJK");
        RequireEqualString(GB_MakeUtf8String(static_cast<char32_t>(0x1F642)), GB_STR("🙂"), "GB_MakeUtf8String(char32_t) emoji");
        RequireEqualString(GB_MakeUtf8String(static_cast<char32_t>(0x110000)), GB_STR("�"), "GB_MakeUtf8String(char32_t) invalid replacement");
    }

    void TestEncodingConversion()
    {
        const std::string asciiText = "Hello_123";
        RequireEqualString(GB_Utf8ToAnsi(asciiText), asciiText, "GB_Utf8ToAnsi ASCII roundtrip");
        RequireEqualString(GB_AnsiToUtf8(asciiText), asciiText, "GB_AnsiToUtf8 ASCII roundtrip");

        const std::string utf8SigBytes = MakeBytes({ 0xEF, 0xBB, 0xBF, 'a', 'b', 'c' });
        RequireEqualString(GB_BytesToUtf8(utf8SigBytes, "utf-8-sig"), "abc", "GB_BytesToUtf8 utf-8-sig");
        RequireEqualString(GB_BytesToUtf8("ASCII", "ascii"), "ASCII", "GB_BytesToUtf8 ascii");
        RequireEqualString(GB_BytesToUtf8(MakeBytes({ 0xE9 }), "latin1"), GB_STR("é"), "GB_BytesToUtf8 latin1");
        RequireEqualString(GB_BytesToUtf8(MakeBytes({ 0x60, 0x4F, 0x7D, 0x59 }), "utf16le"), GB_STR("你好"), "GB_BytesToUtf8 utf16le");
        RequireEqualString(GB_BytesToUtf8(MakeBytes({ 0x4F, 0x60, 0x59, 0x7D }), "utf16be"), GB_STR("你好"), "GB_BytesToUtf8 utf16be");
        RequireEqualString(GB_BytesToUtf8(MakeBytes({ 0xFF, 0xFE, 0x60, 0x4F, 0x7D, 0x59 }), "utf16"), GB_STR("你好"), "GB_BytesToUtf8 utf16 with bom");
        RequireEqualString(GB_BytesToUtf8(MakeBytes({ 0x41, 0x00, 0x00, 0x00, 0x2D, 0x4E, 0x00, 0x00 }), "utf32le"), GB_MakeUtf8String(U'A') + GB_MakeUtf8String(U'中'), "GB_BytesToUtf8 utf32le");

        ExpectThrow([]() { GB_BytesToUtf8(std::string("abc"), "unknown-encoding-name"); }, "GB_BytesToUtf8 unsupported encoding");
        ExpectThrow([]() { GB_BytesToUtf8(MakeBytes({ 0xC0, 0xAF }), "utf8"); }, "GB_BytesToUtf8 invalid utf8 input");
    }

    void TestValidationAndHeuristics()
    {
        const std::string utf8Text = GB_STR("Hello世界");
        RequireTrue(GB_IsUtf8(utf8Text), "GB_IsUtf8 valid utf8", "Expected valid UTF-8.");
        RequireFalse(GB_IsUtf8(MakeBytes({ 0xC0, 0xAF })), "GB_IsUtf8 invalid utf8");
        RequireEqualBool(GB_IsAnsi(std::string()), true, "GB_IsAnsi empty");
        RequireEqualBool(GB_IsAnsi("ASCII"), true, "GB_IsAnsi ascii");
        RequireEqualBool(GB_LooksLikeUtf8("ASCII"), false, "GB_LooksLikeUtf8 ascii ambiguous");
        RequireEqualBool(GB_LooksLikeAnsi("ASCII"), false, "GB_LooksLikeAnsi ascii ambiguous");
        RequireEqualBool(GB_LooksLikeUtf8(GB_STR("你好，世界")), true, "GB_LooksLikeUtf8 chinese utf8");
    }

    void TestWideStringConversion()
    {
        const std::wstring wideText = L"Hello世界";
        const std::string utf8Text = GB_WStringToUtf8(wideText);
        RequireEqualString(utf8Text, GB_STR("Hello世界"), "GB_WStringToUtf8 normal");
        RequireTrue(GB_Utf8ToWString(utf8Text) == wideText, "GB_Utf8ToWString roundtrip", "Wide string roundtrip mismatch.");
        ExpectThrow([]() { GB_Utf8ToWString(MakeBytes({ 0xC0, 0xAF })); }, "GB_Utf8ToWString invalid utf8");
    }

    void TestCodePointAccess()
    {
        const std::string text = GB_MakeUtf8String(U'A') + GB_MakeUtf8String(U'中') + GB_MakeUtf8String(static_cast<char32_t>(0x1F642));
        const std::vector<char32_t> expectedCodePoints = { U'A', U'中', static_cast<char32_t>(0x1F642) };

        RequireEqualCodePointVector(GB_Utf8StringToChar32Vector(text), expectedCodePoints, "GB_Utf8StringToChar32Vector valid");
        RequireEqualSize(GB_GetUtf8Length(text), 3, "GB_GetUtf8Length valid");
        RequireEqualCodePoint(GB_GetUtf8Char(text, 0), U'A', "GB_GetUtf8Char index 0");
        RequireEqualCodePoint(GB_GetUtf8Char(text, 1), U'中', "GB_GetUtf8Char index 1");
        RequireEqualCodePoint(GB_GetUtf8Char(text, 2), static_cast<char32_t>(0x1F642), "GB_GetUtf8Char index 2");
        RequireEqualCodePoint(GB_GetUtf8Char(text, -1), static_cast<char32_t>(0x110000), "GB_GetUtf8Char negative index");
        RequireEqualCodePoint(GB_GetUtf8Char(text, 99), static_cast<char32_t>(0x110000), "GB_GetUtf8Char out of range");

        const std::string invalidText = MakeBytes({ 'A', 0xFF, 'B' });
        RequireEqualSize(GB_GetUtf8Length(invalidText), 3, "GB_GetUtf8Length invalid byte counts as one");
        RequireEqualCodePoint(GB_GetUtf8Char(invalidText, 1), static_cast<char32_t>(0x110000), "GB_GetUtf8Char invalid byte sentinel");

        RequireEqualString(GB_Utf8Substr(text, 1, 1), GB_STR("中"), "GB_Utf8Substr middle");
        RequireEqualString(GB_Utf8Substr(text, 1, std::numeric_limits<int64_t>::max()), GB_STR("中🙂"), "GB_Utf8Substr to end");
        RequireEqualString(GB_Utf8Substr(text, -1, 1), "", "GB_Utf8Substr negative start");
        RequireEqualString(GB_Utf8Substr(text, 5, 1), "", "GB_Utf8Substr start out of range");
    }

    void TestCaseTransformAndSplit()
    {
        RequireEqualString(GB_Utf8ToLower(GB_STR("AbC中!")), GB_STR("abc中!"), "GB_Utf8ToLower ascii only");
        RequireEqualString(GB_Utf8ToUpper(GB_STR("AbC中!")), GB_STR("ABC中!"), "GB_Utf8ToUpper ascii only");

        const std::string splitText = GB_STR("甲,乙,,丙");
        RequireEqualStringVector(GB_Utf8Split(splitText, U',', true), { GB_STR("甲"), GB_STR("乙"), GB_STR("丙") }, "GB_Utf8Split remove empty");
        RequireEqualStringVector(GB_Utf8Split(splitText, U',', false), { GB_STR("甲"), GB_STR("乙"), "", GB_STR("丙") }, "GB_Utf8Split keep empty");
    }

    void TestCompareAndSearch()
    {
        RequireEqualBool(GB_Utf8Equals(GB_STR("Hello"), GB_STR("hELLo"), false), true, "GB_Utf8Equals ascii case insensitive");
        RequireEqualBool(GB_Utf8Equals(GB_STR("Hello"), GB_STR("World"), false), false, "GB_Utf8Equals mismatch");
        RequireTrue(GB_Utf8CompareLogical("file2", "file10") < 0, "GB_Utf8CompareLogical natural order", "Expected file2 < file10.");
        RequireEqualInt(GB_Utf8CompareLogical("same", "same"), 0, "GB_Utf8CompareLogical equal");

        const std::string text = GB_STR("Hello世界Hello");
        RequireEqualBool(GB_Utf8StartsWith(text, GB_STR("heLLo"), false), true, "GB_Utf8StartsWith case insensitive");
        RequireEqualBool(GB_Utf8EndsWith(text, GB_STR("世界Hello"), true), true, "GB_Utf8EndsWith exact");
        RequireEqualBool(GB_Utf8EndsWith(text, GB_STR("世界hello"), false), true, "GB_Utf8EndsWith case insensitive");

        const std::string repeatedText = GB_STR("甲乙甲乙甲");
        const std::string needle = GB_STR("甲乙");
        RequireEqualInt64(GB_Utf8Find(repeatedText, needle, true, 0), 0, "GB_Utf8Find first");
        RequireEqualInt64(GB_Utf8Find(repeatedText, needle, true, 1), 2, "GB_Utf8Find with startPos");
        RequireEqualInt64(GB_Utf8Find(repeatedText, GB_STR("不存在"), true, 0), -1, "GB_Utf8Find not found");
        RequireEqualInt64(GB_Utf8Find(repeatedText, "", true, 3), 3, "GB_Utf8Find empty needle");
        RequireEqualInt64(GB_Utf8FindLast(repeatedText, needle, true), 2, "GB_Utf8FindLast last");
    }

    void TestTrimSimplifyReplaceAndFormat()
    {
        RequireEqualString(GB_Utf8Trim(" \tHello\r\n"), "Hello", "GB_Utf8Trim default");
        RequireEqualString(GB_Utf8TrimLeft("***Hello***", "*"), "Hello***", "GB_Utf8TrimLeft custom");
        RequireEqualString(GB_Utf8TrimRight("***Hello***", "*"), "***Hello", "GB_Utf8TrimRight custom");
        RequireEqualString(GB_Utf8Trim(GB_STR("甲甲测试甲"), GB_STR("甲")), GB_STR("测试"), "GB_Utf8Trim custom utf8 chars");

        const std::string ideographicSpace = GB_MakeUtf8String(static_cast<char32_t>(0x3000));
        RequireEqualString(GB_Utf8Simplified(std::string(" \tHello  \nWorld  ")), "Hello World", "GB_Utf8Simplified ascii spaces");
        RequireEqualString(GB_Utf8Simplified(ideographicSpace + GB_STR("甲") + ideographicSpace + ideographicSpace + GB_STR("乙") + ideographicSpace), GB_STR("甲 乙"), "GB_Utf8Simplified unicode spaces");

        RequireEqualString(GB_Utf8Replace("abcABCabc", "abc", "X", false), "XXX", "GB_Utf8Replace ascii case insensitive");
        RequireEqualString(GB_Utf8Replace(GB_STR("甲乙甲"), GB_STR("甲"), GB_STR("丙"), true), GB_STR("丙乙丙"), "GB_Utf8Replace utf8 exact");
        RequireEqualString(GB_Utf8Replace("hello", "", "X", true), "hello", "GB_Utf8Replace empty oldValue");

        RequireEqualString(GB_Utf8Format("%s-%d", GB_STR("值").c_str(), 7), GB_STR("值-7"), "GB_Utf8Format normal");
        const char* nullFormat = nullptr;
        ExpectThrow([&]() { GB_Utf8Format(nullFormat); }, "GB_Utf8Format null format");
    }
}

int main(int argc, char* argv[])
{
    try
    {
        TestMakeUtf8String();
        TestEncodingConversion();
        TestValidationAndHeuristics();
        TestWideStringConversion();
        TestCodePointAccess();
        TestCaseTransformAndSplit();
        TestCompareAndSearch();
        TestTrimSimplifyReplaceAndFormat();
    }
    catch (const std::exception& exceptionObject)
    {
        std::cerr << "[FAILED] Unexpected exception\n" << exceptionObject.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "[FAILED] Unknown unexpected exception" << std::endl;
        return 1;
    }

    std::cout << "GB_Utf8String tests passed. Total checks: " << totalCaseCount << std::endl;
    return 0;
}
