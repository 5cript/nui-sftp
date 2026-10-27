#include <backend/reduced_motion.hpp>

namespace
{
    /**
     * @brief Value of the prefers-reduced-motion media feature, empty for the system's.
     */
    std::string_view featureValue(Persistence::ReducedMotion preference)
    {
        switch (preference)
        {
            case Persistence::ReducedMotion::Reduce:
                return "reduce";
            case Persistence::ReducedMotion::NoPreference:
                return "no-preference";
            case Persistence::ReducedMotion::System:
                return "";
        }
        return "";
    }
}

nlohmann::json reducedMotionEmulationParameters(Persistence::ReducedMotion preference)
{
    return nlohmann::json{
        {"features",
            nlohmann::json::array({
                {{"name", "prefers-reduced-motion"}, {"value", featureValue(preference)}},
            })},
    };
}
