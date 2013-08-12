/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Guest <thomas.guest@canonical.com>
 */

#include "mir/default_configuration.h"
#include "mir_toolkit/mir_client_library.h"
#include "mir_toolkit/mir_client_library_drm.h"

#include "mir_connection.h"
#include "display_configuration.h"
#include "mir_surface.h"
#include "native_client_platform_factory.h"
#include "egl_native_display_container.h"
#include "default_connection_configuration.h"

#include <set>
#include <unordered_set>
#include <cstddef>

namespace mcl = mir::client;

std::mutex MirConnection::connection_guard;
std::unordered_set<MirConnection*> MirConnection::valid_connections;

namespace
{
class ConnectionList
{
public:
    void insert(MirConnection* connection)
    {
        std::lock_guard<std::mutex> lock(connection_guard);
        connections.insert(connection);
    }

    void remove(MirConnection* connection)
    {
        std::lock_guard<std::mutex> lock(connection_guard);
        connections.erase(connection);
    }

    bool contains(MirConnection* connection)
    {
        std::lock_guard<std::mutex> lock(connection_guard);
        return connections.count(connection);
    }

private:
    std::mutex connection_guard;
    std::unordered_set<MirConnection*> connections;
};

ConnectionList error_connections;

// assign_result is compatible with all 2-parameter callbacks
void assign_result(void *result, void **context)
{
    if (context)
        *context = result;
}

}

MirWaitHandle* mir_connect(char const* socket_file, char const* name, mir_connected_callback callback, void * context)
{

    try
    {
        std::string sock;
        if (socket_file)
            sock = socket_file;
        else
        {
            auto socket_env = getenv("MIR_SOCKET");
            if (socket_env)
                sock = socket_env;
            else
                sock = mir::default_server_socket;
        }
 
        mcl::DefaultConnectionConfiguration conf{sock};

        MirConnection* connection = new MirConnection(conf);

        return connection->connect(name, callback, context);
    }
    catch (std::exception const& x)
    {
        MirConnection* error_connection = new MirConnection();
        error_connections.insert(error_connection);
        error_connection->set_error_message(x.what());
        callback(error_connection, context);
        return 0;
    }
}

MirConnection *mir_connect_sync(char const *server,
                                             char const *app_name)
{
    MirConnection *conn = nullptr;
    mir_wait_for(mir_connect(server, app_name,
                             reinterpret_cast<mir_connected_callback>
                                             (assign_result),
                             &conn));
    return conn;
}

int mir_connection_is_valid(MirConnection * connection)
{
    return MirConnection::is_valid(connection);
}

char const * mir_connection_get_error_message(MirConnection * connection)
{
    return connection->get_error_message();
}

void mir_connection_release(MirConnection * connection)
{
    if (!error_connections.contains(connection))
    {
        auto wait_handle = connection->disconnect();
        wait_handle->wait_for_all();
    }
    else
    {
        error_connections.remove(connection);
    }

    delete connection;
}

MirEGLNativeDisplayType mir_connection_get_egl_native_display(MirConnection *connection)
{
    return connection->egl_native_display();
}

void mir_connection_get_available_surface_formats(
    MirConnection * connection, MirPixelFormat* formats,
    unsigned const int format_size, unsigned int *num_valid_formats)
{
    if ((connection) && (formats) && (num_valid_formats))
        connection->possible_pixel_formats(formats, format_size, *num_valid_formats);
}

MirWaitHandle* mir_connection_create_surface(
    MirConnection* connection,
    MirSurfaceParameters const* params,
    mir_surface_callback callback,
    void* context)
{
    if (error_connections.contains(connection)) return 0;

    try
    {
        return connection->create_surface(*params, callback, context);
    }
    catch (std::exception const&)
    {
        // TODO callback with an error surface
        return 0; // TODO
    }

}

MirSurface* mir_connection_create_surface_sync(
    MirConnection* connection,
    MirSurfaceParameters const* params)
{
    MirSurface *surface = nullptr;

    mir_wait_for(mir_connection_create_surface(connection, params,
        reinterpret_cast<mir_surface_callback>(assign_result),
        &surface));

    return surface;
}

void mir_surface_set_event_handler(MirSurface *surface,
                                   MirEventDelegate const *event_handler)
{
    surface->set_event_handler(event_handler);
}

MirWaitHandle* mir_surface_release(
    MirSurface * surface,
    mir_surface_callback callback, void * context)
{
    return surface->release_surface(callback, context);
}

void mir_surface_release_sync(MirSurface *surface)
{
    mir_wait_for(mir_surface_release(surface,
        reinterpret_cast<mir_surface_callback>(assign_result),
        nullptr));
}

int mir_surface_get_id(MirSurface * surface)
{
    return surface->id();
}

int mir_surface_is_valid(MirSurface* surface)
{
    return surface->is_valid();
}

char const * mir_surface_get_error_message(MirSurface * surface)
{
    return surface->get_error_message();
}

void mir_surface_get_parameters(MirSurface * surface, MirSurfaceParameters *parameters)
{
    *parameters = surface->get_parameters();
}

MirPlatformType mir_surface_get_platform_type(MirSurface * surface)
{
    return surface->platform_type();
}

void mir_surface_get_current_buffer(MirSurface * surface, MirNativeBuffer ** buffer_package_out)
{
    auto package = surface->get_current_buffer_package();
    *buffer_package_out = package.get();
}

void mir_connection_get_platform(MirConnection *connection, MirPlatformPackage *platform_package)
{
    connection->populate(*platform_package);
}

MirDisplayConfiguration* mir_connection_create_display_config(MirConnection *connection)
{
    if (connection)
        return connection->create_copy_of_display_config();
    return nullptr;
}

void mir_display_config_destroy(MirDisplayConfiguration* configuration)
{
    mcl::delete_config_storage(configuration);
}

//TODO: DEPRECATED: remove this function
void mir_connection_get_display_info(MirConnection *connection, MirDisplayInfo *display_info)
{
    auto config = mir_connection_create_display_config(connection);
    if (config->num_displays < 1)
        return;

    MirDisplayOutput* state = nullptr;
    // We can't handle more than one display, so just populate based on the first
    // active display we find.
    for (unsigned int i = 0; i < config->num_displays; ++i) 
    {
        if (config->displays[i].used && config->displays[i].connected &&
            config->displays[i].current_mode < config->displays[i].num_modes)
        {
            state = &config->displays[i];
            break;
        }
    }
    // Oh, oh! No connected outputs?!
    if (state == nullptr)
    {
        memset(display_info, 0, sizeof(*display_info));
        return;
    }

    MirDisplayMode mode = state->modes[state->current_mode];
   
    display_info->width = mode.horizontal_resolution;
    display_info->height = mode.vertical_resolution;

    unsigned int format_items;
    if (state->num_output_formats > mir_supported_pixel_format_max) 
         format_items = mir_supported_pixel_format_max;
    else
         format_items = state->num_output_formats;

    display_info->supported_pixel_format_items = format_items; 
    for(auto i=0u; i < format_items; i++)
    {
        display_info->supported_pixel_format[i] = state->output_formats[i];
    }

    mir_display_config_destroy(config);
}

void mir_surface_get_graphics_region(MirSurface * surface, MirGraphicsRegion * graphics_region)
{
    surface->get_cpu_region( *graphics_region);
}

MirWaitHandle* mir_surface_swap_buffers(MirSurface *surface, mir_surface_callback callback, void * context)
{
    return surface->next_buffer(callback, context);
}

void mir_surface_swap_buffers_sync(MirSurface *surface)
{
    mir_wait_for(mir_surface_swap_buffers(surface,
        reinterpret_cast<mir_surface_callback>(assign_result),
        nullptr));
}

void mir_wait_for(MirWaitHandle* wait_handle)
{
    if (wait_handle)
        wait_handle->wait_for_all();
}

void mir_wait_for_one(MirWaitHandle* wait_handle)
{
    if (wait_handle)
        wait_handle->wait_for_one();
}

MirEGLNativeWindowType mir_surface_get_egl_native_window(MirSurface *surface)
{
    return surface->generate_native_window();
}

MirWaitHandle *mir_connection_drm_auth_magic(MirConnection* connection,
                                             unsigned int magic,
                                             mir_drm_auth_magic_callback callback,
                                             void* context)
{
    return connection->drm_auth_magic(magic, callback, context);
}

MirWaitHandle* mir_surface_set_type(MirSurface *surf,
                                                           MirSurfaceType type)
{
    return surf ? surf->configure(mir_surface_attrib_type, type) : NULL;
}

MirSurfaceType mir_surface_get_type(MirSurface *surf)
{
    MirSurfaceType type = mir_surface_type_normal;

    if (surf)
    {
        // Only the client will ever change the type of a surface so it is
        // safe to get the type from a local cache surf->attrib().

        int t = surf->attrib(mir_surface_attrib_type);
        type = static_cast<MirSurfaceType>(t);
    }

    return type;
}

MirWaitHandle* mir_surface_set_state(MirSurface *surf, MirSurfaceState state)
{
    return surf ? surf->configure(mir_surface_attrib_state, state) : NULL;
}

MirSurfaceState mir_surface_get_state(MirSurface *surf)
{
    MirSurfaceState state = mir_surface_state_unknown;

    if (surf)
    {
        int s = surf->attrib(mir_surface_attrib_state);

        if (s == mir_surface_state_unknown)
        {
            surf->configure(mir_surface_attrib_state,
                            mir_surface_state_unknown)->wait_for_all();
            s = surf->attrib(mir_surface_attrib_state);
        }

        state = static_cast<MirSurfaceState>(s);
    }

    return state;
}

MirWaitHandle* mir_surface_set_swapinterval(MirSurface* surf, int interval)
{
    if ((interval < 0) || (interval > 1))
        return NULL;
    return surf ? surf->configure(mir_surface_attrib_swapinterval, interval) : NULL;
}

int mir_surface_get_swapinterval(MirSurface* surf)
{
    return surf ? surf->attrib(mir_surface_attrib_swapinterval) : -1;
}

void mir_connection_set_display_config_change_callback(MirConnection* connection,
    mir_display_config_callback callback, void* context)
{
    if (connection)
        connection->register_display_change_callback(callback, context);
}

MirWaitHandle* mir_connection_apply_display_config(MirConnection *connection, MirDisplayConfiguration* display_configuration)
{
    if (!connection)
        return NULL;
 
    return connection->configure_display(display_configuration);
}
