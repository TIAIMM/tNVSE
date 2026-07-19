#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace fonthook::vectorfont
{
	template <typename TValue>
	class SparseCodePointTable
	{
	public:
		using Value = TValue;
		static_assert(std::is_unsigned_v<Value> && sizeof(Value) == 4,
			"SparseCodePointTable requires an unsigned 32-bit value type");
		static constexpr Value kUnset = std::numeric_limits<Value>::max();
		static constexpr std::size_t kPageEntryCount = 256;

		Value* GetOrCreate(std::uint16_t encoded) noexcept
		{
			std::unique_ptr<Page>& page = pages_[encoded >> 8];
			if (!page)
			{
				std::unique_ptr<Page> allocated(new (std::nothrow) Page);
				if (!allocated)
					return nullptr;
				allocated->fill(kUnset);
				page = std::move(allocated);
				++allocatedPageCount_;
			}
			return &(*page)[encoded & 0xFFu];
		}

		void Clear() noexcept
		{
			for (std::unique_ptr<Page>& page : pages_)
				page.reset();
			allocatedPageCount_ = 0;
		}

		std::size_t GetAllocatedPageCount() const noexcept
		{
			return allocatedPageCount_;
		}

		std::size_t GetAllocatedBytes() const noexcept
		{
			return allocatedPageCount_ * sizeof(Page);
		}

	private:
		using Page = std::array<Value, kPageEntryCount>;
		std::array<std::unique_ptr<Page>, kPageEntryCount> pages_ = {};
		std::size_t allocatedPageCount_ = 0;
	};
}
