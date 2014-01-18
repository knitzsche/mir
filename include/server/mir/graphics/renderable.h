/*
 * Copyright © 2014 Canonical Ltd.
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
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#ifndef MIR_GRAPHICS_RENDERABLE_H_
#define MIR_GRAPHICS_RENDERABLE_H_

namespace mir
{
namespace graphics
{
#error zonks
class Renderable
{
public:
    virtual std::shared_ptr<mg::Buffer> buffer() const = 0;
    virtual bool alpha_enabled() const = 0;
    virtual geom::Rectangle screen_position() const = 0;
    virtual ~Renderable() = default;

protected:
    Renderable() = default;
    Renderable(Renderable const&) = delete;
    Renderable& operator=(Renderable const&) = delete;
};

}
}

#endif /* MIR_GRAPHICS_RENDERABLE_H_ */
