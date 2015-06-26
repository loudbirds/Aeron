/*
 * Copyright 2015 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <concurrent/logbuffer/DataFrameHeader.h>
#include "ClientConductorFixture.h"

using namespace aeron::concurrent;
using namespace aeron;

#define TERM_LENGTH (LogBufferDescriptor::TERM_MIN_LENGTH)
#define TERM_META_DATA_LENGTH (LogBufferDescriptor::TERM_META_DATA_LENGTH)
#define LOG_META_DATA_LENGTH (LogBufferDescriptor::LOG_META_DATA_LENGTH)
#define SRC_BUFFER_LENGTH 1024

typedef std::array<std::uint8_t, ((TERM_LENGTH * 3) + (TERM_META_DATA_LENGTH * 3) + LOG_META_DATA_LENGTH)> log_buffer_t;
typedef std::array<std::uint8_t, SRC_BUFFER_LENGTH> src_buffer_t;

static const std::string CHANNEL = "udp://localhost:40123";
static const std::int32_t STREAM_ID = 10;
static const std::int32_t SESSION_ID = 200;
static const std::int32_t SUBSCRIBER_POSITION_ID = 0;

static const std::int64_t CORRELATION_ID = 100;
static const std::int32_t TERM_ID_1 = 1;

static const std::array<std::uint8_t, 17> DATA = { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 } };

static const std::int32_t INITIAL_TERM_ID = 0xFEDA;
static const std::int32_t POSITION_BITS_TO_SHIFT = BitUtil::numberOfTrailingZeroes(TERM_LENGTH);
static const util::index_t ALIGNED_FRAME_LENGTH =
    BitUtil::align(DataFrameHeader::LENGTH + (std::int32_t)DATA.size(), FrameDescriptor::FRAME_ALIGNMENT);

class ConnectionTest : public testing::Test, ClientConductorFixture
{
public:
    ConnectionTest() :
        m_srcBuffer(&m_src[0], m_src.size()),
        m_subscriberPosition(m_counterValuesBuffer, SUBSCRIBER_POSITION_ID)
    {
        m_log.fill(0);

        for (int i = 0; i < 3; i++)
        {
            m_termBuffers[i].wrap(&(m_log[i * TERM_LENGTH]), TERM_LENGTH);
            m_metaDataBuffers[i].wrap(&(m_log[(TERM_LENGTH * 3) + (i * TERM_META_DATA_LENGTH)]), TERM_META_DATA_LENGTH);
        }

        m_logMetaDataBuffer.wrap(&m_log[m_log.size() - LOG_META_DATA_LENGTH], LOG_META_DATA_LENGTH);

        m_logMetaDataBuffer.putInt32(LogBufferDescriptor::LOG_MTU_LENGTH_OFFSET, (3 * m_srcBuffer.capacity()));

        m_logBuffers = std::unique_ptr<LogBuffers>(new LogBuffers(&m_log[0], (index_t)m_log.size()));
    }

    virtual void SetUp()
    {
        m_log.fill(0);

        m_logMetaDataBuffer.putInt32(LogBufferDescriptor::LOG_ACTIVE_TERM_ID_OFFSET, TERM_ID_1);
        m_logMetaDataBuffer.putInt32(LogBufferDescriptor::LOG_INITIAL_TERM_ID_OFFSET, TERM_ID_1);
        m_logMetaDataBuffer.putInt32(LogBufferDescriptor::LOG_MTU_LENGTH_OFFSET, (3 * m_srcBuffer.capacity()));
    }

    void insertDataFrame(std::int32_t activeTermId, std::int32_t offset)
    {
        int termBufferIndex = LogBufferDescriptor::indexByTerm(INITIAL_TERM_ID, activeTermId);
        AtomicBuffer& buffer = m_termBuffers[termBufferIndex];
        DataFrameHeader::DataFrameHeaderDefn& frame = buffer.overlayStruct<DataFrameHeader::DataFrameHeaderDefn>(offset);

        frame.frameLength = DataFrameHeader::LENGTH + DATA.size();
        frame.version = DataFrameHeader::CURRENT_VERSION;
        frame.flags = FrameDescriptor::UNFRAGMENTED;
        frame.type = DataFrameHeader::HDR_TYPE_DATA;
        frame.termOffset = offset;
        frame.sessionId = SESSION_ID;
        frame.streamId = STREAM_ID;
        frame.termId = activeTermId;
        buffer.putBytes(offset + DataFrameHeader::LENGTH, (std::uint8_t *)&DATA[0], DATA.size());
    }

    inline util::index_t offsetOfFrame(std::int32_t index)
    {
        return (util::index_t)(index * ALIGNED_FRAME_LENGTH);
    }

protected:
    AERON_DECL_ALIGNED(log_buffer_t m_log, 16);
    AERON_DECL_ALIGNED(src_buffer_t m_src, 16);

    AtomicBuffer m_termBuffers[3];
    AtomicBuffer m_metaDataBuffers[3];
    AtomicBuffer m_logMetaDataBuffer;
    AtomicBuffer m_srcBuffer;

    std::unique_ptr<LogBuffers> m_logBuffers;
    UnsafeBufferPosition m_subscriberPosition;
};

class MockFragmentHandler
{
public:
    MOCK_CONST_METHOD4(onFragment, void(AtomicBuffer&, util::index_t, util::index_t, Header&));

    static fragment_handler_t newFragmentHandler(MockFragmentHandler& handler)
    {
        return [&](AtomicBuffer& buffer, util::index_t offset, util::index_t length, Header& header)
        {
            handler.onFragment(buffer, offset, length, header);
        };
    }
};

TEST_F(ConnectionTest, shouldReportCorrectPositionOnReception)
{
    const std::int32_t messageIndex = 0;
    const std::int32_t initialTermOffset = offsetOfFrame(messageIndex);
    const std::int64_t initialPosition =
        LogBufferDescriptor::computePosition(INITIAL_TERM_ID, initialTermOffset, POSITION_BITS_TO_SHIFT, INITIAL_TERM_ID);
    Connection connection(SESSION_ID, initialPosition, CORRELATION_ID, m_subscriberPosition, *m_logBuffers);
    MockFragmentHandler fragmentHandler;

    EXPECT_EQ(m_subscriberPosition.get(), initialPosition);

    insertDataFrame(INITIAL_TERM_ID, offsetOfFrame(messageIndex));

    EXPECT_CALL(fragmentHandler, onFragment(testing::_, DataFrameHeader::LENGTH, (util::index_t)DATA.size(), testing::_))
        .Times(1);

    const int fragments = connection.poll(MockFragmentHandler::newFragmentHandler(fragmentHandler), INT_MAX);
    EXPECT_EQ(fragments, 1);
    EXPECT_EQ(m_subscriberPosition.get(), initialPosition + ALIGNED_FRAME_LENGTH);
}

TEST_F(ConnectionTest, shouldReportCorrectPositionOnReceptionWithNonZeroPositionInInitialTermId)
{
    const std::int32_t messageIndex = 5;
    const std::int32_t initialTermOffset = offsetOfFrame(messageIndex);
    const std::int64_t initialPosition =
        LogBufferDescriptor::computePosition(INITIAL_TERM_ID, initialTermOffset, POSITION_BITS_TO_SHIFT, INITIAL_TERM_ID);
    Connection connection(SESSION_ID, initialPosition, CORRELATION_ID, m_subscriberPosition, *m_logBuffers);
    MockFragmentHandler fragmentHandler;

    EXPECT_EQ(m_subscriberPosition.get(), initialPosition);

    insertDataFrame(INITIAL_TERM_ID, offsetOfFrame(messageIndex));

    EXPECT_CALL(fragmentHandler, onFragment(testing::_, initialTermOffset + DataFrameHeader::LENGTH, (util::index_t)DATA.size(), testing::_))
        .Times(1);

    const int fragments = connection.poll(MockFragmentHandler::newFragmentHandler(fragmentHandler), INT_MAX);
    EXPECT_EQ(fragments, 1);
    EXPECT_EQ(m_subscriberPosition.get(), initialPosition + ALIGNED_FRAME_LENGTH);
}

TEST_F(ConnectionTest, shouldReportCorrectPositionOnReceptionWithNonZeroPositionInNonInitialTermId)
{
    const std::int32_t activeTermId = INITIAL_TERM_ID + 1;
    const std::int32_t messageIndex = 5;
    const std::int32_t initialTermOffset = offsetOfFrame(messageIndex);
    const std::int64_t initialPosition =
        LogBufferDescriptor::computePosition(activeTermId, initialTermOffset, POSITION_BITS_TO_SHIFT, INITIAL_TERM_ID);
    Connection connection(SESSION_ID, initialPosition, CORRELATION_ID, m_subscriberPosition, *m_logBuffers);
    MockFragmentHandler fragmentHandler;

    EXPECT_EQ(m_subscriberPosition.get(), initialPosition);

    insertDataFrame(activeTermId, offsetOfFrame(messageIndex));

    EXPECT_CALL(fragmentHandler, onFragment(testing::_, initialTermOffset + DataFrameHeader::LENGTH, (util::index_t)DATA.size(), testing::_))
        .Times(1);

    const int fragments = connection.poll(MockFragmentHandler::newFragmentHandler(fragmentHandler), INT_MAX);
    EXPECT_EQ(fragments, 1);
    EXPECT_EQ(m_subscriberPosition.get(), initialPosition + ALIGNED_FRAME_LENGTH);
}
