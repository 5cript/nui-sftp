#pragma once

#include <frontend/settings/group_keys.hpp>
#include <frontend/settings/atomic_setting/bool_setting.hpp>
#include <frontend/settings/atomic_setting/text_setting.hpp>
#include <frontend/settings/atomic_setting/number_setting.hpp>

#include <persistence/state/termios.hpp>

struct TermiosSettings : public GroupKeys
{
    struct InputFlags
    {
        BoolSetting<true> IGNBRK;
        BoolSetting<true> BRKINT;
        BoolSetting<true> IGNPAR;
        BoolSetting<true> PARMRK;
        BoolSetting<true> INPCK;
        BoolSetting<true> ISTRIP;
        BoolSetting<true> INLCR;
        BoolSetting<true> IGNCR;
        BoolSetting<true> ICRNL;
        BoolSetting<true> IUCLC;
        BoolSetting<true> IXON;
        BoolSetting<true> IXANY;
        BoolSetting<true> IXOFF;
        BoolSetting<true> IMAXBEL;
        BoolSetting<true> IUTF8;
    } inputFlags;

    struct OutputFlags
    {

        BoolSetting<true> OPOST;
        BoolSetting<true> OLCUC;
        BoolSetting<true> ONLCR;
        BoolSetting<true> OCRNL;
        BoolSetting<true> ONOCR;
        BoolSetting<true> ONLRET;
        BoolSetting<true> OFILL;
        BoolSetting<true> OFDEL;
        TextSetting<true> NLDLY;
        TextSetting<true> CRDLY;
        TextSetting<true> TABDLY;
        TextSetting<true> BSDLY;
        TextSetting<true> VTDLY;
        TextSetting<true> FFDLY;
    } outputFlags;

    struct ControlFlags
    {
        NumberSetting<unsigned int, true> CBAUD;
        BoolSetting<true> CBAUDEX;
        TextSetting<true> CSIZE;
        BoolSetting<true> CSTOPB;
        BoolSetting<true> CREAD;
        BoolSetting<true> PARENB;
        BoolSetting<true> PARODD;
        BoolSetting<true> HUPCL;
        BoolSetting<true> CLOCAL;
        BoolSetting<true> LOBLK;
        BoolSetting<true> CIBAUD;
        BoolSetting<true> CMSPAR;
        BoolSetting<true> CRTSCTS;
    } controlFlags;

    struct LocalFlags
    {
        BoolSetting<true> ISIG;
        BoolSetting<true> ICANON;
        BoolSetting<true> XCASE;
        BoolSetting<true> ECHO;
        BoolSetting<true> ECHOE;
        BoolSetting<true> ECHOK;
        BoolSetting<true> ECHONL;
        BoolSetting<true> ECHOCTL;
        BoolSetting<true> ECHOPRT;
        BoolSetting<true> ECHOKE;
        BoolSetting<true> FLUSHO;
        BoolSetting<true> NOFLSH;
        BoolSetting<true> TOSTOP;
        BoolSetting<true> PENDIN;
        BoolSetting<true> IEXTEN;
    } localFlags;

    struct CC
    {
        NumberSetting<unsigned char> VDISCARD;
        NumberSetting<unsigned char> VDSUSP;
        NumberSetting<unsigned char> VEOF;
        NumberSetting<unsigned char> VEOL;
        NumberSetting<unsigned char> VEOL2;
        NumberSetting<unsigned char> VERASE;
        NumberSetting<unsigned char> VINTR;
        NumberSetting<unsigned char> VKILL;
        NumberSetting<unsigned char> VLNEXT;
        NumberSetting<unsigned char> VMIN;
        NumberSetting<unsigned char> VQUIT;
        NumberSetting<unsigned char> VREPRINT;
        NumberSetting<unsigned char> VSTART;
        NumberSetting<unsigned char> VSTATUS;
        NumberSetting<unsigned char> VSTOP;
        NumberSetting<unsigned char> VSUSP;
        NumberSetting<unsigned char> VSWTCH;
        NumberSetting<unsigned char> VTIME;
        NumberSetting<unsigned char> VWERASE;
    } cc;

    NumberSetting<unsigned int, true> iSpeed;
    NumberSetting<unsigned int, true> oSpeed;

    Nui::Observed<bool> ccEngaged{false};

    TermiosSettings(std::function<void()> const& onChange);

    void applyToState(Persistence::Termios& state) const;
    void loadFromState(Persistence::Termios const& state);
    void assumeDefaultsFrom(Persistence::Termios const& state);
};