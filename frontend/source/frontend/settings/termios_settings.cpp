#include <frontend/settings/termios_settings.hpp>
#include <persistence/state/termios.hpp>

#include <frontend/settings/nullopt_reset.hpp>

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
            &ccEngaged
        },
    },
    iSpeed{
        language->getObserved("settings", "termios", "iSpeedHelpText"),
        onChange,
        nulloptReset(iSpeed, onChange),
    },
    oSpeed{
        language->getObserved("settings", "termios", "oSpeedHelpText"),
        onChange,
        nulloptReset(oSpeed, onChange),
    }
{}