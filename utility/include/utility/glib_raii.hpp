#pragma once

#include <glib.h>
#include <glib-object.h>

#include <memory>
#include <string>

namespace Utility
{
    /**
     *  @brief RAII wrappers for the GLib / GIO resources that callers tend to repeat
     *         conditional cleanup for. Each deleter is a no-op on null so the aliases
     *         can be default-constructed, reset from raw GLib output parameters, and
     *         moved around without `if (x) g_*_free(x);` at every call site.
     */
    struct GErrorDeleter
    {
        void operator()(GError* e) const noexcept
        {
            if (e)
                g_error_free(e);
        }
    };
    struct GVariantDeleter
    {
        void operator()(GVariant* v) const noexcept
        {
            if (v)
                g_variant_unref(v);
        }
    };
    struct GFreeDeleter
    {
        void operator()(void* p) const noexcept
        {
            if (p)
                g_free(p);
        }
    };
    struct GObjectDeleter
    {
        template <typename T>
        void operator()(T* o) const noexcept
        {
            if (o)
                g_object_unref(o);
        }
    };

    using GErrorPtr = std::unique_ptr<GError, GErrorDeleter>;
    using GVariantPtr = std::unique_ptr<GVariant, GVariantDeleter>;
    using GcharPtr = std::unique_ptr<gchar, GFreeDeleter>;

    /**
     *  @brief Owning unique_ptr for any GObject-derived type (GDBusConnection, GUnixFDList,
     *         GDBusProxy, etc.). Use directly: `Utility::GObjectPtr<GDBusConnection>`.
     */
    template <typename T>
    using GObjectPtr = std::unique_ptr<T, GObjectDeleter>;

    /**
     *  @brief Adopt a raw GError* output and collapse it to a message string. The error
     *         object is freed at scope exit. Pass @p fallback for the case where the
     *         GLib call set @c result=nullptr without populating an error (rare but
     *         allowed by the API contract for many functions).
     */
    inline std::string consumeGError(GError* raw, char const* fallback)
    {
        GErrorPtr err{raw};
        return err && err->message ? std::string{err->message} : std::string{fallback};
    }
}
