#pragma once

#include <ftp/protocol/listing.hpp>
#include <ftp/protocol/reply_error.hpp>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <string_view>

/*
 * The samples in samples/listings are what proftpd 1.3.8c, pure-ftpd 1.0.50, vsftpd 3.0.5 and pyftpdlib 2.0.1 sent for
 * one directory on 2026-10-27, byte for byte. It holds plain.txt (11 bytes), subdir, link-to-plain.txt -> plain.txt,
 * link-to-subdir -> subdir, dangling-link -> does-not-exist, "name with spaces.txt", " leading space.txt", a UTF-8
 * name, "a -> b.txt" (a file), .hidden and old.txt, modified 2020-01-02 03:04:05 UTC, all others 2026-10-01 12:34:56
 * UTC.
 *
 * Many single lines are rewritten from the listing tests of jlaffaye/ftp (ISC), patrickjuchli/basic-ftp (MIT) and
 * Apache Commons Net (Apache 2.0).
 */

namespace FileTransferProtocol::Test
{
    namespace
    {
        using SharedData::FileType;
        using std::filesystem::perms;

        constexpr std::string_view utf8Name =
            "\xC3\xBC"
            "n\xC3\xAF"
            "c\xC3\xB6"
            "d\xC3\xA9-\xE5\x90\x8D\xE5\x89\x8D.txt";

        /** @brief The day the samples were taken. */
        constexpr std::chrono::sys_days samplesTaken{std::chrono::year{2026} / 10 / 28};

        std::string listingSample(std::string_view name)
        {
            std::ifstream file{std::filesystem::path{NUI_SFTP_FTP_LISTING_SAMPLES} / name, std::ios::binary};
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        }

        Listing parsed(ListingParser parser, std::string_view bytes)
        {
            parser.feed(bytes);
            return parser.finish();
        }

        Listing machineListingOf(std::string_view bytes)
        {
            return parsed(ListingParser::machineListing(), bytes);
        }

        Listing listOf(std::string_view bytes, std::chrono::sys_days today = samplesTaken)
        {
            return parsed(ListingParser::list(today), bytes);
        }

        std::set<std::string> namesOf(Listing const& listing)
        {
            std::set<std::string> names{};
            for (auto const& entry : listing.entries)
                names.insert(entry.name);
            return names;
        }

        ListingEntry entryNamed(Listing const& listing, std::string_view name)
        {
            const auto found = std::ranges::find(listing.entries, name, &ListingEntry::name);
            if (found == listing.entries.end())
            {
                ADD_FAILURE() << "No entry named '" << name << "'";
                return {};
            }
            return *found;
        }

        /**
         * @brief The one entry of @p listing, which must have nothing left out.
         */
        ListingEntry onlyEntry(Listing const& listing)
        {
            EXPECT_EQ(listing.entries.size(), 1u);
            EXPECT_EQ(listing.unreadableLines, 0u);
            EXPECT_EQ(listing.refusedNames, 0u);
            return listing.entries.empty() ? ListingEntry{} : listing.entries.front();
        }

        void expectNothingLeftOut(Listing const& listing)
        {
            EXPECT_EQ(listing.unreadableLines, 0u);
            EXPECT_EQ(listing.unreadableFacts, 0u);
            EXPECT_EQ(listing.refusedNames, 0u);
            EXPECT_EQ(listing.repeatedNames, 0u);
        }

        TimeVal utc(int year, unsigned month, unsigned day, int hour, int minute, int second)
        {
            return TimeVal{std::chrono::sys_days{std::chrono::year{year} / month / day}} + std::chrono::hours{hour} +
                std::chrono::minutes{minute} + std::chrono::seconds{second};
        }

        ListedTime listedAt(int year, unsigned month, unsigned day, int hour, int minute)
        {
            return ListedTime{
                .time = std::chrono::local_days{std::chrono::year{year} / month / day} + std::chrono::hours{hour} +
                    std::chrono::minutes{minute},
                .precision = ListedTimePrecision::Minute,
            };
        }

        ListedTime listedOn(int year, unsigned month, unsigned day)
        {
            return ListedTime{
                .time = std::chrono::local_days{std::chrono::year{year} / month / day},
                .precision = ListedTimePrecision::Day,
            };
        }

        const std::set<std::string> fixtureNames{
            "plain.txt",
            "subdir",
            "link-to-plain.txt",
            "link-to-subdir",
            "dangling-link",
            "name with spaces.txt",
            " leading space.txt",
            std::string{utf8Name},
            "a -> b.txt",
            ".hidden",
            "old.txt",
        };

        std::set<std::string> fixtureNamesWithout(std::set<std::string> const& left)
        {
            auto names = fixtureNames;
            for (auto const& name : left)
                names.erase(name);
            return names;
        }

        /**
         * @brief What every LIST sample of the fixture shows, with @p owner and group.
         */
        void expectFixtureList(Listing const& listing, std::string_view owner)
        {
            expectNothingLeftOut(listing);
            EXPECT_EQ(namesOf(listing), fixtureNames);

            const auto plain = entryNamed(listing, "plain.txt");
            EXPECT_EQ(plain.type, FileType::Regular);
            EXPECT_EQ(plain.size, 11u);
            EXPECT_EQ(plain.listedTime, listedAt(2026, 10, 1, 12, 34));
            EXPECT_EQ(
                plain.permissions, perms::owner_read | perms::owner_write | perms::group_read | perms::others_read
            );
            EXPECT_EQ(plain.owner, owner);
            EXPECT_EQ(plain.group, owner);
            EXPECT_EQ(plain.linkTarget, std::nullopt);

            EXPECT_EQ(entryNamed(listing, "subdir").type, FileType::Directory);
            EXPECT_EQ(
                entryNamed(listing, "subdir").permissions,
                perms::owner_all | perms::group_read | perms::group_exec | perms::others_read | perms::others_exec
            );
            EXPECT_EQ(entryNamed(listing, "old.txt").listedTime, listedOn(2020, 1, 2));
            EXPECT_EQ(entryNamed(listing, " leading space.txt").size, 8u);
            EXPECT_EQ(entryNamed(listing, std::string{utf8Name}).size, 5u);

            const auto arrowFile = entryNamed(listing, "a -> b.txt");
            EXPECT_EQ(arrowFile.type, FileType::Regular);
            EXPECT_EQ(arrowFile.linkTarget, std::nullopt);

            const auto link = entryNamed(listing, "link-to-plain.txt");
            EXPECT_EQ(link.type, FileType::Symlink);
            EXPECT_EQ(link.linkTarget, "plain.txt");
            EXPECT_EQ(link.size, 9u);
            EXPECT_EQ(entryNamed(listing, "link-to-subdir").linkTarget, "subdir");
            EXPECT_EQ(entryNamed(listing, "dangling-link").linkTarget, "does-not-exist");
        }
    }

    TEST(ListingSampleTests, ProftpdMachineListing)
    {
        const auto listing = machineListingOf(listingSample("proftpd-mlsd.txt"));
        expectNothingLeftOut(listing);
        // proftpd leaves out the dangling link, cdir and pdir are no children.
        EXPECT_EQ(namesOf(listing), fixtureNamesWithout({"dangling-link"}));

        const auto plain = entryNamed(listing, "plain.txt");
        EXPECT_EQ(plain.type, FileType::Regular);
        EXPECT_EQ(plain.size, 11u);
        EXPECT_EQ(plain.modified, utc(2026, 10, 1, 12, 34, 56));
        EXPECT_EQ(plain.listedTime, std::nullopt);
        EXPECT_EQ(plain.permissions, perms::owner_read | perms::owner_write | perms::group_read | perms::others_read);
        EXPECT_EQ(plain.unique, "70U1002C7A");

        const auto link = entryNamed(listing, "link-to-plain.txt");
        EXPECT_EQ(link.type, FileType::Symlink);
        EXPECT_EQ(link.linkTarget, std::nullopt);
        EXPECT_EQ(link.unique, plain.unique);

        const auto subdir = entryNamed(listing, "subdir");
        EXPECT_EQ(subdir.type, FileType::Directory);
        EXPECT_EQ(subdir.size, std::nullopt);
        EXPECT_EQ(entryNamed(listing, "old.txt").modified, utc(2020, 1, 2, 3, 4, 5));
        EXPECT_EQ(entryNamed(listing, " leading space.txt").size, 8u);
        EXPECT_EQ(entryNamed(listing, "a -> b.txt").type, FileType::Regular);
    }

    TEST(ListingSampleTests, PureFtpdMachineListing)
    {
        const auto listing = machineListingOf(listingSample("pure-ftpd-mlsd.txt"));
        expectNothingLeftOut(listing);
        EXPECT_EQ(namesOf(listing), fixtureNames);

        const auto plain = entryNamed(listing, "plain.txt");
        EXPECT_EQ(plain.owner, "1000");
        EXPECT_EQ(plain.group, "1000");
        EXPECT_EQ(plain.unique, "7bg1002d0a");
        // "OS.unix=slink:" with no target.
        EXPECT_EQ(entryNamed(listing, "dangling-link").type, FileType::Symlink);
        EXPECT_EQ(entryNamed(listing, "dangling-link").linkTarget, std::nullopt);
        EXPECT_EQ(entryNamed(listing, "subdir").size, 4096u);
    }

    TEST(ListingSampleTests, PyftpdlibMachineListing)
    {
        const auto listing = machineListingOf(listingSample("pyftpdlib-mlsd.txt"));
        expectNothingLeftOut(listing);
        EXPECT_EQ(namesOf(listing), fixtureNamesWithout({"dangling-link"}));
        // pyftpdlib follows links and writes the mode as 0o644.
        EXPECT_EQ(entryNamed(listing, "link-to-plain.txt").type, FileType::Regular);
        EXPECT_EQ(entryNamed(listing, "link-to-plain.txt").size, 11u);
        EXPECT_EQ(entryNamed(listing, "link-to-subdir").type, FileType::Directory);
        EXPECT_EQ(
            entryNamed(listing, "plain.txt").permissions,
            perms::owner_read | perms::owner_write | perms::group_read | perms::others_read
        );
    }

    TEST(ListingSampleTests, ProftpdList)
    {
        expectFixtureList(listOf(listingSample("proftpd-list-a.txt")), "spike");
    }

    TEST(ListingSampleTests, PureFtpdList)
    {
        expectFixtureList(listOf(listingSample("pure-ftpd-list-la.txt")), "spike");
    }

    TEST(ListingSampleTests, VsftpdList)
    {
        expectFixtureList(listOf(listingSample("vsftpd-list-a.txt")), "1000");
    }

    TEST(ListingSampleTests, PyftpdlibList)
    {
        expectFixtureList(listOf(listingSample("pyftpdlib-list.txt")), "spike");
    }

    TEST(ListingSampleTests, ProftpdNameListLosesTheListedPath)
    {
        const auto listing = parsed(ListingParser::nameList("listing"), listingSample("proftpd-nlst.txt"));
        expectNothingLeftOut(listing);
        EXPECT_EQ(namesOf(listing), fixtureNamesWithout({".hidden", "dangling-link"}));
        EXPECT_EQ(entryNamed(listing, "plain.txt").type, FileType::Unknown);
    }

    TEST(ListingSampleTests, PyftpdlibNameList)
    {
        const auto listing = parsed(ListingParser::nameList("listing"), listingSample("pyftpdlib-nlst.txt"));
        expectNothingLeftOut(listing);
        EXPECT_EQ(namesOf(listing), fixtureNames);
    }

    TEST(MachineListingTests, FactsAndName)
    {
        const auto entry = onlyEntry(machineListingOf(
            "modify=20150813175250;perm=adfr;size=951;type=file;unique=119FBB87UE;UNIX.group=0;UNIX.mode=0644;"
            "UNIX.owner=0; welcome.msg\r\n"
        ));
        EXPECT_EQ(entry.name, "welcome.msg");
        EXPECT_EQ(entry.type, FileType::Regular);
        EXPECT_EQ(entry.size, 951u);
        EXPECT_EQ(entry.modified, utc(2015, 8, 13, 17, 52, 50));
        EXPECT_EQ(entry.unique, "119FBB87UE");
        EXPECT_EQ(entry.owner, "0");
        EXPECT_EQ(entry.group, "0");
    }

    TEST(MachineListingTests, FactNamesAndTypesInAnyCase)
    {
        const auto entry = onlyEntry(machineListingOf("Modify=20150813175250;SIZE=951;Type=FILE;unix.MODE=0640; x\n"));
        EXPECT_EQ(entry.type, FileType::Regular);
        EXPECT_EQ(entry.size, 951u);
        EXPECT_EQ(entry.modified, utc(2015, 8, 13, 17, 52, 50));
        EXPECT_EQ(entry.permissions, perms::owner_read | perms::owner_write | perms::group_read);

        EXPECT_EQ(onlyEntry(machineListingOf("type=DIR; x\n")).type, FileType::Directory);
        EXPECT_EQ(onlyEntry(machineListingOf("type=os.UNIX=SymLink; x\n")).type, FileType::Symlink);
    }

    TEST(MachineListingTests, NameIsEverythingAfterTheFirstSpace)
    {
        EXPECT_EQ(onlyEntry(machineListingOf("type=file;  two  spaces \r\n")).name, " two  spaces ");
        EXPECT_EQ(onlyEntry(machineListingOf("type=file; a=b;c d\n")).name, "a=b;c d");
    }

    TEST(MachineListingTests, NamesAreBytes)
    {
        // Latin-1 and a lone continuation byte, no UTF-8.
        EXPECT_EQ(onlyEntry(machineListingOf("type=file; Fran\xE7ois\x80.txt\r\n")).name, "Fran\xE7ois\x80.txt");
    }

    TEST(MachineListingTests, EntryWithoutFacts)
    {
        const auto entry = onlyEntry(machineListingOf(" name\n"));
        EXPECT_EQ(entry.name, "name");
        EXPECT_EQ(entry.type, FileType::Unknown);
        EXPECT_EQ(entry.size, std::nullopt);
    }

    TEST(MachineListingTests, MissingFactsStayUnknown)
    {
        const auto entry = onlyEntry(machineListingOf("perm=r; name\n"));
        EXPECT_EQ(entry.type, FileType::Unknown);
        EXPECT_EQ(entry.modified, std::nullopt);
        EXPECT_EQ(entry.permissions, perms::unknown);
        EXPECT_TRUE(entry.owner.empty());
    }

    TEST(MachineListingTests, CurrentAndParentDirectoriesAreNoChildren)
    {
        const auto listing = machineListingOf(
            "type=cdir;modify=20150813224845; /home/user\r\n"
            "type=pdir;modify=20150813224845; ..\r\n"
            "type=pdir; /home\r\n"
            "type=pdir;modify=2015; /\r\n"
            "type=dir; child\r\n"
        );
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"child"});
        expectNothingLeftOut(listing);
    }

    TEST(MachineListingTests, SymlinksAndDevices)
    {
        const auto link = onlyEntry(machineListingOf("Type=OS.unix=slink:/actual/target;Unique=keVO1+4G4; foobar\n"));
        EXPECT_EQ(link.type, FileType::Symlink);
        EXPECT_EQ(link.linkTarget, "/actual/target");

        EXPECT_EQ(onlyEntry(machineListingOf("Type=OS.unix=chr-13/29; device\n")).type, FileType::CharDevice);
        EXPECT_EQ(onlyEntry(machineListingOf("Type=OS.unix=blk-11/108; block\n")).type, FileType::BlockDevice);
    }

    TEST(MachineListingTests, UnknownTypesAndFactsAreIgnored)
    {
        const auto listing = machineListingOf(
            "type=OS.vms=stream-lf;x.custom=1;charset=UTF-8;media-type=text/plain; a\n"
            "type=archive; b\n"
        );
        expectNothingLeftOut(listing);
        EXPECT_EQ(entryNamed(listing, "a").type, FileType::Unknown);
        EXPECT_EQ(entryNamed(listing, "b").type, FileType::Unknown);
    }

    TEST(MachineListingTests, Modes)
    {
        const auto modeOf = [](std::string_view mode)
        {
            return onlyEntry(machineListingOf(fmt::format("UNIX.mode={}; x\n", mode))).permissions;
        };
        EXPECT_EQ(
            modeOf("0755"),
            perms::owner_all | perms::group_read | perms::group_exec | perms::others_read | perms::others_exec
        );
        EXPECT_EQ(modeOf("755"), modeOf("0755"));
        EXPECT_EQ(modeOf("0o755"), modeOf("0755"));
        EXPECT_EQ(modeOf("4755"), modeOf("0755") | perms::set_uid);
        EXPECT_EQ(modeOf("7000"), perms::set_uid | perms::set_gid | perms::sticky_bit);
    }

    TEST(MachineListingTests, OwnerNamesBeforeIds)
    {
        const auto named = onlyEntry(
            machineListingOf("UNIX.ownername=myself;UNIX.groupname=mygroup;UNIX.owner=11;UNIX.group=22; x\n")
        );
        EXPECT_EQ(named.owner, "myself");
        EXPECT_EQ(named.group, "mygroup");

        const auto numbered = onlyEntry(machineListingOf("UNIX.uid=11;UNIX.gid=22; x\n"));
        EXPECT_EQ(numbered.owner, "11");
        EXPECT_EQ(numbered.group, "22");
    }

    TEST(MachineListingTests, DirectorySizeWhenThereIsNoSize)
    {
        EXPECT_EQ(onlyEntry(machineListingOf("sizd=4096; x\n")).size, 4096u);
        EXPECT_EQ(onlyEntry(machineListingOf("sizd=4096;size=7; x\n")).size, 7u);
    }

    TEST(MachineListingTests, UniqueKeepsItsCase)
    {
        const auto listing = machineListingOf("unique=AbC; x\nunique=abc; y\n");
        EXPECT_EQ(entryNamed(listing, "x").unique, "AbC");
        EXPECT_EQ(entryNamed(listing, "y").unique, "abc");
    }

    TEST(MachineListingTests, UnreadableFactsAreLeftOutOfTheirEntry)
    {
        const auto listing = machineListingOf(
            "type=file;modify=2015081317525;size=951; short-time\n"
            "type=file;size=18446744073709551616; size-overflow\n"
            "type=file;size=-1; negative-size\n"
            "type=file;UNIX.mode=0999; mode-not-octal\n"
            "type=file;UNIX.mode=17777; mode-too-large\n"
            "type=file;size=; empty-size\n"
        );
        EXPECT_EQ(listing.entries.size(), 6u);
        EXPECT_EQ(listing.unreadableFacts, 6u);
        EXPECT_EQ(listing.unreadableLines, 0u);

        const auto shortTime = entryNamed(listing, "short-time");
        EXPECT_EQ(shortTime.modified, std::nullopt);
        EXPECT_EQ(shortTime.size, 951u);
        EXPECT_EQ(entryNamed(listing, "size-overflow").size, std::nullopt);
        EXPECT_EQ(entryNamed(listing, "mode-not-octal").permissions, perms::unknown);
        EXPECT_EQ(entryNamed(listing, "mode-too-large").permissions, perms::unknown);
    }

    TEST(MachineListingTests, LargestSize)
    {
        EXPECT_EQ(
            onlyEntry(machineListingOf("size=18446744073709551615; x\n")).size,
            std::numeric_limits<std::uint64_t>::max()
        );
    }

    TEST(MachineListingTests, LinesAgainstTheGrammarAreUnreadable)
    {
        const auto listing = machineListingOf(
            "modify=20150806235817;invalid;UNIX.owner=0; no-equals\n"
            "type=file;=x; empty-fact-name\n"
            "type=file;size=1 missing-last-semicolon\n"
            "type=file;\n"
            "type=file;type=dir; type-twice\n"
            "Size=1;size=1; size-twice-in-other-case\n"
            "type=file;; empty-fact\n"
            "type=file; readable\n"
        );
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"readable"});
        EXPECT_EQ(listing.unreadableLines, 7u);
    }

    TEST(MachineListingTests, FactRepeatedAmongManyIsFound)
    {
        std::string facts{};
        for (std::size_t index = 0u; index < 4'000u; ++index)
            facts += fmt::format("x.fact{}=;", index);
        const auto listing = machineListingOf(fmt::format("{} distinct\n{}X.FACT3999=1; repeated\n", facts, facts));
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"distinct"});
        EXPECT_EQ(listing.unreadableLines, 1u);
    }

    TEST(ListParserTests, UnixLines)
    {
        constexpr std::chrono::sys_days today{std::chrono::year{2017} / 3 / 10};
        const auto listing = listOf(
            "drwxr-xr-x    3 110      1002            3 Dec 02  2009 pub\r\n"
            "-rw-r--r--   1 marketwired marketwired    12016 Mar 16  2016 2016031611G087802-001.newsml\r\n"
            "lrwxrwxrwx   1 root     other          7 Jan 25 00:17 bin -> usr/bin\r\n"
            "----------   1 owner    group         1803128 Jul 10 10:18 ls-lR.Z\r\n"
            "d---------   1 owner    group               0 Nov  9 19:45 Softlib\r\n"
            "-rwxrw-r--+  1 521      101         2080 May 21 10:53 data.csv\r\n"
            "-r--------   0 user group     65222236 Feb 24 00:39 RegularFile\r\n",
            today
        );
        expectNothingLeftOut(listing);
        EXPECT_EQ(listing.entries.size(), 7u);

        const auto pub = entryNamed(listing, "pub");
        EXPECT_EQ(pub.type, FileType::Directory);
        EXPECT_EQ(pub.size, 3u);
        EXPECT_EQ(pub.owner, "110");
        EXPECT_EQ(pub.group, "1002");
        EXPECT_EQ(pub.listedTime, listedOn(2009, 12, 2));

        EXPECT_EQ(entryNamed(listing, "2016031611G087802-001.newsml").owner, "marketwired");
        EXPECT_EQ(entryNamed(listing, "bin").linkTarget, "usr/bin");
        EXPECT_EQ(entryNamed(listing, "bin").listedTime, listedAt(2017, 1, 25, 0, 17));
        EXPECT_EQ(entryNamed(listing, "ls-lR.Z").permissions, perms::none);
        EXPECT_EQ(entryNamed(listing, "ls-lR.Z").listedTime, listedAt(2016, 7, 10, 10, 18));
        EXPECT_EQ(entryNamed(listing, "Softlib").listedTime, listedAt(2016, 11, 9, 19, 45));
        EXPECT_EQ(entryNamed(listing, "data.csv").size, 2080u);
        EXPECT_EQ(entryNamed(listing, "RegularFile").size, 65222236u);
        EXPECT_EQ(entryNamed(listing, "RegularFile").owner, "user");
    }

    TEST(ListParserTests, NamesKeepTheirSpaces)
    {
        const auto listing = listOf(
            "drwxr-xr-x    3 110      1002            3 Dec 02  2009 spaces   dir   name\n"
            "-rwxr-xr-x    3 110      1002      1234567 Dec 02  2009  foo bar \n"
            "-rw-r--r--    1 spike    spike           7 Oct  1 12:34   two leading\n"
        );
        expectNothingLeftOut(listing);
        EXPECT_EQ(namesOf(listing), (std::set<std::string>{"spaces   dir   name", " foo bar ", "  two leading"}));
    }

    TEST(ListParserTests, MissingLinkCountOwnerOrGroup)
    {
        const auto listing = listOf(
            "drwxr-xr-x               folder        0 Aug 15 05:49 !!!-Tipp des Haus!\n"
            "-rw-r--r--        0   18446744073709551615 18446744073709551615 Nov 16  2006 VIDEO_TS.VOB\n"
            "-rw-r--r--   1 owner 1234 Oct  1 12:34 no-group\n"
            "-rw-r--r-- owner group 1234 Oct  1 12:34 no-link-count\n"
            "-rw-r--r-- 1234 Oct  1 12:34 only-size\n"
        );
        expectNothingLeftOut(listing);

        const auto folder = entryNamed(listing, "!!!-Tipp des Haus!");
        EXPECT_EQ(folder.owner, "folder");
        EXPECT_TRUE(folder.group.empty());
        EXPECT_EQ(folder.size, 0u);
        EXPECT_EQ(entryNamed(listing, "VIDEO_TS.VOB").size, std::numeric_limits<std::uint64_t>::max());

        const auto noGroup = entryNamed(listing, "no-group");
        EXPECT_EQ(noGroup.owner, "owner");
        EXPECT_TRUE(noGroup.group.empty());
        const auto noLinkCount = entryNamed(listing, "no-link-count");
        EXPECT_EQ(noLinkCount.owner, "owner");
        EXPECT_EQ(noLinkCount.group, "group");
        EXPECT_TRUE(entryNamed(listing, "only-size").owner.empty());
        EXPECT_EQ(entryNamed(listing, "only-size").size, 1234u);
    }

    TEST(ListParserTests, Devices)
    {
        const auto listing = listOf(
            "crw-rw-rw-   1 root     root       1,   3 Oct  1 12:34 null\n"
            "brw-rw----   1 root     disk       8,0 Oct  1 12:34 sda\n"
        );
        expectNothingLeftOut(listing);
        const auto null = entryNamed(listing, "null");
        EXPECT_EQ(null.type, FileType::CharDevice);
        EXPECT_EQ(null.size, std::nullopt);
        EXPECT_EQ(null.owner, "root");
        EXPECT_EQ(null.group, "root");
        const auto disk = entryNamed(listing, "sda");
        EXPECT_EQ(disk.type, FileType::BlockDevice);
        EXPECT_EQ(disk.group, "disk");
    }

    TEST(ListParserTests, TypesAndSpecialBits)
    {
        const auto listing = listOf(
            "prw-r--r--   1 a b 0 Oct  1 12:34 fifo\n"
            "srwxrwxrwx   1 a b 0 Oct  1 12:34 socket\n"
            "Dr--r--r--   1 a b 0 Oct  1 12:34 door\n"
            "-rwsr-sr-t   1 a b 0 Oct  1 12:34 special-executable\n"
            "-rwSr-Sr-T   1 a b 0 Oct  1 12:34 special-only\n"
            "drwxr-xr-x.  1 a b 0 Oct  1 12:34 selinux-context\n"
            "drwxr-xr-x@  1 a b 0 Oct  1 12:34 extended-attributes\n"
        );
        expectNothingLeftOut(listing);
        EXPECT_EQ(entryNamed(listing, "fifo").type, FileType::Fifo);
        EXPECT_EQ(entryNamed(listing, "socket").type, FileType::Socket);
        EXPECT_EQ(entryNamed(listing, "door").type, FileType::Special);
        EXPECT_EQ(
            entryNamed(listing, "special-executable").permissions,
            perms::owner_read | perms::owner_write | perms::owner_exec | perms::set_uid | perms::group_read |
                perms::group_exec | perms::set_gid | perms::others_read | perms::others_exec | perms::sticky_bit
        );
        EXPECT_EQ(
            entryNamed(listing, "special-only").permissions,
            perms::owner_read | perms::owner_write | perms::set_uid | perms::group_read | perms::set_gid |
                perms::others_read | perms::sticky_bit
        );
        EXPECT_EQ(entryNamed(listing, "selinux-context").type, FileType::Directory);
        EXPECT_EQ(entryNamed(listing, "extended-attributes").type, FileType::Directory);
    }

    TEST(ListParserTests, SymlinkSplitsAtTheFirstArrow)
    {
        const auto link = onlyEntry(listOf("lrwxrwxrwx 1 u g 5 Oct  1 12:34 a -> b -> c\n"));
        EXPECT_EQ(link.name, "a");
        EXPECT_EQ(link.linkTarget, "b -> c");

        const auto withoutTarget = onlyEntry(listOf("lrwxrwxrwx 1 u g 5 Oct  1 12:34 link\n"));
        EXPECT_EQ(withoutTarget.name, "link");
        EXPECT_EQ(withoutTarget.linkTarget, std::nullopt);
    }

    TEST(ListParserTests, MonthsInAnyCase)
    {
        const auto listing = listOf("-rw-r--r-- 1 u g 5 OCT  1 12:34 upper\n-rw-r--r-- 1 u g 5 oct  1 12:34 lower\n");
        EXPECT_EQ(entryNamed(listing, "upper").listedTime, listedAt(2026, 10, 1, 12, 34));
        EXPECT_EQ(entryNamed(listing, "lower").listedTime, listedAt(2026, 10, 1, 12, 34));
    }

    TEST(ListParserTests, YearOfATimeOfDay)
    {
        const auto timeOf = [](std::string_view date, std::chrono::sys_days today)
        {
            return onlyEntry(listOf(fmt::format("-rw-r--r-- 1 u g 5 {} x\n", date), today)).listedTime;
        };
        constexpr std::chrono::sys_days today{std::chrono::year{2026} / 10 / 28};
        EXPECT_EQ(timeOf("Oct 28 23:59", today), listedAt(2026, 10, 28, 23, 59));
        // A day ahead is the time zone of the server.
        EXPECT_EQ(timeOf("Oct 29 10:00", today), listedAt(2026, 10, 29, 10, 0));
        EXPECT_EQ(timeOf("Oct 30 10:00", today), listedAt(2025, 10, 30, 10, 0));
        EXPECT_EQ(
            timeOf("Jan  1 00:30", std::chrono::sys_days{std::chrono::year{2026} / 12 / 31}),
            listedAt(2027, 1, 1, 0, 30)
        );
        EXPECT_EQ(
            timeOf("Dec 31 23:00", std::chrono::sys_days{std::chrono::year{2027} / 1 / 1}),
            listedAt(2026, 12, 31, 23, 0)
        );
        EXPECT_EQ(
            timeOf("Feb 29 12:00", std::chrono::sys_days{std::chrono::year{2029} / 1 / 15}),
            listedAt(2028, 2, 29, 12, 0)
        );
        EXPECT_EQ(
            timeOf("Feb 29 12:00", std::chrono::sys_days{std::chrono::year{2028} / 3 / 1}), listedAt(2028, 2, 29, 12, 0)
        );
    }

    TEST(ListParserTests, TotalAndBlankLinesAreNoEntries)
    {
        const auto listing = listOf("total 12\r\n\r\n-rw-r--r-- 1 u g 5 Oct  1 12:34 x\r\n\r\n");
        EXPECT_EQ(onlyEntry(listing).name, "x");
        expectNothingLeftOut(listing);
        expectNothingLeftOut(listOf("total 0\r\n"));
    }

    TEST(ListParserTests, LinesInNoFormatAreUnreadable)
    {
        const auto listing = listOf(
            "d [R----F--] supervisor            512       Jan 16 18:53 login\n"
            "- [R----F--] rhesus             214059       Oct 20 15:27 cx.exe\n"
            "drwxr-xr-x    3 110      1002            3 Dec 02  209 pub\n"
            "Zrwxrwxrwx   1 root     other          7 Jan 25 00:17 bin -> usr/bin\n"
            "-rwxrwxrwq   1 root     other          7 Jan 25 00:17 bad-letter\n"
            "-rwxrwxrwx*  1 root     other          7 Jan 25 00:17 bad-marker\n"
            "000000000x \n"
            "-rw-r--r-- 1 u g 5 Feb 30  2020 no-such-day\n"
            "-rw-r--r-- 1 u g 5 Oct 32 12:34 day-past-31\n"
            "-rw-r--r-- 1 u g 5 Oct  1 24:00 hour-24\n"
            "-rw-r--r-- 1 u g 5 Oct  1 12:34\n"
            "-rw-r--r-- 1 u g 5 Oct  1 12:34 \n"
            "-rw-r--r-- 1 a b c 5 Oct  1 12:34 owner-with-spaces\n"
            "-rw-r--r-- 1 u g 18446744073709551616 Oct  1 12:34 size-overflow\n"
            "   \n"
            "-rw-r--r-- 1 u g 5 Oct  1 12:34 readable\n"
        );
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"readable"});
        EXPECT_EQ(listing.unreadableLines, 15u);
    }

    TEST(ListParserTests, LeapDayWithoutAYearThatHasOne)
    {
        const auto listing =
            listOf("-rw-r--r-- 1 u g 5 Feb 29 12:00 x\n", std::chrono::sys_days{std::chrono::year{2027} / 3 / 1});
        EXPECT_TRUE(listing.entries.empty());
        EXPECT_EQ(listing.unreadableLines, 1u);
    }

    TEST(ListParserTests, DosLines)
    {
        const auto listing = listOf(
            "08-07-15  07:50PM                  718 Post_PRR_20150901_1166_265118_13049.dat\r\n"
            "08-10-15  02:04PM       <DIR>          Billing\r\n"
            "08-07-2015  07:50PM                  718 four-digit-year.dat\r\n"
            "12-05-96  12:03AM       <DIR>          midnight\r\n"
            "12-05-96  12:03PM                    0 noon\r\n"
            "01-02-69  13:05                     12 twenty-four-hours\r\n"
            "01-02-68  01:05am       <dir>          My Documents\r\n"
        );
        expectNothingLeftOut(listing);
        EXPECT_EQ(listing.entries.size(), 7u);

        const auto file = entryNamed(listing, "Post_PRR_20150901_1166_265118_13049.dat");
        EXPECT_EQ(file.type, FileType::Regular);
        EXPECT_EQ(file.size, 718u);
        EXPECT_EQ(file.listedTime, listedAt(2015, 8, 7, 19, 50));
        const auto directory = entryNamed(listing, "Billing");
        EXPECT_EQ(directory.type, FileType::Directory);
        EXPECT_EQ(directory.size, std::nullopt);
        EXPECT_EQ(directory.listedTime, listedAt(2015, 8, 10, 14, 4));

        EXPECT_EQ(entryNamed(listing, "four-digit-year.dat").listedTime, listedAt(2015, 8, 7, 19, 50));
        EXPECT_EQ(entryNamed(listing, "midnight").listedTime, listedAt(1996, 12, 5, 0, 3));
        EXPECT_EQ(entryNamed(listing, "noon").listedTime, listedAt(1996, 12, 5, 12, 3));
        EXPECT_EQ(entryNamed(listing, "twenty-four-hours").listedTime, listedAt(1969, 1, 2, 13, 5));
        EXPECT_EQ(entryNamed(listing, "My Documents").listedTime, listedAt(2068, 1, 2, 1, 5));
        EXPECT_EQ(entryNamed(listing, "My Documents").type, FileType::Directory);
    }

    TEST(ListParserTests, DosLinesInNoFormatAreUnreadable)
    {
        const auto listing = listOf(
            "13-07-15  07:50PM                  718 month-13\r\n"
            "02-30-15  07:50PM                  718 no-such-day\r\n"
            "08-07-15  13:50PM                  718 pm-past-12\r\n"
            "08-07-15  00:50AM                  718 am-hour-0\r\n"
            "08-07-15  07:50XM                  718 no-half-of-day\r\n"
            "08-07-15  07:50PM                  12a not-a-size\r\n"
            "08-07-15  07:50PM       <DIR>\r\n"
            "08-07-15  07:50PM       <DIR>          \r\n"
            "8-7-15  07:50PM                  718 short-date\r\n"
            "0x-07-15  07:50PM                  718 hexadecimal-month\r\n"
            "08-07-15  07:50PM                  718 readable\r\n"
        );
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"readable"});
        EXPECT_EQ(listing.unreadableLines, 10u);
    }

    TEST(NameListTests, NamesAsSent)
    {
        const auto listing = parsed(ListingParser::nameList(""), "a\r\n b \r\nlisting/c\r\n");
        EXPECT_EQ(namesOf(listing), (std::set<std::string>{"a", " b "}));
        EXPECT_EQ(listing.refusedNames, 1u);
    }

    TEST(NameListTests, ListedPathWithAndWithoutSlash)
    {
        EXPECT_EQ(onlyEntry(parsed(ListingParser::nameList("dir"), "dir/x\n")).name, "x");
        EXPECT_EQ(onlyEntry(parsed(ListingParser::nameList("dir/"), "dir/x\n")).name, "x");
        EXPECT_EQ(onlyEntry(parsed(ListingParser::nameList("/"), "/x\n")).name, "x");
        EXPECT_EQ(onlyEntry(parsed(ListingParser::nameList("dir"), "x\n")).name, "x");
    }

    TEST(NameListTests, NamesOutsideTheListedPathAreRefused)
    {
        const auto listing = parsed(ListingParser::nameList("dir"), "other/x\ndir/sub/y\ndir/z\n");
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"z"});
        EXPECT_EQ(listing.refusedNames, 2u);
    }

    TEST(ListingParserTests, NamesThatNameNoChildAreRefused)
    {
        const std::string tooLong(ListingParser::maximumNameLength + 1u, 'n');
        const std::string longest(ListingParser::maximumNameLength, 'n');
        const auto listing = machineListingOf(
            fmt::format(
                "type=file; \n"
                "type=file; a/b\n"
                "type=file; /\n"
                "type=file; nul{}inside\n"
                "type=file; carriage\rreturn\n"
                "type=file; {}\n"
                "type=file; {}\n"
                "type=file; back\\slash\n",
                std::string_view{"\0", 1u},
                tooLong,
                longest
            )
        );
        EXPECT_EQ(namesOf(listing), (std::set<std::string>{longest, "back\\slash"}));
        EXPECT_EQ(listing.refusedNames, 6u);
    }

    TEST(ListingParserTests, DotAndDotDotAreLeftOutWithoutCounting)
    {
        const auto listing = machineListingOf("type=file; .\ntype=dir; ..\ntype=file; ...\n");
        EXPECT_EQ(namesOf(listing), std::set<std::string>{"..."});
        expectNothingLeftOut(listing);
    }

    TEST(ListingParserTests, RepeatedNamesKeepTheFirst)
    {
        // RFC 3659 7.5.1.6: an item of several types is listed once per type.
        const auto listing =
            machineListingOf("type=file;size=1; twice\ntype=dir; twice\ntype=file; other\ntype=file;size=3; twice\n");
        EXPECT_EQ(listing.entries.size(), 2u);
        EXPECT_EQ(listing.repeatedNames, 2u);
        EXPECT_EQ(listing.entries.front().name, "twice");
        EXPECT_EQ(listing.entries.front().size, 1u);
        EXPECT_EQ(listing.entries.back().name, "other");
    }

    TEST(ListingParserTests, LineEndsOfAnyKind)
    {
        const auto crlf = machineListingOf("type=file; a\r\ntype=file; b\r\n");
        const auto lineFeeds = machineListingOf("type=file; a\ntype=file; b\n");
        const auto lastWithoutEnd = machineListingOf("type=file; a\r\ntype=file; b");
        EXPECT_EQ(namesOf(crlf), (std::set<std::string>{"a", "b"}));
        EXPECT_EQ(crlf, lineFeeds);
        EXPECT_EQ(crlf, lastWithoutEnd);
    }

    TEST(ListingParserTests, SameListingInAnyPieces)
    {
        const auto bytes = listingSample("pure-ftpd-mlsd.txt") + listingSample("proftpd-mlsd.txt") + "type=file; last";
        const auto whole = machineListingOf(bytes);
        EXPECT_EQ(whole.repeatedNames, 10u);

        for (std::size_t pieceSize = 1u; pieceSize < 40u; ++pieceSize)
        {
            auto parser = ListingParser::machineListing();
            for (std::size_t position = 0u; position < bytes.size(); position += pieceSize)
                parser.feed(std::string_view{bytes}.substr(position, pieceSize));
            EXPECT_EQ(parser.finish(), whole) << "pieces of " << pieceSize;
        }
    }

    TEST(ListingParserTests, LongLinesAreSkipped)
    {
        const std::string longName(ListingParser::maximumLineLength, 'n');
        const auto bytes = fmt::format("type=file; {}\r\ntype=file; after\r\n", longName);
        const auto whole = machineListingOf(bytes);
        EXPECT_EQ(namesOf(whole), std::set<std::string>{"after"});
        EXPECT_EQ(whole.unreadableLines, 1u);

        auto parser = ListingParser::machineListing();
        for (std::size_t position = 0u; position < bytes.size(); position += 1000u)
            parser.feed(std::string_view{bytes}.substr(position, 1000u));
        EXPECT_EQ(parser.finish(), whole);

        auto unfinished = ListingParser::machineListing();
        unfinished.feed(fmt::format("type=file; {}", longName));
        EXPECT_EQ(unfinished.finish().unreadableLines, 1u);
    }

    TEST(ListingParserTests, LongestLineIsRead)
    {
        const std::string prefix = "type=file; ";
        const std::string name(ListingParser::maximumNameLength, 'n');
        const std::string facts(ListingParser::maximumLineLength - prefix.size() - name.size() - 2u, 'x');
        // One fact as long as the line allows: x...x=1; then the name and CRLF.
        const auto line = fmt::format("{}=1;{}{}\r\n", facts.substr(0u, facts.size() - 3u), prefix, name);
        ASSERT_EQ(line.size(), ListingParser::maximumLineLength);
        EXPECT_EQ(onlyEntry(machineListingOf(line)).name, name);
    }

    TEST(ListingParserTests, FinishStartsOver)
    {
        auto parser = ListingParser::machineListing();
        parser.feed("type=file; first\ntype=file; unfin");
        EXPECT_EQ(namesOf(parser.finish()), (std::set<std::string>{"first", "unfin"}));
        parser.feed("type=file; second\n");
        EXPECT_EQ(namesOf(parser.finish()), std::set<std::string>{"second"});
    }

    TEST(MachineListReplyTests, EntryOfTheReply)
    {
        const Reply reply{
            .code = 250,
            .lines = {
                "250-Begin",
                " type=file;size=11;modify=20261001123456;UNIX.mode=0644;UNIX.uid=1000;UNIX.gid=1000;"
                "unique=7bg1002d0a; listing/plain.txt",
                "250 End."
            },
        };
        const auto entry = parseMachineListReply(reply);
        ASSERT_TRUE(entry);
        EXPECT_EQ(entry->name, "listing/plain.txt");
        EXPECT_EQ(entry->type, FileType::Regular);
        EXPECT_EQ(entry->size, 11u);
        EXPECT_EQ(entry->modified, utc(2026, 10, 1, 12, 34, 56));
    }

    TEST(MachineListReplyTests, CurrentDirectoryIsADirectory)
    {
        const auto entry = parseMachineListReply(
            Reply{
                .code = 250,
                .lines = {"250- Listing /", " Type=cdir;Perm=el;Unique=keVO1+ZF4; /", "250 End"},
            }
        );
        ASSERT_TRUE(entry);
        EXPECT_EQ(entry->type, FileType::Directory);
        EXPECT_EQ(entry->name, "/");
    }

    TEST(MachineListReplyTests, NameMayBeEmpty)
    {
        const auto entry = parseMachineListReply(Reply{.code = 250, .lines = {"250-x", " type=file; ", "250 y"}});
        ASSERT_TRUE(entry);
        EXPECT_TRUE(entry->name.empty());
    }

    TEST(MachineListReplyTests, RefusedReply)
    {
        const Reply reply{.code = 550, .lines = {"550 'listing/dangling-link' cannot be listed"}};
        const auto entry = parseMachineListReply(reply);
        ASSERT_FALSE(entry);
        EXPECT_EQ(entry.error().kind, replyError(reply, Verb::MachineListSingle).kind);
    }

    TEST(MachineListReplyTests, ReplyWithoutAReadableEntry)
    {
        for (
            auto const& reply : {
                Reply{.code = 250, .lines = {"250 Done"}},
                Reply{.code = 250, .lines = {"250-Begin", "type=file; no-leading-space", "250 End"}},
                Reply{.code = 250, .lines = {"250-Begin", " type=file;size=1 no-semicolon", "250 End"}},
            }
        )
        {
            const auto entry = parseMachineListReply(reply);
            ASSERT_FALSE(entry);
            EXPECT_EQ(entry.error().kind, RemoteIo::RemoteErrorKind::Failure);
            EXPECT_EQ(entry.error().protocolCode, 250);
        }
    }
}
