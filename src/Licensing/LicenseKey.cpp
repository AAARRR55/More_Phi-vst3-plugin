#include "LicenseKey.h"
#include "LicenseTypes.h"

namespace more_phi::licensing {
namespace {

constexpr const char* CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int EXPECTED_MPH1_GROUP_COUNT = 7; // MPH1 + five body groups + checksum group
constexpr int EXPECTED_MPHI_GROUP_COUNT = 5; // MPHI + four legacy purchasing groups

bool isSeparator(juce::juce_wchar c) noexcept
{
    return c == '-' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

juce::String LicenseKey::normalize(juce::String input)
{
    input = input.trim().toUpperCase();

    juce::String compact;
    for (int i = 0; i < input.length(); ++i)
    {
        const auto c = input[i];
        if (!isSeparator(c))
            compact << c;
    }

    compact = compact.replaceCharacter('O', '0')
                     .replaceCharacter('I', '1')
                     .replaceCharacter('L', '1');

    if (compact.startsWith("MPHI") && compact.length() == 24) // 4 prefix + four 5-char purchasing groups
    {
        juce::String formatted;
        formatted << compact.substring(0, 4);
        int index = 4;
        for (int group = 0; group < 4; ++group)
        {
            formatted << "-" << compact.substring(index, index + 5);
            index += 5;
        }
        return formatted;
    }

    if (compact.length() != 25) // 4 prefix + 20 body + 1 checksum, compact form
        return input;

    juce::String formatted;
    formatted << compact.substring(0, 4);
    int index = 4;
    for (int group = 0; group < 5; ++group)
    {
        formatted << "-" << compact.substring(index, index + 4);
        index += 4;
    }
    formatted << "-" << compact.substring(index, index + 1);
    return formatted;
}

int LicenseKey::alphabetIndex(juce::juce_wchar c) noexcept
{
    for (int i = 0; CROCKFORD[i] != '\0'; ++i)
        if (static_cast<juce::juce_wchar>(CROCKFORD[i]) == c)
            return i;
    return -1;
}

uint8_t LicenseKey::checksumIndex(juce::StringRef normalizedWithoutChecksum)
{
    uint32_t crc = 0xFFFFFFFFu;
    const auto text = juce::String(normalizedWithoutChecksum).toUpperCase();

    for (int i = 0; i < text.length(); ++i)
    {
        const auto c = text[i];
        if (isSeparator(c))
            continue;

        crc ^= static_cast<uint8_t>(c & 0xff);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ (0xEDB88320u & static_cast<uint32_t>(-(static_cast<int>(crc & 1u))));
    }

    return static_cast<uint8_t>((crc ^ 0xFFFFFFFFu) % 32u);
}

juce::juce_wchar LicenseKey::checksumChar(uint8_t index) noexcept
{
    return static_cast<juce::juce_wchar>(CROCKFORD[index % 32u]);
}

ParsedLicenseKey LicenseKey::parse(juce::String input)
{
    ParsedLicenseKey result;
    result.normalized = normalize(std::move(input));

    auto groups = juce::StringArray::fromTokens(result.normalized, "-", "");
    groups.removeEmptyStrings();

    if (groups.size() == EXPECTED_MPHI_GROUP_COUNT && groups[0] == "MPHI")
    {
        for (int group = 1; group <= 4; ++group)
        {
            if (groups[group].length() != 5)
            {
                result.error = "Legacy license key groups must contain five characters.";
                return result;
            }

            for (int i = 0; i < groups[group].length(); ++i)
            {
                const auto c = groups[group][i];
                if (!juce::CharacterFunctions::isLetterOrDigit(c))
                {
                    result.error = "Legacy license key contains an unsupported character.";
                    return result;
                }
            }
        }

        result.valid = true;
        return result;
    }

    if (groups.size() != EXPECTED_MPH1_GROUP_COUNT)
    {
        result.error = "License key should look like MPH1-XXXX-XXXX-XXXX-XXXX-XXXX-C or MPHI-XXXXX-XXXXX-XXXXX-XXXXX.";
        return result;
    }

    if (groups[0] != LICENSE_KEY_PREFIX)
    {
        result.error = "License key prefix is not valid for More-Phi.";
        return result;
    }

    for (int group = 1; group <= 5; ++group)
    {
        if (groups[group].length() != 4)
        {
            result.error = "License key groups must contain four characters.";
            return result;
        }

        for (int i = 0; i < groups[group].length(); ++i)
        {
            const auto c = groups[group][i];
            if (alphabetIndex(c) < 0)
            {
                result.error = "License key contains an unsupported character.";
                return result;
            }
        }
    }

    if (groups[6].length() != 1 || alphabetIndex(groups[6][0]) < 0)
    {
        result.error = "License key checksum is invalid.";
        return result;
    }

    const auto body = result.normalized.upToLastOccurrenceOf("-", false, false);
    const auto expected = checksumChar(checksumIndex(body));
    if (groups[6][0] != expected)
    {
        result.error = "License key checksum does not match.";
        return result;
    }

    result.valid = true;
    return result;
}

} // namespace more_phi::licensing