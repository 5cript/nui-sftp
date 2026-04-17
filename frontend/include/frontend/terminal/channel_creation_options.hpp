#pragma once

/**
 * @brief Polymorphic base for options passed into TerminalEngine::createChannel.
 *
 * Each engine defines its own derived struct carrying the fields it needs
 * (e.g. ExecutingChannelCreationOptions for local process spawns). The engine
 * downcasts via dynamic_cast to pick up its own type. Engines that don't need
 * per-call options can accept a plain-base instance and ignore it.
 *
 * Keeping options per-call — rather than captured by the engine constructor —
 * lets settings changes take effect on the next spawned channel without
 * rebuilding the engine.
 */
struct ChannelCreationOptions
{
    virtual ~ChannelCreationOptions() = default;
};
