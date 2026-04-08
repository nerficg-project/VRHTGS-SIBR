// Software Name : SIBR_core
// SPDX-FileCopyrightText: Copyright (c) 2023 Orange
// SPDX-License-Identifier: Apache 2.0
//
// This software is distributed under the Apache 2.0 License;
// see the LICENSE file for more details.
//
// Author: Cédric CHEDALEUX <cedric.chedaleux@orange.com> et al.

#pragma once

#include <core/graphics/RenderTarget.hpp>

namespace sibr
{

    class SwapchainImageRenderTarget : public sibr::IRenderTarget
    {
    public:
        SwapchainImageRenderTarget(uint32_t texture, uint32_t w, uint32_t h);
        ~SwapchainImageRenderTarget(void);

        GLuint texture(uint32_t t = 0) const override;
        GLuint handle(uint32_t t = 0) const override;
        void bind(void) override;
        void unbind(void) override;
        void clear(void) override;
        uint32_t w(void) const override;
        uint32_t h(void) const override;
        GLuint fbo(void) const override;

    private:
        GLuint m_fbo = 0;     ///< Framebuffer handle.
        GLuint m_texture = 0; ///< Color texture handle.
        uint32_t m_W = 0;     ///< Width.
        uint32_t m_H = 0;     ///< Height.
    };

} /*namespace sibr*/
