// Copyright 2015 XLGAMES Inc.
//
// Distributed under the MIT License (See
// accompanying file "LICENSE" or the website
// http://www.opensource.org/licenses/mit-license.php)

#include "DynamicImposters.h"
#include "GestaltResource.h"
#include "SceneEngineUtils.h"
#include "../RenderCore/Metal/State.h"
#include "../RenderCore/Metal/DeviceContext.h"
#include "../RenderCore/Metal/Shader.h"
#include "../RenderCore/Metal/ShaderResource.h"
#include "../RenderCore/Metal/GPUProfiler.h"
#include "../RenderCore/Techniques/ParsingContext.h"
#include "../RenderCore/Techniques/CommonResources.h"
#include "../RenderCore/Techniques/CommonBindings.h"
#include "../RenderCore/Techniques/TechniqueMaterial.h"
#include "../RenderCore/Techniques/RenderStateResolver.h"
#include "../RenderCore/Assets/ModelRunTime.h"
#include "../RenderCore/Assets/DelayedDrawCall.h"
#include "../RenderCore/Assets/SharedStateSet.h"
#include "../BufferUploads/ResourceLocator.h"
#include "../Assets/Assets.h"
#include "../ConsoleRig/Console.h"
#include "../Math/Transformations.h"
#include "../Math/RectanglePacking.h"
#include "../Utility/IteratorUtils.h"
#include "../Utility/MemoryUtils.h"
#include "../Utility/FunctionUtils.h"
#include "../Utility/StringFormat.h"
#include "../Utility/Meta/ClassAccessors.h"
#include "../Utility/Meta/ClassAccessorsImpl.h"
#include "../Utility/Meta/AccessorSerialize.h"

#include "../RenderCore/DX11/Metal/IncludeDX11.h"

namespace SceneEngine
{
    using namespace RenderCore;

    static unsigned GetXYAngle(
        const RenderCore::Assets::ModelScaffold& scaffold,
        const Float3x4& localToWorld, const Float3& cameraPosition, 
        unsigned angleQuant)
    {
            // We're going to get a rough XY angle value from 
            // this matrix... We just want an angle based on the 
            // direction between the center of the object and the
            // camera.
            // Note that we could just use the origin of local space
            // to skip some steps here for a faster result.
        const auto& bb = scaffold.GetStaticBoundingBox();
        Float3 bbCenter = (bb.first + bb.second) * 0.5f;
        
        Float3 worldCenter = TransformPoint(localToWorld, bbCenter);
        Float3 offset = cameraPosition - worldCenter;
        float angle = XlATan2(offset[1], offset[0]);        // note -- z ignored.

            // assuming out atan implementation returns a value between
            // -pi and pi...
            // Note the offset by half our range to make sure that 0.f
            // is right in the middle of one quantized range.
        float range = 2.f * gPI / float(angleQuant);
        auto result = (unsigned)XlFloor((angle + gPI + .5f * range) / range);
        result = result % angleQuant;
        assert(result >= 0 && result < angleQuant);
        return result;
    }

    static Float2 XYAngleAsVector(unsigned xyAngle, unsigned angleQuant)
    {
            // get a "forward" vector that corresponds to the given xy angle.
        const auto factor = float(2.f * gPI) / float(angleQuant);
        const auto angle = float(xyAngle) * factor - gPI;
        return Float2(XlCos(angle), XlSin(angle));
    }

    static UInt2 MipMapDims(UInt2 dims, unsigned mipMapLevel)
    {
        return UInt2(
            std::max(dims[0] >> mipMapLevel, 1u),
            std::max(dims[1] >> mipMapLevel, 1u));
    }

    class ImposterSpriteAtlas
    {
    public:
            // We have 2 options for our altas
            //      we can store a post-lighting image of the object
            //      Or we can store the deferred shading parameters 
            //      (like diffuse, normal, specular parameters, etc).
            // Lets' build in some flexibility so we can select which
            // method to use.

        class Layer
        {
        public:
            GestaltTypes::RTVSRV    _atlas;
            GestaltTypes::RTVSRV    _tempRTV;

            Layer() {}
            ~Layer() {}
            Layer(Layer&& moveFrom) never_throws : _atlas(std::move(moveFrom._atlas)), _tempRTV(std::move(moveFrom._tempRTV)) {}
            Layer& operator=(Layer&& moveFrom) never_throws
            {
                _atlas = std::move(moveFrom._atlas);
                _tempRTV = std::move(moveFrom._tempRTV);
                return *this;
            }
            Layer(GestaltTypes::RTVSRV&& atlas, GestaltTypes::RTVSRV&& tempRTV)
                : _atlas(std::move(atlas)), _tempRTV(std::move(tempRTV)) {}
        };
        std::vector<Layer>  _layers;
        GestaltTypes::DSV   _tempDSV;

        ImposterSpriteAtlas(UInt2 dims, UInt2 tempSurfaceDims, std::initializer_list<Metal::NativeFormat::Enum> layers);
        ImposterSpriteAtlas(ImposterSpriteAtlas&& moveFrom) never_throws;
        ImposterSpriteAtlas& operator=(ImposterSpriteAtlas&& moveFrom) never_throws;
        ImposterSpriteAtlas();
        ~ImposterSpriteAtlas();
    };

    ImposterSpriteAtlas::ImposterSpriteAtlas(UInt2 dims, UInt2 tempSurfaceDims, std::initializer_list<Metal::NativeFormat::Enum> layers)
    {
        using namespace BufferUploads;
        _tempDSV = GestaltTypes::DSV(
            TextureDesc::Plain2D(tempSurfaceDims[0], tempSurfaceDims[1], Metal::NativeFormat::D16_UNORM),
            "SpriteAtlasTemp");
        _layers.reserve(layers.size());
        for (const auto& l:layers)
            _layers.emplace_back(Layer(
                GestaltTypes::RTVSRV(TextureDesc::Plain2D(dims[0], dims[1], (unsigned)l), "SpriteAtlas"),
                GestaltTypes::RTVSRV(TextureDesc::Plain2D(tempSurfaceDims[0], tempSurfaceDims[1], (unsigned)l), "SpriteAtlasTemp")));
    }

    ImposterSpriteAtlas::ImposterSpriteAtlas(ImposterSpriteAtlas&& moveFrom) never_throws
    : _layers(std::move(moveFrom._layers))
    , _tempDSV(std::move(moveFrom._tempDSV))
    {}

    ImposterSpriteAtlas& ImposterSpriteAtlas::operator=(ImposterSpriteAtlas&& moveFrom) never_throws
    {
        _layers = std::move(moveFrom._layers);
        _tempDSV = std::move(moveFrom._tempDSV);
        return *this;
    }

    ImposterSpriteAtlas::ImposterSpriteAtlas() {}
    ImposterSpriteAtlas::~ImposterSpriteAtlas() {}

    static const Metal::InputElementDesc s_inputLayout[] = 
    {
        Metal::InputElementDesc("POSITION", 0, Metal::NativeFormat::R32G32B32_FLOAT),
        Metal::InputElementDesc("XAXIS", 0, Metal::NativeFormat::R32G32B32_FLOAT),
        Metal::InputElementDesc("YAXIS", 0, Metal::NativeFormat::R32G32B32_FLOAT),
        Metal::InputElementDesc("SIZE", 0, Metal::NativeFormat::R32G32_FLOAT),
        Metal::InputElementDesc("SPRITEINDEX", 0, Metal::NativeFormat::R32_UINT)
    };

    class DynamicImposters::Pimpl
    {
    public:
        SharedStateSet* _sharedStateSet;
        std::shared_ptr<Techniques::IStateSetResolver> _stateResolver;

        class QueuedObject
        {
        public:
            const ModelRenderer*    _renderer;
            const ModelScaffold*    _scaffold;
            unsigned                _XYangle;

            uint64 MakeHash() const { return HashCombine(IntegerHash64(size_t(_renderer)), _XYangle); }
        };
        std::vector<std::pair<uint64, QueuedObject>> _queuedObjects;
        std::vector<std::pair<uint64, Float3>> _queuedInstances;

        Config _config;

        static const unsigned MipMapCount = 5;

        using Rectangle = std::pair<UInt2, UInt2>;
        class PreparedSprite
        {
        public:
            Rectangle _rect[MipMapCount];
            Float3 _projectionCentre;
            Float2 _worldSpaceHalfSize;
            uint64 _lastUsed;

            PreparedSprite()
            {
                XlZeroMemory(_rect);
                _projectionCentre = Zero<Float3>();
                _worldSpaceHalfSize = Zero<Float2>();
                _lastUsed = 0;
            }
        };
        std::vector<std::pair<uint64, PreparedSprite>> _preparedSprites;

        RectanglePacker _packer;
        ImposterSpriteAtlas _atlas;
        Techniques::TechniqueMaterial _material;

        unsigned _overflowCounter;
        unsigned _pendingCounter;
        unsigned _copyCounter;

        void BuildNewSprites(
            Metal::DeviceContext& context,
            Techniques::ParsingContext& parserContext);
        PreparedSprite BuildSprite(
            Metal::DeviceContext& context,
            Techniques::ParsingContext& parserContext,
            const QueuedObject& ob);
        void RenderObject(
            Metal::DeviceContext& context,
            Techniques::ParsingContext& parserContext,
            const QueuedObject& ob, 
            const Float4x4& cameraToWorld,
            const Metal::ViewportDesc& viewport,
            const Float4x4& projMatrix) const;
        void CopyToAltas(
            Metal::DeviceContext& context,
            const Rectangle& destination,
            const Rectangle& source);
    };

    static Utility::Internal::StringMeldInPlace<char> QuickMetrics(
        Techniques::ParsingContext& parserContext)
    {
        return StringMeldAppend(parserContext._stringHelpers->_quickMetrics);
    }

    void DynamicImposters::Pimpl::BuildNewSprites(
        Metal::DeviceContext& context,
        Techniques::ParsingContext& parserContext)
    {
        ProtectState protectState;
        const ProtectState::States::BitField volatileStates =
                ProtectState::States::RenderTargets
            | ProtectState::States::Viewports
            | ProtectState::States::DepthStencilState
            | ProtectState::States::BlendState;
        bool initProtectState = false;

        const unsigned maxPreparePerFrame = Tweakable("ImpostersReset", false) ? INT_MAX : 3;
        unsigned preparedThisFrame = 0;

        auto preparedSpritesI = _preparedSprites.begin();
        for (const auto& o:_queuedObjects) {
            uint64 hash = o.first;
            preparedSpritesI = std::lower_bound(preparedSpritesI, _preparedSprites.end(), hash, CompareFirst<uint64, Pimpl::PreparedSprite>());
            if (preparedSpritesI != _preparedSprites.end() && preparedSpritesI->first == hash) {
            } else {
                if (!initProtectState) {
                    protectState = ProtectState(context, volatileStates);
                    initProtectState = true;
                }

                if (preparedThisFrame++ >= maxPreparePerFrame) {
                    ++_pendingCounter;
                    continue;
                }

                TRY {
                    auto newSprite = BuildSprite(context, parserContext, o.second);
                    if (newSprite._rect[0].second[0] > newSprite._rect[0].first[0]) {
                        preparedSpritesI = _preparedSprites.insert(preparedSpritesI, std::make_pair(hash, newSprite));
                    } else {
                        ++_overflowCounter;
                    }
                } CATCH(const ::Assets::Exceptions::AssetException&e) {
                    parserContext.Process(e);
                    ++_pendingCounter;
                } CATCH_END
            }
        }
    }

    void DynamicImposters::Render(
        RenderCore::Metal::DeviceContext& context,
        RenderCore::Techniques::ParsingContext& parserContext,
        unsigned techniqueIndex)
    {
        if (_pimpl->_queuedInstances.empty() || _pimpl->_queuedObjects.empty())
            return;

        auto shader = _pimpl->_material.FindVariation(
            parserContext, techniqueIndex, "game/xleres/vegetation/impostermaterial.txt");
        if (!shader._shaderProgram) return;

            // For each object here, we should look to see if we have a prepared
            // imposter already available. If not, we have to build the imposter.
        _pimpl->BuildNewSprites(context, parserContext);

        class Vertex
        {
        public:
            Float3 _pos; 
            Float3 _xAxis, _uAxis;
            Float2 _size;
            unsigned _spriteIndex;
        };
        std::vector<Vertex> vertices;
        vertices.reserve(_pimpl->_queuedInstances.size());

        auto& projDesc = parserContext.GetProjectionDesc();
        auto cameraRight = ExtractRight_Cam(projDesc._cameraToWorld);
        auto cameraUp = Float3(0.f, 0.f, 1.f); // ExtractUp_Cam(projDesc._cameraToWorld);

            // Now just render a sprite for each instance requested
            // Here, we can choose to sort the instances by sprite id,
            // or just leave them in their original order. All of the
            // sprite textures at in the same atlas; so it's ok to mix
            // the instances up together.
            // In some cases we might want to sort the sprites back-to-front
            // (since they are close to camera facing, we should get correct
            // sorting when sorting on a per-sprite basis)
        std::sort(_pimpl->_queuedInstances.begin(), _pimpl->_queuedInstances.end(), CompareFirst<uint64, Float3>());

        auto preparedSpritesI = _pimpl->_preparedSprites.begin();
        for (auto i=_pimpl->_queuedInstances.cbegin(); i!=_pimpl->_queuedInstances.cend();) {
            uint64 hash = i->first;
            auto start = i++;
            while (i < _pimpl->_queuedInstances.cend() && i->first == hash) ++i;

            preparedSpritesI = std::lower_bound(preparedSpritesI, _pimpl->_preparedSprites.end(), hash, CompareFirst<uint64, Pimpl::PreparedSprite>());
            if (preparedSpritesI == _pimpl->_preparedSprites.end() || preparedSpritesI->first != hash)
                continue;

                // We should have a number of sprites of the same instance. Expand the sprite into 
                // triangles here, on the CPU. 
                // We'll do the projection here because we need to use the parameters from how the
                // sprite was originally projected.

            unsigned spriteIndex = (unsigned)std::distance(_pimpl->_preparedSprites.begin(), preparedSpritesI);
            Float3 projectionCenter = preparedSpritesI->second._projectionCentre;
            Float2 projectionSize = preparedSpritesI->second._worldSpaceHalfSize;

            for (auto s=start; s<i; ++s)
                vertices.push_back(Vertex {s->second + projectionCenter, cameraRight, cameraUp, projectionSize, spriteIndex});
        }

        if (vertices.empty()) return;

            // We don't really have to rebuild this every time. We should only need to 
            // rebuild this table when the prepared sprites array changes.
        class SpriteTableElement
        {
        public:
            UInt4 _coords[Pimpl::MipMapCount];
        };
        auto spriteTable = MakeSharedPktSize(sizeof(UInt4)*512); // sizeof(SpriteTableElement) * _pimpl->_preparedSprites.size());
        const unsigned maxSprites = 512 / Pimpl::MipMapCount;
        for (unsigned c=0; c<std::min(unsigned(_pimpl->_preparedSprites.size()), maxSprites); ++c) {
            auto& e = ((SpriteTableElement*)spriteTable.get())[c];
            for (unsigned m=0; m<Pimpl::MipMapCount; ++m) {
                const auto &r = _pimpl->_preparedSprites[c].second._rect[m];
                e._coords[m] = UInt4(r.first[0], r.first[1], r.second[0], r.second[1]);
            }
        }

            // bind the atlas textures to the find slots (eg, over DiffuseTexture, NormalsTexture, etc)
        for (unsigned c=0; c<_pimpl->_atlas._layers.size(); ++c)
            context.BindPS(MakeResourceList(c, _pimpl->_atlas._layers[c]._atlas.SRV()));

        shader.Apply(context, parserContext, {std::move(spriteTable)});
        
        Metal::VertexBuffer tempvb(AsPointer(vertices.begin()), vertices.size()*sizeof(Vertex));
        context.Bind(MakeResourceList(tempvb), sizeof(Vertex), 0);
        context.Bind(Metal::Topology::PointList);
        context.Bind(Techniques::CommonResources()._blendOpaque);
        context.Bind(Techniques::CommonResources()._dssReadWrite);
        context.Bind(Techniques::CommonResources()._defaultRasterizer);
        context.Draw((unsigned)vertices.size());

        QuickMetrics(parserContext) << "Imposters: Drawing (" << vertices.size() << ") instances from (" << _pimpl->_queuedObjects.size() << ") unique types.\n";
    }

    auto DynamicImposters::Pimpl::BuildSprite(
        RenderCore::Metal::DeviceContext& context,
        RenderCore::Techniques::ParsingContext& parserContext,
        const QueuedObject& ob) -> PreparedSprite
    {
        Metal::GPUProfiler::DebugAnnotation annon(context, L"Imposter-Prepare");

            // First we need to calculate the correct size for the sprite required
            // for this object. Because we're using a generic rectangle packing
            // algorithm, we can choose any size.
            // We want to try to get a 1:1 ratio at a certain distance from the
            // camera. So let's create an imaginary camera at a given distance from the
            // object & find the approximate screen space dimensions (based on the 
            // bounding box).
            //
            // We can perhaps optimise this by analysing the render target after rendering
            // -- though it would require a CPU/GPU sync (or some very complex method to 
            // delay moving the render target into the atlas).
            //
            // We're also going to need mip-maps for the sprites. That gives us two options:
            //      1) use power of 2 dimensions (or close to it) and then use the standard
            //          hardware support for mipmaps
            //      2) use unconstrained dimensions, and allocate each mipmap using a separate
            //          packing operation.

        const auto& cfg = _config;
        auto bb = ob._scaffold->GetStaticBoundingBox();
        Float3 centre = .5f * (bb.first + bb.second);
        Float2 cameraForward = -XYAngleAsVector(ob._XYangle, cfg._angleQuant);
        Float3 camera = centre + cfg._calibrationDistance * -Expand(cameraForward, 0.f);
        
            // Find the vertical and horizontal angles for each corner of the bounding box.
        Float3 corners[] = {
            Float3( bb.first[0],  bb.first[1],  bb.first[2]),
            Float3(bb.second[0],  bb.first[1],  bb.first[2]),
            Float3( bb.first[0], bb.second[1],  bb.first[2]),
            Float3(bb.second[0], bb.second[1],  bb.first[2]),
            Float3( bb.first[0],  bb.first[1], bb.second[2]),
            Float3(bb.second[0],  bb.first[1], bb.second[2]),
            Float3( bb.first[0], bb.second[1], bb.second[2]),
            Float3(bb.second[0], bb.second[1], bb.second[2])
        };

        const auto camToWorld = MakeCameraToWorld(Expand(cameraForward, 0.f), Float3(0.f, 0.f, 1.f), camera);
        const auto worldToCam = InvertOrthonormalTransform(camToWorld);

        const float pixelRatio = 16.f/9.f;    // Adjust the pixel ratio for standard 16:9
        auto virtualProj = PerspectiveProjection(
            cfg._calibrationFov, pixelRatio,
            0.01f, 1000.f, GeometricCoordinateSpace::RightHanded,
            Techniques::GetDefaultClipSpaceType());
        Float2 screenSpaceMin(FLT_MAX, FLT_MAX), screenSpaceMax(-FLT_MAX, -FLT_MAX);
        for (unsigned c=0; c<dimof(corners); ++c) {
            Float4 proj = virtualProj * worldToCam * Expand(corners[c], 1.f);
            Float2 d(proj[0] / proj[3], proj[1] / proj[3]);

            screenSpaceMin[0] = std::min(screenSpaceMin[0], d[0]);
            screenSpaceMin[1] = std::min(screenSpaceMin[1], d[1]);
            screenSpaceMax[0] = std::max(screenSpaceMax[0], d[0]);
            screenSpaceMax[1] = std::max(screenSpaceMax[1], d[1]);
        }

        Float2 c(.5f * (screenSpaceMax[0] + screenSpaceMin[0]), .5f * (screenSpaceMax[1] + screenSpaceMin[1]));
        Float4x4 adjustmentMatrix(
            2.f / (screenSpaceMax[0] - screenSpaceMin[0]), 0.f, 0.f, (2.f * c[0]) / (screenSpaceMax[0] - screenSpaceMin[0]),
            0.f, 2.f / (screenSpaceMax[1] - screenSpaceMin[1]), 0.f, (2.f * c[1]) / (screenSpaceMax[1] - screenSpaceMin[1]),
            0.f, 0.f, 1.f, 0.f,
            0.f, 0.f, 0.f, 1.f);

        Int2 minCoords(
            (int)XlFloor((screenSpaceMin[0] * .5f + .5f) * cfg._calibrationPixels),
            (int)XlFloor((screenSpaceMin[1] * .5f + .5f) * cfg._calibrationPixels));
        Int2 maxCoords(
            (int)XlFloor((screenSpaceMax[0] * .5f + .5f) * cfg._calibrationPixels),
            (int)XlFloor((screenSpaceMax[1] * .5f + .5f) * cfg._calibrationPixels));

            // If the object is too big, we need to constrain the dimensions
            // note that we should adjust the projection matrix as well, to
            // match this.
        UInt2 dims = maxCoords - minCoords;
        if (dims[0] > cfg._maxDims[0]) {
            minCoords[0] += (dims[0] - cfg._maxDims[0]) / 2;
            maxCoords[0] -= (dims[0] - cfg._maxDims[0]) / 2 + 1;
            dims[0] = maxCoords[0] - minCoords[0];
            assert(dims[0] <= cfg._maxDims[0]);
        }
        if (dims[0] < cfg._minDims[0]) {
            minCoords[0] -= (cfg._minDims[0] - dims[0]) / 2;
            maxCoords[0] += (cfg._minDims[0] - dims[0]) / 2 + 1;
            dims[0] = maxCoords[0] - minCoords[0];
            assert(dims[0] >= cfg._minDims[0]);
        }
        if (dims[1] > cfg._maxDims[1]) {
            minCoords[1] += (dims[1] - cfg._maxDims[1]) / 2;
            maxCoords[1] -= (dims[1] - cfg._maxDims[1]) / 2 + 1;
            dims[1] = maxCoords[1] - minCoords[1];
            assert(dims[1] <= cfg._maxDims[1]);
        }
        if (dims[1] < cfg._minDims[1]) {
            minCoords[1] -= (cfg._minDims[1] - dims[1]) / 2;
            maxCoords[1] += (cfg._minDims[1] - dims[1]) / 2 + 1;
            dims[1] = maxCoords[1] - minCoords[1];
            assert(dims[1] >= cfg._minDims[1]);
        }

            // Reserve some space before we render
            // Note that we have to be careful here, because this operation must
            // be reversable. If there is an exception during rendering (eg, pending
            // asset) or a failure while allocating space for mip-maps, then we want
            // to revert to our previous state.
            // Note that copying the packer here results in an allocation... It would
            // be good if we could avoid this allocation!
        RectanglePacker newPacker = _packer;

        Rectangle reservedSpace[MipMapCount];
        for (unsigned c=0; c<MipMapCount; ++c) {
            reservedSpace[c] = newPacker.Add(MipMapDims(dims, c));
            if (    reservedSpace[c].first[0] >= reservedSpace[c].second[0]
                ||  reservedSpace[c].first[1] >= reservedSpace[c].second[1]) {
                return PreparedSprite();
            }
        }

            // Render the object to our temporary buffer with the given camera
            // and focus points (and the viewport we've calculated)
            // Is it best to render with a perspective camera? Or orthogonal?
            // Because the sprite is used at different distances, the perspective
            // is not fixed... But a fixed perspective camera might still be a better
            // approximation than an orthogonal camera.
        RenderObject(
            context, parserContext, ob,
            camToWorld, Metal::ViewportDesc(0.f, 0.f, float(maxCoords[0] - minCoords[0]), float(maxCoords[1] - minCoords[1])),
            adjustmentMatrix * virtualProj);

            // Now we want to copy the rendered sprite into the atlas
            // We should generate the mip-maps in this step as well.
        for (unsigned c=0; c<MipMapCount; ++c) {
            CopyToAltas(
                context, reservedSpace[c],
                Rectangle(UInt2(0, 0), maxCoords - minCoords));
        }

            // once everything is complete (and there is no further possibility of an exception,
            // we should commit our changes to the rectangle packer)
        _packer = newPacker;

        for (unsigned c=0; c<MipMapCount; ++c)
            _copyCounter += 
                    (reservedSpace[c].second[0] - reservedSpace[c].first[0])
                *   (reservedSpace[c].second[1] - reservedSpace[c].first[1])
                ;

        PreparedSprite result;
        for (unsigned c=0; c<MipMapCount; ++c)
            result._rect[c] = reservedSpace[c];
        result._lastUsed = 0;
        result._projectionCentre = centre;
            // We can get the world space half size directly
            // from the bounding box. If we built the projection correctly (and 
            // if the camera is reasonably far from the object), then it should
            // be a simple calculation.
        result._worldSpaceHalfSize[0] = 0.5f * std::max(bb.second[0] - bb.first[0], bb.second[1] - bb.first[1]); // worldSpaceHalfSizeX;
        result._worldSpaceHalfSize[1] = 0.5f * (bb.second[2] - bb.first[2]);
        return result;
    }

    void DynamicImposters::Pimpl::CopyToAltas(
        Metal::DeviceContext& context,
        const Rectangle& destination,
        const Rectangle& source)
    {
        // Copy from the temporary render surface of the altas into
        // the final sprite destination. This will sometimes require
        // re-sampling (for mipmap generation).
        //
        // Let's use a shader-based copy.
        //
        // In an ideal world we would do compression in this step.
        // nVidia has some cuda-based texture compressors that could
        // presumedly compress the texture on the GPU (without needing
        // CPU involvement)
        for (unsigned c=0; c<unsigned(_atlas._layers.size()); ++c) {

                // note that this function isn't ideal currently
                //      -- mipmaps aren't made well (too few samples are used, it's not a proper filter)
            const auto& l = _atlas._layers[c];
            ShaderBasedCopy(
                context,
                l._atlas.RTV(), l._tempRTV.SRV(),
                destination, source, 0);
        }
    }

    void DynamicImposters::Pimpl::RenderObject(
        Metal::DeviceContext& context,
        Techniques::ParsingContext& parserContext,
        const QueuedObject& ob, 
        const Float4x4& cameraToWorld,
        const Metal::ViewportDesc& viewport,
        const Float4x4& cameraToProjection) const
    {
        StringMeldAppend(parserContext._stringHelpers->_errorString)
            << "Building dynamic imposter: (" << ob._scaffold->Filename() << ") angle (" << ob._XYangle << ")\n";

        context.Clear(_atlas._tempDSV.DSV(), 1.f, 0u);

        Metal::RenderTargetView::UnderlyingType rtvs[3];
        auto layerCount = std::min(dimof(rtvs), _atlas._layers.size());
        for (unsigned c=0; c<layerCount; ++c) {
            const auto& l = _atlas._layers[c];
            context.Clear(l._tempRTV.RTV(), {0.f, 0.f, 0.f, 0.f});
            rtvs[c] = l._tempRTV.RTV().GetUnderlying();
        }
        
        context.GetUnderlying()->OMSetRenderTargets((unsigned)layerCount, rtvs, _atlas._tempDSV.DSV().GetUnderlying());
        context.Bind(viewport);
        context.Bind(Techniques::CommonResources()._blendOpaque);
        context.Bind(Techniques::CommonResources()._dssReadWrite);

            // We have to adjust the projection desc and the
            // main transform constant buffer...
            // We will squish the FOV to align the object perfectly within our
            // perspective frustum
        auto oldProjDesc = parserContext.GetProjectionDesc();

        auto& projDesc = parserContext.GetProjectionDesc();
        std::tie(projDesc._verticalFov, projDesc._aspectRatio) = 
            CalculateFov(ExtractMinimalProjection(cameraToProjection), Techniques::GetDefaultClipSpaceType());

        Float4x4 modelToWorld = Identity<Float4x4>();
        const bool viewSpaceNormals = true;

            // Normally the deferred shader writes out world space normals.
            // However, we can cause it to write view space normals by
            // defining world space to be the same as view space.
            // That is, model->world actually transforms into camera space.
            // So, worldToProjection becomes just cameraToProjection
        if (constant_expression<viewSpaceNormals>::result()) {
            projDesc._worldToProjection = cameraToProjection;
            projDesc._cameraToProjection = cameraToProjection;
            projDesc._cameraToWorld = Identity<Float4x4>();
            modelToWorld = InvertOrthonormalTransform(cameraToWorld);
        } else {
            projDesc._worldToProjection = Combine(InvertOrthonormalTransform(cameraToWorld), cameraToProjection);
            projDesc._cameraToProjection = cameraToProjection;
            projDesc._cameraToWorld = cameraToWorld;
        }

        auto cleanup = MakeAutoCleanup(
            [&projDesc, &oldProjDesc, &context, &parserContext]() 
            { 
                projDesc = oldProjDesc;
                auto globalTransform = BuildGlobalTransformConstants(projDesc);
                parserContext.SetGlobalCB(
                    context, Techniques::TechniqueContext::CB_GlobalTransform,
                    &globalTransform, sizeof(globalTransform));
                parserContext.GetTechniqueContext()._runtimeState.SetParameter(u("DECAL_BLEND"), 0u);
            });

        auto globalTransform = BuildGlobalTransformConstants(projDesc);
        parserContext.SetGlobalCB(
            context, Techniques::TechniqueContext::CB_GlobalTransform,
            &globalTransform, sizeof(globalTransform));

            // We need to use the "DECAL_BLEND" mode.
            // This writes the blending alpha to the destination target
        parserContext.GetTechniqueContext()._runtimeState.SetParameter(u("DECAL_BLEND"), 1u);

            // Now we can just render the object.
            // Let's use a temporary DelayedDrawCallSet
            // todo -- it might be ideal to use an MSAA target for this step

        RenderCore::Assets::DelayedDrawCallSet drawCalls(typeid(ModelRenderer).hash_code());
        ob._renderer->Prepare(
            drawCalls, *_sharedStateSet,
            modelToWorld, RenderCore::Assets::MeshToModel(*ob._scaffold));
        ModelRenderer::Sort(drawCalls);

        auto marker = _sharedStateSet->CaptureState(context, _stateResolver, nullptr);
        ModelRenderer::RenderPrepared(
            RenderCore::Assets::ModelRendererContext(context, parserContext, Techniques::TechniqueIndex::Deferred),
            *_sharedStateSet, drawCalls, RenderCore::Assets::DelayStep::OpaqueRender);

            // we also have to render the rest of the geometry (using the same technique)
            // otherwise this geometry will never be rendered.
        ModelRenderer::RenderPrepared(
            RenderCore::Assets::ModelRendererContext(context, parserContext, Techniques::TechniqueIndex::Deferred),
            *_sharedStateSet, drawCalls, RenderCore::Assets::DelayStep::PostDeferred);
        ModelRenderer::RenderPrepared(
            RenderCore::Assets::ModelRendererContext(context, parserContext, Techniques::TechniqueIndex::Deferred),
            *_sharedStateSet, drawCalls, RenderCore::Assets::DelayStep::SortedBlending);
    }

    void DynamicImposters::Reset()
    {
        _pimpl->_queuedObjects.clear();
        _pimpl->_queuedInstances.clear();
        _pimpl->_overflowCounter = 0;
        _pimpl->_pendingCounter = 0;

        if (Tweakable("ImpostersReset", false)) {
            _pimpl->_preparedSprites.clear();
            _pimpl->_packer = RectanglePacker(_pimpl->_packer.TotalSize());
            _pimpl->_copyCounter = 0;
        }
    }

    void DynamicImposters::Queue(
        const ModelRenderer& renderer, 
        const ModelScaffold& scaffold,
        const Float3x4& localToWorld,
        const Float3& cameraPosition)
    {
            // Note that we're ignoring most of the rotation information in the local-to-world
            // For objects that are incorrectly exported (eg wrong up vector), we will sometimes 
            // rotate all instances to compenstate. But that will be ignored when we get to this 
            // point!
        auto qo = Pimpl::QueuedObject { &renderer, &scaffold, GetXYAngle(scaffold, localToWorld, cameraPosition, _pimpl->_config._angleQuant) };
        auto hash = qo.MakeHash();
        auto existing = LowerBound(_pimpl->_queuedObjects, hash);

        if (existing == _pimpl->_queuedObjects.end() || existing->first != hash)
            _pimpl->_queuedObjects.insert(existing, std::make_pair(hash, qo));
                
        _pimpl->_queuedInstances.push_back(std::make_pair(hash, ExtractTranslation(localToWorld)));
    }

    void DynamicImposters::Load(const Config& config)
    {
        Reset();
        _pimpl->_preparedSprites.clear();

        _pimpl->_config = config;

        auto atlasSize = Truncate(config._altasSize);
        _pimpl->_packer = RectanglePacker(atlasSize);
        _pimpl->_copyCounter = 0;

            // the formats we initialize for the atlas really depend on whether we're going
            // to be writing pre-lighting or post-lighting parameters to the sprites.
        _pimpl->_atlas = ImposterSpriteAtlas(
            atlasSize, config._maxDims, { Metal::NativeFormat::R8G8B8A8_UNORM_SRGB, Metal::NativeFormat::R8G8B8A8_UNORM});
    }

    void DynamicImposters::Disable()
    {
        Reset();
        _pimpl->_preparedSprites.clear();
        _pimpl->_config = Config();
        _pimpl->_packer = RectanglePacker();
        _pimpl->_atlas = ImposterSpriteAtlas();
    }

    float DynamicImposters::GetThresholdDistance() const { return _pimpl->_config._thresholdDistance; }
    bool DynamicImposters::IsEnabled() const { return !_pimpl->_atlas._layers.empty(); }

    auto DynamicImposters::GetMetrics() const -> Metrics
    {
        Metrics result;
        result._spriteCount = (unsigned)_pimpl->_preparedSprites.size();
        result._pixelsAllocated = 0;
        for (const auto&i:_pimpl->_preparedSprites) {
            for (unsigned m=0; m<dimof(i.second._rect); ++m) {
                const auto& r = i.second._rect[m];
                result._pixelsAllocated += (r.second[0] - r.first[0]) * (r.second[1] - r.first[1]);
            }
        }
        result._pixelsTotal = 
              _pimpl->_config._altasSize[0]
            * _pimpl->_config._altasSize[1]
            * _pimpl->_config._altasSize[2]
            ;
        result._overflowCounter = _pimpl->_overflowCounter;
        result._pendingCounter = _pimpl->_pendingCounter;
        std::tie(result._largestFreeBlockArea, result._largestFreeBlockSide) 
            = _pimpl->_packer.LargestFreeBlock();
        
        result._bytesPerPixel = 
            (Metal::BitsPerPixel(Metal::NativeFormat::R8G8B8A8_UNORM_SRGB) + Metal::BitsPerPixel(Metal::NativeFormat::R8G8B8A8_UNORM)) / 8;
        result._layerCount = (unsigned)_pimpl->_atlas._layers.size();
        return result;
    }

    Metal::ShaderResourceView DynamicImposters::GetAtlasResource(unsigned layer)
    {
        return _pimpl->_atlas._layers[layer]._atlas.SRV();
    }

    DynamicImposters::DynamicImposters(SharedStateSet& sharedStateSet)
    {
        _pimpl = std::make_unique<Pimpl>();
        _pimpl->_sharedStateSet = &sharedStateSet;
        _pimpl->_stateResolver = Techniques::CreateStateSetResolver_Deferred();
        _pimpl->_overflowCounter = 0;
        _pimpl->_pendingCounter = 0;
        _pimpl->_copyCounter = 0;

        _pimpl->_material = Techniques::TechniqueMaterial(
            Metal::InputLayout(s_inputLayout, dimof(s_inputLayout)),
            {Hash64("SpriteTable")}, ParameterBox());
    }

    DynamicImposters::~DynamicImposters() {}

    DynamicImposters::Config::Config()
    {
        _thresholdDistance = 650.f;
        _angleQuant = 8;
        _calibrationDistance = 650.f;
        _calibrationFov = Deg2Rad(30.f);
        _calibrationPixels = 512;
        _minDims = UInt2(32, 32);
        _maxDims = UInt2(128, 128);
        _altasSize = UInt3(512, 512, 1); // UInt3(4096, 2048, 1);
    }

    DynamicImposters::Metrics::Metrics()
    {
        _spriteCount = 0;
        _pixelsAllocated = 0;
        _pixelsTotal = 0;
        _largestFreeBlockArea = UInt2(0,0);
        _largestFreeBlockSide = UInt2(0,0);
        _overflowCounter = 0;
        _pendingCounter = 0;
        _bytesPerPixel = 0;
        _layerCount = 0;
    }
}

template<> const ClassAccessors& GetAccessors<SceneEngine::DynamicImposters::Config>()
{
    using Obj = SceneEngine::DynamicImposters::Config;
    static ClassAccessors props(typeid(Obj).hash_code());
    static bool init = false;
    if (!init) {
        props.Add(u("ThresholdDistance"), DefaultGet(Obj, _thresholdDistance),  DefaultSet(Obj, _thresholdDistance));
        props.Add(u("AngleQuant"), DefaultGet(Obj, _angleQuant),  DefaultSet(Obj, _angleQuant));
        props.Add(u("CalibrationDistance"), DefaultGet(Obj, _calibrationDistance),  DefaultSet(Obj, _calibrationDistance));
        props.Add(u("CalibrationFov"), 
            [](const Obj& obj) { return Rad2Deg(obj._calibrationFov); }, 
            [](Obj& obj, float value) { obj._calibrationFov = Deg2Rad(value); });
        props.Add(u("CalibrationPixels"), DefaultGet(Obj, _calibrationPixels),  DefaultSet(Obj, _calibrationPixels));
        props.Add(u("MinDims"), DefaultGet(Obj, _minDims),  DefaultSet(Obj, _minDims));
        props.Add(u("MaxDims"), DefaultGet(Obj, _maxDims),  DefaultSet(Obj, _maxDims));
        props.Add(u("AltasSize"), DefaultGet(Obj, _altasSize),  DefaultSet(Obj, _altasSize));

        init = true;
    }
    return props;
}