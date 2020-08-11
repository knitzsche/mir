/*
 * Copyright (C) 2018 Marius Gripsgard <marius@ubports.com>
 * Copyright (C) 2019 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 or 3,
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
 */

#include "xwayland_connector.h"

#include "wayland_connector.h"
#include "xwayland_server.h"
#include "xwayland_spawner.h"
#include "xwayland_wm.h"
#include "mir/log.h"
#include "mir/dispatch/multiplexing_dispatchable.h"
#include "mir/dispatch/readable_fd.h"
#include "mir/executor.h"

#include <unistd.h>

namespace mf = mir::frontend;
namespace md = mir::dispatch;

mf::XWaylandConnector::XWaylandConnector(
    std::shared_ptr<Executor> const& main_loop,
    std::shared_ptr<WaylandConnector> const& wayland_connector,
    std::string const& xwayland_path)
    : main_loop{main_loop},
      wayland_connector{wayland_connector},
      xwayland_path{xwayland_path}
{
    if (access(xwayland_path.c_str(), F_OK | X_OK) != 0)
    {
        fatal_error("Cannot execute Xwayland: --xwayland-path %s", xwayland_path.c_str());
    }
}

mf::XWaylandConnector::~XWaylandConnector()
{
    if (spawner || server || wm || wm_event_thread)
    {
        fatal_error(
            "XWaylandConnector was not stopped before being destroyed "
            "(spawner: %s, server: %s, wm: %s, wm_event_thread: %s)",
            spawner ? "exists" : "null",
            wm ? "exists" : "null",
            spawner ? "exists" : "null",
            wm_event_thread ? "exists" : "null");
    }
}

void mf::XWaylandConnector::start()
{
    if (wayland_connector->get_extension("x11-support"))
    {
        std::unique_lock<std::mutex> lock{mutex};
        create_spawner(lock);
        mir::log_info("Listening for X11 connections on DISPLAY %s", spawner->x11_display().c_str());
    }
}

void mf::XWaylandConnector::stop()
{
    std::unique_lock<std::mutex> lock{mutex};
    restart_in_progress = false;
    tear_down(lock);
}

int mf::XWaylandConnector::client_socket_fd() const
{
    return -1;
}

int mf::XWaylandConnector::client_socket_fd(
    std::function<void(std::shared_ptr<scene::Session> const& session)> const& /*connect_handler*/) const
{
    return -1;
}

auto mf::XWaylandConnector::socket_name() const -> optional_value<std::string>
{
    std::lock_guard<std::mutex> lock{mutex};

    if (spawner)
    {
        return spawner->x11_display();
    }
    else
    {
        return optional_value<std::string>();
    }
}

void mf::XWaylandConnector::create_spawner(std::unique_lock<std::mutex>& lock)
{
    if (!lock)
    {
        lock.lock();
    }

    if (spawner)
    {
        return;
    }

    spawner = std::make_unique<XWaylandSpawner>([this]() { spawn(); });
}

void mf::XWaylandConnector::tear_down(std::unique_lock<std::mutex>& lock)
{
    if (!lock)
    {
        lock.lock();
    }

    bool const was_running{server};

    auto local_spawner{std::move(spawner)};
    auto local_wm_event_thread{std::move(wm_event_thread)};
    auto local_wm{std::move(wm)};
    auto local_server{std::move(server)};

    lock.unlock();

    local_spawner.reset();
    local_wm_event_thread.reset();
    local_wm.reset();
    local_server.reset();

    if (was_running)
    {
        mir::log_info("XWayland stopped");
    }
}

void mf::XWaylandConnector::spawn()
{
    std::lock_guard<std::mutex> lock{mutex};

    if (server || !spawner || restart_in_progress)
    {
        // If we have a server then we've already spawned
        // If we don't have a spawner then this connector has been stopped or never started (and shouldn't spawn)
        // If a restart is in progress than we shouldn't spawn
        // In any case, nothing to do
        return;
    }

    try
    {
        server = std::make_unique<XWaylandServer>(
            wayland_connector,
            *spawner,
            xwayland_path);
        wm = std::make_unique<XWaylandWM>(
            wayland_connector,
            server->client(),
            server->wm_fd());
        auto const wm_dispatcher = std::make_shared<md::ReadableFd>(server->wm_fd(), [this]()
            {
                std::lock_guard<std::mutex> lock{mutex};
                if (wm)
                {
                    wm->handle_events();
                }
            });
        wm_event_thread = std::make_unique<mir::dispatch::ThreadedDispatcher>(
            "Mir/X11 WM Reader",
            wm_dispatcher,
            [this]()
            {
                // The window manager threw an exception handling X11 events

                log(
                    logging::Severity::error,
                    MIR_LOG_COMPONENT,
                    std::current_exception(),
                    "X11 window manager error, killing XWayland");

                std::lock_guard<std::mutex> lock{mutex};
                restart(lock);
            });
        mir::log_info("XWayland is running");
    }
    catch (...)
    {
        log(
            logging::Severity::error,
            MIR_LOG_COMPONENT,
            std::current_exception(),
            "Spawning XWayland failed");

        restart(lock);
    }
}

void mf::XWaylandConnector::restart(std::lock_guard<std::mutex> const&)
{
    restart_in_progress = true;

    std::weak_ptr<XWaylandConnector> weak{shared_from_this()};

    // We can't destroy our components from inside a call from those same components,
    // so we call tear_down() on the main loop instead
    main_loop->spawn([weak]()
        {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            std::unique_lock<std::mutex> lock{self->mutex};

            if (!self->restart_in_progress)
            {
                return;
            }
            self->tear_down(lock);

            if (!lock)
            {
                lock.lock();
            }
            if (!self->restart_in_progress)
            {
                return;
            }
            self->restart_in_progress = false;
            self->create_spawner(lock);
        });
}
