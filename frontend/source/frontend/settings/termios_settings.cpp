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

TermiosSettings::TermiosSettings(std::function<void()> const& onChange)
    : inputFlags{
            .IGNBRK{
                language->getObserved("settings", "termios", "inputFlags", "IGNBRKHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IGNBRK.value(Persistence::Termios::InputFlags::saneDefaults().IGNBRK_);
                    onChange();
                }
            },
            .BRKINT{
                language->getObserved("settings", "termios", "inputFlags", "BRKINTHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.BRKINT.value(Persistence::Termios::InputFlags::saneDefaults().BRKINT_);
                    onChange();
                }
            },
            .IGNPAR{
                language->getObserved("settings", "termios", "inputFlags", "IGNPARHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IGNPAR.value(Persistence::Termios::InputFlags::saneDefaults().IGNPAR_);
                    onChange();
                }
            },
            .PARMRK{
                language->getObserved("settings", "termios", "inputFlags", "PARMRKHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.PARMRK.value(Persistence::Termios::InputFlags::saneDefaults().PARMRK_);
                    onChange();
                }
            },
            .INPCK{
                language->getObserved("settings", "termios", "inputFlags", "INPCKHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.INPCK.value(Persistence::Termios::InputFlags::saneDefaults().INPCK_);
                    onChange();
                }
            },
            .ISTRIP{
                language->getObserved("settings", "termios", "inputFlags", "ISTRIPHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.ISTRIP.value(Persistence::Termios::InputFlags::saneDefaults().ISTRIP_);
                    onChange();
                }
            },
            .INLCR{
                language->getObserved("settings", "termios", "inputFlags", "INLCRHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.INLCR.value(Persistence::Termios::InputFlags::saneDefaults().INLCR_);
                    onChange();
                }
            },
            .IGNCR{
                language->getObserved("settings", "termios", "inputFlags", "IGNCRHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IGNCR.value(Persistence::Termios::InputFlags::saneDefaults().IGNCR_);
                    onChange();
                }
            },
            .ICRNL{
                language->getObserved("settings", "termios", "inputFlags", "ICRNLHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.ICRNL.value(Persistence::Termios::InputFlags::saneDefaults().ICRNL_);
                    onChange();
                }
            },
            .IUCLC{
                language->getObserved("settings", "termios", "inputFlags", "IUCLCHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IUCLC.value(Persistence::Termios::InputFlags::saneDefaults().IUCLC_);
                    onChange();
                }
            },
            .IXON{
                language->getObserved("settings", "termios", "inputFlags", "IXONHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IXON.value(Persistence::Termios::InputFlags::saneDefaults().IXON_);
                    onChange();
                }
            },
            .IXANY{
                language->getObserved("settings", "termios", "inputFlags", "IXANYHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IXANY.value(Persistence::Termios::InputFlags::saneDefaults().IXANY_);
                    onChange();
                }
            },
            .IXOFF{
                language->getObserved("settings", "termios", "inputFlags", "IXOFFHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IXOFF.value(Persistence::Termios::InputFlags::saneDefaults().IXOFF_);
                    onChange();
                }
            },
            .IMAXBEL{
                language->getObserved("settings", "termios", "inputFlags", "IMAXBELHelpText"),
                onChange,
                [this, onChange]()
                {
                    inputFlags.IMAXBEL.value(Persistence::Termios::InputFlags::saneDefaults().IMAXBEL_);
                    onChange();
                }
            },
            .IUTF8{
                language->getObserved("settings", "termios", "inputFlags", "IUTF8HelpText"),
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
                language->getObserved("settings", "termios", "outputFlags", "OPOSTHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OPOST.value(Persistence::Termios::OutputFlags::saneDefaults().OPOST_);
                    onChange();
                }
            },
            .OLCUC{
                language->getObserved("settings", "termios", "outputFlags", "OLCUCHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OLCUC.value(Persistence::Termios::OutputFlags::saneDefaults().OLCUC_);
                    onChange();
                }
            },
            .ONLCR{
                language->getObserved("settings", "termios", "outputFlags", "ONLCRHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.ONLCR.value(Persistence::Termios::OutputFlags::saneDefaults().ONLCR_);
                    onChange();
                }
            },
            .OCRNL{
                language->getObserved("settings", "termios", "outputFlags", "OCRNLHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OCRNL.value(Persistence::Termios::OutputFlags::saneDefaults().OCRNL_);
                    onChange();
                }
            },
            .ONOCR{
                language->getObserved("settings", "termios", "outputFlags", "ONOCRHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.ONOCR.value(Persistence::Termios::OutputFlags::saneDefaults().ONOCR_);
                    onChange();
                }
            },
            .ONLRET{
                language->getObserved("settings", "termios", "outputFlags", "ONLRETHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.ONLRET.value(Persistence::Termios::OutputFlags::saneDefaults().ONLRET_);
                    onChange();
                }
            },
            .OFILL{
                language->getObserved("settings", "termios", "outputFlags", "OFILLHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OFILL.value(Persistence::Termios::OutputFlags::saneDefaults().OFILL_);
                    onChange();
                }
            },
            .OFDEL{
                language->getObserved("settings", "termios", "outputFlags", "OFDELHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.OFDEL.value(Persistence::Termios::OutputFlags::saneDefaults().OFDEL_);
                    onChange();
                }
            },
            .NLDLY{
                language->getObserved("settings", "termios", "outputFlags", "NLDLYHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.NLDLY.value(Persistence::Termios::OutputFlags::saneDefaults().NLDLY_);
                    onChange();
                }
            },
            .CRDLY{
                language->getObserved("settings", "termios", "outputFlags", "CRDLYHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.CRDLY.value(Persistence::Termios::OutputFlags::saneDefaults().CRDLY_);
                    onChange();
                }
            },
            .TABDLY{
                language->getObserved("settings", "termios", "outputFlags", "TABDLYHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.TABDLY.value(Persistence::Termios::OutputFlags::saneDefaults().TABDLY_);
                    onChange();
                }
            },
            .BSDLY{
                language->getObserved("settings", "termios", "outputFlags", "BSDLYHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.BSDLY.value(Persistence::Termios::OutputFlags::saneDefaults().BSDLY_);
                    onChange();
                }
            },
            .VTDLY{
                language->getObserved("settings", "termios", "outputFlags", "VTDLYHelpText"),
                onChange,
                [this, onChange]()
                {
                    outputFlags.VTDLY.value(Persistence::Termios::OutputFlags::saneDefaults().VTDLY_);
                    onChange();
                }
            },
            .FFDLY{
                language->getObserved("settings", "termios", "outputFlags", "FFDLYHelpText"),
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
            language->getObserved("settings", "termios", "controlFlags", "CBAUDHelpText"),
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
            language->getObserved("settings", "termios", "controlFlags", "CBAUDEXHelpText"),
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
            language->getObserved("settings", "termios", "controlFlags", "CSIZEHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.CSIZE.value(Persistence::Termios::ControlFlags::saneDefaults().CSIZE_);
                onChange();
            }
        },
        .CSTOPB{
            language->getObserved("settings", "termios", "controlFlags", "CSTOPBHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.CSTOPB.value(Persistence::Termios::ControlFlags::saneDefaults().CSTOPB_);
                onChange();
            }
        },
        .CREAD{
            language->getObserved("settings", "termios", "controlFlags", "CREADHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.CREAD.value(Persistence::Termios::ControlFlags::saneDefaults().CREAD_);
                onChange();
            }
        },
        .PARENB{
            language->getObserved("settings", "termios", "controlFlags", "PARENBHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.PARENB.value(Persistence::Termios::ControlFlags::saneDefaults().PARENB_);
                onChange();
            }
        },
        .PARODD{
            language->getObserved("settings", "termios", "controlFlags", "PARODDHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.PARODD.value(Persistence::Termios::ControlFlags::saneDefaults().PARODD_);
                onChange();
            }
        },
        .HUPCL{
            language->getObserved("settings", "termios", "controlFlags", "HUPCLHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.HUPCL.value(Persistence::Termios::ControlFlags::saneDefaults().HUPCL_);
                onChange();
            }
        },
        .CLOCAL{
            language->getObserved("settings", "termios", "controlFlags", "CLOCALHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.CLOCAL.value(Persistence::Termios::ControlFlags::saneDefaults().CLOCAL_);
                onChange();
            }
        },
        .LOBLK{
            language->getObserved("settings", "termios", "controlFlags", "LOBLKHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.LOBLK.value(Persistence::Termios::ControlFlags::saneDefaults().LOBLK_);
                onChange();
            }
        },
        .CIBAUD{
            language->getObserved("settings", "termios", "controlFlags", "CIBAUDHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.CIBAUD.value(Persistence::Termios::ControlFlags::saneDefaults().CIBAUD_);
                onChange();
            }
        },
        .CMSPAR{
            language->getObserved("settings", "termios", "controlFlags", "CMSPARHelpText"),
            onChange,
            [this, onChange]()
            {
                controlFlags.CMSPAR.value(Persistence::Termios::ControlFlags::saneDefaults().CMSPAR_);
                onChange();
            }
        },
        .CRTSCTS{
            language->getObserved("settings", "termios", "controlFlags", "CRTSCTSHelpText"),
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
                language->getObserved("settings", "termios", "localFlags", "ISIGHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFlags.ISIG.value(Persistence::Termios::LocalFlags::saneDefaults().ISIG_);
                    onChange();
                }
            },
            .ICANON{
                language->getObserved("settings", "termios", "localFlags", "ICANONHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFlags.ICANON.value(Persistence::Termios::LocalFlags::saneDefaults().ICANON_);
                    onChange();
                }
            },
            .XCASE{
                language->getObserved("settings", "termios", "localFlags", "XCASEHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFlags.XCASE.value(Persistence::Termios::LocalFlags::saneDefaults().XCASE_);
                    onChange();
                }
            },
            .ECHO{
                language->getObserved("settings", "termios", "localFlags", "ECHOHelpText"),
                onChange,
                [this, onChange]()
                {
                    localFlags.ECHO.value(Persistence::Termios::LocalFlags::saneDefaults().ECHO_);
                    onChange();
                }
            },
        .ECHOE{
            language->getObserved("settings", "termios", "localFlags", "ECHOEHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOE.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOE_);
                onChange();
            }
        },
        .ECHOK{
            language->getObserved("settings", "termios", "localFlags", "ECHOKHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOK.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOK_);
                onChange();
            }
        },
        .ECHONL{
            language->getObserved("settings", "termios", "localFlags", "ECHONLHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHONL.value(Persistence::Termios::LocalFlags::saneDefaults().ECHONL_);
                onChange();
            }
        },
        .ECHOCTL{
            language->getObserved("settings", "termios", "localFlags", "ECHOCTLHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOCTL.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOCTL_);
                onChange();
            }
        },
        .ECHOPRT{
            language->getObserved("settings", "termios", "localFlags", "ECHOPRTHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOPRT.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOPRT_);
                onChange();
            }
        },
        .ECHOKE{
            language->getObserved("settings", "termios", "localFlags", "ECHOKEHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.ECHOKE.value(Persistence::Termios::LocalFlags::saneDefaults().ECHOKE_);
                onChange();
            }
        },
        .FLUSHO{
            language->getObserved("settings", "termios", "localFlags", "FLUSHOHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.FLUSHO.value(Persistence::Termios::LocalFlags::saneDefaults().FLUSHO_);
                onChange();
            }
        },
        .NOFLSH{
            language->getObserved("settings", "termios", "localFlags", "NOFLSHHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.NOFLSH.value(Persistence::Termios::LocalFlags::saneDefaults().NOFLSH_);
                onChange();
            }
        },
        .TOSTOP{
            language->getObserved("settings", "termios", "localFlags", "TOSTOPHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.TOSTOP.value(Persistence::Termios::LocalFlags::saneDefaults().TOSTOP_);
                onChange();
            }
        },
        .PENDIN{
            language->getObserved("settings", "termios", "localFlags", "PENDINHelpText"),
            onChange,
            [this, onChange]()
            {
                localFlags.PENDIN.value(Persistence::Termios::LocalFlags::saneDefaults().PENDIN_);
                onChange();
            }
        },
        .IEXTEN{
            language->getObserved("settings", "termios", "localFlags", "IEXTENHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VDISCARDHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VDSUSPHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VEOFHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VEOLHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VEOL2HelpText"),
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
            language->getObserved("settings", "termios", "cc", "VERASEHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VINTRHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VKILLHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VLNEXTHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VMINHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VQUITHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VREPRINTHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VSTARTHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VSTATUSHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VSTOPHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VSUSPHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VSWTCHHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VTIMEHelpText"),
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
            language->getObserved("settings", "termios", "cc", "VWERASEHelpText"),
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
        language->getObserved("settings", "termios", "iSpeedHelpText"),
        onChange,
        nulloptReset(iSpeed, onChange),
        {
            .minValue = 0,
        }
    },
    oSpeed{
        language->getObserved("settings", "termios", "oSpeedHelpText"),
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
                inputFlags.IGNBRK("IGNBRK"),
                inputFlags.BRKINT("BRKINT"),
                inputFlags.IGNPAR("IGNPAR"),
                inputFlags.PARMRK("PARMRK"),
                inputFlags.INPCK("INPCK"),
                inputFlags.ISTRIP("ISTRIP"),
                inputFlags.INLCR("INLCR"),
                inputFlags.IGNCR("IGNCR"),
                inputFlags.ICRNL("ICRNL"),
                inputFlags.IUCLC("IUCLC"),
                inputFlags.IXON("IXON"),
                inputFlags.IXANY("IXANY"),
                inputFlags.IXOFF("IXOFF"),
                inputFlags.IMAXBEL("IMAXBEL"),
                inputFlags.IUTF8("IUTF8")
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "outputFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                outputFlags.OPOST("OPOST"),
                outputFlags.OLCUC("OLCUC"),
                outputFlags.ONLCR("ONLCR"),
                outputFlags.OCRNL("OCRNL"),
                outputFlags.ONOCR("ONOCR"),
                outputFlags.ONLRET("ONLRET"),
                outputFlags.OFILL("OFILL"),
                outputFlags.OFDEL("OFDEL"),
                outputFlags.NLDLY("NLDLY"),
                outputFlags.CRDLY("CRDLY"),
                outputFlags.TABDLY("TABDLY"),
                outputFlags.BSDLY("BSDLY"),
                outputFlags.VTDLY("VTDLY"),
                outputFlags.FFDLY("FFDLY")
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "controlFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                controlFlags.CBAUD("CBAUD"),
                controlFlags.CBAUDEX("CBAUDEX"),
                controlFlags.CSIZE("CSIZE"),
                controlFlags.CSTOPB("CSTOPB"),
                controlFlags.CREAD("CREAD"),
                controlFlags.PARENB("PARENB"),
                controlFlags.PARODD("PARODD"),
                controlFlags.HUPCL("HUPCL"),
                controlFlags.CLOCAL("CLOCAL"),
                controlFlags.LOBLK("LOBLK"),
                controlFlags.CIBAUD("CIBAUD"),
                controlFlags.CMSPAR("CMSPAR"),
                controlFlags.CRTSCTS("CRTSCTS")
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "localFlagsSubgroupTitle")),
        subgroup(
            {.onChange = onChange_},
            fragment(
                localFlags.ISIG("ISIG"),
                localFlags.ICANON("ICANON"),
                localFlags.XCASE("XCASE"),
                localFlags.ECHO("ECHO"),
                localFlags.ECHOE("ECHOE"),
                localFlags.ECHOK("ECHOK"),
                localFlags.ECHONL("ECHONL"),
                localFlags.ECHOPRT("ECHOPRT"),
                localFlags.ECHOKE("ECHOKE"),
                localFlags.FLUSHO("FLUSHO"),
                localFlags.NOFLSH("NOFLSH"),
                localFlags.TOSTOP("TOSTOP"),
                localFlags.PENDIN("PENDIN"),
                localFlags.IEXTEN("IEXTEN")
            )
        ),
        h1{class_ = "settings-header"}(language->getObserved("settings", "termios", "ccSettingsSubgroupTitle")),
        subgroup(
            {.engagedStatus = &ccEngaged,
                .groupTitle = language->getObserved("settings", "ccSettingsSubgroupTitle"),
                .onChange = onChange_},
            fragment(
                cc.VDISCARD("VDISCARD"),
                cc.VDSUSP("VDSUSP"),
                cc.VEOF("VEOF"),
                cc.VEOL("VEOL"),
                cc.VEOL2("VEOL2"),
                cc.VERASE("VERASE"),
                cc.VINTR("VINTR"),
                cc.VKILL("VKILL"),
                cc.VLNEXT("VLNEXT"),
                cc.VMIN("VMIN"),
                cc.VQUIT("VQUIT"),
                cc.VREPRINT("VREPRINT"),
                cc.VSTART("VSTART"),
                cc.VSTATUS("VSTATUS"),
                cc.VSTOP("VSTOP"),
                cc.VSUSP("VSUSP"),
                cc.VSWTCH("VSWTCH"),
                cc.VTIME("VTIME"),
                cc.VWERASE("VWERASE")
            )
        ),
        iSpeed(language->getObserved("settings", "termios", "iSpeedHelpText")),
        oSpeed(language->getObserved("settings", "termios", "oSpeedHelpText"))
    );
}