#pragma once

#include <persistence/state/termios.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#ifndef _WIN32
#    include <termios.h>
#endif

namespace Test
{
    namespace
    {
        /**
         * @brief Decodes RFC 4254 encoded terminal modes into opcode/value pairs.
         * @param modes The encoded modes, terminated by TTY_OP_END.
         */
        std::map<std::uint8_t, std::uint32_t> decodePtyModes(std::vector<unsigned char> const& modes)
        {
            std::map<std::uint8_t, std::uint32_t> decoded{};
            for (std::size_t position = 0; position + 5 <= modes.size() && modes[position] != 0; position += 5)
            {
                decoded[modes[position]] = (static_cast<std::uint32_t>(modes[position + 1]) << 24u) |
                    (static_cast<std::uint32_t>(modes[position + 2]) << 16u) |
                    (static_cast<std::uint32_t>(modes[position + 3]) << 8u) |
                    static_cast<std::uint32_t>(modes[position + 4]);
            }
            return decoded;
        }
    }

    TEST(TermiosTests, DefaultControlCharactersAreSentUnderTheirSshOpcodes)
    {
        const auto modes = decodePtyModes(Persistence::Termios::saneDefaults().toSshPtyMode());

        constexpr std::uint8_t vintr = 1u;
        constexpr std::uint8_t vquit = 2u;
        constexpr std::uint8_t verase = 3u;
        constexpr std::uint8_t vkill = 4u;
        constexpr std::uint8_t veof = 5u;
        constexpr std::uint8_t vsusp = 10u;
        EXPECT_EQ(modes.at(vintr), 3u);
        EXPECT_EQ(modes.at(vquit), 28u);
        EXPECT_EQ(modes.at(verase), 127u);
        EXPECT_EQ(modes.at(vkill), 21u);
        EXPECT_EQ(modes.at(veof), 4u);
        EXPECT_EQ(modes.at(vsusp), 26u);
    }

#ifndef _WIN32
    TEST(TermiosTests, AssembledControlCharactersAreIndexedByTheTermiosConstants)
    {
        const auto characters = Persistence::Termios::CC{}.assemble();

        ASSERT_EQ(characters.size(), static_cast<std::size_t>(NCCS));
        EXPECT_EQ(characters[VINTR], 3u);
        EXPECT_EQ(characters[VERASE], 127u);
        EXPECT_EQ(characters[VEOF], 4u);
        EXPECT_EQ(characters[VSUSP], 26u);
        EXPECT_EQ(characters[VMIN], 1u);
    }
#endif
}
