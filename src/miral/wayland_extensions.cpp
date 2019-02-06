/*
 * Copyright © 2018 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Alan Griffiths <alan@octopull.co.uk>
 */

#include "miral/wayland_extensions.h"

#include <mir/server.h>
#include <mir/options/option.h>
#include <mir/options/configuration.h>

#include <set>
#include <mir/abnormal_exit.h>

namespace mo = mir::options;

struct miral::WaylandExtensions::Self
{
    Self(std::string const& default_value) : default_value{default_value}
    {
    }

    void callback(mir::Server& server) const
    {
        validate(server.get_options()->get<std::string>(mo::wayland_extensions_opt));
        // TODO pass bespoke stuff into server!
    }

    std::string const default_value;

    void validate(std::string extensions) const
    {
        std::set<std::string> selected_extension;
        auto extensions_ = extensions + ':';

        for (char const* start = extensions_.c_str(); char const* end = strchr(start, ':'); start = end+1)
        {
            if (start != end)
                selected_extension.insert(std::string{start, end});
        }

        std::set<std::string> supported_extension;
        std::string available_extensions = mo::wayland_extensions_value;
        for (auto const& extension : wayland_extension_hooks)
            available_extensions += ":" + extension.name;
        available_extensions += ":zwlr_layer_shell_v1";
        available_extensions += ':';

        for (char const* start = available_extensions.c_str(); char const* end = strchr(start, ':'); start = end+1)
        {
            if (start != end)
                supported_extension.insert(std::string{start, end});
        }

        std::set<std::string> errors;

        set_difference(begin(selected_extension), end(selected_extension),
                       begin(supported_extension), end(supported_extension),
                       std::inserter(errors, begin(errors)));

        if (!errors.empty())
        {
            throw mir::AbnormalExit{"Unsupported wayland extensions in: " + extensions};
        }
    }

    struct WaylandExtensionHook
    {
        std::string name;
        std::function<std::shared_ptr<void>(wl_display*)> builder;
    };

    std::vector<WaylandExtensionHook> wayland_extension_hooks;

    void add_extension(std::string const& name, std::function<std::shared_ptr<void>(wl_display*)> builder)
    {
        wayland_extension_hooks.push_back({name, builder});
    }
};

miral::WaylandExtensions::WaylandExtensions() :
    WaylandExtensions{mo::wayland_extensions_value}
{
}

miral::WaylandExtensions::WaylandExtensions(std::string const& default_value) :
    self{std::make_shared<Self>(default_value)}
{
}

auto miral::WaylandExtensions::supported_extensions() const -> std::string
{
    return mo::wayland_extensions_value;
}

void miral::WaylandExtensions::operator()(mir::Server& server) const
{
    self->validate(self->default_value);
    server.add_configuration_option(mo::wayland_extensions_opt, "Wayland extensions to enable", self->default_value);

    server.add_pre_init_callback([self=self, &server]
        {
            for (auto const& hook : self->wayland_extension_hooks)
                server.add_wayland_extension(hook.name, hook.builder);
        });

    server.add_init_callback([this, &server]{ self->callback(server); });
}

miral::WaylandExtensions::~WaylandExtensions() = default;
miral::WaylandExtensions::WaylandExtensions(WaylandExtensions const&) = default;
auto miral::WaylandExtensions::operator=(WaylandExtensions const&) -> WaylandExtensions& = default;

auto miral::with_extension(
    WaylandExtensions const& wayland_extensions,
    std::string const& name, std::function<std::shared_ptr<void>(wl_display*)> builder) -> WaylandExtensions
{
    wayland_extensions.self->add_extension(name, builder);
    return wayland_extensions;
}
