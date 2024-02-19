#include "AsyncResourceGatherer.hpp"
#include "../config/ConfigManager.hpp"
#include "../core/Egl.hpp"
#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <algorithm>
#include "../core/hyprlock.hpp"

std::mutex cvmtx;

CAsyncResourceGatherer::CAsyncResourceGatherer() {
    initThread = std::thread([this]() {
        this->gather();
        this->asyncLoopThread = std::thread([this]() { this->asyncAssetSpinLock(); });
        this->asyncLoopThread.detach();
    });
    initThread.detach();
}

SPreloadedAsset* CAsyncResourceGatherer::getAssetByID(const std::string& id) {
    if (asyncLoopState.busy)
        return nullptr;

    std::lock_guard<std::mutex> lg(asyncLoopState.loopMutex);

    for (auto& a : assets) {
        if (a.first == id)
            return &a.second;
    }

    if (!preloadTargets.empty()) {
        apply();
        for (auto& a : assets) {
            if (a.first == id)
                return &a.second;
        }
    }

    return nullptr;
}

void CAsyncResourceGatherer::gather() {
    const auto CWIDGETS = g_pConfigManager->getWidgetConfigs();

    g_pEGL->makeCurrent(nullptr);

    // gather resources to preload
    // clang-format off
    int preloads = std::count_if(CWIDGETS.begin(), CWIDGETS.end(), [](const auto& w) {
        return w.type == "background";
    });
    // clang-format on

    progress = 0;
    for (auto& c : CWIDGETS) {
        if (c.type == "background") {
            progress += 1.0 / (preloads + 1.0);

            std::string path = std::any_cast<Hyprlang::STRING>(c.values.at("path"));
            std::string id   = std::string{"background:"} + path;

            // preload bg img
            const auto CAIROISURFACE = cairo_image_surface_create_from_png(path.c_str());

            const auto CAIRO = cairo_create(CAIROISURFACE);
            cairo_scale(CAIRO, 1, 1);

            const auto TARGET = &preloadTargets.emplace_back(CAsyncResourceGatherer::SPreloadTarget{});

            TARGET->size = {cairo_image_surface_get_width(CAIROISURFACE), cairo_image_surface_get_height(CAIROISURFACE)};
            TARGET->type = TARGET_IMAGE;
            TARGET->id   = id;

            const auto DATA      = cairo_image_surface_get_data(CAIROISURFACE);
            TARGET->cairo        = CAIRO;
            TARGET->cairosurface = CAIROISURFACE;
            TARGET->data         = DATA;
        }
    }

    ready = true;
}

void CAsyncResourceGatherer::apply() {

    for (auto& t : preloadTargets) {
        if (t.type == TARGET_IMAGE) {
            const auto  ASSET = &assets[t.id];

            const auto  CAIROFORMAT = cairo_image_surface_get_format((cairo_surface_t*)t.cairosurface);
            const GLint glIFormat   = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_RGB32F : GL_RGBA;
            const GLint glFormat    = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_RGB : GL_RGBA;
            const GLint glType      = CAIROFORMAT == CAIRO_FORMAT_RGB96F ? GL_FLOAT : GL_UNSIGNED_BYTE;

            ASSET->texture.m_vSize = t.size;
            ASSET->texture.allocate();

            glBindTexture(GL_TEXTURE_2D, ASSET->texture.m_iTexID);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            if (CAIROFORMAT != CAIRO_FORMAT_RGB96F) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            }
            glTexImage2D(GL_TEXTURE_2D, 0, glIFormat, ASSET->texture.m_vSize.x, ASSET->texture.m_vSize.y, 0, glFormat, glType, t.data);

            cairo_destroy((cairo_t*)t.cairo);
            cairo_surface_destroy((cairo_surface_t*)t.cairosurface);
        } else {
            Debug::log(ERR, "Unsupported type in ::apply() {}", (int)t.type);
        }
    }

    preloadTargets.clear();

    applied = true;
}

void CAsyncResourceGatherer::renderText(const SPreloadRequest& rq) {
    SPreloadTarget target;
    target.type = TARGET_IMAGE; /* text is just an image lol */
    target.id   = rq.id;

    const int         FONTSIZE   = rq.props.contains("font_size") ? std::any_cast<int>(rq.props.at("font_size")) : 16;
    const CColor      FONTCOLOR  = rq.props.contains("color") ? std::any_cast<CColor>(rq.props.at("color")) : CColor(1.0, 1.0, 1.0, 1.0);
    const std::string FONTFAMILY = rq.props.contains("font_family") ? std::any_cast<std::string>(rq.props.at("font_family")) : "Sans";

    auto              CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 1080 /* dummy value */);
    auto              CAIRO        = cairo_create(CAIROSURFACE);

    // draw title using Pango
    PangoLayout* layout = pango_cairo_create_layout(CAIRO);
    pango_layout_set_text(layout, rq.asset.c_str(), -1);

    PangoFontDescription* fontDesc = pango_font_description_from_string(FONTFAMILY.c_str());
    pango_font_description_set_size(fontDesc, FONTSIZE * PANGO_SCALE);
    pango_layout_set_font_description(layout, fontDesc);
    pango_font_description_free(fontDesc);
    int layoutWidth, layoutHeight;
    pango_layout_get_size(layout, &layoutWidth, &layoutHeight);

    // TODO: avoid this?
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);
    CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, layoutWidth / PANGO_SCALE, layoutHeight / PANGO_SCALE);
    CAIRO        = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    // render the thing
    cairo_set_source_rgba(CAIRO, FONTCOLOR.r, FONTCOLOR.g, FONTCOLOR.b, FONTCOLOR.a);

    cairo_move_to(CAIRO, 0, 0);
    pango_cairo_show_layout(CAIRO, layout);

    g_object_unref(layout);

    cairo_surface_flush(CAIROSURFACE);

    target.cairo        = CAIRO;
    target.cairosurface = CAIROSURFACE;
    target.data         = cairo_image_surface_get_data(CAIROSURFACE);
    target.size         = {layoutWidth / PANGO_SCALE, layoutHeight / PANGO_SCALE};

    preloadTargets.push_back(target);
}

void CAsyncResourceGatherer::asyncAssetSpinLock() {
    while (!g_pHyprlock->m_bTerminate) {

        std::unique_lock lk(cvmtx);
        if (asyncLoopState.pending == false) // avoid a lock if a thread managed to request something already since we .unlock()ed
            asyncLoopState.loopGuard.wait_for(lk, std::chrono::seconds(5), [this] { return asyncLoopState.pending; }); // wait for events

        asyncLoopState.requestMutex.lock();

        if (asyncLoopState.requests.empty()) {
            asyncLoopState.requestMutex.unlock();
            continue;
        }

        auto requests = asyncLoopState.requests;
        asyncLoopState.requests.clear();

        asyncLoopState.requestMutex.unlock();

        // process requests

        asyncLoopState.busy = true;
        for (auto& r : requests) {
            if (r.type == TARGET_TEXT) {
                renderText(r);
            } else {
                Debug::log(ERR, "Unsupported async preload type {}??", (int)r.type);
            }
        }

        asyncLoopState.busy = false;
    }
}

void CAsyncResourceGatherer::requestAsyncAssetPreload(const SPreloadRequest& request) {
    std::lock_guard<std::mutex> lg(asyncLoopState.requestMutex);
    asyncLoopState.requests.push_back(request);
    std::unique_lock lk(cvmtx);
    asyncLoopState.pending = true;
    asyncLoopState.loopGuard.notify_all();
}