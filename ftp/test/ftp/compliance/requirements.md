# FTP client requirements

Every requirement on the client, from the RFCs and from what real servers do, and the tests that cover it as
`Suite.Test`. A requirement without tests has none yet. The How column says how the client meets it; one that does not
apply to this client starts with "Not applicable:", one the client deliberately does not follow starts with
"Deviation:", both with the reason. Keep the Tests column in step when a test covers a requirement or changes its name.

Levels are those of the RFC text. RFC 959, RFC 854 and RFC 2228 predate or do not use the RFC 2119 keywords, their
"must" and "should" are read as MUST and SHOULD. Where a row is about server behaviour the client has to cope with, it
carries the server's level ("a server MAY send X" is a MAY row "Accept X").

| ID | Source | Level | Obligation | How the client meets it | Tests |
|---|---|---|---|---|---|
| R854_Nvt_BareCrAsCrNul | RFC 854 NVT printer and keyboard | MUST | Send a CR that is not part of CRLF as CR NUL, never a bare CR | Command refuses CR in arguments, so no bare CR is sent | `CommandTests.LineBreaksAndNulInTheArgumentAreRefused` |
| R854_Nvt_StripNulAfterCr | RFC 854 NVT printer and keyboard | SHOULD | Drop a NUL that follows a CR in received reply text |  |  |
| R854_Synch_SynchAfterIp | RFC 854 Synch signal | MUST | Follow a Telnet IP with a Synch, the DM sent as the last octet of a TCP urgent send | Not applicable: the client sends no Telnet IP |  |
| R959_2_3_ControlOpenDuringTransfer | RFC 959 2.3 | MUST | Keep the control connection open while a data transfer runs |  |  |
| R959_3_1_1_3_ImageStoredAsReceived | RFC 959 3.1.1.3 | MUST | Store downloaded TYPE I data exactly as the octets arrived |  |  |
| R959_3_1_2_SameParametersBothWays | RFC 959 3.1.2 | MUST | Store and retrieve a file with the same TYPE, MODE and STRU when it must come back identical |  |  |
| R959_3_2_DefaultDataPorts | RFC 959 3.2, 3.3, 5.2 | MUST | Support transfers over the default data ports | Not applicable: passive mode only, active mode comes later |  |
| R959_3_2_ListenBeforeTransferCommand | RFC 959 3.2, 5.2 | MUST | Listen on the data port before sending the transfer command when the client is the passive side | Not applicable: passive mode only, the server listens |  |
| R959_3_3_DataConnectionManagement | RFC 959 3.3 | MUST | Treat the end of the data connection as the end of a stream mode transfer | Transfer ends on data EOF plus the final reply |  |
| R959_3_3_UploadEofByClose | RFC 959 3.3, 3.4.1 | MUST | Mark the end of a stream mode upload by closing the data connection |  |  |
| R959_3_3_DataConnectionPerTransfer | RFC 959 3.3, 3.4.1 | MUST | Establish a data connection for each transfer and close it after | DataConnection is created per transfer |  |
| R959_4_1_1_UserPass | RFC 959 4.1.1 | MUST | Send USER, then PASS after 331, ACCT after 332 | Login flow |  |
| R959_4_1_1_CwdCdup | RFC 959 4.1.1 | MUST | Change directories with CWD and CDUP and honour their replies | Metadata operations |  |
| R959_4_1_1_Quit | RFC 959 2.3, 4.1.1 | SHOULD | End a session with QUIT and wait for 221 within a bound | ControlConnection close |  |
| R959_4_1_1_HidePassword | RFC 959 4.1.1 | MUST | Mask the password in display and logs, the server cannot hide it | Command::toLogString() logs the verb alone, never an argument | `CommandTests.LogStringIsTheVerbAlone` |
| R959_4_1_1_AccountLater | RFC 959 4.1.1 | SHOULD | Answer a 332 to a command after login with ACCT, treat 532 as an account needed for that command |  |  |
| R959_4_1_1_CwdCdupReplies | RFC 959 4.1.1, Appendix II | MUST | Accept the same replies for CDUP as for CWD, 200 and 250 alike as success |  |  |
| R959_4_1_2_Pasv | RFC 959 4.1.2; RFC 2428 4 | MAY | Use PASV for a data connection when EPSV fails | DataConnection setup |  |
| R959_4_1_2_TypeImage | RFC 959 4.1.2 | MUST | Send TYPE I before binary transfers, the default is ASCII | Login flow sets TYPE I |  |
| R959_4_1_2_StruFile | RFC 959 4.1.2; client scope | MUST NOT | Never send another STRU than the default file structure | Verb has no STRU, the client keeps the default file structure | `CommandTests.CommandsTheClientMustNeverSendCannotBeEncoded` |
| R959_4_1_2_ModeStream | RFC 959 4.1.2; client scope | MUST NOT | Never send another MODE than the default stream mode | Verb has no MODE, the client keeps the default stream mode | `CommandTests.CommandsTheClientMustNeverSendCannotBeEncoded` |
| R959_4_1_2_ParametersBeforeService | RFC 959 4.1.2 | MUST | Send TYPE and the passive mode command before the service command they apply to |  |  |
| R959_4_1_3_RetrStor | RFC 959 4.1.3 | MUST | Follow the 1yz, data, 2yz sequence of RETR and STOR | Transfer flow |  |
| R959_4_1_3_Appe | RFC 959 4.1.3; RFC 1123 4.1.2.13 | MUST | Append with APPE where the file system asks for it | Transfer flow |  |
| R959_4_1_3_Rest | RFC 959 4.1.3 | MUST | Send REST immediately before the transfer it applies to | Transfer flow |  |
| R959_4_1_3_RnfrRnto | RFC 959 4.1.3 | MUST | Send RNTO right after a 350 reply to RNFR, with no command between | Metadata operations |  |
| R959_4_1_3_Abor | RFC 959 4.1.3, 5.4 | MUST | Handle 225 or 226 to ABOR, with a 426 first when a transfer was running | Cancel and drain |  |
| R959_4_1_3_DeleRmdMkd | RFC 959 4.1.3 | MUST | Remove and create with DELE, RMD and MKD and honour their replies | Metadata operations |  |
| R959_4_1_3_Pwd | RFC 959 4.1.3 | MUST | Read the current directory from the 257 reply of PWD | Metadata operations |  |
| R959_4_1_3_ListNlst | RFC 959 4.1.3 | MUST | Read LIST and NLST over a data connection | Listing flow |  |
| R959_4_1_3_ServerPathSyntax | RFC 959 4.1.3 | MUST | Write path arguments in the server's path conventions |  |  |
| R959_4_1_3_ListTypeAscii | RFC 959 4.1.3 | MUST | Set TYPE A or E before LIST and NLST | Not applicable: binary transfers only, RFC 1123 4.1.2.7 gives LIST and NLST an implied TYPE AN |  |
| R959_4_1_3_DeleteConfirmation | RFC 959 4.1.3 | SHOULD | Ask any extra delete confirmation in the client, the server asks none |  |  |
| R959_4_1_3_StatPathOnControl | RFC 959 4.1.3 | MUST | Read the output of STAT with a path from the control reply, no data connection is used |  |  |
| R959_4_1_3_TelnetSignalsBeforeAbor | RFC 959 4.1.3 | SHOULD | Precede ABOR, and STAT during a transfer, with Telnet IP and Synch | Deviation: ABOR goes in band without IP and Synch, TLS has no urgent data (RFC 4217 14) |  |
| R959_4_1_3_ListFormats | RFC 959 4.1.3 | SHOULD | Do not assume one format of LIST output | ListingParser::list() reads the Unix and the DOS format line by line, a line in neither is counted as unreadable | `ListParserTests.UnixLines`, `ListParserTests.DosLines`, `ListParserTests.LinesInNoFormatAreUnreadable`, `ListingSampleTests.VsftpdList` |
| R959_4_2_ReplyFormat | RFC 959 4.2 | MUST | A reply is three digits, a space or hyphen, text and CRLF | ReplyFramer, replyCodeOf | `ReplyFramerTests.SingleLineReply`, `ReplyFramerTests.CodeWithoutTextIsAReply`, `ReplyFramerTests.LinesThatStartNoReplyBreakTheProtocol`, `ReplyFramerTests.ReplyCodeOfALine` |
| R959_4_2_MultiLineContinuation | RFC 959 4.2 | MUST | A multi-line reply ends with a line of the same code and a space | ReplyFramer ends a multi-line reply on its own code and a space | `ReplyFramerTests.MultiLineReplyOfRfc959`, `ReplyFramerTests.MultiLineReplyWithTheCodeOnEveryLine`, `ReplyFramerTests.LineWithTheOpeningCodeAndAHyphenDoesNotEndTheReply`, `ReplyFramerTests.LineWithAnotherCodeFollowedByTextIsText`, `ReplyFramerTests.LineWithAnotherCodeFollowedByTheEndIsText` |
| R959_4_2_FirstDigitSemantics | RFC 959 4.2 | MUST | The first digit says whether the command succeeded, goes on or failed | classifyReply | `ReplyCodeTests.FirstDigitGivesTheClass`, `ReplyErrorTests.PositiveRepliesAreUnexpected` |
| R959_4_2_OneFinalReplyPerCommand | RFC 959 4.2 | MUST | Each command gets exactly one final reply, FIFO on the control connection | ControlConnection pending replies |  |
| R959_4_2_PreliminaryReplies | RFC 959 4.2 | MUST | Wait for the final reply behind a 1yz reply | ControlConnection pending replies |  |
| R959_4_2_NoCommandBeforeCompletion | RFC 959 4.2, 4.1.3 | MUST NOT | Send no other command between a 1yz and its completion reply, ABOR, STAT and QUIT excepted |  |  |
| R959_4_2_RestartSequenceAfterFailure | RFC 959 4.2 | MUST | After a failure inside USER, PASS, ACCT or RNFR, RNTO, restart the sequence from its first command |  |  |
| R959_4_2_SendInformationAfter3yz | RFC 959 4.2 | SHOULD | Answer a 3yz reply with the command that supplies the missing information |  |  |
| R959_4_2_RetryAfter4yz | RFC 959 4.2 | SHOULD | After a 4yz reply return to the start of the command sequence, a retry may succeed |  |  |
| R959_4_2_NoRepeatAfter5yz | RFC 959 4.2 | SHOULD NOT | Do not repeat the same request unchanged after a 5yz reply |  |  |
| R959_4_2_1_421AnyCommand | RFC 959 4.2.1 | MAY | Accept 421 as the reply to any command, followed by the server closing the connection | replyError maps 421 after a command to ConnectionLost | `ReplyErrorTests.IdleTimeoutIsALostConnection` |
| R959_5_2_TelnetConventions | RFC 959 5.2 | SHOULD | Follow the Telnet conventions on the control connection |  |  |
| R959_5_2_WaitBeforeNextTransfer | RFC 959 5.2 | SHOULD | Wait for the 226 or 250 of a transfer before the next transfer command |  |  |
| R959_5_2_DrainOnPeerClose | RFC 959 5.2 | SHOULD | When the peer closes a connection, read its remaining data at once and close this side |  |  |
| R959_5_3_CommandSyntax | RFC 959 5.3; RFC 2640 3.1 | MUST | Verb, exactly one space, argument, CRLF | Command::encode() | `CommandTests.VerbAndArgumentAreSeparatedByOneSpaceAndEndInCrLf`, `CommandTests.CommandWithoutArgumentIsTheVerbAlone`, `CommandTests.EveryVerbHasItsOwnWireName` |
| R959_5_3_NoLineEndsInArguments | RFC 959 5.3 | MUST NOT | Never put CR or LF into an argument | Command::encode() refuses CR, LF and NUL | `CommandTests.LineBreaksAndNulInTheArgumentAreRefused` |
| R959_5_4_CommandReplySequences | RFC 959 5.4 | MUST | Accept the replies listed for each command, judge any other by its first digit | replyError for every negative reply, the operations for the positive ones | `ReplyErrorTests.CodesOfRfc959`, `ReplyErrorTests.UnknownCodesUseTheirFirstDigit` |
| R959_5_4_WaitForPrimaryReply | RFC 959 5.4 | SHOULD | Wait for the primary reply to a command before sending the next one |  |  |
| R959_5_4_WaitForGreeting | RFC 959 5.4 | SHOULD | Wait for the 220 greeting before the first command |  |  |
| R959_5_4_DelayedGreeting | RFC 959 5.4 | SHOULD | After a 120 greeting keep waiting for the 220 instead of hanging up |  |  |
| R959_5_4_LoginWithoutPassword | RFC 959 5.4 | MUST | Accept 230 to USER, and 202 to PASS or ACCT, as a finished login |  |  |
| R959_AppendixII_257Quoting | RFC 959 Appendix II | SHOULD | Read a quoted path with doubled quotes from a 257 reply | parsePathReply | `ReplyParserTests.PathReply`, `ReplyParserTests.PathReplyUndoublesQuotes`, `ReplyParserTests.PathReplyWithoutAQuotedPathIsUnreadable` |
| R959_AppendixII_UseMkdReplyName | RFC 959 Appendix II | MUST | Refer to a new directory by the absolute name from the 257 reply to MKD |  |  |
| R959_AppendixII_MkdExistingDirectory | RFC 959 Appendix II | MUST | Treat the error reply to MKD of an existing directory as failure, whatever its code (521 is seen) | replyError reads 521 like 550, an "already exists" at the end of a line is AlreadyExists | `ReplyErrorTests.ExistingDirectoryOfRfc959AppendixII` |
| R1123_4_1_2_1_TypeLocal8 | RFC 1123 4.1.2.1, 4.1.5 | MUST | Support TYPE L 8 next to TYPE I | Deviation: TYPE I does the same on 8-bit hosts and every server has it |  |
| R1123_4_1_2_1_TypeLocalM | RFC 1123 4.1.2.1, 4.1.5 | MAY | Support TYPE L m on a host with m-bit words | Not applicable: 8-bit host, TYPE I only |  |
| R1123_4_1_2_2_TypeTelnetFormat | RFC 1123 4.1.2.2, 4.1.5 | SHOULD | Implement TYPE A T as TYPE A N where the host makes no difference | Not applicable: binary transfers only, TYPE A may come later |  |
| R1123_4_1_2_3_NoPageStructure | RFC 1123 4.1.2.3, 4.1.5 | SHOULD NOT | Do not implement page structure |  |  |
| R1123_4_1_2_4_InvertibleTransform | RFC 1123 4.1.2.4, 4.1.5 | SHOULD | Keep record to file structure transformations invertible | Not applicable: file structure only |  |
| R1123_4_1_2_5_PortBeforeEachTransfer | RFC 1123 4.1.2.5, 4.1.5 | SHOULD | In stream mode send PORT with a non-default port before each transfer command | Not applicable: passive mode only, active mode comes later |  |
| R1123_4_1_2_6_PasvReplyFormat | RFC 1123 4.1.2.6 | MUST | Scan the 227 text for the six numbers, parentheses may be missing | parsePassiveReply | `ReplyParserTests.PassiveReplyWithParentheses`, `ReplyParserTests.PassiveReplyWithoutParentheses`, `ReplyParserTests.PassiveReplyScansPastOtherNumbers`, `ReplyParserTests.PassiveReplyThatNamesNoEndpointIsUnreadable` |
| R1123_4_1_2_6_PasvPerTransfer | RFC 1123 4.1.2.6 | MUST | Send PASV for every transfer | DataConnection setup |  |
| R1123_4_1_2_7_NlstNamesUsable | RFC 1123 4.1.2.7, 4.1.5 | MUST | Use NLST names as they are in later transfer commands | ListingParser::nameList() keeps the bytes of each name, without the listed path some servers put before it | `ListingSampleTests.ProftpdNameListLosesTheListedPath`, `ListingSampleTests.PyftpdlibNameList`, `NameListTests.NamesAsSent` |
| R1123_4_1_2_7_ListImpliedAscii | RFC 1123 4.1.2.7, 4.1.5 | SHOULD | Read LIST and NLST data as ASCII lines ending in CRLF, whatever TYPE is set | ListingParser ends a line at LF with or without the CR before it | `ListingParserTests.LineEndsOfAnyKind` |
| R1123_4_1_2_9_StouFileName | RFC 1123 4.1.2.9, 4.1.5 | MUST | Read the unique name of STOU from a "125 FILE:" or "150 FILE:" reply | Not applicable: the client never sends STOU |  |
| R1123_4_1_2_10_NoReadBoundaries | RFC 1123 4.1.2.10 | MUST NOT | Do not rely on TCP read boundaries on the control connection | ReplyFramer takes bytes in any pieces | `ReplyFramerTests.ReadBoundariesDoNotMatter`, `ReplyFramerTests.ReadBoundariesDoNotMatterForTelnetCommandsAndBareLineFeeds`, `ReplyFramerTests.ReadBoundariesDoNotMatterForAPossibleEnd`, `ReplyFramerTests.SeveralRepliesInOnePiece` |
| R1123_4_1_2_11_MultiLineReplies | RFC 1123 4.1.2.11 | MUST | Handle multi-line replies for every command | ReplyFramer | `ReplyFramerTests.MultiLineReplyOfRfc959`, `ReplyFramerTests.PureFtpdGreeting` |
| R1123_4_1_2_11_UnknownCodesUseTheHighDigit | RFC 1123 4.1.2.11 | SHOULD | Base procedural decisions on the first digit of a reply code, unknown codes included | classifyReply, replyError | `ReplyCodeTests.UnknownCodesUseTheHighDigit`, `ReplyErrorTests.UnknownCodesUseTheirFirstDigit` |
| R1123_4_1_2_11_No421Special | RFC 1123 4.1.2.11 | SHOULD NOT | Do not handle 421 differently from a closed connection | Reply mapping |  |
| R1123_4_1_2_11_RecoverFromLineLimit | RFC 1123 4.1.2.11 | MUST | When a multi-line reply exceeds a line limit, skip the excess lines to its end and recover | ReplyFramer skips lines past its limits up to the end line | `ReplyFramerTests.LinesPastTheLineCountAreSkippedUpToTheEnd`, `ReplyFramerTests.LinesPastTheReplySizeAreSkippedUpToTheEnd`, `ReplyFramerTests.EndlessMultiLineReplyKeepsItsMemoryBounded` |
| R1123_4_1_2_11_DetectServerClose | RFC 1123 4.1.2.11 | SHOULD | Detect the server closing the control connection |  |  |
| R1123_4_1_2_12_NoTelnetNegotiation | RFC 1123 4.1.2.12 | MUST NOT | Send no Telnet commands other than Synch and IP, negotiate no options | Nothing answers Telnet negotiation, the framer drops it | `ReplyFramerTests.TelnetNegotiationIsRemoved`, `CommandTests.InterpretAsCommandInTheArgumentIsDoubled` |
| R1123_4_1_2_13_TypeAscii | RFC 1123 4.1.2.13, 4.1.5; RFC 959 3.1.1.1 | MUST | Support TYPE A N, the default type | Not applicable: binary transfers only, TYPE A may come later |  |
| R1123_4_1_2_13_RecordStructure | RFC 1123 4.1.2.13, 4.1.5; RFC 959 3.1.2.2 | MUST | Support record structure on a host whose file system has records | Not applicable: file structure only, the host has no record files |  |
| R1123_4_1_2_13_ModeStruCommands | RFC 1123 4.1.2.13, 4.1.5 | MUST | Support the MODE and STRU commands for MODE S and STRU F | Not applicable: stream mode and file structure only, the defaults need no command |  |
| R1123_4_1_2_13_PortCommand | RFC 1123 4.1.2.13, 4.1.5 | MUST | Support the PORT command | Not applicable: active mode comes later |  |
| R1123_4_1_2_13_SystHelp | RFC 1123 4.1.2.13, 4.1.5 | MUST | Support the SYST and HELP commands | Deviation: the client is a file manager, not an interactive FTP program, nothing in it needs SYST or HELP |  |
| R1123_4_1_2_13_MinimumCommands | RFC 1123 4.1.2.13, 4.1.5 | MUST | Support USER, PASS, ACCT, PASV, TYPE, RETR, STOR, APPE, RNFR, RNTO, DELE, CWD, CDUP, RMD, MKD, PWD, LIST, NLST, STAT, NOOP and QUIT |  |  |
| R1123_4_1_3_1_ExperimentalDirectoryCommands | RFC 1123 4.1.3.1, 4.1.5 | SHOULD | Retry MKD, RMD, PWD, CDUP and CWD as XMKD, XRMD, XPWD, XCUP and XCWD after 500 or 502 | Deviation: the client sends only MKD, RMD, PWD, CDUP and CWD, which the servers of the last decades know |  |
| R1123_4_1_3_2_ReplyTimeouts | RFC 1123 4.1.3.2 | MUST | Time out waiting for replies, as a client driven by a program |  |  |
| R1123_4_1_3_4_AsyncRestartMarkers | RFC 1123 4.1.3.4 | MUST NOT | Do not assume that 110 restart markers are synchronous | Not applicable: the MUST NOT binds a sender of restart markers, stream mode has none |  |
| R1123_4_1_3_4_CheckpointAtMarker | RFC 1123 4.1.3.4, 4.1.5 | SHOULD | Write data to stable storage at a received restart marker before recording its position | Not applicable: stream mode has no restart markers |  |
| R1123_4_1_3_4_RestReplies554And555 | RFC 1123 4.1.3.4 | MAY | Map 554 (bad REST position) and 555 (type or structure mismatch) on the command after REST, and 555 on APPE |  |  |
| R1123_4_1_4_1_ArbitraryPathnames | RFC 1123 4.1.4.1, 4.1.5 | MUST | Accept remote path names as arbitrary strings of any length, free of local conventions |  |  |
| R1123_4_1_4_2_QuoteCommand | RFC 1123 4.1.4.2, 4.1.5 | MUST | Offer a QUOTE command that sends any string and shows every reply | Deviation for now: a command to send raw lines is planned for later |  |
| R1123_4_1_4_2_SendCommandsAtOnce | RFC 1123 4.1.4.2, 4.1.5 | SHOULD | Send transfer control commands when the user gives them, not batched until a transfer |  |  |
| R1123_4_1_4_3_ShowErrorText | RFC 1123 4.1.4.3, 4.1.5 | SHOULD | Show the user the full text of every error reply | replyError keeps the whole reply text in the message | `ReplyErrorTests.ErrorCarriesCodeAndTextOfTheReply` |
| R1123_4_1_4_3_VerboseMode | RFC 1123 4.1.4.3, 4.1.5 | SHOULD | Offer a verbose mode with all commands sent and all replies received |  |  |
| R1123_4_1_4_4_Synchronization | RFC 1123 4.1.4.4 | SHOULD | Be forgiving of missing and unexpected replies to stay in step | ControlConnection marks itself dead |  |
| R1123_4_1_5_OptionalCommandsUsed | RFC 1123 4.1.5 | MAY | Support REST, ABOR and SITE |  |  |
| R1123_4_1_5_OptionalCommandsUnused | RFC 1123 4.1.5 | MAY | Support SMNT, REIN, STOU and ALLO | Not applicable: the client never sends them |  |
| R1123_4_1_5_OptionalTypesAndModes | RFC 1123 4.1.5 | MAY | Support TYPE A C, TYPE E and MODE B | Not applicable: TYPE I and stream mode only |  |
| R2228_2_TryOtherMechanisms | RFC 2228 2 | MAY | Try another security mechanism after AUTH is refused | Not applicable: TLS is the only mechanism used |  |
| R2228_2_IntegrityProtectedCommands | RFC 2228 2, 3 | MUST | With an integrity mechanism, send every command as MIC or ENC until CCC succeeds | Not applicable: TLS protects the control connection, MIC, CONF and ENC are not used |  |
| R2228_3_ReplyFirstDigit | RFC 2228 3 | MUST | Judge security replies by their first digit, servers may substitute codes that differ in the last digit |  |  |
| R2228_3_ReauthorizeAfterAuth | RFC 2228 3; RFC 4217 4.2 | MUST | After an accepted AUTH send USER, PASS and ACCT again as needed |  |  |
| R2228_3_AdatAfterAuth | RFC 2228 3 | MUST | Send ADAT only after a successful AUTH and before the exchange completes | Not applicable: AUTH TLS has no ADAT exchange |  |
| R2228_3_ResetStateOnAdatError | RFC 2228 3 | MUST | Reset the internal security state when ADAT fails | Not applicable: AUTH TLS has no ADAT exchange |  |
| R2228_3_ResetServerStateOutOfStep | RFC 2228 3 | MUST | Reset the server security state when the security exchange gets out of step | Not applicable: AUTH TLS has no ADAT exchange |  |
| R2228_3_PbszAfterSecurityExchange | RFC 2228 3; RFC 4217 9 | MUST | Send PBSZ only after a successful security exchange, the TLS handshake |  |  |
| R2228_3_PbszFits32Bits | RFC 2228 3, 8.1 | MUST | Send a PBSZ value that fits an unsigned 32-bit integer |  |  |
| R2228_3_PbszSmallerSize | RFC 2228 3 | MUST | Read a "PBSZ=" value in the 200 reply and use the smaller of both sizes |  |  |
| R2228_4_UserReply232 | RFC 2228 4; RFC 4217 12.2 | SHOULD | Accept 232 to USER as logged in through the security exchange, send no PASS |  |  |
| R2228_4_Show336Challenge | RFC 2228 4 | MUST | On 336 to USER show the challenge before asking for the password |  |  |
| R2228_4_NoPasswordPromptBeforeUser | RFC 2228 4 | SHOULD | Prompt for the password only after USER has been sent |  |  |
| R2228_5_1_Reply234StartsTls | RFC 2228 3, 5.1; RFC 4217 4.2 | MUST | Start the TLS handshake only on 234 to AUTH TLS, 334 is wrong for TLS |  |  |
| R2228_5_1_AdatReplies | RFC 2228 3, 5.1 | MUST | Handle 235 and 335 to ADAT | Not applicable: AUTH TLS has no ADAT exchange |  |
| R2228_5_1_Reply431 | RFC 2228 3, 5.1 | SHOULD | Treat 431 to AUTH or PROT as a resource the server lacks for now | replyError | `ReplyErrorTests.CodesOfTheSecurityExtensions` |
| R2228_5_1_Reply533 | RFC 2228 3, 5.1, 8.2 | SHOULD | Treat 533 as a command protection level denied by policy | replyError | `ReplyErrorTests.CodesOfTheSecurityExtensions` |
| R2228_5_1_Reply537 | RFC 2228 3, 5.1 | SHOULD | Treat 537 as a command protection the mechanism does not support | Not applicable: MIC, CONF and ENC are not used |  |
| R2228_5_2_ProtectedReplies | RFC 2228 5.2, 8.2 | MUST | Decode and verify 631, 632 and 633 replies, line by line when multi-line | Not applicable: TLS protects the control connection, no 63z replies |  |
| R2228_6_Reply534OnTransfer | RFC 2228 6, 8.2 | MUST | Treat 534 to STOR, RETR, LIST, NLST or APPE as a data protection level the server refuses |  |  |
| R2228_6_Reply535OnTransfer | RFC 2228 6, 8.2 | MUST | Treat 535 to a transfer command as a failed data protection check |  |  |
| R2228_6_ProtectAfterTransforms | RFC 2228 6 | MUST | Apply data protection after the type, structure and mode transformations |  |  |
| R2228_6_BlockEncapsulation | RFC 2228 6 | MUST | Send protected data in length-prefixed blocks no larger than PBSZ, ending with an empty block | Not applicable: RFC 4217 9 uses PBSZ 0 and no encapsulation with TLS |  |
| R2228_6_ReadEncapsulatedBlocks | RFC 2228 6 | MUST | Read length-prefixed protected blocks until an empty one | Not applicable: RFC 4217 9 uses PBSZ 0 and no encapsulation with TLS |  |
| R2228_7_EncryptPass | RFC 2228 7; RFC 7151 4 | SHOULD | Send PASS encrypted whenever possible |  |  |
| R2228_8_2_PbszReplies | RFC 2228 8.2 | MUST | Handle 200, 503, 500, 501, 421 and 530 to PBSZ |  |  |
| R2228_8_2_SecurityCommandErrors | RFC 2228 8.2 | MUST | Accept 500, 501, 502, 533 and 421 to any security command |  |  |
| R2228_10_Base64CaseSensitive | RFC 2228 10 | MUST NOT | Never ignore case in base64 data of ADAT, MIC, CONF, ENC and 63z replies | Not applicable: AUTH TLS uses no base64 exchange |  |
| R2228_AppendixI_Gssapi | RFC 2228 Appendix I | MUST | Follow the GSSAPI client exchange | Not applicable: TLS is the only mechanism used |  |
| R2228_AppendixII_KerberosV4 | RFC 2228 Appendix II | MUST | Follow the KERBEROS_V4 client exchange | Not applicable: TLS is the only mechanism used |  |
| R2228_AuthReplies | RFC 2228 3, 8.2 | MUST | Handle 234, 334, 431, 500, 501, 502, 504, 534 and 421 to AUTH | Login flow |  |
| R2228_PbszBeforeProt | RFC 2228 3; RFC 4217 9 | MUST | Send PBSZ before PROT | Login flow |  |
| R2228_ProtReplies | RFC 2228 3, 8.2 | MUST | Handle 200, 504, 536, 503, 534, 431, 500, 501, 421 and 530 to PROT | Login flow |  |
| R2228_CccNotSent | RFC 2228 7 | SHOULD NOT | Never send CCC | Verb has no CCC | `CommandTests.CommandsTheClientMustNeverSendCannotBeEncoded` |
| R2389_2_2_AnyReplyMultiLine | RFC 2389 2.2; RFC 3659 2.4; RFC 7151 2.2 | SHOULD | Accept a multi-line reply to every command, QUIT and extension commands included | ReplyFramer frames every reply the same way | `ReplyFramerTests.MultiLineReplyWithTheCodeOnEveryLine` |
| R2389_3_UnknownFeatures | RFC 2389 3 | MUST | Accept unknown features in a FEAT reply without error | Features keeps every line | `FeaturesTests.ProftpdFeatures` |
| R2389_3_2_SingleLine211 | RFC 2389 3.2 | SHOULD | Treat a single-line 211 reply to FEAT as a server with no extensions | Features::fromReply | `FeaturesTests.SingleLineReplyListsNothing` |
| R2389_3_2_FirstLineText | RFC 2389 3.2 | MUST | Ignore the text of the first FEAT reply line | Features::fromReply reads from the second line | `FeaturesTests.PureFtpdFeatures` |
| R2389_3_2_FeatureLineSyntax | RFC 2389 3.2 | MUST | Read each feature line as a label and optional parameters after the single leading space | Features::fromReply | `FeaturesTests.ExtraSpacesAroundAFeatureAreDropped`, `FeaturesTests.LinesWithoutALeadingSpaceAreNoFeatures`, `FeaturesTests.NamesAndParametersCompareInAnyCase` |
| R2389_3_2_FeatureOrder | RFC 2389 3.2 | MAY | Do not depend on the order of feature lines, it may change between replies |  |  |
| R2389_3_2_OnlyListedExtensions | RFC 2389 3.2 | MAY | Assume the server supports only the extensions FEAT lists | Deviation: FEAT alone decides nothing, vsftpd leaves out AUTH TLS and UTF8, MLSD is probed once |  |
| R2389_3_2_ProbeAfterFeatError | RFC 2389 3.2 | SHOULD | After an error reply to FEAT test extensions one by one |  |  |
| R2389_4_OptsReplies | RFC 2389 4 | MUST | Read 200 to OPTS as accepted, 451 as temporary, 501 as permanent, 500 or 502 as no OPTS |  |  |
| R2389_4_OptsWithFeat | RFC 2389 4 | MUST | Assume OPTS exists when the server supports FEAT |  |  |
| R2389_FeatFormat | RFC 2389 | MUST | Read 211 multi-line replies with one leading space per feature | Features::fromReply | `FeaturesTests.VsftpdFeatures`, `FeaturesTests.ProftpdFeatures`, `FeaturesTests.PureFtpdFeatures` |
| R2389_NoFeat | RFC 2389 3.2 | MUST | Treat 500 or 502 to FEAT as no FEAT, older extensions may still exist | Features::fromReply | `FeaturesTests.NoFeatCommandMeansNoFeatures` |
| R2389_OptsSyntax | RFC 2389 | MUST | Send OPTS with the command name and its options | Command encoding |  |
| R2428_2_EprtSyntax | RFC 2428 2 | MUST | Send EPRT with one space, a delimiter from ASCII 33 to 126, AF number, address and port | Not applicable: active mode comes later |  |
| R2428_2_EprtPipeDelimiter | RFC 2428 2 | SHOULD | Use the pipe character (ASCII 124) as EPRT delimiter unless the address contains it | Not applicable: active mode comes later |  |
| R2428_2_EprtAddressFormat | RFC 2428 2 | MUST | Give the EPRT address in dotted decimal for AF 1 and IPv6 text form for AF 2 | Not applicable: active mode comes later |  |
| R2428_3_EpsvReply229 | RFC 2428 3 | MUST | Accept only 229 as the success reply to EPSV | parseExtendedPassiveReply | `ReplyParserTests.UnexpectedPositiveReplyBreaksTheProtocol` |
| R2428_3_EpsvControlAddress | RFC 2428 3 | MUST | Connect the EPSV data connection to the address and network protocol of the control connection |  |  |
| R2428_3_EpsvWithProtocol | RFC 2428 3 | MAY | Ask for a network protocol with EPSV and an AF number | Not applicable: EPSV is sent without an argument |  |
| R2428_3_AborOnUnusableProtocol | RFC 2428 3 | MUST | Send ABOR when EPSV yields a protocol the connecting host cannot use | Not applicable: only for proxy transfers, which the client does not do |  |
| R2428_4_OnlyEpsvAfterEpsvAll | RFC 2428 3, 4 | MUST | After EPSV ALL use no other data connection command | Not applicable: the client never sends EPSV ALL |  |
| R2428_4_NewSessionAfterEpsvAll | RFC 2428 4 | MUST | Start a new session for a three-way transfer after EPSV ALL | Not applicable: the client never sends EPSV ALL |  |
| R2428_4_EpsvAll | RFC 2428 4 | SHOULD | Send EPSV ALL as soon as possible when only two-way transfers follow | Deviation: never sent, it would rule out the PASV fallback for the rest of the session | `CommandTests.ExtendedPassiveTakesNoArgument` |
| R2428_4_UseEpsv | RFC 2428 4 | MUST | Use EPSV for transfers between the two hosts, over IPv4 and IPv6 | DataConnection setup tries EPSV first, PASV only as a fallback on IPv4 |  |
| R2428_EpsvFormat | RFC 2428 2, 3 | MUST | Read the 229 format with any delimiter | parseExtendedPassiveReply | `ReplyParserTests.ExtendedPassiveReply`, `ReplyParserTests.ExtendedPassiveReplyScansPastOtherParentheses`, `ReplyParserTests.ExtendedPassiveReplyThatNamesNoPortIsUnreadable` |
| R2428_522Handling | RFC 2428 2, 3 | MUST | Fall back to the protocols a 522 names, for EPRT | Not applicable: neither EPRT nor EPSV with an argument is sent |  |
| R2577_4_VerifyDataPeer | RFC 2577 4 | SHOULD | Check that a data connection accepted on a listening port comes from the expected server | Not applicable: active mode comes later |  |
| R2577_5_FailedPassDelay | RFC 2577 5 | SHOULD | Allow several seconds for the reply to a wrong PASS |  |  |
| R2577_5_421AfterFailedLogins | RFC 2577 5 | MUST | Expect 421 and a closed connection after a few failed PASS attempts |  |  |
| R2577_7_UniformUserReply | RFC 2577 7 | SHOULD | Expect 331 to any user name, an unknown user then fails only at PASS |  |  |
| R2577_PortTheft | RFC 2577 8 | SHOULD | Use random local ports for data connections | The operating system picks the local port of every data connection |  |
| R2577_NoPlaintextPasswords | RFC 2577 5, 6; RFC 2228 7 | SHOULD | Encrypt the password whenever possible, never send it in plaintext when the session asks for TLS | Login flow |  |
| R2640_3_1_Utf8Support | RFC 2640 3.1 | MUST | Support UTF-8 path names in commands and replies |  |  |
| R2640_3_1_SingleSpaceBeforePathname | RFC 2640 3.1 | MUST | Put exactly one space between the command and the path name | Command::encode() | `CommandTests.VerbAndArgumentAreSeparatedByOneSpaceAndEndInCrLf`, `CommandTests.ArgumentIsSentAsItIs` |
| R2640_3_1_SpacesBelongToPathname | RFC 2640 3.1 | MUST | Treat spaces after the first separator as part of the path name, never trim them | Command::encode() sends the argument as it is | `CommandTests.ArgumentIsSentAsItIs` |
| R2640_3_1_NoPaddedCommands | RFC 2640 3.1 | MUST NOT | Do not pad three-letter commands such as CWD or MKD to four characters with a space | Command::encode() | `CommandTests.CommandWithoutArgumentIsTheVerbAlone`, `CommandTests.ArgumentIsSentAsItIs` |
| R2640_3_1_StripNulOnReceipt | RFC 2640 3.1 | MUST | Remove the NUL from a CR NUL pair in received path names | parsePathReply | `ReplyParserTests.PathReplyReadsCarriageReturnNulAsCarriageReturn` |
| R2640_3_1_OtherEncodingChoice | RFC 2640 3.1 | MAY | Let users choose another path name encoding as well |  |  |
| R2640_3_1_KeepUtf8WhenUnconvertible | RFC 2640 3.1 | SHOULD | Keep UTF-8 path names that cannot be converted to a usable local code set |  |  |
| R2640_3_1_RawBytesWhenUnknown | RFC 2640 3.1; RFC 3659 7.3 | SHOULD | Use the raw bytes of a path name whose encoding cannot be determined | ListingEntry::name holds the bytes the server sent | `MachineListingTests.NamesAreBytes` |
| R2640_3_3_DisplayUtf8 | RFC 2640 3.3 | SHOULD | Parse UTF-8 path names correctly and try to display them |  |  |
| R2640_3_3_FeatUtf8 | RFC 2640 3.2, 3.3 | MUST | Learn UTF-8 support from FEAT and its UTF8 feature, matched in any case | Features::has | `FeaturesTests.PureFtpdFeatures` |
| R2640_3_3_NonUtf8ServerDisplay | RFC 2640 3.3 | SHOULD | Adapt the display when the server is not UTF-8 |  |  |
| R2640_3_3_NonUtf8Assumptions | RFC 2640 3.3 | MAY | For a server that is not UTF-8, assume another encoding, ask the user, or remember it per server |  |  |
| R2640_3_3_PercentEncodeUndisplayable | RFC 2640 3.3 | SHOULD | Show octets of undisplayable characters as %HH |  |  |
| R2640_3_3_LocalCharsetForNonUtf8 | RFC 2640 3.3 | MAY | Read path names that are not valid UTF-8 in the local character set |  |  |
| R2640_4_ProtocolElements | RFC 2640 4 | SHOULD | Treat commands and reply codes as protocol elements that no language affects |  |  |
| R2640_4_LangCommand | RFC 2640 4, 4.2 | MUST | Support the LANG command | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_1_HostOrLangFirst | RFC 2640 4.1 | MUST | Send HOST or LANG before any other command except FEAT | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_1_LangWithoutHost | RFC 2640 4.1 | MUST | Send LANG when HOST is not supported | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_1_LangBeforeLogin | RFC 2640 4.1 | SHOULD | Send LANG before authentication | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_1_FeatBeforeLang | RFC 2640 4.1 | SHOULD | Send FEAT before LANG to learn the offered languages | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_1_Utf8ReplyText | RFC 2640 4.1 | SHOULD | Decode reply text as UTF-8 in the negotiated language | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_2_LangRefused | RFC 2640 4.2 | SHOULD | Expect 500 or 502 to LANG from older servers | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_4_3_LangFeature | RFC 2640 4.3 | MUST | Read the LANG feature as tags separated by ";", a "*" marking the current one | Deviation: LANG only changes the language of reply texts and few servers have it, the client sends none |  |
| R2640_A_1_EightBitSafe | RFC 2640 A.1 | SHOULD | Keep path name handling 8-bit safe, no NUL termination or use of the high bit |  |  |
| R2640_A_1_MixedDirections | RFC 2640 A.1 | SHOULD | Be ready to display path names that mix scripts and writing directions |  |  |
| R2640_Utf8Pathnames | RFC 2640 2, 3.1 | SHOULD | Send path names as UTF-8 | Arguments are sent as the bytes they are | `CommandTests.ArgumentIsSentAsItIs` |
| R2640_ValidityCheck | RFC 2640 3.1 | MUST | Check received names for valid UTF-8 | Listing parsers keep bytes and flag invalid names |  |
| R2640_CrNulInPathnames | RFC 2640 3.1 | MUST | Treat a CR in a path name as CR NUL on the wire | Deviation: names with CR or LF are refused before sending, a received CR NUL becomes CR | `CommandTests.LineBreaksAndNulInTheArgumentAreRefused`, `ReplyParserTests.PathReplyReadsCarriageReturnNulAsCarriageReturn` |
| R3659_2_2_RetransmitIdenticalPathname | RFC 3659 2.2 | SHOULD | Keep received path names byte-exact so they can be sent back unchanged |  |  |
| R3659_2_2_NoLossyCharsetRoundTrip | RFC 3659 2.2 | SHOULD NOT | Do not convert UTF-8 path names to a narrower local charset and back |  |  |
| R3659_2_2_SpacesInReplyPathnames | RFC 3659 2.2 | MUST | Treat every space after the one separator, trailing spaces included, as part of a path name in replies |  |  |
| R3659_2_2_SupportRfc2640 | RFC 3659 2.2 | MUST | Support RFC 2640 when using MLST | UTF-8 path names as RFC 2640 3 asks, no LANG (R2640_4_LangCommand) |  |
| R3659_2_2_EscapeIac | RFC 3659 2.2 | MUST | Send a 0xFF octet of a path name on the control connection as IAC IAC | Command::encode() | `CommandTests.InterpretAsCommandInTheArgumentIsDoubled` |
| R3659_2_2_UnescapeIac | RFC 3659 2.2 | MUST | Read IAC IAC in path names of control replies as one 0xFF octet | ReplyFramer | `ReplyFramerTests.EscapedInterpretAsCommandIsOneByte` |
| R3659_2_2_2_LiteralPathnames | RFC 3659 2.2.2 | MUST | Send literal path names to SIZE, MDTM, MLST and MLSD, match patterns on a listing in the client |  |  |
| R3659_2_3_LeapSecond | RFC 3659 2.3 | MAY | Accept 60 as the seconds of a time-val | parseTimeVal | `ReplyParserTests.LeapSecondIsTheNextMinute` |
| R3659_2_3_FractionPrecision | RFC 3659 2.3 | MAY | Accept time-val fractions with any number of digits | parseTimeVal keeps microseconds | `ReplyParserTests.TimeValWithFractions` |
| R3659_2_3_NoAddedPrecision | RFC 3659 2.3; draft-somers-ftp-mfxx-04 2.3 | MUST NOT | Send no more fraction digits in a time-val than the local time really has | formatTimeVal writes whole seconds | `ReplyParserTests.TimeValIsWrittenInWholeSeconds` |
| R3659_2_3_NoBarePeriod | RFC 3659 2.3 | MUST NOT | Send no "." in a time-val without a digit after it | formatTimeVal writes no fraction | `ReplyParserTests.TimeValIsWrittenInWholeSeconds` |
| R3659_2_3_SendUtc | RFC 3659 2.3; draft-somers-ftp-mfxx-04 2.3 | MUST | Send time-vals in UTC and the Gregorian calendar | formatTimeVal takes sys_seconds | `ReplyParserTests.TimeValIsWrittenInWholeSeconds` |
| R3659_3_PreferModifyFact | RFC 3659 3 | SHOULD | Prefer the modify fact of MLST over MDTM where available |  |  |
| R3659_3_CompareTimeBeforeRetrRestart | RFC 3659 3 | SHOULD | Before restarting a RETR, compare the server time read before the first RETR with the current one, do not restart if they differ |  |  |
| R3659_3_AllowClockSkew | RFC 3659 3 | SHOULD | Allow for clock skew when comparing server and local times |  |  |
| R3659_3_CheckBeforeStorRestart | RFC 3659 3 | MAY | Before restarting a STOR, compare the MDTM of the remote part with the local file time |  |  |
| R3659_3_1_UnretrievableObjectTime | RFC 3659 3.1 | MAY | Do not trust an MDTM time for an object that cannot be retrieved, it may be unspecified |  |  |
| R3659_3_2_MdtmErrors | RFC 3659 3.2 | SHOULD | Map 550 to MDTM as no time available, 500 and 501 as syntax errors, 4xy as temporary | parseModificationTimeReply, replyError | `ReplyParserTests.ModificationTimeReply` |
| R3659_3_3_FeatMdtm | RFC 3659 3.3 | MUST | Learn MDTM support from a FEAT line "MDTM" in any case | Features::has | `FeaturesTests.NamesAndParametersCompareInAnyCase` |
| R3659_4_SizeIsExact | RFC 3659 4 | MUST | Rely on SIZE as the exact octet count for the current TYPE, servers send no estimates | parseSizeReply reads the 213 reply to SIZE | `ReplyParserTests.SizeReply`, `ReplyParserTests.SizeReplyThatIsNoSizeIsUnreadable` |
| R3659_4_2_Size550NotFinal | RFC 3659 4.2 | MUST NOT | Do not take 550 to SIZE as proof that the file cannot be transferred |  |  |
| R3659_4_2_SizeErrors | RFC 3659 4.2 | SHOULD | Map 550, 500, 501 and 4xy replies to SIZE | parseSizeReply, replyError | `ReplyParserTests.SizeReplyOfAFailureIsItsError` |
| R3659_4_3_FeatSize | RFC 3659 4.3 | MUST | Learn SIZE support from a FEAT line "SIZE" in any case |  |  |
| R3659_4_4_SizeOnlyForPrecision | RFC 3659 4.4 | SHOULD NOT | Send SIZE only when the exact size matters, such as a restart |  |  |
| R3659_5_1_RetrRestartOffset | RFC 3659 5.1, 5.3 | MUST | For a RETR restart, send the number of octets already stored locally as the REST value |  |  |
| R3659_5_1_StorRestartSize | RFC 3659 5.1 | MUST | For a STOR restart, learn the stored size with SIZE first |  |  |
| R3659_5_3_RestSyntax | RFC 3659 5.3 | MUST | Send REST in stream mode with a decimal octet count only |  |  |
| R3659_5_3_RestWithRetrOrStor | RFC 3659 5.3 | SHOULD | Follow REST with RETR or STOR, not APPE or STOU |  |  |
| R3659_5_3_StorRestartAtServerSize | RFC 3659 5.3 | MUST | Restart a STOR only at the size the server reports, and send data at least up to the old size |  |  |
| R3659_5_3_MergeRetrData | RFC 3659 5.3 | MUST | Write restarted RETR data at the REST offset of the local partial file |  |  |
| R3659_5_3_ResendRest | RFC 3659 5.3 | SHOULD | Send REST again when the transfer command after it was not sent successfully |  |  |
| R3659_5_3_Rest0 | RFC 3659 5.3 | SHOULD | Send REST 0 when a pending REST must not apply to the next transfer |  |  |
| R3659_5_3_RestErrorMeansUnsupported | RFC 3659 5.3 | MUST | Read an error reply to REST itself as no restart support or a malformed value |  |  |
| R3659_5_3_RangeErrorsOnTransfer | RFC 3659 5.3 | SHOULD | Expect a bad restart position to fail the following transfer command, not REST |  |  |
| R3659_5_4_FeatRestStream | RFC 3659 5.4 | MUST | Learn stream restart support from a FEAT line "REST STREAM" in any case | Features::has("REST", "STREAM") | `FeaturesTests.VsftpdFeatures`, `FeaturesTests.NamesAndParametersCompareInAnyCase` |
| R3659_6_1_NoSlashInNames | RFC 3659 6.1 | MUST | With TVFS, split path names at "/", no file name contains one |  |  |
| R3659_6_2_RootNotCombined | RFC 3659 6.2 | MUST NOT | Never join the root "/" with a name into a path holding "//" |  |  |
| R3659_6_2_1_NoDoubleSlashMeaning | RFC 3659 6.2.1 | SHOULD NOT | Assume no meaning for "//" in path names |  |  |
| R3659_6_2_1_NoTrailingSlash | RFC 3659 6.2.1 | SHOULD NOT | Send no path ending in "/" other than the root, its meaning is undefined |  |  |
| R3659_6_2_1_CaseSignificant | RFC 3659 6.2.1 | SHOULD | Treat the case of names as significant unless known otherwise |  |  |
| R3659_6_2_1_NoMagicNames | RFC 3659 6.2.1 | SHOULD NOT | Do not rely on ".", ".." or drive names in TVFS paths, none are defined |  |  |
| R3659_6_3_FeatTvfs | RFC 3659 6.3 | MUST | Learn TVFS support from a FEAT line starting with "TVFS" in any case |  |  |
| R3659_6_3_IgnoreTvfsSuffix | RFC 3659 6.3 | SHOULD | Ignore unknown text after "TVFS" in its feature line |  |  |
| R3659_7_1_MlsxSyntax | RFC 3659 7.1 | MUST | Send MLST and MLSD with no argument or exactly one path name |  |  |
| R3659_7_1_OmitArgumentForCurrentDirectory | RFC 3659 7.1 | MUST | List the current directory by omitting the argument, no shorthand like "." is defined |  |  |
| R3659_7_1_MlstOnControlOnly | RFC 3659 7.1 | MUST | Read MLST from the control reply and open no data connection for it |  |  |
| R3659_7_1_MlsdOnNonDirectory | RFC 3659 7.1, 7.2.1 | MUST | Expect 501 and no data connection when MLSD names something that is not a directory |  |  |
| R3659_7_2_MlstEntryLeadingSpace | RFC 3659 7.2 | MUST | Read each MLST entry line after its one leading space | parseMachineListReply reads the line that starts with a space | `MachineListReplyTests.EntryOfTheReply`, `MachineListReplyTests.ReplyWithoutAReadableEntry` |
| R3659_7_2_FactsPathnameSeparator | RFC 3659 7.2 | MUST | Split an entry at its first space, facts before it and the path name after it | Listing parsers | `MachineListingTests.NameIsEverythingAfterTheFirstSpace` |
| R3659_7_2_EmptyFactSet | RFC 3659 7.2, 7.9 | MUST | Accept entries without facts that start with the separating space | Listing parsers | `MachineListingTests.EntryWithoutFacts` |
| R3659_7_2_SameNameSeveralTypes | RFC 3659 7.2, 7.5.1.6 | MUST | Accept several entries for one path name, each with its own type fact | The first entry of a name is kept and the others counted, an entry of the file explorer has one type | `ListingParserTests.RepeatedNamesKeepTheFirst` |
| R3659_7_2_NoFormatVariation | RFC 3659 7.1, 7.2 | MUST NOT | Expect no headers, titles, blank lines, tabs or padding spaces in MLSx output |  |  |
| R3659_7_2_MlsdKeepsType | RFC 3659 7.2 | MUST | Keep the current TYPE across MLSD, its data connection always acts as TYPE L 8, MODE S, STRU F |  |  |
| R3659_7_2_LongLines | RFC 3659 7.2 | SHOULD | Accept MLSD lines of any length | Deviation: lines past 64 KiB are skipped and counted and names past 1024 bytes refused, so a server cannot exhaust memory | `ListingParserTests.LongLinesAreSkipped`, `ListingParserTests.LongestLineIsRead` |
| R3659_7_2_FactsVaryPerEntry | RFC 3659 7.2 | MAY | Accept a different set and order of facts on each entry | Listing parsers | `ListingSampleTests.ProftpdMachineListing`, `ListingSampleTests.PureFtpdMachineListing` |
| R3659_7_3_EightBitClean | RFC 3659 7, 7.3 | MUST | Send and receive all 256 octet values on the control connection | Replies keep every byte, arguments refuse CR, LF and NUL (R959_5_3_NoLineEndsInArguments) | `ReplyFramerTests.NulAndInvalidUtf8AreKeptAndShownEscaped` |
| R3659_7_3_1_MlstReturnedName | RFC 3659 7.3.1 | MAY | Accept an MLST entry named differently from the request, a full TVFS path or an empty name | parseMachineListReply keeps the name as sent | `MachineListReplyTests.EntryOfTheReply`, `MachineListReplyTests.CurrentDirectoryIsADirectory`, `MachineListReplyTests.NameMayBeEmpty` |
| R3659_7_3_1_MlsdUnqualifiedNames | RFC 3659 7.3.1 | SHOULD | Read MLSD names as names inside the listed directory | A name with "/" is refused as no child of the listed directory | `ListingParserTests.NamesThatNameNoChildAreRefused` |
| R3659_7_4_FactSyntax | RFC 3659 7.4 | MUST | Split facts at each ";" and every fact at its first "=" | Listing parsers | `MachineListingTests.LinesAgainstTheGrammarAreUnreadable`, `MachineListingTests.SymlinksAndDevices` |
| R3659_7_5_FactNamesAnyCase | RFC 3659 7.5, 7.7.8 | MUST | Compare fact names without regard to case | Listing parsers | `MachineListingTests.FactNamesAndTypesInAnyCase`, `MachineListingTests.LinesAgainstTheGrammarAreUnreadable` |
| R3659_7_5_1_TypeValuesAnyCase | RFC 3659 7.5.1, 7.5.1.5 | MUST | Compare type values, OS.name=kind included, without regard to case | Listing parsers | `MachineListingTests.FactNamesAndTypesInAnyCase` |
| R3659_7_5_1_4_PdirOptional | RFC 3659 7.5.1.4 | SHOULD | Do not rely on pdir entries, a listing may hold none or several | pdir entries are skipped, none is needed | `MachineListingTests.CurrentAndParentDirectoriesAreNoChildren`, `ListingSampleTests.PyftpdlibMachineListing` |
| R3659_7_5_1_5_UnknownOsTypes | RFC 3659 7.5.1.5 | MAY | Accept OS.name=kind types the client does not know | They stay of unknown type | `MachineListingTests.UnknownTypesAndFactsAreIgnored` |
| R3659_7_5_1_5_SharedUniqueTransfer | RFC 3659 7.5.1.5 | MAY | Transfer a file listed under several names with one unique value only once |  |  |
| R3659_7_5_2_UniqueOpaque | RFC 3659 7.5.2 | SHOULD | Compare unique values as opaque, case-sensitive strings | ListingEntry::unique keeps the value as sent | `MachineListingTests.UniqueKeepsItsCase` |
| R3659_7_5_2_UniqueEqualityOnly | RFC 3659 7.5.2 | SHOULD | Conclude from equal unique values only that names share one object |  |  |
| R3659_7_5_2_UniqueWithinConnection | RFC 3659 7.5.2 | SHOULD | Compare unique values only within one control connection |  |  |
| R3659_7_5_3_ModifyChangeNotProof | RFC 3659 7.5.3 | SHOULD NOT | Do not take a changed modify value as proof that the content changed |  |  |
| R3659_7_5_5_PermLetters | RFC 3659 7.5.5 | MUST | Read perm as an unordered set of letters in any case |  |  |
| R3659_7_5_5_PermIsAHint | RFC 3659 7.5.5 | MUST NOT | Never take a perm letter as a guarantee that the command will succeed |  |  |
| R3659_7_5_7_SizeFactApproximate | RFC 3659 7.5.7 | MUST | Treat the size fact as approximate, it may change before the RETR |  |  |
| R3659_7_5_7_SizeForAccuracy | RFC 3659 7.5.7 | SHOULD | Use SIZE for an exact size and the size fact for display |  |  |
| R3659_7_5_10_AnyFactMayBeMissing | RFC 3659 7.5.10, 7.8 | MAY | Cope with MLSx output that lacks any fact, the type fact included | A missing fact leaves its field unknown | `MachineListingTests.MissingFactsStayUnknown`, `MachineListingTests.EntryWithoutFacts` |
| R3659_7_6_2_UnknownLocalFacts | RFC 3659 7.6, 7.6.2 | SHOULD NOT | Make no use of X. or OS-specific facts without knowing their meaning | Only type, size, sizd, modify, unique and the UNIX mode, owner and group facts are read | `MachineListingTests.UnknownTypesAndFactsAreIgnored` |
| R3659_7_7_8_CaseDependentSelection | RFC 3659 7.7.8 | MUST | Keep names that differ only in case apart when choosing files |  |  |
| R3659_7_8_FeatMlst | RFC 3659 7.8 | MUST | Learn MLST and MLSD support from the MLST FEAT line, there is no MLSD line |  |  |
| R3659_7_8_FeatFactList | RFC 3659 7.8 | MUST | Read the facts of the MLST feature line, a "*" marking those sent by default | Features::machineListFact | `FeaturesTests.ProftpdFeatures`, `FeaturesTests.FactsOfMachineListingAreOfferedOrEnabled`, `FeaturesTests.NoMachineListingHasNoFacts` |
| R3659_7_9_OptsMlstSyntax | RFC 3659 7.9 | MUST | Send OPTS MLST (there is no OPTS MLSD) with fact names each followed by ";" and no spaces |  |  |
| R3659_7_9_RequestedFactsOnly | RFC 3659 7.9 | MUST NOT | Expect only requested facts after OPTS MLST, and accept requested ones that are missing |  |  |
| R3659_7_9_DefaultFactsPreferred | RFC 3659 7.9 | SHOULD NOT | Request facts outside the server's default set only for a real operational need |  |  |
| R3659_7_9_OptsMlstBeforeLogin | RFC 3659 7.9 | MAY | Expect OPTS MLST to be refused before login |  |  |
| R3659_7_9_1_OptsMlstReply | RFC 3659 7.9.1 | MUST | Read the facts now selected from the "MLST OPTS" text of the 200 reply, in any case and order |  |  |
| R3659_8_OtherCommandsAnyCharset | RFC 3659 8 | MUST | Allow any character set in path arguments and replies of APPE, CWD, DELE, MKD, PWD, RETR, RMD, RNFR, RNTO, STAT, STOR and STOU |  |  |
| R3659_8_SameSpaceCrNulHandling | RFC 3659 8 | SHOULD | Treat spaces, CR and NUL in those arguments as for MLST |  |  |
| R3659_11_FeatOptsBeforeLogin | RFC 3659 11 | MAY | Send FEAT and OPTS before login |  |  |
| R3659_11_NoMlsxBeforeLogin | RFC 3659 11 | MUST NOT | Send no MLST or MLSD before login |  |  |
| R3659_MdtmTimeVal | RFC 3659 2.3, 3.1 | MUST | Read time-val as UTC with optional fractions | parseTimeVal, parseModificationTimeReply | `ReplyParserTests.TimeVal`, `ReplyParserTests.TimeValWithFractions`, `ReplyParserTests.TimeValOutOfRangeIsUnreadable`, `ReplyParserTests.ModificationTimeReply` |
| R3659_RestStream | RFC 3659 5.1, 5.3 | MUST | Use REST STREAM semantics and placement | Transfer flow |  |
| R3659_MlstGrammar | RFC 3659 7.2, 7.4 | MUST | Read MLST and MLSD entries by the grammar and their facts | Listing parsers | `MachineListingTests.FactsAndName`, `MachineListingTests.LinesAgainstTheGrammarAreUnreadable`, `MachineListReplyTests.EntryOfTheReply` |
| R3659_CdirPdir | RFC 3659 7.5.1.2, 7.5.1.4 | SHOULD | Do not list cdir and pdir entries as children | Listing parsers | `MachineListingTests.CurrentAndParentDirectoriesAreNoChildren` |
| R3659_TvfsPaths | RFC 3659 | SHOULD | Use slash separated paths when TVFS is offered | Metadata operations |  |
| R3659_MlstErrors | RFC 3659 7.2.1 | SHOULD | Map the error replies of MLST and MLSD | Reply mapping |  |
| R4217_1_SslAllowed | RFC 4217 1 | MAY | Negotiate SSL instead of TLS when so configured | Not applicable: TLS 1.2 is the minimum, SSL is never negotiated |  |
| R4217_4_1_SendAuthTls | RFC 4217 4.1 | SHOULD | Send AUTH TLS to secure a session |  |  |
| R4217_4_1_ContinueAfterAuthRejection | RFC 4217 4.1 | MAY | Go on unprotected after AUTH is rejected, if policy allows | Not applicable: a session that asks for TLS never goes on without it |  |
| R4217_4_2_UserStillRequired | RFC 4217 4.2; RFC 2228 2 | MUST | Send USER even when TLS already identified the client |  |  |
| R4217_4_2_ResetAfterAuth | RFC 4217 4.2 | MUST | Treat TYPE, data port, working directory and other transfer parameters as reset after AUTH |  |  |
| R4217_6_FeatAuthTls | RFC 4217 6 | MUST | Read AUTH TLS, PBSZ and PROT from FEAT, a TLS-C synonym need not appear | Features::has("AUTH", "TLS") | `FeaturesTests.ProftpdFeatures` |
| R4217_7_ClientIsTlsClient | RFC 4217 7 | MUST | Be the TLS client on data connections too | DataConnection TLS |  |
| R4217_8_ProtAnyTime | RFC 4217 8 | MAY | Change the data protection level with PROT at any point of the session | Not applicable: PROT P is set once per control connection |  |
| R4217_9_ProtLevelPersists | RFC 4217 9 | MUST | Protect every later data connection at the accepted PROT level until PROT, REIN or AUTH changes it |  |  |
| R4217_9_ProtClearOrPrivate | RFC 4217 9 | MUST | Send only PROT C or PROT P with TLS |  |  |
| R4217_9_DataHandshakeFirst | RFC 4217 9 | MUST | With PROT P finish the data connection TLS handshake before any data |  |  |
| R4217_9_NoEncapsulation | RFC 4217 9 | SHOULD NOT | Do not wrap TLS data in RFC 2228 protection blocks |  |  |
| R4217_9_InitialClear | RFC 4217 9; RFC 2228 3 | MUST | Treat data connections as Clear until a PROT P is accepted |  |  |
| R4217_10_1_FailedTlsReplies | RFC 4217 10.1 | SHOULD | After a failed control handshake accept a clear 421, a protected 421 or a dropped connection |  |  |
| R4217_10_2_Reply521 | RFC 4217 10.2 | SHOULD | Read 521 to a transfer command as a PROT setting the server refuses |  |  |
| R4217_10_2_Reply522 | RFC 4217 10.2, 11.4 | MUST | Read 522 to a transfer command, first or after 150, as data protection too weak or a failed data handshake |  |  |
| R4217_10_3_FlexibleSecuring | RFC 4217 10.3 | SHOULD | Secure the session at the point the server or user policy demands |  |  |
| R4217_10_3_ServerDictatesTiming | RFC 4217 10.3 | SHOULD | Let the server dictate when AUTH, PBSZ and PROT are sent | Deviation: AUTH TLS follows the greeting and PBSZ 0, PROT P follow the handshake, no setting changes that order |  |
| R4217_10_3_UserOverridesTiming | RFC 4217 10.3 | MUST | Let the user override when AUTH, PBSZ and PROT are sent | Deviation: AUTH TLS follows the greeting and PBSZ 0, PROT P follow the handshake, no setting changes that order |  |
| R4217_10_3_AuthNotForcedFirst | RFC 4217 10.3 | SHOULD NOT | Do not insist on AUTH as the first command, nor on PBSZ and PROT right after it | Deviation: AUTH TLS follows the greeting and PBSZ 0, PROT P follow the handshake, no setting changes that order |  |
| R4217_10_3_ClearQuitAfterFailedTls | RFC 4217 10.3 | SHOULD | Send an unprotected QUIT when the TLS negotiation fails completely |  |  |
| R4217_10_3_ProtectedQuitOnRejectedPeer | RFC 4217 10.3 | SHOULD | Send a protected QUIT when TLS succeeds but the server's parameters are unacceptable |  |  |
| R4217_10_3_DropOnBrokenHandshake | RFC 4217 10.3 | SHOULD | Drop the control connection when the handshake fails midway |  |  |
| R4217_10_4_DataNegotiation | RFC 4217 10.4 | SHOULD NOT | Do not read a refused transfer as a refusal of that PROT level for the session | DataConnection TLS |  |
| R4217_10_4_ClearListingsFirst | RFC 4217 10.4 | MAY | Try LIST and NLST on a Clear data connection first, with a user option that forbids it | Not applicable: listings use PROT P like transfers |  |
| R4217_10_4_ClientCertificateForUser | RFC 4217 10.4 | SHOULD | Use a client certificate that identifies a user, not the client host | Not applicable: client certificates come later |  |
| R4217_12_1_PbszProtLoginOrder | RFC 4217 12.1 | MAY | Accept a server that takes PBSZ and PROT only after login, or before it |  |  |
| R4217_12_5_ConnectBeforeTransferReply | RFC 4217 12.5 | SHOULD | Open the passive data connection without waiting for the 150 reply |  |  |
| R4217_13_ReinClearsTls | RFC 4217 13 | MUST | Treat TLS as cleared on control and data connections after REIN | Not applicable: the client never sends REIN |  |
| R4217_14_EverythingInsideTls | RFC 4217 14 | MUST | Send all data, Telnet commands included, inside the TLS session | ABOR goes in band on the TLS stream, without urgent data |  |
| R4217_15_1_1_CertificateVerification | RFC 4217 15.1.1; RFC 7151 4 | SHOULD | Verify the certificate of the server | Tls trust |  |
| R4217_15_1_1_SameDataCertificate | RFC 4217 15.1.1 | SHOULD | Check that data connections show the same server certificate as the control connection |  |  |
| R4217_15_1_2_SameClientCertificate | RFC 4217 15.1.2 | SHOULD | Present the same client certificate on data and control connections | Not applicable: client certificates come later |  |
| R4217_15_3_CccIssues | RFC 4217 15.3 | SHOULD NOT | Do not assume the server accepts CCC | Not applicable: the client never sends CCC |  |
| R4217_17_AuthParameterTls | RFC 4217 17 | MUST | Send the AUTH parameter as "TLS" |  |  |
| R4217_AuthTlsPbszProt | RFC 4217 4.1, 8, 9, 17 | SHOULD | Send AUTH TLS, then PBSZ 0 and PROT P | Login flow |  |
| MFXX_3_MfmtNotMdtm | draft-somers-ftp-mfxx-04 3 | SHOULD | Set modification times with MFMT, not with an MDTM that takes a time |  |  |
| MFXX_3_MffForBothTimes | draft-somers-ftp-mfxx-04 3, 4 | SHOULD | Use MFF to change modification and creation time together | Not applicable: the client sets only modification times |  |
| MFXX_3_1_ReadActualTime | draft-somers-ftp-mfxx-04 3.1 | SHOULD | Take the time in the 213 "Modify=" reply as the one actually set, it may differ from the request |  |  |
| MFXX_3_2_MfmtErrors | draft-somers-ftp-mfxx-04 3.2 | MUST | Read 501 to MFMT as an invalid time, 550 as a missing object, 500 as a syntax error, 4xy as temporary |  |  |
| MFXX_3_3_FeatMfmt | draft-somers-ftp-mfxx-04 3.3 | MUST | Learn MFMT support from a FEAT line "MFMT" in any case | Features::has | `FeaturesTests.ProftpdFeatures` |
| MFXX_4_1_MfctReply | draft-somers-ftp-mfxx-04 4.1 | SHOULD | Take the time in the 213 "Create=" reply to MFCT as the one set | Not applicable: the client never sends MFCT |  |
| MFXX_5_MfmtNotMff | draft-somers-ftp-mfxx-04 5 | SHOULD | Use MFMT rather than MFF to change only the modification time |  |  |
| MFXX_5_1_NoAssumptionAfterMffError | draft-somers-ftp-mfxx-04 5.1 | MUST | After an MFF error assume nothing about which facts changed | Not applicable: the client never sends MFF |  |
| MFXX_MfmtFormat | draft-somers-ftp-mfxx-04 3 | MUST | Send MFMT with a time-val and the path | formatTimeVal | `ReplyParserTests.TimeValIsWrittenInWholeSeconds`, `ReplyParserTests.TimeValOutsideItsYearsIsNotWritten` |
| R7151_3_HostBeforeAuthentication | RFC 7151 3 | MUST | Send HOST, if at all, before the user is authenticated | Not applicable: HOST is planned for later |  |
| R7151_3_1_HostnameAsEntered | RFC 7151 3.1 | SHOULD | Send the fully qualified host name used for the address lookup, before CNAME resolution | Not applicable: HOST is planned for later |  |
| R7151_3_1_IdnaALabels | RFC 7151 3.1 | MUST | Send internationalized host names as IDNA A-labels | Not applicable: HOST is planned for later |  |
| R7151_3_1_LiteralAddress | RFC 7151 3.1 | MAY | Send the literal IPv4 or bracketed IPv6 address the user gave | Not applicable: HOST is planned for later |  |
| R7151_3_1_NoPort | RFC 7151 3.1 | MUST NOT | Send no port number in HOST | Not applicable: HOST is planned for later |  |
| R7151_3_2_HostReplies | RFC 7151 3.1, 3.2 | SHOULD | Read 220 to HOST as accepted, 421 as temporarily unavailable with the connection closing, 504 as an unknown host | Not applicable: HOST is planned for later |  |
| R7151_3_2_1_HostAfterRein | RFC 7151 3.2.1 | MUST | Send HOST again after REIN | Not applicable: HOST is planned for later and REIN is never sent |  |
| R7151_3_2_2_SendHost | RFC 7151 3.2.2 | MUST | Send HOST after connecting, or after REIN, before USER | Not applicable: HOST is planned for later |  |
| R7151_3_3_RetryHost | RFC 7151 3.3 | MAY | Send another HOST after an error reply the server ignored | Not applicable: HOST is planned for later |  |
| R7151_3_3_NoVirtualHosts | RFC 7151 3.3 | SHOULD | Read 500 or 502 to HOST as no virtual host support, then log in anyway | Not applicable: HOST is planned for later |  |
| R7151_3_3_OtherErrorsEndSession | RFC 7151 3.3 | SHOULD | End the session on any other error reply to HOST | Not applicable: HOST is planned for later |  |
| R7151_3_4_FeatHost | RFC 7151 3.4 | MUST | Learn HOST support from a FEAT line "HOST" in any case | Not applicable: HOST is planned for later |  |
| R7151_4_HostMatchesCertificate | RFC 7151 4 | SHOULD | Check that the HOST name matches the identity in the server certificate | Not applicable: HOST is planned for later |  |
| R7151_4_HostBeforeHandshakeWithoutSni | RFC 7151 4 | SHOULD | Send HOST before the TLS handshake when the client hello carries no server_name | Not applicable: HOST is planned for later |  |
| R7151_4_HostAfterHandshakeWithSni | RFC 7151 4 | MAY | Send HOST after the TLS handshake when server_name carries the same name | Not applicable: HOST is planned for later |  |
| DF_ImplicitFtps | De facto | MUST | Start TLS at once on port 990 | Login flow |  |
| DF_OptsUtf8On | De facto | SHOULD | Send OPTS UTF8 ON and accept a refusal | Login flow |  |
| DF_DataTlsSessionReuse | De facto; RFC 4217 10.2 | MUST | Resume the control session on data connections | DataConnection TLS |  |
| DF_CloseNotifyBeforeFinalReply | De facto | MUST | Answer the close_notify of a download before waiting for the 226 | Transfer close rules |  |
| DF_UploadCloseNotify | De facto; RFC 4217 12.6, 12.7 | MUST | End an upload with close_notify, not FIN alone | Transfer close rules |  |
| DF_AlpnFtp | De facto | MUST | Offer the ALPN protocol ftp or none | Tls context |  |
| DF_AborReplyVariants | De facto | MUST | Accept every ABOR reply order seen on real servers | Cancel and drain |  |
| DF_SizeInTypeImage | De facto | MUST | Send SIZE only in TYPE I | Metadata operations |  |
| DF_BareLineFeedInReplies | De facto | SHOULD | Accept a bare LF as the end of a reply line | ReplyFramer | `ReplyFramerTests.BareLineFeedEndsALine` |
| DF_UnescapedIacInReplies | De facto | SHOULD | Keep a 0xFF that the server did not escape as IAC IAC, in path names of any code set | ReplyFramer keeps IAC before any byte other than IAC, WILL, WONT, DO and DONT | `ReplyFramerTests.UnescapedByteThatStartsNoTelnetCommandIsKept`, `ReplyFramerTests.InterpretAsCommandBeforeAnotherCommandIsAnUnescapedByte` |
| DF_ReplyEndingOnAnotherCode | De facto | SHOULD | Accept a multi-line reply that ends on another code (glFTPd) | ReplyFramer holds the line as a possible end until the next line or settle() | `ReplyFramerTests.LineWithAnotherCodeIsHeldAsAPossibleEnd`, `ReplyFramerTests.LaterLineWithAnotherCodeTakesOverThePossibleEnd`, `ReplyFramerTests.FinishSettlesAPossibleEnd`, `ReplyFramerTests.SettleWaitsForAFollowingLineThatStarted` |
| DF_DataToControlPeer | De facto; RFC 2577 2, 3 | MUST | Connect a PASV data connection to the control connection's peer, never to the announced address | DataConnection, unless the session trusts the announced address |  |
| DF_ConnectionLimitReplies | De facto | MUST | Read 421 at the greeting and 530 after PASS that name too many connections as a connection limit | replyError | `ReplyErrorTests.ConnectionLimits` |
| DF_LoginBanReplies | De facto | SHOULD | Read a 421 or 530 that names too many failed logins as failed authentication, not as a connection limit | replyError | `ReplyErrorTests.BansAfterFailedLoginsAreFailedAuthentication` |
| DF_ErrnoTextsOfReplies | De facto | SHOULD | Tell missing, denied, existing, full and over quota apart by the errno or fixed text a line ends with, never by an echoed path | replyError for 450, 451, 521, 550 and 553 | `ReplyErrorTests.ErrnoTextsDecideAmbiguousCodes`, `ReplyErrorTests.WordsOfAnEchoedPathDecideNothing`, `ReplyErrorTests.TextsOfWindowsServers`, `ReplyErrorTests.FixedTextsLeaveAmbiguousCodesAsFailures` |
| DF_SetModificationTime | De facto | SHOULD | Set modification times with MFMT, else MDTM with a time (vsftpd) or SITE UTIME (proftpd) | Metadata operations, without reading the reply text |  |
| DF_SiteChmod | De facto | MAY | Change permissions with SITE CHMOD | Metadata operations |  |
