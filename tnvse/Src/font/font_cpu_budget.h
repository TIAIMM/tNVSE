#pragma once

#include <cstddef>
#include <cstdint>

namespace fonthook::vectorfont
{
	enum class CpuMemoryCategory : std::uint8_t
	{
		GlyphBitmap,
		PreparedText,
		TextArtifact,
		AtlasMetadata,
		PersistentMapping,
		RuntimeMetadata,
		Count
	};

	inline std::size_t ResolveCpuMemoryCategoryHeadroom(std::size_t budgetBytes,
		std::size_t totalBytes, std::size_t categoryBytes,
		std::size_t preferredLimit)
	{
		const std::size_t otherBytes = categoryBytes <= totalBytes
			? totalBytes - categoryBytes : totalBytes;
		if (otherBytes >= budgetBytes)
			return 0;
		const std::size_t headroom = budgetBytes - otherBytes;
		return preferredLimit < headroom ? preferredLimit : headroom;
	}

	void AddCpuMemoryUsage(CpuMemoryCategory category, std::size_t bytes);
	void RemoveCpuMemoryUsage(CpuMemoryCategory category, std::size_t bytes);
	std::size_t GetCpuMemoryUsage(CpuMemoryCategory category);
	std::size_t GetCpuMemoryUsage();
	std::size_t GetCpuMemoryBudget();
	std::size_t GetCpuMemoryCategoryHeadroom(CpuMemoryCategory category,
		std::size_t preferredLimit);
	bool IsCpuMemoryBudgetExceeded();
	void EnforceCpuMemoryBudget(const char* phase);
	void ReportCpuMemoryBudget(const char* phase, bool force = false);

	// A lease follows the allocation's real lifetime. Removing an LRU entry does
	// not pretend to reclaim memory while a shape, TLS hot entry, or another
	// shared_ptr still owns the underlying object.
	class CpuMemoryLease
	{
	public:
		CpuMemoryLease() = default;
		CpuMemoryLease(CpuMemoryCategory category, std::size_t bytes)
		{
			Reset(category, bytes);
		}
		~CpuMemoryLease()
		{
			Release();
		}

		CpuMemoryLease(const CpuMemoryLease&) = delete;
		CpuMemoryLease& operator=(const CpuMemoryLease&) = delete;
		CpuMemoryLease(CpuMemoryLease&& other) noexcept
			: category_(other.category_), bytes_(other.bytes_)
		{
			other.bytes_ = 0;
		}
		CpuMemoryLease& operator=(CpuMemoryLease&& other) noexcept
		{
			if (this != &other)
			{
				Release();
				category_ = other.category_;
				bytes_ = other.bytes_;
				other.bytes_ = 0;
			}
			return *this;
		}

		void Reset(CpuMemoryCategory category, std::size_t bytes)
		{
			if (category_ == category && bytes_ == bytes)
				return;
			Release();
			category_ = category;
			bytes_ = bytes;
			if (bytes_)
				AddCpuMemoryUsage(category_, bytes_);
		}
		void Release()
		{
			if (bytes_)
				RemoveCpuMemoryUsage(category_, bytes_);
			bytes_ = 0;
		}
		std::size_t GetBytes() const { return bytes_; }

	private:
		CpuMemoryCategory category_ = CpuMemoryCategory::RuntimeMetadata;
		std::size_t bytes_ = 0;
	};
}
