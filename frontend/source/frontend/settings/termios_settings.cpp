#include <frontend/settings/termios_settings.hpp>
#include <frontend/settings/subgroup.hpp>
#include <persistence/state/termios.hpp>

#include <frontend/settings/nullopt_reset.hpp>
#include <frontend/settings/optional_converters.hpp>
#include <frontend/settings/setting_helper.hpp>

#include <nui/frontend/elements.hpp>
#include <nui/frontend/attributes.hpp>

namespace
{
    template <typename NumberSetting>
    auto makeCharacterValueConstraints()
    {
        return typename NumberSetting::ConstructionArgs{
            .minValue = 0,
            .maxValue = 255,
        };
    }
}

TermiosSettings::TermiosSettings(SettingFactory const& factory, std::function<void()> const& onChange)
    : factory_{factory.forGroup(groupKey)}
    , inputFlagsFactory_{factory_.within({"termios", "inputFlagsSubgroupTitle"})}
    , outputFlagsFactory_{factory_.within({"termios", "outputFlagsSubgroupTitle"})}
    , controlFlagsFactory_{factory_.within({"termios", "controlFlagsSubgroupTitle"})}
    , localFlagsFactory_{factory_.within({"termios", "localFlagsSubgroupTitle"})}
    , ccFactory_{factory_.within({"termios", "ccSettingsSubgroupTitle"})}
    , inputFlags{
            .IGNBRK{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IGNBRK"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IGNBRK.value(Persistence::Termios::InputFlags::saneDefaults().IGNBRK_);
                    onChange();
                }
            },
            .BRKINT{
                inputFlagsFactory_.identity({"termios", "inputFlags", "BRKINT"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.BRKINT.value(Persistence::Termios::InputFlags::saneDefaults().BRKINT_);
                    onChange();
                }
            },
            .IGNPAR{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IGNPAR"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IGNPAR.value(Persistence::Termios::InputFlags::saneDefaults().IGNPAR_);
                    onChange();
                }
            },
            .PARMRK{
                inputFlagsFactory_.identity({"termios", "inputFlags", "PARMRK"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.PARMRK.value(Persistence::Termios::InputFlags::saneDefaults().PARMRK_);
                    onChange();
                }
            },
            .INPCK{
                inputFlagsFactory_.identity({"termios", "inputFlags", "INPCK"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.INPCK.value(Persistence::Termios::InputFlags::saneDefaults().INPCK_);
                    onChange();
                }
            },
            .ISTRIP{
                inputFlagsFactory_.identity({"termios", "inputFlags", "ISTRIP"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.ISTRIP.value(Persistence::Termios::InputFlags::saneDefaults().ISTRIP_);
                    onChange();
                }
            },
            .INLCR{
                inputFlagsFactory_.identity({"termios", "inputFlags", "INLCR"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.INLCR.value(Persistence::Termios::InputFlags::saneDefaults().INLCR_);
                    onChange();
                }
            },
            .IGNCR{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IGNCR"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IGNCR.value(Persistence::Termios::InputFlags::saneDefaults().IGNCR_);
                    onChange();
                }
            },
            .ICRNL{
                inputFlagsFactory_.identity({"termios", "inputFlags", "ICRNL"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.ICRNL.value(Persistence::Termios::InputFlags::saneDefaults().ICRNL_);
                    onChange();
                }
            },
            .IUCLC{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IUCLC"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IUCLC.value(Persistence::Termios::InputFlags::saneDefaults().IUCLC_);
                    onChange();
                }
            },
            .IXON{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IXON"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IXON.value(Persistence::Termios::InputFlags::saneDefaults().IXON_);
                    onChange();
                }
            },
            .IXANY{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IXANY"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IXANY.value(Persistence::Termios::InputFlags::saneDefaults().IXANY_);
                    onChange();
                }
            },
            .IXOFF{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IXOFF"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IXOFF.value(Persistence::Termios::InputFlags::saneDefaults().IXOFF_);
                    onChange();
                }
            },
            .IMAXBEL{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IMAXBEL"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IMAXBEL.value(Persistence::Termios::InputFlags::saneDefaults().IMAXBEL_);
                    onChange();
                }
            },
            .IUTF8{
                inputFlagsFactory_.identity({"termios", "inputFlags", "IUTF8"}),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IUTF8.value(Persistence::Termios::InputFlags::saneDefaults().IUTF8_);
                    onChange();
                }
            },
        }
    , outputFlags{
            .OPOST{
                outputFlagsFactory_.identity({"termios", "outputFlags", "OPOST"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OPOST.value(Persistence::Termios::OutputFlags::saneDefaults().OPOST_);
                    onChange();
                }
            },
            .OLCUC{
                outputFlagsFactory_.identity({"termios", "outputFlags", "OLCUC"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OLCUC.value(Persistence::Termios::OutputFlags::saneDefaults().OLCUC_);
                    onChange();
                }
            },
            .ONLCR{
                outputFlagsFactory_.identity({"termios", "outputFlags", "ONLCR"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.ONLCR.value(Persistence::Termios::OutputFlags::saneDefaults().ONLCR_);
                    onChange();
                }
            },
            .OCRNL{
                outputFlagsFactory_.identity({"termios", "outputFlags", "OCRNL"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OCRNL.value(Persistence::Termios::OutputFlags::saneDefaults().OCRNL_);
                    onChange();
                }
            },
            .ONOCR{
                outputFlagsFactory_.identity({"termios", "outputFlags", "ONOCR"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.ONOCR.value(Persistence::Termios::OutputFlags::saneDefaults().ONOCR_);
                    onChange();
                }
            },
            .ONLRET{
                outputFlagsFactory_.identity({"termios", "outputFlags", "ONLRET"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.ONLRET.value(Persistence::Termios::OutputFlags::saneDefaults().ONLRET_);
                    onChange();
                }
            },
            .OFILL{
                outputFlagsFactory_.identity({"termios", "outputFlags", "OFILL"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OFILL.value(Persistence::Termios::OutputFlags::saneDefaults().OFILL_);
                    onChange();
                }
            },
            .OFDEL{
                outputFlagsFactory_.identity({"termios", "outputFlags", "OFDEL"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OFDEL.value(Persistence::Termios::OutputFlags::saneDefaults().OFDEL_);
                    onChange();
                }
            },
            .NLDLY{
                outputFlagsFactory_.identity({"termios", "outputFlags", "NLDLY"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.NLDLY.value(Persistence::Termios::OutputFlags::saneDefaults().NLDLY_);
                    onChange();
                }
            },
            .CRDLY{
                outputFlagsFactory_.identity({"termios", "outputFlags", "CRDLY"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.CRDLY.value(Persistence::Termios::OutputFlags::saneDefaults().CRDLY_);
                    onChange();
                }
            },
            .TABDLY{
                outputFlagsFactory_.identity({"termios", "outputFlags", "TABDLY"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.TABDLY.value(Persistence::Termios::OutputFlags::saneDefaults().TABDLY_);
                    onChange();
                }
            },
            .BSDLY{
                outputFlagsFactory_.identity({"termios", "outputFlags", "BSDLY"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.BSDLY.value(Persistence::Termios::OutputFlags::saneDefaults().BSDLY_);
                    onChange();
                }
            },
            .VTDLY{
                outputFlagsFactory_.identity({"termios", "outputFlags", "VTDLY"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.VTDLY.value(Persistence::Termios::OutputFlags::saneDefaults().VTDLY_);
                    onChange();
                }
            },
            .FFDLY{
                outputFlagsFactory_.identity({"termios", "outputFlags", "FFDLY"}),
                onChange,
                [this, onChange]()
                {
                    outputFlags.FFDLY.value(Persistence::Termios::OutputFlags::saneDefaults().FFDLY_);
                    onChange();
                }
            },
        },
        controlFlags{
        .CBAUD{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CBAUD"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CBAUD.value(Persistence::Termios::ControlFlags::saneDefaults().CBAUD_);
                onChange();
            },
            {
                .minValue = 0,
            }
        },
        .CBAUDEX{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CBAUDEX"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CBAUDEX.value(
                    Persistence::Termios::ControlFlags::saneDefaults().CBAUDEX_
                );
                onChange();
            }
        },
        .CSIZE{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CSIZE"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CSIZE.value(Persistence::Termios::ControlFlags::saneDefaults().CSIZE_);
                onChange();
            }
        },
        .CSTOPB{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CSTOPB"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CSTOPB.value(Persistence::Termios::ControlFlags::saneDefaults().CSTOPB_);
                onChange();
            }
        },
        .CREAD{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CREAD"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CREAD.value(Persistence::Termios::ControlFlags::saneDefaults().CREAD_);
                onChange();
            }
        },
        .PARENB{
            controlFlagsFactory_.identity({"termios", "controlFlags", "PARENB"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.PARENB.value(Persistence::Termios::ControlFlags::saneDefaults().PARENB_);
                onChange();
            }
        },
        .PARODD{
            controlFlagsFactory_.identity({"termios", "controlFlags", "PARODD"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.PARODD.value(Persistence::Termios::ControlFlags::saneDefaults().PARODD_);
                onChange();
            }
        },
        .HUPCL{
            controlFlagsFactory_.identity({"termios", "controlFlags", "HUPCL"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.HUPCL.value(Persistence::Termios::ControlFlags::saneDefaults().HUPCL_);
                onChange();
            }
        },
        .CLOCAL{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CLOCAL"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CLOCAL.value(Persistence::Termios::ControlFlags::saneDefaults().CLOCAL_);
                onChange();
            }
        },
        .LOBLK{
            controlFlagsFactory_.identity({"termios", "controlFlags", "LOBLK"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.LOBLK.value(Persistence::Termios::ControlFlags::saneDefaults().LOBLK_);
                onChange();
            }
        },
        .CIBAUD{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CIBAUD"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CIBAUD.value(Persistence::Termios::ControlFlags::saneDefaults().CIBAUD_);
                onChange();
            }
        },
        .CMSPAR{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CMSPAR"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CMSPAR.value(Persistence::Termios::ControlFlags::saneDefaults().CMSPAR_);
                onChange();
            }
        },
        .CRTSCTS{
            controlFlagsFactory_.identity({"termios", "controlFlags", "CRTSCTS"}),
            onChange,
            [this, onChange]()
            {
                controlFlags.CRTSCTS.value(Persistence::Termios::ControlFlags::saneDefaults().CRTSCTS_);
                onChange();
            }
        },
        },
        localFlags{
            .ISIG{
                localFlagsFactory_.identity({"termios", "localFlags", "ISIG"}),
                onChange,
                [this, onChange]()
                {
                    localFlags.ISIG.value(Persistence::Termios::LocalFlags::saneDefaults().ISIG_);
                    onChange();
                }
            },
            .ICANON{
                localFlagsFactory_.identity({"termios", "localFlags", "ICANON"}),
                onChange,
                [this, onChange]()
                {
                    localFlags.ICANON.value(Persistence::Termios::LocalFlags::saneDefaults().ICANON_);
                    onChange();
                }
            },
            .XCASE{
                localFlagsFactory_.identity({"termios", "localFlags", "XCASE"}),
                onChange,
                [this, onChange]()
                {
                    localFlags.XCASE.value(Persistence::Termios::LocalFlags::saneDefaults().XCASE_);
                    onChange();
                }
            },
            .ECHO{
                localFlagsFactory_.identity({"termios", "localFlags", "ECHO"}),
                onChange,
                [this, onChange]()
                {
                    localFlags.ECHO.value(Persistence::Termios::LocalFlags::saneDefaults().ECHO_);
                    onChange();
                }
            },
        .ECHOE{
            localFlagsFactory_.identity({"termios", "localFlags", "ECHOE"}),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOE.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOE_);
                onChange();
            }
        },
        .ECHOK{
            localFlagsFactory_.identity({"termios", "localFlags", "ECHOK"}),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOK.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOK_);
                onChange();
            }
        },
        .ECHONL{
            localFlagsFactory_.identity({"termios", "localFlags", "ECHONL"}),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHONL.value(Persistence::Termios::LocalFlags::saneDefaults().ECHONL_);
                onChange();
            }
        },
        .ECHOCTL{
            localFlagsFactory_.identity({"termios", "localFlags", "ECHOCTL"}),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOCTL.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOCTL_);
                onChange();
            }
        },
        .ECHOPRT{
            localFlagsFactory_.identity({"termios", "localFlags", "ECHOPRT"}),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOPRT.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOPRT_);
                onChange();
            }
        },
        .ECHOKE{
            localFlagsFactory_.identity({"termios", "localFlags", "ECHOKE"}),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOKE.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOKE_);
                onChange();
            }
        },
        .FLUSHO{
            localFlagsFactory_.identity({"termios", "localFlags", "FLUSHO"}),
            onChange,
            [this, onChange]()
            {
                localFlags.FLUSHO.value(Persistence::Termios::LocalFlags::saneDefaults().FLUSHO_);
                onChange();
            }
        },
        .NOFLSH{
            localFlagsFactory_.identity({"termios", "localFlags", "NOFLSH"}),
            onChange,
            [this, onChange]()
            {
                localFlags.NOFLSH.value(Persistence::Termios::LocalFlags::saneDefaults().NOFLSH_);
                onChange();
            }
        },
        .TOSTOP{
            localFlagsFactory_.identity({"termios", "localFlags", "TOSTOP"}),
            onChange,
            [this, onChange]()
            {
                localFlags.TOSTOP.value(Persistence::Termios::LocalFlags::saneDefaults().TOSTOP_);
                onChange();
            }
        },
        .PENDIN{
            localFlagsFactory_.identity({"termios", "localFlags", "PENDIN"}),
            onChange,
            [this, onChange]()
            {
                localFlags.PENDIN.value(Persistence::Termios::LocalFlags::saneDefaults().PENDIN_);
                onChange();
            }
        },
        .IEXTEN{
            localFlagsFactory_.identity({"termios", "localFlags", "IEXTEN"}),
            onChange,
            [this, onChange]()
            {
                localFlags.IEXTEN.value(Persistence::Termios::LocalFlags::saneDefaults().IEXTEN_);
                onChange();
            }
        },
    },
    cc{
        .VDISCARD{
            ccFactory_.identity({"termios", "cc", "VDISCARD"}),
            onChange,
            [this, onChange]()
            {
                cc.VDISCARD.value(Persistence::Termios::CC{}.VDISCARD_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VDISCARD)>(),
            &ccEngaged
        },
        .VDSUSP{
            ccFactory_.identity({"termios", "cc", "VDSUSP"}),
            onChange,
            [this, onChange]()
            {
                cc.VDSUSP.value(Persistence::Termios::CC{}.VDSUSP_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VDSUSP)>(),
            &ccEngaged
        },
        .VEOF{
            ccFactory_.identity({"termios", "cc", "VEOF"}),
            onChange,
            [this, onChange]()
            {
                cc.VEOF.value(Persistence::Termios::CC{}.VEOF_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VEOF)>(),
            &ccEngaged
        },
        .VEOL{
            ccFactory_.identity({"termios", "cc", "VEOL"}),
            onChange,
            [this, onChange]()
            {
                cc.VEOL.value(Persistence::Termios::CC{}.VEOL_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VEOL)>(),
            &ccEngaged
        },
        .VEOL2{
            ccFactory_.identity({"termios", "cc", "VEOL2"}),
            onChange,
            [this, onChange]()
            {
                cc.VEOL2.value(Persistence::Termios::CC{}.VEOL2_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VEOL2)>(),
            &ccEngaged
        },
        .VERASE{
            ccFactory_.identity({"termios", "cc", "VERASE"}),
            onChange,
            [this, onChange]()
            {
                cc.VERASE.value(Persistence::Termios::CC{}.VERASE_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VERASE)>(),
            &ccEngaged
        },
        .VINTR{
            ccFactory_.identity({"termios", "cc", "VINTR"}),
            onChange,
            [this, onChange]()
            {
                cc.VINTR.value(Persistence::Termios::CC{}.VINTR_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VINTR)>(),
            &ccEngaged
        },
        .VKILL{
            ccFactory_.identity({"termios", "cc", "VKILL"}),
            onChange,
            [this, onChange]()
            {
                cc.VKILL.value(Persistence::Termios::CC{}.VKILL_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VKILL)>(),
            &ccEngaged
        },
        .VLNEXT{
            ccFactory_.identity({"termios", "cc", "VLNEXT"}),
            onChange,
            [this, onChange]()
            {
                cc.VLNEXT.value(Persistence::Termios::CC{}.VLNEXT_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VLNEXT)>(),
            &ccEngaged
        },
        .VMIN{
            ccFactory_.identity({"termios", "cc", "VMIN"}),
            onChange,
            [this, onChange]()
            {
                cc.VMIN.value(Persistence::Termios::CC{}.VMIN_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VMIN)>(),
            &ccEngaged
        },
        .VQUIT{
            ccFactory_.identity({"termios", "cc", "VQUIT"}),
            onChange,
            [this, onChange]()
            {
                cc.VQUIT.value(Persistence::Termios::CC{}.VQUIT_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VQUIT)>(),
            &ccEngaged
        },
        .VREPRINT{
            ccFactory_.identity({"termios", "cc", "VREPRINT"}),
            onChange,
            [this, onChange]()
            {
                cc.VREPRINT.value(Persistence::Termios::CC{}.VREPRINT_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VREPRINT)>(),
            &ccEngaged
        },
        .VSTART{
            ccFactory_.identity({"termios", "cc", "VSTART"}),
            onChange,
            [this, onChange]()
            {
                cc.VSTART.value(Persistence::Termios::CC{}.VSTART_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VSTART)>(),
            &ccEngaged
        },
        .VSTATUS{
            ccFactory_.identity({"termios", "cc", "VSTATUS"}),
            onChange,
            [this, onChange]()
            {
                cc.VSTATUS.value(Persistence::Termios::CC{}.VSTATUS_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VSTATUS)>(),
            &ccEngaged
        },
        .VSTOP{
            ccFactory_.identity({"termios", "cc", "VSTOP"}),
            onChange,
            [this, onChange]()
            {
                cc.VSTOP.value(Persistence::Termios::CC{}.VSTOP_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VSTOP)>(),
            &ccEngaged
        },
        .VSUSP{
            ccFactory_.identity({"termios", "cc", "VSUSP"}),
            onChange,
            [this, onChange]()
            {
                cc.VSUSP.value(Persistence::Termios::CC{}.VSUSP_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VSUSP)>(),
            &ccEngaged
        },
        .VSWTCH{
            ccFactory_.identity({"termios", "cc", "VSWTCH"}),
            onChange,
            [this, onChange]()
            {
                cc.VSWTCH.value(Persistence::Termios::CC{}.VSWTCH_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VSWTCH)>(),
            &ccEngaged
        },
        .VTIME{
            ccFactory_.identity({"termios", "cc", "VTIME"}),
            onChange,
            [this, onChange]()
            {
                cc.VTIME.value(Persistence::Termios::CC{}.VTIME_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VTIME)>(),
            &ccEngaged
        },
        .VWERASE{
            ccFactory_.identity({"termios", "cc", "VWERASE"}),
            onChange,
            [this, onChange]()
            {
                cc.VWERASE.value(Persistence::Termios::CC{}.VWERASE_);
                onChange();
            },
            makeCharacterValueConstraints<decltype(cc.VWERASE)>(),
            &ccEngaged
        },
    },
    iSpeed{
        factory_.identity({"termios", "iSpeed"}, {.labelKey = SettingKeyPath{"termios", "iSpeedHelpText"}}),
        onChange,
        nulloptReset(iSpeed, onChange),
        {
            .minValue = 0,
        }
    },
    oSpeed{
        factory_.identity({"termios", "oSpeed"}, {.labelKey = SettingKeyPath{"termios", "oSpeedHelpText"}}),
        onChange,
        nulloptReset(oSpeed, onChange),
        {
            .minValue = 0,
        }
    },
    onChange_{onChange}
{}

void TermiosSettings::applyToState(Persistence::Termios& state) const
{
    state.inputFlags = Persistence::Termios::InputFlags{
        .IGNBRK_ = inputFlags.IGNBRK.value(),
        .BRKINT_ = inputFlags.BRKINT.value(),
        .IGNPAR_ = inputFlags.IGNPAR.value(),
        .PARMRK_ = inputFlags.PARMRK.value(),
        .INPCK_ = inputFlags.INPCK.value(),
        .ISTRIP_ = inputFlags.ISTRIP.value(),
        .INLCR_ = inputFlags.INLCR.value(),
        .IGNCR_ = inputFlags.IGNCR.value(),
        .ICRNL_ = inputFlags.ICRNL.value(),
        .IUCLC_ = inputFlags.IUCLC.value(),
        .IXON_ = inputFlags.IXON.value(),
        .IXANY_ = inputFlags.IXANY.value(),
        .IXOFF_ = inputFlags.IXOFF.value(),
        .IMAXBEL_ = inputFlags.IMAXBEL.value(),
        .IUTF8_ = inputFlags.IUTF8.value(),
    };

    state.outputFlags = Persistence::Termios::OutputFlags{
        .OPOST_ = outputFlags.OPOST.value(),
        .OLCUC_ = outputFlags.OLCUC.value(),
        .ONLCR_ = outputFlags.ONLCR.value(),
        .OCRNL_ = outputFlags.OCRNL.value(),
        .ONOCR_ = outputFlags.ONOCR.value(),
        .ONLRET_ = outputFlags.ONLRET.value(),
        .OFILL_ = outputFlags.OFILL.value(),
        .OFDEL_ = outputFlags.OFDEL.value(),
        .NLDLY_ = outputFlags.NLDLY.value(),
        .CRDLY_ = outputFlags.CRDLY.value(),
        .TABDLY_ = outputFlags.TABDLY.value(),
        .BSDLY_ = outputFlags.BSDLY.value(),
        .VTDLY_ = outputFlags.VTDLY.value(),
        .FFDLY_ = outputFlags.FFDLY.value(),
    };

    state.controlFlags = Persistence::Termios::ControlFlags{
        .CBAUD_ = controlFlags.CBAUD.value(),
        .CBAUDEX_ = controlFlags.CBAUDEX.value(),
        .CSIZE_ = controlFlags.CSIZE.value(),
        .CSTOPB_ = controlFlags.CSTOPB.value(),
        .CREAD_ = controlFlags.CREAD.value(),
        .PARENB_ = controlFlags.PARENB.value(),
        .PARODD_ = controlFlags.PARODD.value(),
        .HUPCL_ = controlFlags.HUPCL.value(),
        .CLOCAL_ = controlFlags.CLOCAL.value(),
        .LOBLK_ = controlFlags.LOBLK.value(),
        .CIBAUD_ = controlFlags.CIBAUD.value(),
        .CMSPAR_ = controlFlags.CMSPAR.value(),
        .CRTSCTS_ = controlFlags.CRTSCTS.value(),
    };

    state.localFlags = Persistence::Termios::LocalFlags{
        .ISIG_ = localFlags.ISIG.value(),
        .ICANON_ = localFlags.ICANON.value(),
        .XCASE_ = localFlags.XCASE.value(),
        .ECHO_ = localFlags.ECHO.value(),
        .ECHOE_ = localFlags.ECHOE.value(),
        .ECHOK_ = localFlags.ECHOK.value(),
        .ECHONL_ = localFlags.ECHONL.value(),
        .ECHOCTL_ = localFlags.ECHOCTL.value(),
        .ECHOPRT_ = localFlags.ECHOPRT.value(),
        .ECHOKE_ = localFlags.ECHOKE.value(),
        .FLUSHO_ = localFlags.FLUSHO.value(),
        .NOFLSH_ = localFlags.NOFLSH.value(),
        .TOSTOP_ = localFlags.TOSTOP.value(),
        .PENDIN_ = localFlags.PENDIN.value(),
        .IEXTEN_ = localFlags.IEXTEN.value(),
    };

    if (ccEngaged.value())
    {
        state.cc = Persistence::Termios::CC{
            .VDISCARD_ = cc.VDISCARD.valueIsValid() ? cc.VDISCARD.value() : Persistence::Termios::CC{}.VDISCARD_,
            .VDSUSP_ = cc.VDSUSP.valueIsValid() ? cc.VDSUSP.value() : Persistence::Termios::CC{}.VDSUSP_,
            .VEOF_ = cc.VEOF.valueIsValid() ? cc.VEOF.value() : Persistence::Termios::CC{}.VEOF_,
            .VEOL_ = cc.VEOL.valueIsValid() ? cc.VEOL.value() : Persistence::Termios::CC{}.VEOL_,
            .VEOL2_ = cc.VEOL2.valueIsValid() ? cc.VEOL2.value() : Persistence::Termios::CC{}.VEOL2_,
            .VERASE_ = cc.VERASE.valueIsValid() ? cc.VERASE.value() : Persistence::Termios::CC{}.VERASE_,
            .VINTR_ = cc.VINTR.valueIsValid() ? cc.VINTR.value() : Persistence::Termios::CC{}.VINTR_,
            .VKILL_ = cc.VKILL.valueIsValid() ? cc.VKILL.value() : Persistence::Termios::CC{}.VKILL_,
            .VLNEXT_ = cc.VLNEXT.valueIsValid() ? cc.VLNEXT.value() : Persistence::Termios::CC{}.VLNEXT_,
            .VMIN_ = cc.VMIN.valueIsValid() ? cc.VMIN.value() : Persistence::Termios::CC{}.VMIN_,
            .VQUIT_ = cc.VQUIT.valueIsValid() ? cc.VQUIT.value() : Persistence::Termios::CC{}.VQUIT_,
            .VREPRINT_ = cc.VREPRINT.valueIsValid() ? cc.VREPRINT.value() : Persistence::Termios::CC{}.VREPRINT_,
            .VSTART_ = cc.VSTART.valueIsValid() ? cc.VSTART.value() : Persistence::Termios::CC{}.VSTART_,
            .VSTATUS_ = cc.VSTATUS.valueIsValid() ? cc.VSTATUS.value() : Persistence::Termios::CC{}.VSTATUS_,
            .VSTOP_ = cc.VSTOP.valueIsValid() ? cc.VSTOP.value() : Persistence::Termios::CC{}.VSTOP_,
            .VSUSP_ = cc.VSUSP.valueIsValid() ? cc.VSUSP.value() : Persistence::Termios::CC{}.VSUSP_,
            .VSWTCH_ = cc.VSWTCH.valueIsValid() ? cc.VSWTCH.value() : Persistence::Termios::CC{}.VSWTCH_,
            .VTIME_ = cc.VTIME.valueIsValid() ? cc.VTIME.value() : Persistence::Termios::CC{}.VTIME_,
            .VWERASE_ = cc.VWERASE.valueIsValid() ? cc.VWERASE.value() : Persistence::Termios::CC{}.VWERASE_,
        };
    }
    else
    {
        state.cc = std::nullopt;
    }

    assignIfValid(state.iSpeed, iSpeed);
    assignIfValid(state.oSpeed, oSpeed);
}

void TermiosSettings::loadFromState(Persistence::Termios const& state)
{
    inputFlags.IGNBRK.value(state.inputFlags.IGNBRK_);
    inputFlags.BRKINT.value(state.inputFlags.BRKINT_);
    inputFlags.IGNPAR.value(state.inputFlags.IGNPAR_);
    inputFlags.PARMRK.value(state.inputFlags.PARMRK_);
    inputFlags.INPCK.value(state.inputFlags.INPCK_);
    inputFlags.ISTRIP.value(state.inputFlags.ISTRIP_);
    inputFlags.INLCR.value(state.inputFlags.INLCR_);
    inputFlags.IGNCR.value(state.inputFlags.IGNCR_);
    inputFlags.ICRNL.value(state.inputFlags.ICRNL_);
    inputFlags.IUCLC.value(state.inputFlags.IUCLC_);
    inputFlags.IXON.value(state.inputFlags.IXON_);
    inputFlags.IXANY.value(state.inputFlags.IXANY_);
    inputFlags.IXOFF.value(state.inputFlags.IXOFF_);
    inputFlags.IMAXBEL.value(state.inputFlags.IMAXBEL_);
    inputFlags.IUTF8.value(state.inputFlags.IUTF8_);

    outputFlags.OPOST.value(state.outputFlags.OPOST_);
    outputFlags.OLCUC.value(state.outputFlags.OLCUC_);
    outputFlags.ONLCR.value(state.outputFlags.ONLCR_);
    outputFlags.OCRNL.value(state.outputFlags.OCRNL_);
    outputFlags.ONOCR.value(state.outputFlags.ONOCR_);
    outputFlags.ONLRET.value(state.outputFlags.ONLRET_);
    outputFlags.OFILL.value(state.outputFlags.OFILL_);
    outputFlags.OFDEL.value(state.outputFlags.OFDEL_);
    outputFlags.NLDLY.value(state.outputFlags.NLDLY_);
    outputFlags.CRDLY.value(state.outputFlags.CRDLY_);
    outputFlags.TABDLY.value(state.outputFlags.TABDLY_);
    outputFlags.BSDLY.value(state.outputFlags.BSDLY_);
    outputFlags.VTDLY.value(state.outputFlags.VTDLY_);
    outputFlags.FFDLY.value(state.outputFlags.FFDLY_);

    controlFlags.CBAUD.value(state.controlFlags.CBAUD_);
    controlFlags.CBAUDEX.value(state.controlFlags.CBAUDEX_);
    controlFlags.CSIZE.value(state.controlFlags.CSIZE_);
    controlFlags.CSTOPB.value(state.controlFlags.CSTOPB_);
    controlFlags.CREAD.value(state.controlFlags.CREAD_);
    controlFlags.PARENB.value(state.controlFlags.PARENB_);
    controlFlags.PARODD.value(state.controlFlags.PARODD_);
    controlFlags.HUPCL.value(state.controlFlags.HUPCL_);
    controlFlags.CLOCAL.value(state.controlFlags.CLOCAL_);
    controlFlags.LOBLK.value(state.controlFlags.LOBLK_);
    controlFlags.CIBAUD.value(state.controlFlags.CIBAUD_);
    controlFlags.CMSPAR.value(state.controlFlags.CMSPAR_);
    controlFlags.CRTSCTS.value(state.controlFlags.CRTSCTS_);

    localFlags.ISIG.value(state.localFlags.ISIG_);
    localFlags.ICANON.value(state.localFlags.ICANON_);
    localFlags.XCASE.value(state.localFlags.XCASE_);
    localFlags.ECHO.value(state.localFlags.ECHO_);
    localFlags.ECHOE.value(state.localFlags.ECHOE_);
    localFlags.ECHOK.value(state.localFlags.ECHOK_);
    localFlags.ECHONL.value(state.localFlags.ECHONL_);
    localFlags.ECHOCTL.value(state.localFlags.ECHOCTL_);
    localFlags.ECHOPRT.value(state.localFlags.ECHOPRT_);
    localFlags.ECHOKE.value(state.localFlags.ECHOKE_);
    localFlags.FLUSHO.value(state.localFlags.FLUSHO_);
    localFlags.NOFLSH.value(state.localFlags.NOFLSH_);
    localFlags.TOSTOP.value(state.localFlags.TOSTOP_);
    localFlags.PENDIN.value(state.localFlags.PENDIN_);
    localFlags.IEXTEN.value(state.localFlags.IEXTEN_);

    if (state.cc.has_value())
    {
        ccEngaged = true;
        cc.VDISCARD.value(state.cc->VDISCARD_);
        cc.VDSUSP.value(state.cc->VDSUSP_);
        cc.VEOF.value(state.cc->VEOF_);
        cc.VEOL.value(state.cc->VEOL_);
        cc.VEOL2.value(state.cc->VEOL2_);
        cc.VERASE.value(state.cc->VERASE_);
        cc.VINTR.value(state.cc->VINTR_);
        cc.VKILL.value(state.cc->VKILL_);
        cc.VLNEXT.value(state.cc->VLNEXT_);
        cc.VMIN.value(state.cc->VMIN_);
        cc.VQUIT.value(state.cc->VQUIT_);
        cc.VREPRINT.value(state.cc->VREPRINT_);
        cc.VSTART.value(state.cc->VSTART_);
        cc.VSTATUS.value(state.cc->VSTATUS_);
        cc.VSTOP.value(state.cc->VSTOP_);
        cc.VSUSP.value(state.cc->VSUSP_);
        cc.VSWTCH.value(state.cc->VSWTCH_);
        cc.VTIME.value(state.cc->VTIME_);
        cc.VWERASE.value(state.cc->VWERASE_);
    }
    else
    {
        ccEngaged = false;
        cc.VDISCARD.value(Persistence::Termios::CC{}.VDISCARD_);
        cc.VDSUSP.value(Persistence::Termios::CC{}.VDSUSP_);
        cc.VEOF.value(Persistence::Termios::CC{}.VEOF_);
        cc.VEOL.value(Persistence::Termios::CC{}.VEOL_);
        cc.VEOL2.value(Persistence::Termios::CC{}.VEOL2_);
        cc.VERASE.value(Persistence::Termios::CC{}.VERASE_);
        cc.VINTR.value(Persistence::Termios::CC{}.VINTR_);
        cc.VKILL.value(Persistence::Termios::CC{}.VKILL_);
        cc.VLNEXT.value(Persistence::Termios::CC{}.VLNEXT_);
        cc.VMIN.value(Persistence::Termios::CC{}.VMIN_);
        cc.VQUIT.value(Persistence::Termios::CC{}.VQUIT_);
        cc.VREPRINT.value(Persistence::Termios::CC{}.VREPRINT_);
        cc.VSTART.value(Persistence::Termios::CC{}.VSTART_);
        cc.VSTATUS.value(Persistence::Termios::CC{}.VSTATUS_);
        cc.VSTOP.value(Persistence::Termios::CC{}.VSTOP_);
        cc.VSUSP.value(Persistence::Termios::CC{}.VSUSP_);
        cc.VSWTCH.value(Persistence::Termios::CC{}.VSWTCH_);
        cc.VTIME.value(Persistence::Termios::CC{}.VTIME_);
        cc.VWERASE.value(Persistence::Termios::CC{}.VWERASE_);
    }

    iSpeed.value(state.iSpeed);
    oSpeed.value(state.oSpeed);
}

void TermiosSettings::assumeDefaultsFrom(Persistence::Termios const& state)
{
    inputFlags.IGNBRK.inherit(state.inputFlags.IGNBRK_);
    inputFlags.BRKINT.inherit(state.inputFlags.BRKINT_);
    inputFlags.IGNPAR.inherit(state.inputFlags.IGNPAR_);
    inputFlags.PARMRK.inherit(state.inputFlags.PARMRK_);
    inputFlags.INPCK.inherit(state.inputFlags.INPCK_);
    inputFlags.ISTRIP.inherit(state.inputFlags.ISTRIP_);
    inputFlags.INLCR.inherit(state.inputFlags.INLCR_);
    inputFlags.IGNCR.inherit(state.inputFlags.IGNCR_);
    inputFlags.ICRNL.inherit(state.inputFlags.ICRNL_);
    inputFlags.IUCLC.inherit(state.inputFlags.IUCLC_);
    inputFlags.IXON.inherit(state.inputFlags.IXON_);
    inputFlags.IXANY.inherit(state.inputFlags.IXANY_);
    inputFlags.IXOFF.inherit(state.inputFlags.IXOFF_);
    inputFlags.IMAXBEL.inherit(state.inputFlags.IMAXBEL_);
    inputFlags.IUTF8.inherit(state.inputFlags.IUTF8_);

    outputFlags.OPOST.inherit(state.outputFlags.OPOST_);
    outputFlags.OLCUC.inherit(state.outputFlags.OLCUC_);
    outputFlags.ONLCR.inherit(state.outputFlags.ONLCR_);
    outputFlags.OCRNL.inherit(state.outputFlags.OCRNL_);
    outputFlags.ONOCR.inherit(state.outputFlags.ONOCR_);
    outputFlags.ONLRET.inherit(state.outputFlags.ONLRET_);
    outputFlags.OFILL.inherit(state.outputFlags.OFILL_);
    outputFlags.OFDEL.inherit(state.outputFlags.OFDEL_);
    outputFlags.NLDLY.inherit(state.outputFlags.NLDLY_);
    outputFlags.CRDLY.inherit(state.outputFlags.CRDLY_);
    outputFlags.TABDLY.inherit(state.outputFlags.TABDLY_);
    outputFlags.BSDLY.inherit(state.outputFlags.BSDLY_);
    outputFlags.VTDLY.inherit(state.outputFlags.VTDLY_);
    outputFlags.FFDLY.inherit(state.outputFlags.FFDLY_);

    controlFlags.CBAUD.inherit(state.controlFlags.CBAUD_);
    controlFlags.CBAUDEX.inherit(state.controlFlags.CBAUDEX_);
    controlFlags.CSIZE.inherit(state.controlFlags.CSIZE_);
    controlFlags.CSTOPB.inherit(state.controlFlags.CSTOPB_);
    controlFlags.CREAD.inherit(state.controlFlags.CREAD_);
    controlFlags.PARENB.inherit(state.controlFlags.PARENB_);
    controlFlags.PARODD.inherit(state.controlFlags.PARODD_);
    controlFlags.HUPCL.inherit(state.controlFlags.HUPCL_);
    controlFlags.CLOCAL.inherit(state.controlFlags.CLOCAL_);
    controlFlags.LOBLK.inherit(state.controlFlags.LOBLK_);
    controlFlags.CIBAUD.inherit(state.controlFlags.CIBAUD_);
    controlFlags.CMSPAR.inherit(state.controlFlags.CMSPAR_);
    controlFlags.CRTSCTS.inherit(state.controlFlags.CRTSCTS_);

    localFlags.ISIG.inherit(state.localFlags.ISIG_);
    localFlags.ICANON.inherit(state.localFlags.ICANON_);
    localFlags.XCASE.inherit(state.localFlags.XCASE_);
    localFlags.ECHO.inherit(state.localFlags.ECHO_);
    localFlags.ECHOE.inherit(state.localFlags.ECHOE_);
    localFlags.ECHOK.inherit(state.localFlags.ECHOK_);
    localFlags.ECHONL.inherit(state.localFlags.ECHONL_);
    localFlags.ECHOCTL.inherit(state.localFlags.ECHOCTL_);
    localFlags.ECHOPRT.inherit(state.localFlags.ECHOPRT_);
    localFlags.ECHOKE.inherit(state.localFlags.ECHOKE_);
    localFlags.FLUSHO.inherit(state.localFlags.FLUSHO_);
    localFlags.NOFLSH.inherit(state.localFlags.NOFLSH_);
    localFlags.TOSTOP.inherit(state.localFlags.TOSTOP_);
    localFlags.PENDIN.inherit(state.localFlags.PENDIN_);
    localFlags.IEXTEN.inherit(state.localFlags.IEXTEN_);

    if (state.cc)
    {
        cc.VDISCARD.inheritValue(state.cc->VDISCARD_);
        cc.VDSUSP.inheritValue(state.cc->VDSUSP_);
        cc.VEOF.inheritValue(state.cc->VEOF_);
        cc.VEOL.inheritValue(state.cc->VEOL_);
        cc.VEOL2.inheritValue(state.cc->VEOL2_);
        cc.VERASE.inheritValue(state.cc->VERASE_);
        cc.VINTR.inheritValue(state.cc->VINTR_);
        cc.VKILL.inheritValue(state.cc->VKILL_);
        cc.VLNEXT.inheritValue(state.cc->VLNEXT_);
        cc.VMIN.inheritValue(state.cc->VMIN_);
        cc.VQUIT.inheritValue(state.cc->VQUIT_);
        cc.VREPRINT.inheritValue(state.cc->VREPRINT_);
        cc.VSTART.inheritValue(state.cc->VSTART_);
        cc.VSTATUS.inheritValue(state.cc->VSTATUS_);
        cc.VSTOP.inheritValue(state.cc->VSTOP_);
        cc.VSUSP.inheritValue(state.cc->VSUSP_);
        cc.VSWTCH.inheritValue(state.cc->VSWTCH_);
        cc.VTIME.inheritValue(state.cc->VTIME_);
        cc.VWERASE.inheritValue(state.cc->VWERASE_);
    }
    else
    {
        cc.VDISCARD.inherit(std::nullopt);
        cc.VDSUSP.inherit(std::nullopt);
        cc.VEOF.inherit(std::nullopt);
        cc.VEOL.inherit(std::nullopt);
        cc.VEOL2.inherit(std::nullopt);
        cc.VERASE.inherit(std::nullopt);
        cc.VINTR.inherit(std::nullopt);
        cc.VKILL.inherit(std::nullopt);
        cc.VLNEXT.inherit(std::nullopt);
        cc.VMIN.inherit(std::nullopt);
        cc.VQUIT.inherit(std::nullopt);
        cc.VREPRINT.inherit(std::nullopt);
        cc.VSTART.inherit(std::nullopt);
        cc.VSTATUS.inherit(std::nullopt);
        cc.VSTOP.inherit(std::nullopt);
        cc.VSUSP.inherit(std::nullopt);
        cc.VSWTCH.inherit(std::nullopt);
        cc.VTIME.inherit(std::nullopt);
        cc.VWERASE.inherit(std::nullopt);
    }

    iSpeed.inherit(state.iSpeed);
    oSpeed.inherit(state.oSpeed);
}

Nui::ElementRenderer TermiosSettings::render()
{
    using namespace Nui::Elements;
    using namespace Nui::Attributes;

    return fragment(
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "inputFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                inputFlags.IGNBRK(),
                inputFlags.BRKINT(),
                inputFlags.IGNPAR(),
                inputFlags.PARMRK(),
                inputFlags.INPCK(),
                inputFlags.ISTRIP(),
                inputFlags.INLCR(),
                inputFlags.IGNCR(),
                inputFlags.ICRNL(),
                inputFlags.IUCLC(),
                inputFlags.IXON(),
                inputFlags.IXANY(),
                inputFlags.IXOFF(),
                inputFlags.IMAXBEL(),
                inputFlags.IUTF8()
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "outputFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                outputFlags.OPOST(),
                outputFlags.OLCUC(),
                outputFlags.ONLCR(),
                outputFlags.OCRNL(),
                outputFlags.ONOCR(),
                outputFlags.ONLRET(),
                outputFlags.OFILL(),
                outputFlags.OFDEL(),
                outputFlags.NLDLY(),
                outputFlags.CRDLY(),
                outputFlags.TABDLY(),
                outputFlags.BSDLY(),
                outputFlags.VTDLY(),
                outputFlags.FFDLY()
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "controlFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                controlFlags.CBAUD(),
                controlFlags.CBAUDEX(),
                controlFlags.CSIZE(),
                controlFlags.CSTOPB(),
                controlFlags.CREAD(),
                controlFlags.PARENB(),
                controlFlags.PARODD(),
                controlFlags.HUPCL(),
                controlFlags.CLOCAL(),
                controlFlags.LOBLK(),
                controlFlags.CIBAUD(),
                controlFlags.CMSPAR(),
                controlFlags.CRTSCTS()
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "localFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                localFlags.ISIG(),
                localFlags.ICANON(),
                localFlags.XCASE(),
                localFlags.ECHO(),
                localFlags.ECHOE(),
                localFlags.ECHOK(),
                localFlags.ECHONL(),
                localFlags.ECHOCTL(),
                localFlags.ECHOPRT(),
                localFlags.ECHOKE(),
                localFlags.FLUSHO(),
                localFlags.NOFLSH(),
                localFlags.TOSTOP(),
                localFlags.PENDIN(),
                localFlags.IEXTEN()
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "ccSettingsSubgroupTitle")),
        subgroup(
            {.engagedStatus = &ccEngaged,
                .groupTitle = language->getObserved("settings", "ccSettingsSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                cc.VDISCARD(),
                cc.VDSUSP(),
                cc.VEOF(),
                cc.VEOL(),
                cc.VEOL2(),
                cc.VERASE(),
                cc.VINTR(),
                cc.VKILL(),
                cc.VLNEXT(),
                cc.VMIN(),
                cc.VQUIT(),
                cc.VREPRINT(),
                cc.VSTART(),
                cc.VSTATUS(),
                cc.VSTOP(),
                cc.VSUSP(),
                cc.VSWTCH(),
                cc.VTIME(),
                cc.VWERASE()
            )
        ),
        iSpeed(),
        oSpeed()
    );
}