#pragma once

#include <persistence/state_core.hpp>

#include <string>
#include <vector>

namespace Persistence
{
    struct Termios : public DefaultMissingMember
    {
        struct InputFlags : public DefaultMissingMember
        {
            std::optional<bool> IGNBRK_{std::nullopt};
            std::optional<bool> BRKINT_{std::nullopt};
            std::optional<bool> IGNPAR_{std::nullopt};
            std::optional<bool> PARMRK_{std::nullopt};
            std::optional<bool> INPCK_{std::nullopt};
            std::optional<bool> ISTRIP_{std::nullopt};
            std::optional<bool> INLCR_{std::nullopt};
            std::optional<bool> IGNCR_{std::nullopt};
            std::optional<bool> ICRNL_{std::nullopt};
            std::optional<bool> IUCLC_{std::nullopt};
            std::optional<bool> IXON_{std::nullopt};
            std::optional<bool> IXANY_{std::nullopt};
            std::optional<bool> IXOFF_{std::nullopt};
            std::optional<bool> IMAXBEL_{std::nullopt};
            std::optional<bool> IUTF8_{std::nullopt};

            unsigned int assemble() const;
            void useDefaultsFrom(InputFlags const& other);
            static InputFlags saneDefaults()
            {
                return InputFlags{
                    .IGNBRK_ = false,
                    .BRKINT_ = false,
                    .IGNPAR_ = false,
                    .PARMRK_ = false,
                    .INPCK_ = false,
                    .ISTRIP_ = false,
                    .INLCR_ = false,
                    .IGNCR_ = false,
                    .ICRNL_ = true,
                    .IUCLC_ = false,
                    .IXON_ = true,
                    .IXANY_ = false,
                    .IXOFF_ = false,
                    .IMAXBEL_ = false,
                    .IUTF8_ = true
                };
            }
        };
        InputFlags inputFlags{InputFlags{}};

        struct OutputFlags : public DefaultMissingMember
        {
            std::optional<bool> OPOST_{std::nullopt};
            std::optional<bool> OLCUC_{std::nullopt};
            std::optional<bool> ONLCR_{std::nullopt};
            std::optional<bool> OCRNL_{std::nullopt};
            std::optional<bool> ONOCR_{std::nullopt};
            std::optional<bool> ONLRET_{std::nullopt};
            std::optional<bool> OFILL_{std::nullopt};
            std::optional<bool> OFDEL_{std::nullopt};
            std::optional<std::string> NLDLY_{std::nullopt};
            std::optional<std::string> CRDLY_{std::nullopt};
            std::optional<std::string> TABDLY_{std::nullopt};
            std::optional<std::string> BSDLY_{std::nullopt};
            std::optional<std::string> VTDLY_{std::nullopt};
            std::optional<std::string> FFDLY_{std::nullopt};

            unsigned int assemble() const;
            void useDefaultsFrom(OutputFlags const& other);
            static OutputFlags saneDefaults()
            {
                return OutputFlags{
                    .OPOST_ = true,
                    .OLCUC_ = false,
                    .ONLCR_ = false,
                    .OCRNL_ = false,
                    .ONOCR_ = false,
                    .ONLRET_ = false,
                    .OFILL_ = false,
                    .OFDEL_ = false,
                    .NLDLY_ = "NL0",
                    .CRDLY_ = "CR0",
                    .TABDLY_ = "TAB0",
                    .BSDLY_ = "BS0",
                    .VTDLY_ = "VT0",
                    .FFDLY_ = "FF0"
                };
            }
        };
        OutputFlags outputFlags{OutputFlags{}};

        struct ControlFlags : public DefaultMissingMember
        {
            std::optional<unsigned int> CBAUD_{std::nullopt};
            std::optional<bool> CBAUDEX_{std::nullopt};
            std::optional<std::string> CSIZE_{std::nullopt};
            std::optional<bool> CSTOPB_{std::nullopt};
            std::optional<bool> CREAD_{std::nullopt};
            std::optional<bool> PARENB_{std::nullopt};
            std::optional<bool> PARODD_{std::nullopt};
            std::optional<bool> HUPCL_{std::nullopt};
            std::optional<bool> CLOCAL_{std::nullopt};
            std::optional<bool> LOBLK_{std::nullopt};
            std::optional<bool> CIBAUD_{std::nullopt};
            std::optional<bool> CMSPAR_{std::nullopt};
            std::optional<bool> CRTSCTS_{std::nullopt};

            unsigned int assemble() const;
            void useDefaultsFrom(ControlFlags const& other);
            static ControlFlags saneDefaults()
            {
                return ControlFlags{
                    .CBAUD_ = 0,
                    .CBAUDEX_ = false,
                    .CSIZE_ = "CS8",
                    .CSTOPB_ = false,
                    .CREAD_ = true,
                    .PARENB_ = false,
                    .PARODD_ = false,
                    .HUPCL_ = false,
                    .CLOCAL_ = false,
                    .LOBLK_ = false,
                    .CIBAUD_ = false,
                    .CMSPAR_ = false,
                    .CRTSCTS_ = false
                };
            }
        };
        ControlFlags controlFlags{ControlFlags{}};

        struct LocalFlags : public DefaultMissingMember
        {
            std::optional<bool> ISIG_{std::nullopt};
            std::optional<bool> ICANON_{std::nullopt};
            std::optional<bool> XCASE_{std::nullopt};
            std::optional<bool> ECHO_{std::nullopt};
            std::optional<bool> ECHOE_{std::nullopt};
            std::optional<bool> ECHOK_{std::nullopt};
            std::optional<bool> ECHONL_{std::nullopt};
            std::optional<bool> ECHOCTL_{std::nullopt};
            std::optional<bool> ECHOPRT_{std::nullopt};
            std::optional<bool> ECHOKE_{std::nullopt};
            std::optional<bool> FLUSHO_{std::nullopt};
            std::optional<bool> NOFLSH_{std::nullopt};
            std::optional<bool> TOSTOP_{std::nullopt};
            std::optional<bool> PENDIN_{std::nullopt};
            std::optional<bool> IEXTEN_{std::nullopt};

            unsigned int assemble() const;
            void useDefaultsFrom(LocalFlags const& other);
            static LocalFlags saneDefaults()
            {
                return LocalFlags{
                    .ISIG_ = true,
                    .ICANON_ = true,
                    .XCASE_ = false,
                    .ECHO_ = true,
                    .ECHOE_ = true,
                    .ECHOK_ = true,
                    .ECHONL_ = false,
                    .ECHOCTL_ = true,
                    .ECHOPRT_ = false,
                    .ECHOKE_ = true,
                    .FLUSHO_ = false,
                    .NOFLSH_ = false,
                    .TOSTOP_ = false,
                    .PENDIN_ = false,
                    .IEXTEN_ = true
                };
            }
        };
        LocalFlags localFlags{LocalFlags{}};

        struct CC : public DefaultMissingMember
        {
            unsigned char VDISCARD_{3};
            unsigned char VDSUSP_{28};
            unsigned char VEOF_{127};
            unsigned char VEOL_{21};
            unsigned char VEOL2_{4};
            unsigned char VERASE_{0};
            unsigned char VINTR_{1};
            unsigned char VKILL_{0};
            unsigned char VLNEXT_{17};
            unsigned char VMIN_{19};
            unsigned char VQUIT_{26};
            unsigned char VREPRINT_{0};
            unsigned char VSTART_{18};
            unsigned char VSTATUS_{15};
            unsigned char VSTOP_{23};
            unsigned char VSUSP_{22};
            unsigned char VSWTCH_{0};
            unsigned char VTIME_{0};
            unsigned char VWERASE_{0};

            std::vector<unsigned char> assemble() const;
        };
        std::optional<CC> cc{std::nullopt};

        std::optional<unsigned int> iSpeed{0};
        std::optional<unsigned int> oSpeed{0};

        void useDefaultsFrom(Termios const& other);
        static Termios saneDefaults();
    };
    void to_json(nlohmann::json& j, Termios::InputFlags const& inputFlags);
    void from_json(nlohmann::json const& j, Termios::InputFlags& inputFlags);
    void to_json(nlohmann::json& j, Termios::OutputFlags const& outputFlags);
    void from_json(nlohmann::json const& j, Termios::OutputFlags& outputFlags);
    void to_json(nlohmann::json& j, Termios::ControlFlags const& controlFlags);
    void from_json(nlohmann::json const& j, Termios::ControlFlags& controlFlags);
    void to_json(nlohmann::json& j, Termios::LocalFlags const& localFlags);
    void from_json(nlohmann::json const& j, Termios::LocalFlags& localFlags);
    void to_json(nlohmann::json& j, Termios::CC const& cc);
    void from_json(nlohmann::json const& j, Termios::CC& cc);
    void to_json(nlohmann::json& j, Termios const& termios);
    void from_json(nlohmann::json const& j, Termios& termios);
}