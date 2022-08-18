#pragma once

#include <string>
#include <string_view>
#include <iosfwd>
#include <set>
#include <memory>
#include <cmath>
#include <cassert>
#include <optional>
#include <numeric>

#include "SortedVector.hpp"

#include <unicode/utext.h>
#include <unicode/brkiter.h>

namespace unicode
{

/// Helpers that deal with unicode
namespace detail
{

/// Get character break iterator at the beginning of openned unicode text 
std::unique_ptr<icu::BreakIterator> 
getCharacterBreakIterator(UText *utext) noexcept
{
	UErrorCode errorCode = U_ZERO_ERROR;
	std::unique_ptr<icu::BreakIterator> it {
		icu::BreakIterator::createCharacterInstance(
			icu::Locale::getDefault(), 
			errorCode
		)
	};
	if (U_FAILURE(errorCode))
	{
		return nullptr;
	}

	errorCode = U_ZERO_ERROR;
	it->setText(utext, errorCode);
	if (U_FAILURE(errorCode))
	{
		return nullptr;
	}
	return it;
}

/// Open utf-8 string as unicode text
UText openUText(std::string_view str) noexcept
{
	UErrorCode errorCode = U_ZERO_ERROR;
	UText utext = UTEXT_INITIALIZER;
	utext_openUTF8(&utext, str.data(), str.size(), &errorCode);
	if (U_FAILURE(errorCode))
	{
		return UTEXT_INITIALIZER;
	}
	return utext;
}

/// Calculate size of utf-8 string in characters
uint32_t calculateSizeInCharacters(std::string_view text) noexcept
{
	if (text.empty()) { return 0; }
	if (text.size() == 1) { return 1; }

	auto utext = openUText(text);

	auto it = getCharacterBreakIterator(&utext);

	uint32_t size = 0;
	for (
		int32_t end = it->next(); 
		end != icu::BreakIterator::DONE; 
		end = it->next()
	)
	{
		++size;
	}

	utext_close(&utext);

	return size;
}

} // namespace detail

/// Structure to differentiate between char and count in repeat function
struct Times { uint32_t count; };

/**
 *	 Unicode string - collection of user-percieved characters,
 * 	 which may be represented with multiple Unicode code points.
 * 
 *   TODO: requirements for the container and readonly version
 * 
 *   TODO: determine underlying UTF-8, UTF-16 or UTF-32 encoding on the fly
 */
template<typename Storage = std::string>
class BasicString
{
public:
	/// Type of string size in characters.
	///
	/// @details 
	///  Is less than @c size_t because of: 
	///	 	* @c BreakIterator limitations
	///     * @c Block memory efficiency
	///     * Why would you need contignuous strings that big?
	using SizeType = uint32_t;

	/// Type for index of Character
	using CharacterIndex = uint32_t;
	/// Relative character index
	using RelativeCharacterIndex = int32_t;

	/// Character inside of string
	/// TODO: special class, that may be used to modify string
	using Character = std::string_view;

	/// Get maximum number of characters in string
	static constexpr SizeType maxSize() noexcept 
	{
		return SizeType(std::numeric_limits<int32_t>::max());
	}

	/// Create unicode string from string with only ASCII characters.
	/// @warning No validation is performed.
	static BasicString fromASCII(Storage ascii) noexcept
	{
		BasicString str;
		str._layout.size = ascii.size();
		str._layout.averageCharacterSize = 1;
		str.bytes = std::move(ascii);
		return str;
	}

	/// Get ASCII character
	static BasicString repeat(char c, Times times) noexcept
	{
		return BasicString::fromASCII(Storage(times.count, c));
	}

	/// Create empty string
	BasicString() noexcept : _layout(Layout{.averageCharacterSize = 1, .size = 0}) {}
	/// Create string from ASCII character
	BasicString(char c) noexcept 
		: BasicString(BasicString::fromASCII(Storage(1, c))) {}
	/// Create string from ASCII characters
	BasicString(std::initializer_list<char> chars) noexcept 
		: BasicString(BasicString::fromASCII(Storage(chars))) {}
	/// Create string from UTF-8 encoded characters
	BasicString(const char *str) : bytes(str) {}
	/// Create string from UTF-8 encoded characters
	BasicString(std::string_view view) : bytes(view) {}
	/// Create unicode string from utf-8 encoded string
	BasicString(std::string str) noexcept : bytes(std::move(str)) {}

	/// Don't create string from nullptr
	BasicString(std::nullptr_t) = delete;

	/// Does string contain only ASCII characters?
	[[nodiscard("Possibly expensive O(n) operation")]]
	bool isASCII() const noexcept 
	{ 
		return layout().isASCII();
	}

	/// Is string empty?
	bool isEmpty() const noexcept { return bytes.empty(); }

	/// Get size of string in characters
	[[nodiscard("Possibly expensive O(n) operation")]]
	SizeType size() const noexcept 
	{ 
		/// TODO: SizeRequest helper, that will lazily calculate size
		return layout().size;
	}

	operator const std::string &() const noexcept { return bytes; }
	operator std::string_view() const noexcept { return bytes; }

	/// Get character by index. Negative index is counted from the end.
	template<std::unsigned_integral PositiveIndex>
	[[nodiscard("Possibly expensive O(n) operation")]]
	Character operator[](PositiveIndex index) const noexcept
	{
		assert(index < size() && "out of bounds");
		auto [byteIndex, size] = layout().getCharacterByteIndexAndSize(index);
		return Character{bytes.data() + byteIndex, size};
	}

	// Get character by index. Negative index is counted from the end.
	template<std::signed_integral NegativeIndex>
	[[nodiscard("Possibly expensive O(n) operation")]]
	Character operator[](NegativeIndex index) const noexcept
	{
		CharacterIndex characterIndex = index;
		if (index < 0)
		{
			characterIndex = absoluteIndex(index);
		}
		return (*this)[characterIndex];
	}

	/// Convert relative index from end to absolute
	[[nodiscard("Possibly expensive O(n) operation")]]
	CharacterIndex absoluteIndex(RelativeCharacterIndex relative) const noexcept
	{
		assert(relative <= 0 && "Past end index");
		assert(SizeType(-relative) <= size() && "out of range");
		return RelativeCharacterIndex(size()) + relative;
	}

	/// Convert relative to some position index to absolute
	CharacterIndex absoluteIndex(
		RelativeCharacterIndex relative, 
		CharacterIndex start
	) const noexcept
	{
		assert(
			relative >= 0 || start >= CharacterIndex(-relative) && 
			"index below zero"
		);
		assert(relative <= 0 || start + relative <= size() && "past end index");
		return start + relative;
	}

	/// Append  utf-8 string to the end of this string
	BasicString &operator+=(std::string_view str)
	{
		if (str.empty()) { return *this; }

		// Appended string is bigger
		if (bytes.size() <= str.size())
		{
			// just clear layout information to reevaluate later
			_layout = Layout{};
		}
		bytes += str;
		
		if (not isEmpty() && layoutIsEvaluated())
		{
			/// TODO: reevaluate layout
			_layout = Layout{};
		}
		
		return *this;
	}

	friend std::ostream &operator<<(std::ostream &os, const BasicString &str)
	{
		return os << str.bytes;
	}

	friend std::istream &operator>>(std::istream &is, BasicString &str)
	{
		return is >> str.bytes;
	}
private:
	/// UTF-8 encoded content of string
	Storage bytes;

	/// Metadata about string layout
	struct Layout
	{
		/// Size in bytes
		using ByteSize = uint16_t;

		/// Type for index of a byte in character
		using ByteIndex = std::string::size_type;

		/// Type for difference between byte indexes
		using ByteDifference = std::string::difference_type;

		enum { NOT_EVALUATED = 0 };
		/// Average character size in bytes
		ByteSize averageCharacterSize : 8 = NOT_EVALUATED;

		/// Number of characters in string
		SizeType size = -1;

		/// Sequence of characters with size that differs from average.
		///
		/// @note 
		///	 This must be as much memory efficient as possible,
		///	 as it will be created every ~8 characters (average word size)
		///  in languages like Russian
		struct [[gnu::packed]] Block
		{
			/// Index of the first character in block
			CharacterIndex firstCharacter = 0;
			/// Difference from first byte estimation
			ByteDifference firstByteDifference : 32 = 0;
			/// Count of characters in block. 
			/// 0 means 1 character, as block may not be empty
			SizeType charactersCountMinusOne : 4 = 0;
			/// Size of character in bytes.
			/// 0 means 1 byte, as character occupies at least 1 byte
			ByteSize characterSizeMinusOne : 4 = 0;

			/// TODO: maybe store difference from first byte estimation?
			/// it will be sum of byte differences from previous blocks
			/// ByteDifference firstByteDifference = 0;

			/// Get size of character in bytes
			constexpr ByteSize characterSize() const noexcept
			{
				return characterSizeMinusOne + 1;
			}

			/// Set character size in bytes
			constexpr void setCharacterSize(ByteSize size) noexcept
			{
				assert(size > 0 && size <= 16 && "invalid character size");
				characterSizeMinusOne = size - 1;
			}

			/// Get count of characters in block
			constexpr SizeType charactersCount() const noexcept
			{
				return charactersCountMinusOne + 1;
			}

			/// Set character count in block
			constexpr void setCharactersCount(SizeType count) noexcept
			{
				assert(count > 0 && count <= 16 && "invalid character count");
				charactersCountMinusOne = count - 1;
			}

			/// Increment characters count
			constexpr void incrementCharactersCount() noexcept
			{
				assert(not isFull() && "block is full");
				++charactersCountMinusOne;
			}

			/// Is block full?
			constexpr bool isFull() const noexcept
			{
				return charactersCount() == 16;
			}

			/// Does this block contain character with given index?
			constexpr bool containsCharacterIndex(
				CharacterIndex index
			) const noexcept
			{
				return 
					index >= firstCharacter && 
					index < firstCharacter + CharacterIndex(charactersCount());
			}

			/// Sort blocks by first character index
			constexpr auto operator<=>(const Block &other) const noexcept
			{
				return firstCharacter <=> other.firstCharacter;
			}

			/// Sort blocks by first character index
			constexpr auto operator<=>(CharacterIndex index) const noexcept
			{
				return firstCharacter <=> index;
			}
		};
		static_assert(sizeof(Block) == 9, "Block is not packed");

		/// Set of character sequences where size differs from average.
		/// Sorted by first character index
		SortedVector<Block, std::less<>> blocks;

		/// Is layout evaluated?
		constexpr bool isEvaluated() const noexcept
		{
			return averageCharacterSize != NOT_EVALUATED;
		}

		/// Is this layout for ASCII string?
		constexpr bool isASCII() const noexcept
		{
			assert(isEvaluated() && "Layout is not evaluated");
			return averageCharacterSize == 1 && blocks.isEmpty();
		}
		
		/// Evaluate layout for utf-8 string
		static Layout getFor(std::string_view str) noexcept
		{
			assert(str.size() <= BasicString::maxSize() && "String is too big");

			auto size = detail::calculateSizeInCharacters(str);

			// Don't need to do anything else for ASCII.
			if (size == SizeType(str.size())) 
			{ 
				return Layout{
					.averageCharacterSize = 1, 
					.size = size
				}; 
			}

			Layout layout{
				.averageCharacterSize = 
					ByteSize(std::round(double(str.size()) / size)),
				.size = size
			};

			auto utext = detail::openUText(str);
			auto it = detail::getCharacterBreakIterator(&utext);

			ByteSize previousCharacterSize = layout.averageCharacterSize;
			std::optional<Block> currentBlock;
			CharacterIndex characterIndex = 0;
			ByteDifference byteDifference = 0;
			for (
				auto start = it->first(), end = it->next();
				end != icu::BreakIterator::DONE; 
				start = end, end = it->next(), ++characterIndex)
			{
				ByteSize characterByteSize = end - start;

				// This character may be part of block
				if (characterByteSize == previousCharacterSize)
				{
					// Character size is the same as average
					if (previousCharacterSize == layout.averageCharacterSize)
					{
						continue;
					}

					assert(
						currentBlock.has_value() && 
						"current block is not set"
					);

					// Add character to block, if it's not full
					if (not currentBlock->isFull())
					{
						currentBlock->incrementCharactersCount();
						continue;
					}

					// block is full -> falthrough to creating new block
				}

				// Add previous block to set
				if (currentBlock)
				{
					byteDifference += 
						currentBlock->charactersCount() * 
						layout.characterSizeDifference(*currentBlock);
					layout.blocks.push_back(std::move(*currentBlock));
				}

				// New block needed
				currentBlock = Block{
					.firstCharacter = characterIndex,
					.firstByteDifference = byteDifference
				};
				currentBlock->setCharacterSize(characterByteSize);

				previousCharacterSize = characterByteSize;
			}

			// Add last block to set
			if (currentBlock) 
			{ 
				layout.blocks.push_back(std::move(*currentBlock)); 
			}

			utext_close(&utext);

			return layout;
		}


		/// Get iterator to block that contains character or just before it
		auto getNearestLeftBlock(CharacterIndex index) const noexcept
		{
			auto block = blocks.upper_bound(index);
			return std::make_reverse_iterator(block);
		}

		/// Estimate first byte index for character
		ByteIndex estimateFirstByteIndex(CharacterIndex index) const noexcept
		{
			assert(isEvaluated() && "layout is not evaluated");
			return index * averageCharacterSize;
		}

		/// Differance from average character size in block
		ByteDifference characterSizeDifference(
			const Block &block
		) const noexcept
		{
			return 
				ByteDifference(block.characterSize()) - 
				ByteDifference(averageCharacterSize);
		}

		/// Information about character
		struct ByteIndexAndSize
		{
			/// Index of first byte
			ByteIndex index;
			/// Size in bytes
			ByteSize size;
		};

		/// Get byte index and size for character
		ByteIndexAndSize getCharacterByteIndexAndSize(
			CharacterIndex characterIndex
		) const noexcept
		{
			assert(isEvaluated() && "layout is not evaluated");

			ByteIndexAndSize res{
				.index = estimateFirstByteIndex(characterIndex),
			 	.size = averageCharacterSize
			};

			if (blocks.isEmpty()) { return res; }

			auto end = getNearestLeftBlock(characterIndex);
			if (
				end != blocks.rend()
			)
			{
				res.index += end->firstByteDifference;
				if (end->containsCharacterIndex(characterIndex))
				{
					res.size = end->characterSize();
					res.index += 
						(characterIndex - end->firstCharacter) * characterSizeDifference(*end);
				}
			}

			return res;
		}
	};

	/// Metadata about string layout. 
	/// @warning don't use it directly, as it may not be evaluated. 
	/// Use getter layout() instead
	mutable Layout _layout;

	/// Check that layout is evaluated, without evaluatiation
	bool layoutIsEvaluated() const noexcept
	{
		return _layout.isEvaluated();
	}

	/// Get layout of string
	[[nodiscard("Possibly expensive O(n) operation")]]
	Layout & layout() const noexcept
	{
		if (not _layout.isEvaluated())
		{
			_layout = Layout::getFor(bytes);
		}
		return _layout;
	}
};

/// Alias for default string
using String = BasicString<>;

} // namespace unicode
