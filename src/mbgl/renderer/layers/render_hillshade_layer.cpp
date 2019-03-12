#include <mbgl/renderer/layers/render_hillshade_layer.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/sources/render_raster_dem_source.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/programs/programs.hpp>
#include <mbgl/programs/hillshade_program.hpp>
#include <mbgl/programs/hillshade_prepare_program.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/style/layers/hillshade_layer_impl.hpp>
#include <mbgl/util/geo.hpp>
#include <mbgl/util/offscreen_texture.hpp>

namespace mbgl {

using namespace style;
RenderHillshadeLayer::RenderHillshadeLayer(Immutable<style::HillshadeLayer::Impl> _impl)
    : RenderLayer(std::move(_impl)),
      unevaluated(impl().paint.untransitioned()) {
}

const style::HillshadeLayer::Impl& RenderHillshadeLayer::impl() const {
    return static_cast<const style::HillshadeLayer::Impl&>(*baseImpl);
}

const std::array<float, 2> RenderHillshadeLayer::getLatRange(const UnwrappedTileID& id) {
   const LatLng latlng0 = LatLng(id);
   const LatLng latlng1 = LatLng(UnwrappedTileID(id.canonical.z, id.canonical.x, id.canonical.y + 1));
   return {{ (float)latlng0.latitude(), (float)latlng1.latitude() }};
}

const std::array<float, 2> RenderHillshadeLayer::getLight(const PaintParameters& parameters){
    float azimuthal = evaluated.get<HillshadeIlluminationDirection>() * util::DEG2RAD;
    if (evaluated.get<HillshadeIlluminationAnchor>() == HillshadeIlluminationAnchorType::Viewport) azimuthal = azimuthal - parameters.state.getBearing();
    return {{evaluated.get<HillshadeExaggeration>(), azimuthal}};
}

void RenderHillshadeLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl().paint.transitioned(parameters, std::move(unevaluated));
}

void RenderHillshadeLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    evaluated = unevaluated.evaluate(parameters);
    passes = (evaluated.get<style::HillshadeExaggeration >() > 0)
                 ? (RenderPass::Translucent | RenderPass::Pass3D)
                 : RenderPass::None;
}

bool RenderHillshadeLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderHillshadeLayer::hasCrossfade() const {
    return false;
}

void RenderHillshadeLayer::render(PaintParameters& parameters, RenderSource* src) {
    if (parameters.pass != RenderPass::Translucent && parameters.pass != RenderPass::Pass3D)
        return;

    RenderRasterDEMSource* demsrc = static_cast<RenderRasterDEMSource*>(src);
    const uint8_t TERRAIN_RGB_MAXZOOM = 15;
    const uint8_t maxzoom = demsrc != nullptr ? demsrc->getMaxZoom() : TERRAIN_RGB_MAXZOOM;

    auto draw = [&] (const mat4& matrix,
                     const auto& vertexBuffer,
                     const auto& indexBuffer,
                     const auto& segments,
                     const UnwrappedTileID& id,
                     const auto& textureBindings) {
        auto& programInstance = parameters.programs.getHillshadeLayerPrograms().hillshade;

        const HillshadeProgram::Binders paintAttributeData{ evaluated, 0 };

        const auto allUniformValues = programInstance.computeAllUniformValues(
            HillshadeProgram::LayoutUniformValues {
                uniforms::u_matrix::Value( matrix ),
                uniforms::u_highlight::Value( evaluated.get<HillshadeHighlightColor>() ),
                uniforms::u_shadow::Value( evaluated.get<HillshadeShadowColor>() ),
                uniforms::u_accent::Value( evaluated.get<HillshadeAccentColor>() ),
                uniforms::u_light::Value( getLight(parameters) ),
                uniforms::u_latrange::Value( getLatRange(id) ),
            },
            paintAttributeData,
            evaluated,
            parameters.state.getZoom()
        );
        const auto allAttributeBindings = programInstance.computeAllAttributeBindings(
            vertexBuffer,
            paintAttributeData,
            evaluated
        );

        checkRenderability(parameters, programInstance.activeBindingCount(allAttributeBindings));

        programInstance.draw(
            parameters.context,
            gfx::Triangles(),
            parameters.depthModeForSublayer(0, gfx::DepthMaskType::ReadOnly),
            gfx::StencilMode::disabled(),
            parameters.colorModeForRenderPass(),
            gfx::CullFaceMode::disabled(),
            indexBuffer,
            segments,
            allUniformValues,
            allAttributeBindings,
            textureBindings,
            getID()
        );
    };

    mat4 mat;
    matrix::ortho(mat, 0, util::EXTENT, -util::EXTENT, 0, 0, 1);
    matrix::translate(mat, mat, 0, -util::EXTENT, 0);

    for (const RenderTile& tile : renderTiles) {
        auto bucket_ = tile.tile.getBucket<HillshadeBucket>(*baseImpl);
        if (!bucket_) {
            continue;
        }
        HillshadeBucket& bucket = *bucket_;

        if (!bucket.hasData()){
            continue;
        }

        if (!bucket.isPrepared() && parameters.pass == RenderPass::Pass3D) {
            assert(bucket.dem);
            const uint16_t stride = bucket.getDEMData().stride;
            const uint16_t tilesize = bucket.getDEMData().dim;
            OffscreenTexture view(parameters.context, { tilesize, tilesize });
            view.bind();

            const Properties<>::PossiblyEvaluated properties;
            const HillshadePrepareProgram::Binders paintAttributeData{ properties, 0 };
            
            auto& programInstance = parameters.programs.getHillshadeLayerPrograms().hillshadePrepare;

            const auto allUniformValues = programInstance.computeAllUniformValues(
                HillshadePrepareProgram::LayoutUniformValues {
                    uniforms::u_matrix::Value( mat ),
                    uniforms::u_dimension::Value( {{stride, stride}} ),
                    uniforms::u_zoom::Value( float(tile.id.canonical.z) ),
                    uniforms::u_maxzoom::Value( float(maxzoom) ),
                },
                paintAttributeData,
                properties,
                parameters.state.getZoom()
            );
            const auto allAttributeBindings = programInstance.computeAllAttributeBindings(
                parameters.staticData.rasterVertexBuffer,
                paintAttributeData,
                properties
            );

            checkRenderability(parameters, programInstance.activeBindingCount(allAttributeBindings));

            programInstance.draw(
                parameters.context,
                gfx::Triangles(),
                parameters.depthModeForSublayer(0, gfx::DepthMaskType::ReadOnly),
                gfx::StencilMode::disabled(),
                parameters.colorModeForRenderPass(),
                gfx::CullFaceMode::disabled(),
                parameters.staticData.quadTriangleIndexBuffer,
                parameters.staticData.rasterSegments,
                allUniformValues,
                allAttributeBindings,
                HillshadePrepareProgram::TextureBindings{
                    textures::u_image::Value{ *bucket.dem->resource },
                },
                getID()
            );
            bucket.texture = std::move(view.getTexture());
            bucket.setPrepared(true);
        } else if (parameters.pass == RenderPass::Translucent) {
            assert(bucket.texture);

            if (bucket.vertexBuffer && bucket.indexBuffer && !bucket.segments.empty()) {
                // Draw only the parts of the tile that aren't drawn by another tile in the layer.
                draw(parameters.matrixForTile(tile.id, true),
                     *bucket.vertexBuffer,
                     *bucket.indexBuffer,
                     bucket.segments,
                     tile.id,
                     HillshadeProgram::TextureBindings{
                         textures::u_image::Value{ *bucket.texture->resource,  gfx::TextureFilterType::Linear },
                     });
            } else {
                // Draw the full tile.
                draw(parameters.matrixForTile(tile.id, true),
                     parameters.staticData.rasterVertexBuffer,
                     parameters.staticData.quadTriangleIndexBuffer,
                     parameters.staticData.rasterSegments,
                     tile.id,
                     HillshadeProgram::TextureBindings{
                         textures::u_image::Value{ *bucket.texture->resource,  gfx::TextureFilterType::Linear },
                     });
            }
        }
        

    }
}

} // namespace mbgl
