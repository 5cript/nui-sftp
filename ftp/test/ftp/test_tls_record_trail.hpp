#pragma once

#include <ftp/client/tls_record_trail.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace FileTransferProtocol::Test
{
    namespace
    {
        std::string tlsRecord(unsigned char contentType, std::size_t length)
        {
            std::string record{
                static_cast<char>(contentType),
                '\x03',
                '\x03',
                static_cast<char>(length >> 8u),
                static_cast<char>(length & 0xffu)
            };
            record.append(length, '\x17');
            return record;
        }

        /** @brief A session ticket and application data, as a server sends them right after the handshake. */
        std::string recordsBeforePlaintext()
        {
            return tlsRecord(23u, 250u) + tlsRecord(23u, 3u);
        }
    }

    TEST(TlsRecordTrailTests, RecordsLeaveNothingOutside)
    {
        TlsRecordTrail trail{};
        trail.feed(recordsBeforePlaintext());
        EXPECT_EQ(trail.outsideRecords(), "");
    }

    TEST(TlsRecordTrailTests, PlaintextBehindRecordsInTheSameReadIsKept)
    {
        TlsRecordTrail trail{};
        trail.feed(recordsBeforePlaintext() + "500 OOPS: vsf_sysutil_bind\r\n");
        EXPECT_EQ(trail.outsideRecords(), "500 OOPS: vsf_sysutil_bind\r\n");
    }

    TEST(TlsRecordTrailTests, PlaintextIsFoundWhereverTheReadsCutTheRecords)
    {
        const auto bytes = recordsBeforePlaintext() + "500 OOPS: vsf_sysutil_bind\r\n";
        for (std::size_t piece = 1u; piece <= 7u; ++piece)
        {
            TlsRecordTrail trail{};
            for (std::size_t offset = 0u; offset < bytes.size(); offset += piece)
                trail.feed(std::string_view{bytes}.substr(offset, piece));
            EXPECT_EQ(trail.outsideRecords(), "500 OOPS: vsf_sysutil_bind\r\n") << "pieces of " << piece;
        }
    }

    TEST(TlsRecordTrailTests, PlaintextShorterThanAHeaderIsKept)
    {
        TlsRecordTrail trail{};
        trail.feed(recordsBeforePlaintext() + "500");
        EXPECT_EQ(trail.outsideRecords(), "500");
    }

    TEST(TlsRecordTrailTests, AHeaderTooLongForTlsIsNoRecord)
    {
        TlsRecordTrail trail{};
        const auto header = tlsRecord(23u, 0u).substr(0u, 3u) + "\xff\xff";
        trail.feed(header);
        EXPECT_EQ(trail.outsideRecords(), header);
    }

    TEST(TlsRecordTrailTests, WhatFollowsIsKeptUpToItsLimit)
    {
        TlsRecordTrail trail{};
        trail.feed("500 ");
        trail.feed(std::string(2u * TlsRecordTrail::keptBytes, 'x'));
        EXPECT_EQ(trail.outsideRecords().size(), TlsRecordTrail::keptBytes);
        EXPECT_TRUE(trail.outsideRecords().starts_with("500 x"));
    }
}
