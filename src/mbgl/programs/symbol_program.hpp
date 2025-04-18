#pragma once

#include <mbgl/math/clamp.hpp>
#include <mbgl/util/interpolate.hpp>

#include <mbgl/programs/attributes.hpp>
#include <mbgl/programs/collision_box_program.hpp>
#include <mbgl/programs/uniforms.hpp>
#include <mbgl/programs/textures.hpp>
#include <mbgl/programs/segment.hpp>
#include <mbgl/util/geometry.hpp>
#include <mbgl/util/size.hpp>
#include <mbgl/style/layers/symbol_layer_properties.hpp>
#include <mbgl/style/layers/symbol_layer_impl.hpp>
#include <mbgl/renderer/layers/render_symbol_layer.hpp>

#include <cmath>
#include <array>

#include <mbgl/shaders/shader_manifest.hpp>
#if MLN_RENDER_BACKEND_OPENGL
#include <mbgl/gl/program.hpp>
#endif

namespace mbgl {

const uint16_t MAX_GLYPH_ICON_SIZE = 255;
const uint16_t SIZE_PACK_FACTOR = 128;
const uint16_t MAX_PACKED_SIZE = MAX_GLYPH_ICON_SIZE * SIZE_PACK_FACTOR;

namespace style {
class SymbolPropertyValues;
} // namespace style

class RenderTile;
class TransformState;

namespace uniforms {
MBGL_DEFINE_UNIFORM_MATRIX(double, 4, coord_matrix);
MBGL_DEFINE_UNIFORM_MATRIX(double, 4, label_plane_matrix);
MBGL_DEFINE_UNIFORM_SCALAR(bool, is_halo);
MBGL_DEFINE_UNIFORM_SCALAR(float, gamma_scale);

MBGL_DEFINE_UNIFORM_SCALAR(bool, is_text);
MBGL_DEFINE_UNIFORM_SCALAR(bool, is_size_zoom_constant);
MBGL_DEFINE_UNIFORM_SCALAR(bool, is_size_feature_constant);
MBGL_DEFINE_UNIFORM_SCALAR(float, size_t);
MBGL_DEFINE_UNIFORM_SCALAR(float, size);
MBGL_DEFINE_UNIFORM_SCALAR(bool, rotate_symbol);
MBGL_DEFINE_UNIFORM_SCALAR(float, aspect_ratio);
} // namespace uniforms

using SymbolLayoutAttributes = TypeList<attributes::pos_offset, attributes::data<uint16_t, 4>, attributes::pixeloffset>;

using SymbolDynamicLayoutAttributes = TypeList<attributes::projected_pos>;

using SymbolOpacityAttributes = TypeList<attributes::fade_opacity>;

struct ZoomEvaluatedSize {
    bool isZoomConstant;
    bool isFeatureConstant;
    float sizeT;
    float size;
    float layoutSize;
};
// Mimic the PaintPropertyBinder technique specifically for the {text,icon}-size
// layout properties in order to provide a 'custom' scheme for encoding the
// necessary attribute data.  As with PaintPropertyBinder, SymbolSizeBinder is
// an abstract class whose implementations handle the particular attribute &
// uniform logic needed by each possible type of the {Text,Icon}Size properties.
class SymbolSizeBinder {
public:
    virtual ~SymbolSizeBinder() = default;

    using UniformList =
        TypeList<uniforms::is_size_zoom_constant, uniforms::is_size_feature_constant, uniforms::size_t, uniforms::size>;
    using UniformValues = gfx::UniformValues<UniformList>;

    static std::unique_ptr<SymbolSizeBinder> create(float tileZoom,
                                                    const style::PropertyValue<float>& sizeProperty,
                                                    float defaultValue);

    virtual Range<float> getVertexSizeData(const GeometryTileFeature& feature) = 0;
    virtual ZoomEvaluatedSize evaluateForZoom(float currentZoom) const = 0;

    UniformValues uniformValues(float currentZoom) const {
        const ZoomEvaluatedSize u = evaluateForZoom(currentZoom);
        return UniformValues{uniforms::is_size_zoom_constant::Value(u.isZoomConstant),
                             uniforms::is_size_feature_constant::Value(u.isFeatureConstant),
                             uniforms::size_t::Value(u.sizeT),
                             uniforms::size::Value(u.size)};
    }
};

class ConstantSymbolSizeBinder final : public SymbolSizeBinder {
public:
    ConstantSymbolSizeBinder(const float /*tileZoom*/, const float& size, const float /*defaultValue*/) noexcept
        : layoutSize(size) {}

    ConstantSymbolSizeBinder(const float /*tileZoom*/, const style::Undefined&, const float defaultValue) noexcept
        : layoutSize(defaultValue) {}

    ConstantSymbolSizeBinder(const float tileZoom,
                             const style::PropertyExpression<float>& expression_,
                             const float /*defaultValue*/
                             )
        : layoutSize(expression_.evaluate(tileZoom + 1)),
          expression(expression_) {
        const Range<float> zoomLevels = expression_.getCoveringStops(tileZoom, tileZoom + 1);
        coveringRanges = std::make_tuple(
            zoomLevels, Range<float>{expression_.evaluate(zoomLevels.min), expression_.evaluate(zoomLevels.max)});
    }

    Range<float> getVertexSizeData(const GeometryTileFeature&) noexcept override { return {0.0f, 0.0f}; };

    ZoomEvaluatedSize evaluateForZoom(float currentZoom) const override {
        float size = layoutSize;
        bool isZoomConstant = !(coveringRanges || expression);
        if (coveringRanges) {
            // Even though we could get the exact value of the camera function
            // at z = currentZoom, we intentionally do not: instead, we
            // interpolate between the camera function values at a pair of zoom
            // stops covering [tileZoom, tileZoom + 1] in order to be consistent
            // with this restriction on composite functions.
            const Range<float>& zoomLevels = std::get<0>(*coveringRanges);
            const Range<float>& sizeLevels = std::get<1>(*coveringRanges);
            float t = util::clamp(expression->interpolationFactor(zoomLevels, currentZoom), 0.0f, 1.0f);
            size = sizeLevels.min + t * (sizeLevels.max - sizeLevels.min);
        } else if (expression) {
            size = expression->evaluate(currentZoom);
        }

        const float unused = 0.0f;
        return {.isZoomConstant = isZoomConstant,
                .isFeatureConstant = true,
                .sizeT = unused,
                .size = size,
                .layoutSize = layoutSize};
    }

    float layoutSize;
    std::optional<std::tuple<Range<float>, Range<float>>> coveringRanges;
    std::optional<style::PropertyExpression<float>> expression;
};

class SourceFunctionSymbolSizeBinder final : public SymbolSizeBinder {
public:
    SourceFunctionSymbolSizeBinder(const float /*tileZoom*/,
                                   style::PropertyExpression<float> expression_,
                                   const float defaultValue_) noexcept
        : expression(std::move(expression_)),
          defaultValue(defaultValue_) {}

    Range<float> getVertexSizeData(const GeometryTileFeature& feature) override {
        const float size = expression.evaluate(feature, defaultValue);
        return {size, size};
    };

    ZoomEvaluatedSize evaluateForZoom(float) const noexcept override {
        const float unused = 0.0f;
        return {
            .isZoomConstant = true, .isFeatureConstant = false, .sizeT = unused, .size = unused, .layoutSize = unused};
    }

    style::PropertyExpression<float> expression;
    const float defaultValue;
};

class CompositeFunctionSymbolSizeBinder final : public SymbolSizeBinder {
public:
    CompositeFunctionSymbolSizeBinder(const float tileZoom,
                                      style::PropertyExpression<float> expression_,
                                      const float defaultValue_) noexcept
        : expression(std::move(expression_)),
          defaultValue(defaultValue_),
          layoutZoom(tileZoom + 1),
          coveringZoomStops(expression.getCoveringStops(tileZoom, tileZoom + 1)) {}

    Range<float> getVertexSizeData(const GeometryTileFeature& feature) override {
        return {expression.evaluate(coveringZoomStops.min, feature, defaultValue),
                expression.evaluate(coveringZoomStops.max, feature, defaultValue)};
    };

    ZoomEvaluatedSize evaluateForZoom(float currentZoom) const override {
        float sizeInterpolationT = util::clamp(
            expression.interpolationFactor(coveringZoomStops, currentZoom), 0.0f, 1.0f);

        const float unused = 0.0f;
        return {.isZoomConstant = false,
                .isFeatureConstant = false,
                .sizeT = sizeInterpolationT,
                .size = unused,
                .layoutSize = unused};
    }

    style::PropertyExpression<float> expression;
    const float defaultValue;
    float layoutZoom;
    Range<float> coveringZoomStops;
};

class SymbolProgramBase : public gfx::Shader {
public:
    static gfx::Vertex<SymbolLayoutAttributes> layoutVertex(Point<float> labelAnchor,
                                                            Point<float> o,
                                                            float glyphOffsetY,
                                                            uint16_t tx,
                                                            uint16_t ty,
                                                            const Range<float>& sizeData,
                                                            bool isSDF,
                                                            Point<float> pixelOffset,
                                                            Point<float> minFontScale) {
        const uint16_t aSizeMin = (std::min(MAX_PACKED_SIZE, static_cast<uint16_t>(sizeData.min * SIZE_PACK_FACTOR))
                                   << 1) +
                                  uint16_t(isSDF);
        const uint16_t aSizeMax = std::min(MAX_PACKED_SIZE, static_cast<uint16_t>(sizeData.max * SIZE_PACK_FACTOR));
        return {
            // combining pos and offset to reduce number of vertex attributes
            // passed to shader (8 max for some devices)
            {{static_cast<int16_t>(labelAnchor.x),
              static_cast<int16_t>(labelAnchor.y),
              static_cast<int16_t>(std::round(o.x * 32)), // use 1/32 pixels for placement
              static_cast<int16_t>(std::round((o.y + glyphOffsetY) * 32))}},
            {{tx, ty, aSizeMin, aSizeMax}},
            {{static_cast<int16_t>(pixelOffset.x * 16),
              static_cast<int16_t>(pixelOffset.y * 16),
              static_cast<int16_t>(minFontScale.x * 256),
              static_cast<int16_t>(minFontScale.y * 256)}},
        };
    }

    static gfx::Vertex<SymbolDynamicLayoutAttributes> dynamicLayoutVertex(Point<float> anchorPoint, float labelAngle) {
        return {{{anchorPoint.x, anchorPoint.y, labelAngle}}};
    }

    static gfx::Vertex<SymbolOpacityAttributes> opacityVertex(bool placed, float opacity) {
        return {{{static_cast<float>((static_cast<uint8_t>(opacity * 127) << 1) | static_cast<uint8_t>(placed))}}};
    }
};

template <class Name,
          shaders::BuiltIn ShaderSource,
          gfx::PrimitiveType Primitive,
          class LayoutAttributeList,
          class LayoutUniformList,
          class Textures,
          class PaintProps>
class SymbolProgram : public SymbolProgramBase {
public:
    using LayoutVertex = gfx::Vertex<LayoutAttributeList>;

    using LayoutAndSizeAttributeList =
        TypeListConcat<LayoutAttributeList, SymbolDynamicLayoutAttributes, SymbolOpacityAttributes>;

    using PaintProperties = PaintProps;
    using Binders = PaintPropertyBinders<typename PaintProperties::DataDrivenProperties>;

    using PaintAttributeList = typename Binders::AttributeList;
    using AttributeList = TypeListConcat<LayoutAndSizeAttributeList, PaintAttributeList>;
    using AttributeBindings = gfx::AttributeBindings<AttributeList>;

    using LayoutUniformValues = gfx::UniformValues<LayoutUniformList>;
    using SizeUniformList = typename SymbolSizeBinder::UniformList;
    using PaintUniformList = typename Binders::UniformList;
    using UniformList = TypeListConcat<LayoutUniformList, SizeUniformList, PaintUniformList>;
    using UniformValues = gfx::UniformValues<UniformList>;

    using TextureList = Textures;
    using TextureBindings = gfx::TextureBindings<TextureList>;

    std::unique_ptr<gfx::Program<Name>> program;

    // NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
    SymbolProgram([[maybe_unused]] const ProgramParameters& programParameters) {
        switch (gfx::Backend::GetType()) {
#if MLN_RENDER_BACKEND_OPENGL
            case gfx::Backend::Type::OpenGL: {
                program = std::make_unique<gl::Program<Name>>(programParameters.withDefaultSource(
                    {gfx::Backend::Type::OpenGL,
                     shaders::ShaderSource<ShaderSource, gfx::Backend::Type::OpenGL>::vertex,
                     shaders::ShaderSource<ShaderSource, gfx::Backend::Type::OpenGL>::fragment}));
                break;
            }
#endif
            default: {
                throw std::runtime_error("Unsupported rendering backend!");
            }
        }
    }

    static UniformValues computeAllUniformValues(const LayoutUniformValues& layoutUniformValues,
                                                 const SymbolSizeBinder& symbolSizeBinder,
                                                 const Binders& paintPropertyBinders,
                                                 const typename PaintProperties::PossiblyEvaluated& currentProperties,
                                                 float currentZoom) {
        return layoutUniformValues.concat(symbolSizeBinder.uniformValues(currentZoom))
            .concat(paintPropertyBinders.uniformValues(currentZoom, currentProperties));
    }

    static AttributeBindings computeAllAttributeBindings(
        const gfx::VertexBuffer<LayoutVertex>& layoutVertexBuffer,
        const gfx::VertexBuffer<gfx::Vertex<SymbolDynamicLayoutAttributes>>& dynamicLayoutVertexBuffer,
        const gfx::VertexBuffer<gfx::Vertex<SymbolOpacityAttributes>>& opacityVertexBuffer,
        const Binders& paintPropertyBinders,
        const typename PaintProperties::PossiblyEvaluated& currentProperties) {
        assert(layoutVertexBuffer.elements == dynamicLayoutVertexBuffer.elements &&
               layoutVertexBuffer.elements == opacityVertexBuffer.elements);
        return gfx::AttributeBindings<LayoutAttributeList>(layoutVertexBuffer)
            .concat(gfx::AttributeBindings<SymbolDynamicLayoutAttributes>(dynamicLayoutVertexBuffer))
            .concat(gfx::AttributeBindings<SymbolOpacityAttributes>(opacityVertexBuffer))
            .concat(paintPropertyBinders.attributeBindings(currentProperties));
    }

    static uint32_t activeBindingCount(const AttributeBindings& allAttributeBindings) {
        return allAttributeBindings.activeCount();
    }

    template <class DrawMode>
    void draw(gfx::Context& context,
              gfx::RenderPass& renderPass,
              const DrawMode& drawMode,
              const gfx::DepthMode& depthMode,
              const gfx::StencilMode& stencilMode,
              const gfx::ColorMode& colorMode,
              const gfx::CullFaceMode& cullFaceMode,
              const gfx::IndexBuffer& indexBuffer,
              const Segment<AttributeList>& segment,
              const UniformValues& uniformValues,
              const AttributeBindings& allAttributeBindings,
              const TextureBindings& textureBindings,
              const std::string& layerID) {
        static_assert(Primitive == gfx::PrimitiveTypeOf<DrawMode>::value, "incompatible draw mode");

        if (!program) {
            return;
        }

        auto drawScopeIt = segment.drawScopes.find(layerID);
        if (drawScopeIt == segment.drawScopes.end()) {
            drawScopeIt = segment.drawScopes.emplace(layerID, context.createDrawScope()).first;
        }

        program->draw(context,
                      renderPass,
                      drawMode,
                      depthMode,
                      stencilMode,
                      colorMode,
                      cullFaceMode,
                      uniformValues,
                      drawScopeIt->second,
                      allAttributeBindings.offset(segment.vertexOffset),
                      textureBindings,
                      indexBuffer,
                      segment.indexOffset,
                      segment.indexLength);
    }

    template <class DrawMode>
    void draw(gfx::Context& context,
              gfx::RenderPass& renderPass,
              const DrawMode& drawMode,
              const gfx::DepthMode& depthMode,
              const gfx::StencilMode& stencilMode,
              const gfx::ColorMode& colorMode,
              const gfx::CullFaceMode& cullFaceMode,
              const gfx::IndexBuffer& indexBuffer,
              const SegmentVector<AttributeList>& segments,
              const UniformValues& uniformValues,
              const AttributeBindings& allAttributeBindings,
              const TextureBindings& textureBindings,
              const std::string& layerID) {
        static_assert(Primitive == gfx::PrimitiveTypeOf<DrawMode>::value, "incompatible draw mode");

        if (!program) {
            return;
        }

        for (const auto& segment : segments) {
            draw(context,
                 renderPass,
                 drawMode,
                 depthMode,
                 stencilMode,
                 colorMode,
                 cullFaceMode,
                 indexBuffer,
                 segment,
                 uniformValues,
                 allAttributeBindings,
                 textureBindings,
                 layerID);
        }
    }
};

class SymbolIconProgram final : public SymbolProgram<SymbolIconProgram,
                                                     shaders::BuiltIn::SymbolIconProgram,
                                                     gfx::PrimitiveType::Triangle,
                                                     SymbolLayoutAttributes,
                                                     TypeList<uniforms::matrix,
                                                              uniforms::label_plane_matrix,
                                                              uniforms::coord_matrix,
                                                              uniforms::extrude_scale,
                                                              uniforms::texsize,
                                                              uniforms::fade_change,
                                                              uniforms::is_text,
                                                              uniforms::camera_to_center_distance,
                                                              uniforms::pitch,
                                                              uniforms::pitch_with_map,
                                                              uniforms::rotate_symbol,
                                                              uniforms::aspect_ratio>,
                                                     TypeList<textures::texture>,
                                                     style::IconPaintProperties> {
public:
    static constexpr std::string_view Name{"SymbolIconProgram"};
    const std::string_view typeName() const noexcept override { return Name; }

    using SymbolProgram::SymbolProgram;

    static LayoutUniformValues layoutUniformValues(bool isText,
                                                   bool hasVariablePacement,
                                                   const style::SymbolPropertyValues&,
                                                   const Size& texsize,
                                                   const std::array<float, 2>& pixelsToGLUnits,
                                                   bool alongLine,
                                                   const RenderTile&,
                                                   const TransformState&,
                                                   float symbolFadeChange);
};

enum class SymbolSDFPart {
    Fill = 1,
    Halo = 0
};

using SymbolSDFProgramUniforms = TypeList<uniforms::matrix,
                                          uniforms::label_plane_matrix,
                                          uniforms::coord_matrix,
                                          uniforms::extrude_scale,
                                          uniforms::texsize,
                                          uniforms::fade_change,
                                          uniforms::is_text,
                                          uniforms::camera_to_center_distance,
                                          uniforms::pitch,
                                          uniforms::pitch_with_map,
                                          uniforms::rotate_symbol,
                                          uniforms::aspect_ratio,
                                          uniforms::gamma_scale,
                                          uniforms::device_pixel_ratio,
                                          uniforms::is_halo>;

template <class Name, shaders::BuiltIn ShaderSource, class PaintProperties>
// NOLINTNEXTLINE(bugprone-crtp-constructor-accessibility)
class SymbolSDFProgram : public SymbolProgram<Name,
                                              ShaderSource,
                                              gfx::PrimitiveType::Triangle,
                                              SymbolLayoutAttributes,
                                              SymbolSDFProgramUniforms,
                                              TypeList<textures::texture>,
                                              PaintProperties> {
public:
    using BaseProgram = SymbolProgram<Name,
                                      ShaderSource,
                                      gfx::PrimitiveType::Triangle,
                                      SymbolLayoutAttributes,
                                      SymbolSDFProgramUniforms,
                                      TypeList<textures::texture>,
                                      PaintProperties>;

    using LayoutUniformValues = typename BaseProgram::LayoutUniformValues;

    using BaseProgram::BaseProgram;

    static LayoutUniformValues layoutUniformValues(bool isText,
                                                   bool hasVariablePacement,
                                                   const style::SymbolPropertyValues&,
                                                   const Size& texsize,
                                                   const std::array<float, 2>& pixelsToGLUnits,
                                                   float pixelRatio,
                                                   bool alongLine,
                                                   const RenderTile&,
                                                   const TransformState&,
                                                   float SymbolFadeChange,
                                                   SymbolSDFPart);
};

using SymbolTextAndIconProgramUniforms = TypeList<uniforms::texsize_icon>;

class SymbolTextAndIconProgram final
    : public SymbolProgram<SymbolTextAndIconProgram,
                           shaders::BuiltIn::SymbolTextAndIconProgram,
                           gfx::PrimitiveType::Triangle,
                           SymbolLayoutAttributes,
                           TypeListConcat<SymbolSDFProgramUniforms, SymbolTextAndIconProgramUniforms>,
                           TypeList<textures::texture, textures::texture_icon>,
                           style::TextPaintProperties> {
public:
    static constexpr std::string_view Name{"SymbolTextAndIconProgram"};
    const std::string_view typeName() const noexcept override { return Name; }

    using BaseProgram = SymbolProgram<SymbolTextAndIconProgram,
                                      shaders::BuiltIn::SymbolTextAndIconProgram,
                                      gfx::PrimitiveType::Triangle,
                                      SymbolLayoutAttributes,
                                      TypeListConcat<SymbolSDFProgramUniforms, SymbolTextAndIconProgramUniforms>,
                                      TypeList<textures::texture, textures::texture_icon>,
                                      style::TextPaintProperties>;

    using LayoutUniformValues = typename BaseProgram::LayoutUniformValues;

    using BaseProgram::BaseProgram;

    static LayoutUniformValues layoutUniformValues(bool hasVariablePacement,
                                                   const style::SymbolPropertyValues&,
                                                   const Size& texsize,
                                                   const Size& texsize_icon,
                                                   const std::array<float, 2>& pixelsToGLUnits,
                                                   float pixelRatio,
                                                   bool alongLine,
                                                   const RenderTile&,
                                                   const TransformState&,
                                                   float SymbolFadeChange,
                                                   SymbolSDFPart);
};

class SymbolSDFIconProgram final : public SymbolSDFProgram<SymbolSDFIconProgram,
                                                           shaders::BuiltIn::SymbolSDFIconProgram,
                                                           style::IconPaintProperties> {
public:
    static constexpr std::string_view Name{"SymbolSDFIconProgram"};
    const std::string_view typeName() const noexcept override { return Name; }

    using SymbolSDFProgram::SymbolSDFProgram;
};

class SymbolSDFTextProgram final : public SymbolSDFProgram<SymbolSDFTextProgram,
                                                           shaders::BuiltIn::SymbolSDFTextProgram,
                                                           style::TextPaintProperties> {
public:
    static constexpr std::string_view Name{"SymbolSDFTextProgram"};
    const std::string_view typeName() const noexcept override { return Name; }

    using SymbolSDFProgram::SymbolSDFProgram;
};

using SymbolLayoutVertex = gfx::Vertex<SymbolLayoutAttributes>;
using SymbolIconAttributes = SymbolIconProgram::AttributeList;
using SymbolTextAttributes = SymbolSDFTextProgram::AttributeList;

} // namespace mbgl
