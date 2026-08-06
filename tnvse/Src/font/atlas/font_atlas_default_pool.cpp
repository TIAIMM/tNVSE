#include "font_atlas_internal.h"

#include "font_manager.h"
#include "load_config.h"
#include "native_calls.h"

#include "NiDX9Renderer.hpp"
#include "NiFixedString.hpp"
#include "NiGlobalStringTable.hpp"
#include "NiDX9TextureData.hpp"
#include "NiTriShapeData.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "font_atlas_resource_internal.h"

namespace fonthook::vectorfont
{
	using namespace implementation::font_atlas_resource;

	namespace implementation::font_atlas_resource
	{
		bool CanAliasRebuiltDefaultPoolTexture(
			const AtlasResource& target,
			const AtlasResource& source)
		{
			if (&target == &source
				|| target.backend != AtlasBackend::DefaultPool
				|| source.backend != AtlasBackend::DefaultPool
				|| !target.property || !source.property
				|| !target.compactSnapshot || !source.compactSnapshot
				|| !target.pageContentHash
				|| target.pageContentHash != source.pageContentHash
				|| target.width != source.width
				|| target.height != source.height
				|| target.renderMode != source.renderMode
				|| target.padding != source.padding
				|| target.levelZeroOnly != source.levelZeroOnly)
			{
				return false;
			}
			const CompactAtlasSnapshot& targetSnapshot =
				*target.compactSnapshot;
			const CompactAtlasSnapshot& sourceSnapshot =
				*source.compactSnapshot;
			const AtlasSnapshotHeader& targetHeader =
				targetSnapshot.sourceHeader;
			const AtlasSnapshotHeader& sourceHeader =
				sourceSnapshot.sourceHeader;
			return !targetSnapshot.sourcePath.empty()
				&& !sourceSnapshot.sourcePath.empty()
				&& _wcsicmp(targetSnapshot.sourcePath.c_str(),
					sourceSnapshot.sourcePath.c_str()) == 0
				&& targetSnapshot.pixelMode == sourceSnapshot.pixelMode
				&& targetHeader.snapshotHash == sourceHeader.snapshotHash
				&& targetHeader.pageContentHash
					== sourceHeader.pageContentHash
				&& targetHeader.payloadChecksum
					== sourceHeader.payloadChecksum
				&& targetHeader.pixelBytes == sourceHeader.pixelBytes
				&& targetHeader.placementCount
					== sourceHeader.placementCount;
		}

		bool AttachRebuiltDefaultPoolTexture(
			AtlasResource& target, AtlasResource& source)
		{
			if (!CanAliasRebuiltDefaultPoolTexture(target, source))
				return false;
			NiTexture* targetTexture = GetAtlasTexture(target);
			NiTexture* sourceTexture = GetAtlasTexture(source);
			NiDX9TextureData* targetData =
				targetTexture ? targetTexture->GetDX9RendererData() : nullptr;
			IDirect3DTexture9* sharedTexture =
				QueryAtlasD3DTexture(source);
			if (!targetTexture || !sourceTexture || !targetData
				|| !sharedTexture)
			{
				if (sharedTexture)
					sharedTexture->Release();
				return false;
			}
			if (targetData->m_pkD3DTexture)
			{
				targetData->m_pkD3DTexture->Release();
				targetData->m_pkD3DTexture = nullptr;
			}
			targetData->m_pkD3DTexture = sharedTexture;
			if (!targetData->InitializeFromD3DTexture(sharedTexture))
			{
				targetData->m_pkD3DTexture = nullptr;
				sharedTexture->Release();
				return false;
			}
			targetTexture->m_kFormatPrefs = sourceTexture->m_kFormatPrefs;
			target.pixelMode = source.pixelMode;
			target.mipLevels = source.mipLevels;
			target.resetPending = false;
			target.sharedGpuPage = true;
			source.sharedGpuPage = true;
			NotifyNativeA8AtlasTextureMutation();
			return true;
		}

		void ReleaseDefaultPoolTexture(AtlasResource& resource)
		{
			if (resource.backend != AtlasBackend::DefaultPool)
				return;
			NiTexture* texture = GetAtlasTexture(resource);
			NiDX9TextureData* data = texture ? texture->GetDX9RendererData() : nullptr;
			if (data && data->m_pkD3DTexture)
			{
				data->m_pkD3DTexture->Release();
				data->m_pkD3DTexture = nullptr;
				data->m_uiLevels = 0;
				NotifyNativeA8AtlasTextureMutation();
			}
			resource.resetPending = true;
			State().defaultPoolMaintenancePending.store(true, std::memory_order_release);
		}

		bool RebuildDefaultPoolTexture(AtlasResource& resource)
		{
			if (resource.backend != AtlasBackend::DefaultPool || !resource.property)
				return true;
			NiTexture* texture = GetAtlasTexture(resource);
			NiDX9TextureData* data = texture ? texture->GetDX9RendererData() : nullptr;
			if (!texture || !data)
				return false;
			AtlasPixelMode mode = resource.pixelMode;
			for (UInt32 attempt = 0; attempt < 2; ++attempt)
			{
				IDirect3DTexture9* d3dTexture = CreateDynamicAtlasTexture(
					resource.width, resource.height, mode, resource.mipLevels);
				if (d3dTexture && PopulateDefaultTexture(d3dTexture, resource, mode))
				{
					data->m_pkD3DTexture = d3dTexture;
					if (data->InitializeFromD3DTexture(d3dTexture))
					{
						texture->m_kFormatPrefs.m_ePixelLayout = mode == AtlasPixelMode::A8
							? NiTexture::FormatPrefs::SINGLE_COLOR_8
							: NiTexture::FormatPrefs::TRUE_COLOR_32;
						texture->m_kFormatPrefs.m_eMipMapped = d3dTexture->GetLevelCount() > 1
							? NiTexture::FormatPrefs::YES : NiTexture::FormatPrefs::NO;
						resource.pixelMode = mode;
						resource.mipLevels = d3dTexture->GetLevelCount();
						resource.resetPending = false;
						// Each wrapper owns a separate rebuilt D3D texture after reset.
						resource.sharedGpuPage = false;
						NotifyNativeA8AtlasTextureMutation();
						RecordFreeTypePerf(FreeTypePerfCounter::AtlasUpload);
						RecordFreeTypePerf(FreeTypePerfCounter::AtlasUploadBytes,
							static_cast<UInt64>(GetAtlasStorageBytes(resource.width,
								resource.height, mode, resource.mipLevels)));
						return true;
					}
					data->m_pkD3DTexture = nullptr;
				}
				if (d3dTexture)
					d3dTexture->Release();
				if (mode != AtlasPixelMode::A8)
					break;
				mode = AtlasPixelMode::Argb32;
			}
			resource.resetPending = true;
			State().defaultPoolMaintenancePending.store(true, std::memory_order_release);
			return false;
		}

		bool DefaultPoolResetCallback(bool beforeReset, void*)
		{
			AtlasState& state = State();
			if (state.defaultPoolShutdown)
				return true;
			const bool logResetTiming = g_bEnableFreeTypeFontRenderingLog;
			const auto started = logResetTiming
				? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			UInt32 processed = 0;
			UInt32 failed = 0;
			UInt32 shared = 0;
			{
				std::lock_guard<std::mutex> lock(state.atlasMutex);
				if (beforeReset)
				{
					state.directProfilesAvailable.store(
						false, std::memory_order_release);
				}
				if (!beforeReset)
					ResolveGpuAtlasBudget(true);
				std::unordered_set<AtlasResource*> visited;
				std::vector<AtlasResource*> rebuiltPages;
				auto process = [&](AtlasResource& resource)
				{
					if (resource.backend != AtlasBackend::DefaultPool)
						return;
					if (!visited.insert(&resource).second)
						return;
					++processed;
					if (beforeReset)
						ReleaseDefaultPoolTexture(resource);
					else
					{
						AtlasResource* physical = nullptr;
						for (AtlasResource* candidate : rebuiltPages)
						{
							if (candidate
								&& CanAliasRebuiltDefaultPoolTexture(
									resource, *candidate))
							{
								physical = candidate;
								break;
							}
						}
						if (physical
							&& AttachRebuiltDefaultPoolTexture(
								resource, *physical))
						{
							++shared;
							return;
						}
						if (!RebuildDefaultPoolTexture(resource))
						{
							++failed;
							return;
						}
						if (resource.pageContentHash
							&& resource.compactSnapshot)
						{
							rebuiltPages.push_back(&resource);
						}
					}
				};
				for (auto& [key, entry] : state.atlasCache)
				{
					if (entry.resource)
						process(*entry.resource);
				}
				for (RetiredAtlasGeneration& retired : state.retiredAtlases)
				{
					if (retired.resource)
						process(*retired.resource);
				}
				// A sealed table owns its exact logical wrapper separately from the
				// de-duplicated physical atlas list. Those wrappers also carry DEFAULT
				// resources and must participate in both halves of a device reset.
				for (const auto& [identity, weakProfile] :
					state.sealedDirectProfiles)
				{
					const auto sealed = weakProfile.lock();
					if (!sealed)
						continue;
					for (const auto& owners : sealed->tableAtlasOwners)
					{
						for (const auto& atlas : owners)
						{
							if (atlas)
								process(*atlas);
						}
					}
				}
				if (!beforeReset)
				{
					RefreshAtlasCacheGpuAccountingLocked(state);
					PruneRetiredAtlases();
					TrimAtlasCache(state);
				}
				state.defaultPoolMaintenancePending.store(
					HasDefaultPoolMaintenanceLocked(), std::memory_order_release);
				if (!beforeReset)
				{
					for (auto profile =
						state.sealedDirectProfiles.begin();
						profile
							!= state.sealedDirectProfiles.end();)
					{
						const auto sealed =
							profile->second.lock();
						if (!sealed)
						{
							profile = state.sealedDirectProfiles.erase(
								profile);
							continue;
						}
						bool invalidPage = std::any_of(
							sealed->atlases.begin(),
							sealed->atlases.end(),
							[&](const auto& atlas)
							{
								return !atlas
									|| atlas->resetPending
									|| !atlas->property
									|| !GetAtlasTexture(*atlas)
									|| atlas->pixelMode
										!= sealed->pixelMode
									|| atlas->renderMode
										!= sealed->renderMode
									|| atlas->padding
										!= sealed->padding;
							});
						for (const auto& owners : sealed->tableAtlasOwners)
						{
							invalidPage = invalidPage || std::any_of(
								owners.begin(), owners.end(),
								[&](const auto& atlas)
								{
									return !atlas
										|| atlas->resetPending
										|| !atlas->property
										|| !GetAtlasTexture(*atlas)
										|| atlas->pixelMode
											!= sealed->pixelMode
										|| atlas->renderMode
											!= sealed->renderMode
										|| atlas->padding
											!= sealed->padding;
								});
						}
						if (!invalidPage)
						{
							++profile;
							continue;
						}
						for (const auto& table : sealed->tables)
						{
							if (table && table->validity)
							{
								table->validity->store(
									false,
									std::memory_order_release);
							}
						}
						profile =
							state.sealedDirectProfiles.erase(
								profile);
					}
					// Profiles whose pages rebuilt successfully are usable
					// immediately. Failed profiles carry a false per-table
					// token and therefore reopen compatibility state lazily.
					state.directProfilesAvailable.store(
						true, std::memory_order_release);
				}
			}
			if (logResetTiming)
			{
				const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - started).count();
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: DEFAULT atlas reset phase=%s generations=%u shared=%u failed=%u timeUs=%lld",
					beforeReset ? "release" : "rebuild",
					processed, shared, failed,
					static_cast<long long>(elapsed));
			}
			return true;
		}
	}

	void InitializeDefaultPoolAtlasLifecycle()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeDefaultPoolAtlas
			|| State().defaultPoolResetRegistered)
			return;
		State().defaultPoolShutdown = false;
		NiDX9Renderer* renderer = NiDX9Renderer::GetSingleton();
		if (!renderer || !renderer->GetD3DDevice())
			return;
		ResolveGpuAtlasBudget(true);
		ThisStdCall<UInt32>(0x86BAE0, renderer, DefaultPoolResetCallback, nullptr);
		State().defaultPoolResetRegistered = true;
		gLog.FormattedMessage(
			"tnvse_freetype_font: registered DEFAULT atlas device reset lifecycle");
	}

	void PumpDefaultPoolAtlasLifecycle()
	{
		if (!g_bEnableFreeTypeFontRendering || !g_bEnableFreeTypeDefaultPoolAtlas
			|| State().defaultPoolShutdown)
			return;
		InitializeDefaultPoolAtlasLifecycle();
		if (!State().defaultPoolMaintenancePending.load(std::memory_order_acquire))
			return;
		static UInt32 retryFrame = 0;
		const bool retry = (++retryFrame % 120u) == 0;
		if (!retry)
			return;
		std::lock_guard<std::mutex> lock(State().atlasMutex);
		PruneRetiredAtlases();
		for (auto& [key, entry] : State().atlasCache)
		{
			if (entry.resource && entry.resource->resetPending)
				RebuildDefaultPoolTexture(*entry.resource);
		}
		for (RetiredAtlasGeneration& retired : State().retiredAtlases)
		{
			if (retired.resource && retired.resource->resetPending)
				RebuildDefaultPoolTexture(*retired.resource);
		}
		State().defaultPoolMaintenancePending.store(
			HasDefaultPoolMaintenanceLocked(), std::memory_order_release);
	}

	void ShutdownDefaultPoolAtlasLifecycle()
	{
		// NVSE broadcasts ExitGame while the active StartMenu can still own and
		// render text shapes during its final fade.  A menu overhaul may keep many
		// retired atlas generations alive; synchronously clearing them here walks
		// and destroys large pixel buffers and glyph maps before the window closes,
		// leaving the game visibly stuck on that transition frame.  Quiesce reset
		// handling and new DEFAULT-pool allocations, but keep all referenced atlas
		// generations valid.  ExitProcess reclaims them with the rest of the address
		// space, while normal device-reset and runtime eviction paths are unchanged.
		State().defaultPoolShutdown = true;
		if (g_bEnableFreeTypeFontRenderingLog)
			gLog.FormattedMessage(
				"tnvse_freetype_font: atlas lifecycle quiesced for process exit; resource reclamation deferred");
	}
}
