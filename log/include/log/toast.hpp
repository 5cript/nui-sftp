#pragma once

namespace Log
{
    /**
     * @brief Whether a message also reaches the frontend's log hooks, which show errors to the user as toasts. The log
     *        and the console get it either way. Failures the user is shown elsewhere, or that are retried, skip it.
     */
    enum class Toast
    {
        Show,
        Skip,
    };
}
