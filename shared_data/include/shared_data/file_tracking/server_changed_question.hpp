#pragma once

#include <ids/ids.hpp>
#include <shared_data/shared_data.hpp>
#include <utility/describe.hpp>
#include <paths/paths.hpp>

namespace SharedData
{
    /**
     * @brief The answer to a ServerChangedQuestion. Replace uploads the local version, Keep leaves the server's and
     *        asks again on the next save. Dismissed: another dialog took the place of the question, it and the
     *        waiting ones are asked again on the next save.
     */
    BOOST_DEFINE_ENUM_CLASS(ServerChangedAnswer, Replace, Keep, Dismissed)

    /**
     * @brief The tracker asks whether a local change may replace the server's copy at remotePath, which changed since
     *        it was opened or last uploaded.
     */
    struct ServerChangedQuestion
    {
        Ids::QuestionId questionId;
        Paths::RemotePath remotePath;
    };
    BOOST_DESCRIBE_STRUCT(ServerChangedQuestion, (), (questionId, remotePath))
}
