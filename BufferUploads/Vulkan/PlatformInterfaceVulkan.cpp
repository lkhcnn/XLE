// Copyright 2015 XLGAMES Inc.
//
// Distributed under the MIT License (See
// accompanying file "LICENSE" or the website
// http://www.opensource.org/licenses/mit-license.php)

#include "../../Core/Prefix.h"
#include "../../RenderCore/Metal/Metal.h"

#if GFXAPI_ACTIVE == GFXAPI_VULKAN

    #include "../PlatformInterface.h"
    #include "../DataPacket.h"
	#include "../../RenderCore/IThreadContext.h"
    #include "../../RenderCore/Metal/Resource.h"
    #include "../../RenderCore/ResourceUtils.h"

    namespace BufferUploads { namespace PlatformInterface
    {
        using namespace RenderCore;

        unsigned UnderlyingDeviceContext::PushToTexture(
            UnderlyingResource& resource, const BufferDesc& desc,
            const Box2D& box, 
            const ResourceInitializer& data)
        {
            // In Vulkan, the only way we have to send data to a resource is by using
            // a memory map and CPU assisted copy. 
            assert(desc._type == BufferDesc::Type::Texture);
            if (box == Box2D())
                return Metal::CopyViaMemoryMap(*_renderCoreContext->GetDevice(), &resource, data);

            // When we have a box, we support writing to only a single subresource
            // We will iterate through the subresources an mip a single one
            auto dev = _renderCoreContext->GetDevice();
            auto copiedBytes = 0u;
            for (unsigned mip=0; mip<std::max(1u, unsigned(desc._textureDesc._mipCount)); ++mip)
                for (unsigned arrayLayer=0; arrayLayer<std::max(1u, unsigned(desc._textureDesc._arrayCount)); ++arrayLayer) {
                    auto srd = data(mip, arrayLayer);
                    if (!srd._data || !srd._size) continue;

                    Metal::ResourceMap map(*dev, &resource, Metal::Resource::SubResource{mip, arrayLayer});
                    copiedBytes += CopyMipLevel(
                        map.GetData(), map.GetDataSize(), map.GetPitches(), 
                        desc._textureDesc,
                        box, srd);
                }

            return copiedBytes;
        }

        unsigned UnderlyingDeviceContext::PushToStagingTexture(
			UnderlyingResource& resource, const BufferDesc&desc, 
            const Box2D& box,
            const ResourceInitializer& data)
        {
            // Because this is a "staging" resource, we can assume it's not being
            // used currently. So, it's fine to just map the member and use a CPU assisted copy
            // Note that in Vulkan, we can map the entire resource once and just copy each
            // subresource as we go through.
            // The process is the same as PushToTexture...
            return PushToTexture(resource, desc, box, data);
        }

        void UnderlyingDeviceContext::UpdateFinalResourceFromStaging(
			UnderlyingResource& finalResource, UnderlyingResource& staging, 
			const BufferDesc& destinationDesc, 
            unsigned lodLevelMin, unsigned lodLevelMax, unsigned stagingLODOffset)
        {
            auto metalContext = Metal::DeviceContext::Get(*_renderCoreContext);
            if (    (lodLevelMin == ~unsigned(0x0) || lodLevelMax == ~unsigned(0x0)) 
                &&  destinationDesc._type == BufferDesc::Type::Texture && !stagingLODOffset) {
                Metal::Copy(
                    *metalContext, 
                    &finalResource, &staging,
                    Metal::ImageLayout::TransferDstOptimal, Metal::ImageLayout::TransferSrcOptimal);
            } else {
                for (unsigned a=0; a<destinationDesc._textureDesc._arrayCount; ++a) {
                    for (unsigned c=lodLevelMin; c<=lodLevelMax; ++c) {
                        Metal::CopyPartial(
                            *metalContext,
                            Metal::CopyPartial_Dest(&finalResource, Metal::Resource::SubResource{c, a}),
                            Metal::CopyPartial_Src(&staging, Metal::Resource::SubResource{c-stagingLODOffset, a}));
                    }
                }
            }
        }

        unsigned UnderlyingDeviceContext::PushToBuffer(
            UnderlyingResource& resource, const BufferDesc& desc, unsigned offset,
            const void* data, size_t dataSize)
        {
            // note -- this is a direct, immediate map... There must be no contention while we map.
            assert(desc._type == BufferDesc::Type::LinearBuffer);
            Metal::ResourceMap map(*_renderCoreContext->GetDevice(), &resource, Metal::Resource::SubResource{0,0}, offset);
            auto copyAmount = std::min(map.GetDataSize(), dataSize);
            if (copyAmount > 0)
                XlCopyMemory(map.GetData(), data, copyAmount);
            return (unsigned)copyAmount;
        }

        void UnderlyingDeviceContext::ResourceCopy_DefragSteps(
			UnderlyingResource& destination, UnderlyingResource& source, 
			const std::vector<DefragStep>& steps)
        {
        }

        void UnderlyingDeviceContext::ResourceCopy(UnderlyingResource& destination, UnderlyingResource& source)
        {
            auto metalContext = Metal::DeviceContext::Get(*_renderCoreContext);
            assert(metalContext);
            Metal::Copy(*metalContext, &destination, &source);
        }

        Metal::CommandListPtr UnderlyingDeviceContext::ResolveCommandList()
        {
            auto metalContext = Metal::DeviceContext::Get(*_renderCoreContext);
            assert(metalContext);
            return metalContext->ResolveCommandList();;
        }

        void                        UnderlyingDeviceContext::BeginCommandList()
        {
            auto metalContext = Metal::DeviceContext::Get(*_renderCoreContext);
            assert(metalContext);
            metalContext->BeginCommandList();
        }

        intrusive_ptr<DataPacket> UnderlyingDeviceContext::Readback(const ResourceLocator& locator)
        {
            return nullptr;
        }

        // UnderlyingDeviceContext::MappedBuffer UnderlyingDeviceContext::Map(UnderlyingResource& resource, MapType::Enum mapType, unsigned subResource)
        // {
        //     return MappedBuffer();
        // }
        // 
        // UnderlyingDeviceContext::MappedBuffer UnderlyingDeviceContext::MapPartial(UnderlyingResource& resource, MapType::Enum mapType, unsigned offset, unsigned size, unsigned subResource)
        // {
        //     return MappedBuffer();
        // }

        // void UnderlyingDeviceContext::Unmap(UnderlyingResource& resource, unsigned subResourceIndex)
        // {
        // }

		std::shared_ptr<IDevice> UnderlyingDeviceContext::GetObjectFactory()
		{
			return _renderCoreContext->GetDevice();
		}

        UnderlyingDeviceContext::UnderlyingDeviceContext(IThreadContext& renderCoreContext) 
        : _renderCoreContext(&renderCoreContext)
        {
        }

        ResourcePtr CreateResource(IDevice& device, const BufferDesc& desc, DataPacket* initialisationData)
        {
			if (initialisationData) {
				return device.CreateResource(desc,
					[initialisationData](unsigned mipIndex, unsigned arrayIndex) -> SubResourceInitData
					{
						SubResourceInitData result;
						auto sr = DataPacket::TexSubRes(mipIndex, arrayIndex);
						result._data = initialisationData->GetData(sr);
						result._size = initialisationData->GetDataSize(sr);
						auto pitches = initialisationData->GetPitches(sr);
						result._pitches._rowPitch = pitches._rowPitch;
						result._pitches._slicePitch = pitches._slicePitch;
						return result;
					});
			} else {
				return device.CreateResource(desc);
			}
        }

		BufferDesc ExtractDesc(UnderlyingResource& resource)
        {
            return Metal::ExtractDesc(&resource);
        }

    }}

#endif