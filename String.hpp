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
int32_t calculateSizeInCharacters(std::string_view text) noexcept
{
	if (text.empty()) { return 0; }
	if (text.size() == 1) { return 1; }

	auto utext = openUText(text);

	auto it = getCharacterBreakIterator(&utext);

	int32_t size = 0;
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
 *   TODO: change to template<typename Container> CharacterView
 * 
 *   TODO: determine underlying UTF-8, UTF-16 or UTF-32 encoding on the fly
 */
class String
{
public:
	/// Type of string size in characters.
	///
	/// @details 
	///  Is less than @c size_t because of: 
	///	 	* @c BreakIterator limitations
	///     * @c Block memory efficiency
	///     * Why would you need more than 2 GiB of contignuous text data?
	using SizeType = uint32_t;

	/// Type for index of Character
	using CharacterIndex = int32_t;

	/// Character inside of string
	/// TODO: special class, that may be used to modify string
	using Character = std::string_view;

	/// Get maximum number of characters in string
	static constexpr SizeType maxSize() noexcept 
	{
		return SizeType(std::numeric_limits<CharacterIndex>::max());
	}

	/// Create unicode string from string with only ASCII characters.
	/// @warning No validation is performed.
	static String fromASCII(std::string ascii) noexcept
	{
		String str;
		str._layout.size = ascii.size();
		str._layout.averageCharacterSize = 1;
		str.bytes = std::move(ascii);
		return str;
	}

	/// Get ASCII character
	static String repeat(char c, Times times) noexcept
	{
		return String::fromASCII(std::string(times.count, c));
	}

	/// Create empty string
	String() noexcept : _layout(Layout{.averageCharacterSize = 1, .size = 0}) {}
	/// Create string from ASCII character
	String(char c) noexcept : String(String::fromASCII(std::string(1, c))) {}
	/// Create string from ASCII characters
	String(std::initializer_list<char> chars) noexcept 
		: String(String::fromASCII(std::string(chars))) {}
	/// Create string from UTF-8 encoded characters
	String(const char *str) : bytes(str) {}
	/// Create string from UTF-8 encoded characters
	String(std::string_view view) : bytes(view) {}
	/// Create unicode string from utf-8 encoded string
	String(std::string str) noexcept : bytes(std::move(str)) {}

	/// Don't create string from nullptr
	String(std::nullptr_t) = delete;

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
	[[nodiscard("Possibly expensive O(n) operation")]]
	Character operator[](CharacterIndex index) const noexcept
	{
		/// TODO: add optimization for some small indexes
		assert(index >= 0 || SizeType(-index) <= size() && "out of range");
		assert(SizeType(index) < size() && "out of range");
		assert(SizeType(index) <= maxSize() && "Index is too big");

		// Convert negative index to positive
		if (index < 0) 
		{ 
			index += CharacterIndex(size()); 
		}

		auto characterSize = layout().getCharacterSize(index);
		auto firstByteIndex = layout().getFirstByteIndexForCharacter(index);
		return Character{bytes.data() + firstByteIndex, characterSize};
	}

	friend std::ostream &operator<<(std::ostream &os, const String &str)
	{
		return os << str.bytes;
	}

	friend std::istream &operator>>(std::istream &is, String &str)
	{
		return is >> str.bytes;
	}
private:
	/// UTF-8 encoded content of string
	std::string bytes;

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
		static_assert(sizeof(Block) == 5, "Block is not packed");

		/// Set of character sequences where size differs from average.
		/// Sorted by first character index
		std::set<Block, std::less<>> blocks;

		/// Is layout evaluated?
		constexpr bool isEvaluated() const noexcept
		{
			return averageCharacterSize != NOT_EVALUATED;
		}

		/// Is this layout for ASCII string?
		constexpr bool isASCII() const noexcept
		{
			assert(isEvaluated() && "Layout is not evaluated");
			return averageCharacterSize == 1 && blocks.empty();
		}
		
		/// Evaluate layout for utf-8 string
		void evaluateFor(std::string_view str) noexcept
		{
			assert(str.size() <= String::maxSize() && "String is too big");

			averageCharacterSize = 1;
			size = detail::calculateSizeInCharacters(str);

			// Don't need to do anything else for ASCII.
			// @warning don't use isASCII() here, 
			// as we didn't correct average size
			if (size == SizeType(str.size())) { return; }

			averageCharacterSize = std::round(double(str.size()) / size);

			auto utext = detail::openUText(str);
			auto it = detail::getCharacterBreakIterator(&utext);

			ByteSize previousCharacterSize = averageCharacterSize;
			std::optional<Block> currentBlock;
			for (
				auto start = it->first(), end = it->next(), characterIndex = 0; 
				end != icu::BreakIterator::DONE; 
				start = end, end = it->next(), ++characterIndex)
			{
				ByteSize characterByteSize = end - start;

				// This character may be part of block
				if (characterByteSize == previousCharacterSize)
				{
					// Character size is the same as average
					if (previousCharacterSize == averageCharacterSize)
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
					blocks.insert(std::move(*currentBlock));
				}

				// New block needed
				currentBlock = Block{.firstCharacter = characterIndex};
				currentBlock->setCharacterSize(characterByteSize);

				previousCharacterSize = characterByteSize;
			}

			// Add last block to set
			if (currentBlock) 
			{ 
				blocks.insert(std::move(*currentBlock)); 
			}

			utext_close(&utext);
		}



		/// Get size of character in bytes
		ByteSize getCharacterSize(CharacterIndex index) const noexcept
		{
			assert(isEvaluated() && "layout is not evaluated");

			auto block = blocks.upper_bound(index);
			if (block == blocks.begin()) { return averageCharacterSize; }

			--block;

			if (block->containsCharacterIndex(index))
			{
				return block->characterSize();
			}
			return averageCharacterSize;
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

		/// Difference from first byte index estimation for character index
		ByteDifference byteDifferenceForCharacter(
			CharacterIndex index
		) const noexcept
		{
			assert(isEvaluated() && "layout is not evaluated");
			auto end = blocks.upper_bound(index);
			return std::accumulate(
				blocks.begin(), end, ByteDifference(0), 
				[this, index](ByteDifference diff, const Block &block)
				{
					return diff + 
						characterSizeDifference(block) * 
						std::min(
							block.charactersCount(), 
							SizeType(index - block.firstCharacter)
						);
				}
			);
		}

		/// Get index of first byte for character
		ByteIndex getFirstByteIndexForCharacter(
			CharacterIndex characterIndex
		) const noexcept
		{
			return 
				estimateFirstByteIndex(characterIndex) + 
				byteDifferenceForCharacter(characterIndex);
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
			_layout.evaluateFor(bytes);
		}
		return _layout;
	}
};

} // namespace unicode
