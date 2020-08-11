/*
 * Copyright (C) 2018 Marius Gripsgard <marius@ubports.com>
 * Copyright (C) 2019-2020 Canonical Ltd.
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

#include "xwayland_server.h"
#include "xwayland_spawner.h"
#include "wayland_connector.h"
#include "mir/log.h"

#include <boost/throw_exception.hpp>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>

namespace mf = mir::frontend;
namespace md = mir::dispatch;
using namespace std::chrono_literals;

namespace
{
/// client and server are symetrical, they only differ in how they are used
struct SocketPair
{
    mir::Fd client;
    mir::Fd server;
};

auto make_socket_pair() -> SocketPair
{
    int pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pipe) < 0)
    {
        BOOST_THROW_EXCEPTION(std::system_error(errno, std::system_category(), "Creating socket pair failed"));
    }
    return SocketPair{mir::Fd{pipe[0]}, mir::Fd{pipe[1]}};
}

void exec_xwayland(
    mf::XWaylandSpawner const& spawner,
    std::string const& xwayland_path,
    mir::Fd wayland_client_fd,
    mir::Fd x11_wm_server_fd)
{
    mf::XWaylandSpawner::set_cloexec(wayland_client_fd, false);
    mf::XWaylandSpawner::set_cloexec(x11_wm_server_fd, false);

    setenv("WAYLAND_SOCKET", std::to_string(wayland_client_fd).c_str(), 1);

    auto const x11_wm_server = std::to_string(x11_wm_server_fd);
    auto const dsp_str = spawner.x11_display();

    // Propagate SIGUSR1 to parent process
    struct sigaction action;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &action, nullptr);

    std::vector<char const*> args =
        {
            xwayland_path.c_str(),
            dsp_str.c_str(),
            "-rootless",
            "-wm", x11_wm_server.c_str(),
            "-terminate",
        };

    for (auto const& fd : spawner.socket_fds())
    {
        mf::XWaylandSpawner::set_cloexec(fd, false);
        args.push_back("-listen");
        // strdup() may look like a leak, but execvp() will trash all resource management
        args.push_back(strdup(std::to_string(fd).c_str()));
    }

    if (auto const xwayland_option = getenv("MIR_XWAYLAND_OPTION"))
        args.push_back(xwayland_option);

    args.push_back(nullptr);

    execvp(xwayland_path.c_str(), const_cast<char* const*>(args.data()));
    perror(("Failed to execute Xwayland binary: xwayland_path='" + xwayland_path + "'").c_str());
}

auto fork_xwayland_process(
    mf::XWaylandSpawner const& spawner,
    std::string const& xwayland_path) -> mf::XWaylandServer::XWaylandProcess
{
    auto const wayland_pipe = make_socket_pair();
    auto const x11_wm_pipe = make_socket_pair();

    mir::log_info("Starting XWayland");
    pid_t const xwayland_pid = fork();

    switch (xwayland_pid)
    {
    case -1:
        BOOST_THROW_EXCEPTION(std::system_error(errno, std::system_category(), "Failed to fork XWayland process"));

    case 0:
        exec_xwayland(spawner, xwayland_path, wayland_pipe.client, x11_wm_pipe.server);
        // Only reached if Xwayland was not executed
        abort();

    default:
        return mf::XWaylandServer::XWaylandProcess{xwayland_pid, wayland_pipe.server, x11_wm_pipe.client};
    }
}

bool spin_wait_for(sig_atomic_t& xserver_ready)
{
    auto const end = std::chrono::steady_clock::now() + 5s;

    while (std::chrono::steady_clock::now() < end && !xserver_ready)
    {
        std::this_thread::sleep_for(100ms);
    }

    return xserver_ready;
}

auto connect_xwayland_wl_client(
    std::shared_ptr<mf::WaylandConnector> const& wayland_connector,
    mir::Fd const& wayland_fd) -> wl_client*
{
    // We need to set up the signal handling before connecting wl_client_server_fd
    static sig_atomic_t xserver_ready{ false };

    // In practice, there ought to be no contention on xserver_ready, but let's be certain
    static std::mutex xserver_ready_mutex;
    std::lock_guard<decltype(xserver_ready_mutex)> lock{xserver_ready_mutex};
    xserver_ready = false;

    struct sigaction action;
    struct sigaction old_action;
    action.sa_handler = [](int) { xserver_ready = true; };
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGUSR1, &action, &old_action);

    struct CreateClientContext
    {
        std::mutex mutex;
        wl_client* client{nullptr};
        bool ready{false};
        std::condition_variable condition_variable;
    };

    auto const ctx = std::make_shared<CreateClientContext>();

    wayland_connector->run_on_wayland_display(
        [ctx, wayland_fd](wl_display* display)
        {
            std::lock_guard<std::mutex> lock{ctx->mutex};
            ctx->client = wl_client_create(display, wayland_fd);
            ctx->ready = true;
            ctx->condition_variable.notify_all();
        });

    std::unique_lock<std::mutex> client_lock{ctx->mutex};
    if (!ctx->condition_variable.wait_for(client_lock, 10s, [ctx]{ return ctx->ready; }))
    {
        // "Shouldn't happen" but this is better than hanging.
        BOOST_THROW_EXCEPTION(std::runtime_error("Creating XWayland wl_client timed out"));
    }

    if (!ctx->client)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to create XWayland wl_client"));
    }

    //The client can connect, now wait for it to signal ready (SIGUSR1)
    auto const xwayland_startup_timed_out = !spin_wait_for(xserver_ready);
    sigaction(SIGUSR1, &old_action, nullptr);

    if (xwayland_startup_timed_out)
    {
        BOOST_THROW_EXCEPTION(std::runtime_error("XWayland server failed to start"));
    }

    return ctx->client;
}
}

mf::XWaylandServer::XWaylandServer(
    std::shared_ptr<WaylandConnector> const& wayland_connector,
    XWaylandSpawner const& spawner,
    std::string const& xwayland_path)
    : xwayland_process{fork_xwayland_process(spawner, xwayland_path)},
      wayland_client{connect_xwayland_wl_client(wayland_connector, xwayland_process.wayland_server_fd)},
      running{true}
{
}

mf::XWaylandServer::~XWaylandServer()
{
    mir::log_info("Deiniting xwayland server");

    // Terminate any running xservers
    if (kill(xwayland_process.pid, SIGTERM) == 0)
    {
        std::this_thread::sleep_for(100ms);// After 100ms...
        if (is_running())
        {
            mir::log_info("Xwayland didn't close, killing it");
            kill(xwayland_process.pid, SIGKILL);     // ...then kill it!
        }
    }
}

auto mf::XWaylandServer::is_running() const -> bool
{
    std::lock_guard<std::mutex> lock{mutex};

    if (running)
    {
        int status; // Special waitpid() status, not the process exit status
        if (waitpid(xwayland_process.pid, &status, WNOHANG) != 0)
        {
            running = false;
            if (WIFEXITED(status))
            {
                exit_code = WEXITSTATUS(status);
            }
        }
    }

    return running;
}
