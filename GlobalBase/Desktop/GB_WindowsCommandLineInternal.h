#ifndef GLOBALBASE_WINDOWS_COMMAND_LINE_INTERNAL_H_H
#define GLOBALBASE_WINDOWS_COMMAND_LINE_INTERNAL_H_H

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace GB_WindowsCommandLineInternal
{
    inline size_t CheckedAdd(const size_t leftValue, const size_t rightValue)
    {
        if (rightValue > std::numeric_limits<size_t>::max() - leftValue)
        {
            throw std::length_error("Windows command-line argument is too long.");
        }
        return leftValue + rightValue;
    }

    inline size_t CheckedDouble(const size_t value)
    {
        if (value > std::numeric_limits<size_t>::max() / 2)
        {
            throw std::length_error("Windows command-line argument is too long.");
        }
        return value * 2;
    }

    inline bool NeedsQuotes(const std::wstring& argument)
    {
        return argument.empty() || argument.find_first_of(L" \t\"") != std::wstring::npos;
    }

    inline size_t GetQuotedArgumentLength(const std::wstring& argument)
    {
        if (!NeedsQuotes(argument))
        {
            return argument.size();
        }

        size_t quotedLength = 2;
        size_t backslashCount = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                backslashCount++;
                continue;
            }

            if (character == L'"')
            {
                quotedLength = CheckedAdd(quotedLength, CheckedAdd(CheckedDouble(backslashCount), 2));
                backslashCount = 0;
                continue;
            }

            quotedLength = CheckedAdd(quotedLength, CheckedAdd(backslashCount, 1));
            backslashCount = 0;
        }

        return CheckedAdd(quotedLength, CheckedDouble(backslashCount));
    }

    inline void AppendQuotedArgument(std::wstring& commandLine, const std::wstring& argument)
    {
        if (!NeedsQuotes(argument))
        {
            commandLine.append(argument);
            return;
        }

        commandLine.push_back(L'"');
        size_t backslashCount = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                backslashCount++;
                continue;
            }

            if (character == L'"')
            {
                commandLine.append(CheckedAdd(CheckedDouble(backslashCount), 1), L'\\');
                commandLine.push_back(L'"');
                backslashCount = 0;
                continue;
            }

            commandLine.append(backslashCount, L'\\');
            backslashCount = 0;
            commandLine.push_back(character);
        }

        commandLine.append(CheckedDouble(backslashCount), L'\\');
        commandLine.push_back(L'"');
    }

    inline std::wstring QuoteArgument(const std::wstring& argument)
    {
        std::wstring quotedArgument;
        quotedArgument.reserve(GetQuotedArgumentLength(argument));
        AppendQuotedArgument(quotedArgument, argument);
        return quotedArgument;
    }

    inline std::wstring BuildParameters(const std::vector<std::wstring>& arguments)
    {
        size_t parameterLength = arguments.empty() ? 0 : arguments.size() - 1;
        for (const std::wstring& argument : arguments)
        {
            parameterLength = CheckedAdd(parameterLength, GetQuotedArgumentLength(argument));
        }

        std::wstring parameters;
        parameters.reserve(parameterLength);
        for (size_t argumentIndex = 0; argumentIndex < arguments.size(); argumentIndex++)
        {
            if (argumentIndex != 0)
            {
                parameters.push_back(L' ');
            }
            AppendQuotedArgument(parameters, arguments[argumentIndex]);
        }
        return parameters;
    }
}

#endif // GLOBALBASE_WINDOWS_COMMAND_LINE_INTERNAL_H_H
