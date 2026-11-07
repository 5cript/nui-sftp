# FTP client requirements

Every requirement on the client, from the RFCs and from what real servers do, and the tests that cover it as
`Suite.Test`. A requirement without tests has none yet. One that does not apply to the client says why in its Tests
cell. Keep the Tests column in step when a test covers a requirement or changes its name.

| ID | Source | Level | Obligation | How the client meets it | Tests |
|---|---|---|---|---|---|
| R959_3_2_DataConnectionPerTransfer | RFC 959 3.2 | MUST | Establish a data connection for each transfer and close it after | DataConnection is created per transfer | |
| R959_3_3_DataConnectionManagement | RFC 959 3.3 | MUST | Treat the end of the data connection as the end of a stream mode transfer | Transfer ends on data EOF plus the final reply | |
| R959_4_1_1_UserPass | RFC 959 4.1.1 | MUST | Send USER, then PASS after 331, ACCT after 332 | Login flow | |
| R959_4_1_1_CwdCdup | RFC 959 4.1.1 | MUST | Change directories with CWD and CDUP and honour their replies | Metadata operations | |
| R959_4_1_1_Quit | RFC 959 4.1.1 | SHOULD | End a session with QUIT and wait for 221 within a bound | ControlConnection close | |
| R959_4_1_2_Pasv | RFC 959 4.1.2 | MUST | Use PASV for a data connection when EPSV is not available | DataConnection setup | |
| R959_4_1_2_TypeImage | RFC 959 4.1.2 | MUST | Send TYPE I before binary transfers, the default is ASCII | Login flow sets TYPE I | |
| R959_4_1_2_StruFile | RFC 959 4.1.2 | MUST NOT | Never send another STRU than the default file structure | The encoder has no STRU | |
| R959_4_1_2_ModeStream | RFC 959 4.1.2 | MUST NOT | Never send another MODE than the default stream mode | The encoder has no MODE | |
| R959_4_1_3_RetrStor | RFC 959 4.1.3 | MUST | Follow the 1yz, data, 2yz sequence of RETR and STOR | Transfer flow | |
| R959_4_1_3_Appe | RFC 959 4.1.3 | MUST | Append with APPE where the file system asks for it | Transfer flow | |
| R959_4_1_3_Rest | RFC 959 4.1.3 | MUST | Send REST immediately before the transfer it applies to | Transfer flow | |
| R959_4_1_3_RnfrRnto | RFC 959 4.1.3 | MUST | Send RNTO only after a 350 reply to RNFR | Metadata operations | |
| R959_4_1_3_Abor | RFC 959 4.1.3 | MUST | Handle the 426 then 226 and the 226 alone sequences of ABOR | Cancel and drain | |
| R959_4_1_3_DeleRmdMkd | RFC 959 4.1.3 | MUST | Remove and create with DELE, RMD and MKD and honour their replies | Metadata operations | |
| R959_4_1_3_Pwd | RFC 959 4.1.3 | MUST | Read the current directory from the 257 reply of PWD | Metadata operations | |
| R959_4_1_3_ListNlst | RFC 959 4.1.3 | MUST | Read LIST and NLST over a data connection | Listing flow | |
| R959_4_1_3_SiteStatSystNoop | RFC 959 4.1.3 | MAY | Treat SITE, STAT, SYST and NOOP replies as optional capabilities | Metadata operations, keepalive | |
| R959_4_2_ReplyFormat | RFC 959 4.2 | MUST | A reply is three digits, a space or hyphen, text and CRLF | Reply framing | |
| R959_4_2_MultiLineContinuation | RFC 959 4.2 | MUST | A multi-line reply ends with a line of the same code and a space | Reply framing | |
| R959_4_2_FirstDigitSemantics | RFC 959 4.2 | MUST | The first digit says whether the command succeeded, goes on or failed | classifyReply | `ReplyCodeTests.FirstDigitGivesTheClass` |
| R959_4_2_OneFinalReplyPerCommand | RFC 959 4.2 | MUST | Each command gets exactly one final reply, FIFO on the control connection | ControlConnection pending replies | |
| R959_4_2_PreliminaryReplies | RFC 959 4.2 | MUST | Wait for the final reply behind a 1yz reply | ControlConnection pending replies | |
| R959_4_2_2_NumericOrderList | RFC 959 4.2.2 | MUST | Map every listed reply code to an outcome | Reply mapping | |
| R959_5_3_CommandSyntax | RFC 959 5.3 | MUST | Upper case verb, one space, argument, CRLF | Command encoding | |
| R959_5_3_NoLineEndsInArguments | RFC 959 5.3 | MUST NOT | Never put CR or LF into an argument | Command encoding refuses them | |
| R959_5_4_CommandReplySequences | RFC 959 5.4 | MUST | Accept exactly the replies listed for each command | Reply mapping | |
| R959_AppendixII_257Quoting | RFC 959 Appendix II | MUST | Read a quoted path with doubled quotes from a 257 reply | Typed reply parser | |
| R1123_4_1_2_6_PasvReplyFormat | RFC 1123 4.1.2.6 | MUST | Do not assume the format of the text around the PASV numbers | Typed reply parser | |
| R1123_4_1_2_6_PasvPerTransfer | RFC 1123 4.1.2.6 | MUST | Send PASV for every transfer | DataConnection setup | |
| R1123_4_1_2_7_ListFormats | RFC 1123 4.1.2.7 | SHOULD | Do not assume one format of LIST output | Listing parsers | |
| R1123_4_1_2_10_TelnetEndOfLine | RFC 1123 4.1.2.10 | MUST | Lines end in CRLF on the control connection | Reply framing | |
| R1123_4_1_2_10_NoReadBoundaries | RFC 1123 4.1.2.10 | MUST NOT | Do not rely on TCP read boundaries on the control connection | Reply framing is fed in any split | |
| R1123_4_1_2_11_MultiLineReplies | RFC 1123 4.1.2.11 | MUST | Handle multi-line replies for every command | Reply framing | |
| R1123_4_1_2_11_UnknownCodesUseTheHighDigit | RFC 1123 4.1.2.11 | MUST | Use only the high digit of an unknown reply code | classifyReply | `ReplyCodeTests.UnknownCodesUseTheHighDigit` |
| R1123_4_1_2_11_No421Special | RFC 1123 4.1.2.11 | SHOULD NOT | Do not handle 421 differently from a closed connection | Reply mapping | |
| R1123_4_1_2_12_NoTelnetNegotiation | RFC 1123 4.1.2.12 | MUST NOT | Send no Telnet commands other than Synch and IP, negotiate no options | The encoder has none | |
| R1123_4_1_2_12_DataPortOnControlAddress | RFC 1123 4.1.2.12 | MUST | Use the address of the control connection for the data connection | DataConnection ignores the announced address | |
| R1123_4_1_3_4_AsyncRestartMarkers | RFC 1123 4.1.3.4 | MUST NOT | Do not assume that 110 restart markers are synchronous | Stream mode only, 110 is skipped | |
| R1123_4_1_4_4_Synchronization | RFC 1123 4.1.4.4 | MUST | Keep commands and replies in step after failures | ControlConnection marks itself dead | |
| R2228_AuthReplies | RFC 2228 | MUST | Handle 234, 334, 431, 500, 502, 504 and 534 to AUTH | Login flow | |
| R2228_PbszBeforeProt | RFC 2228 | MUST | Send PBSZ before PROT | Login flow | |
| R2228_ProtReplies | RFC 2228 | MUST | Handle the 536 and 534 replies to PROT | Login flow | |
| R2228_CccNotSent | RFC 2228 | MUST NOT | Never send CCC | The encoder has no CCC | |
| R2389_FeatFormat | RFC 2389 | MUST | Read 211 multi-line replies with one leading space per feature | FEAT parser | |
| R2389_NoFeat | RFC 2389 | MUST | Treat 500 and 502 to FEAT as no features | FEAT parser | |
| R2389_OptsSyntax | RFC 2389 | MUST | Send OPTS with the command name and its options | Command encoding | |
| R2428_EpsvFormat | RFC 2428 | MUST | Read the 229 format with any delimiter | Typed reply parser | |
| R2428_EpsvAllNeverSent | RFC 2428 | MUST NOT | Never send EPSV ALL | The encoder has no EPSV ALL | |
| R2428_522Handling | RFC 2428 | MUST | Fall back to the protocols a 522 names, for EPRT | Active mode, a later parity step | |
| R2428_Ipv6 | RFC 2428 | MUST | Use EPSV on IPv6, PASV has no IPv6 form | DataConnection setup | |
| R2640_Utf8Pathnames | RFC 2640 | SHOULD | Send path names as UTF-8 | Command encoding | |
| R2640_ValidityCheck | RFC 2640 | SHOULD | Check received names for valid UTF-8 | Listing parsers keep bytes and flag invalid names | |
| R2640_CrNulInPathnames | RFC 2640 | MUST | Treat a CR in a path name as CR NUL on the wire | Command encoding | |
| R3659_SizeTypeImage | RFC 3659 | MUST | Ask SIZE in TYPE I and read the 213 reply | Typed reply parser | |
| R3659_MdtmTimeVal | RFC 3659 | MUST | Read time-val as UTC with optional fractions | Typed reply parser | |
| R3659_RestStream | RFC 3659 | MUST | Use REST STREAM semantics and placement | Transfer flow | |
| R3659_MlstGrammar | RFC 3659 | MUST | Read MLST and MLSD entries by the grammar and their facts | Listing parsers | |
| R3659_CdirPdir | RFC 3659 | MUST | Do not list cdir and pdir entries as children | Listing parsers | |
| R3659_TvfsPaths | RFC 3659 | SHOULD | Use slash separated paths when TVFS is offered | Metadata operations | |
| R3659_MlstErrors | RFC 3659 | MUST | Map the error replies of MLST and MLSD | Reply mapping | |
| R4217_AuthTlsPbszProt | RFC 4217 | MUST | Send AUTH TLS, then PBSZ 0 and PROT P | Login flow | |
| R4217_7_ClientIsTlsClient | RFC 4217 7 | MUST | Be the TLS client on data connections too | DataConnection TLS | |
| R4217_9_Replies | RFC 4217 9 | MUST | Handle the PBSZ and PROT replies | Login flow | |
| R4217_10_3_ControlNegotiation | RFC 4217 10.3 | MUST | Follow the client side of the control connection negotiation | Login flow | |
| R4217_10_4_DataNegotiation | RFC 4217 10.4 | MUST | Follow the client side of the data connection negotiation | DataConnection TLS | |
| R4217_14_AborAndStatWithoutUrgent | RFC 4217 14 | MUST | Send ABOR and STAT without urgent data | Cancel and drain | |
| R4217_15_1_1_CertificateVerification | RFC 4217 15.1.1 | SHOULD | Verify the certificate of the server | Tls trust | |
| R4217_15_2_Considerations | RFC 4217 15.2 | SHOULD | Protect user names and passwords, resist bounce and port stealing | Credentials only after the trust decision | |
| R4217_15_3_CccIssues | RFC 4217 15.3 | SHOULD NOT | Do not drop back to plaintext on the control connection | The encoder has no CCC | |
| R2577_BounceAttack | RFC 2577 | MUST | Ignore the announced address of a passive reply | DataConnection ignores it | |
| R2577_PortTheft | RFC 2577 | SHOULD | Connect at once to the announced port, check the peer through TLS | DataConnection TLS | |
| R2577_NoPlaintextPasswords | RFC 2577 | MUST NOT | Never send a password in plaintext when TLS is required | Login flow | |
| MFXX_MfmtFormat | draft-somers-ftp-mfxx | MUST | Send MFMT with a time-val and read its 213 reply | Metadata operations | |
| DF_ImplicitFtps | De facto | MUST | Start TLS at once on port 990 | Login flow | |
| DF_OptsUtf8On | De facto | SHOULD | Send OPTS UTF8 ON and accept a refusal | Login flow | |
| DF_DataTlsSessionReuse | De facto | MUST | Resume the control session on data connections | DataConnection TLS | |
| DF_CloseNotifyBeforeFinalReply | De facto | MUST | Answer the close_notify of a download before waiting for the 226 | Transfer close rules | |
| DF_UploadCloseNotify | De facto | MUST | End an upload with close_notify, not FIN alone | Transfer close rules | |
| DF_AlpnFtp | De facto | MUST | Offer the ALPN protocol ftp or none | Tls context | |
| DF_AborReplyVariants | De facto | MUST | Accept every ABOR reply order seen on real servers | Cancel and drain | |
| DF_SizeInTypeImage | De facto | MUST | Send SIZE only in TYPE I | Metadata operations | |
