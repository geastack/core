// SPDX-License-Identifier: Apache-2.0
#include "tree_state.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <new>
#include <memory>
#include <vector>
#include <utility>

// TreeState is a single large struct (kMaxNodes-scaled per-node arrays: nodes,
// styles, attributes, class lists, custom props, style overrides, dirty/bounds
// tables) — on the order of ~170 KB. Internal SRAM is the scarce resource on the
// ESP32-S3 (~330 KB, and WiFi/BLE need DMA-capable internal RAM that can't live
// in PSRAM); the default `new` lands this pool in internal RAM, starving the
// radios (WiFi `mem fail` / socket open `sock < 0` during fetches). The tree is
// CPU/cache-accessed only (never DMA, never touched with the flash cache off),
// so it belongs in PSRAM like the framebuffer. Other targets keep plain `new`.
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace gea::embedded::ui
{

namespace
{
std::size_t g_nodeTextStorageBytes = 0;
template <class T> struct NodeTextAllocator {
	using value_type = T;
	NodeTextAllocator() = default;
	template <class U> NodeTextAllocator(const NodeTextAllocator<U> &) {}
	T *allocate(std::size_t n)
	{
#if defined(ESP_PLATFORM)
		// Match TreeState placement even when the board sends small ordinary
		// allocations to SRAM. Node text must not steal the audio heap.
		T *result = static_cast<T *>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM));
		if (!result) result = std::allocator<T>{}.allocate(n);
#else
		T *result = std::allocator<T>{}.allocate(n);
#endif
		g_nodeTextStorageBytes += n * sizeof(T);
		return result;
	}
	void deallocate(T *ptr, std::size_t n)
	{
		g_nodeTextStorageBytes -= n * sizeof(T);
#if defined(ESP_PLATFORM)
		// ESP-IDF new/delete and capability allocations use the same heap.
		heap_caps_free(ptr);
#else
		std::allocator<T>{}.deallocate(ptr, n);
#endif
	}
	template <class U> bool operator==(const NodeTextAllocator<U> &) const
	{
		return true;
	}
	template <class U> bool operator!=(const NodeTextAllocator<U> &) const
	{
		return false;
	}
};
union NodeTextEntry {
	// A free entry has no live string, so its reuse link occupies the same
	// bytes instead of adding a word to every nonempty label.
	std::string text;
	std::uint32_t nextFree;
	NodeTextEntry() : nextFree(UINT32_MAX) {}
	~NodeTextEntry() {} // Live strings are destroyed when their owner clears.
};
struct NodeTextPool {
	// Fixed small pages avoid libc++ deque's 4 KiB minimum block for a UI
	// containing only a handful of labels. Page addresses never move.
	static constexpr std::size_t kPageEntries = 16;
	struct PageDeleter {
		void operator()(NodeTextEntry *page) const {
			for (std::size_t i = 0; i < kPageEntries; ++i) page[i].~NodeTextEntry();
			NodeTextAllocator<NodeTextEntry>{}.deallocate(page, kPageEntries);
		}
	};
	std::vector<NodeTextEntry *, NodeTextAllocator<NodeTextEntry *>> pages;
	std::size_t size = 0;
	std::uint32_t freeHead = UINT32_MAX;
	NodeTextEntry &at(std::size_t index)
	{
		return pages[index / kPageEntries][index % kPageEntries];
	}
	void append()
	{
		if (size == pages.size() * kPageEntries) {
			std::unique_ptr<NodeTextEntry[], PageDeleter> page(NodeTextAllocator<NodeTextEntry>{}.allocate(kPageEntries));
			for (std::size_t i = 0; i < kPageEntries; ++i) new (&page[i]) NodeTextEntry;
			pages.push_back(page.get());
			page.release();
		}
		++size;
	}
};
NodeTextPool *g_nodeTextPool = nullptr;
const std::string g_emptyNodeText;
NodeTextPool &nodeTextPool()
{
	if (!g_nodeTextPool)
		g_nodeTextPool = new (NodeTextAllocator<NodeTextPool>{}.allocate(1)) NodeTextPool;
	return *g_nodeTextPool;
}
} // namespace

std::size_t NodeText::storageBytes()
{
	return g_nodeTextStorageBytes;
}

StorageUsage NodeText::storageUsage()
{
    StorageUsage usage;
    usage.staticBytes = sizeof(g_nodeTextStorageBytes) + sizeof(g_nodeTextPool) + sizeof(g_emptyNodeText);
    usage.addString(g_emptyNodeText);
    if (!g_nodeTextPool) return usage;
    const auto &pool = *g_nodeTextPool;
    usage.addAllocation(g_nodeTextPool, sizeof(NodeTextPool));
    usage.addVector(pool.pages);
    for (const auto *page : pool.pages)
        usage.addAllocation(page, NodeTextPool::kPageEntries * sizeof(NodeTextEntry));
    for (std::size_t index = 0; index < pool.size; ++index) {
        bool free = false;
        // Census only: walking the free chain avoids allocating a temporary
        // live bitmap, and never reads a destroyed union member as a string.
        for (auto handle = pool.freeHead; handle != UINT32_MAX; handle = g_nodeTextPool->at(handle).nextFree)
            if (handle == index) { free = true; break; }
        if (!free) usage.addString(g_nodeTextPool->at(index).text);
    }
    return usage;
}

NodeText::NodeText(const NodeText &other)
{
	if (!other.empty())
		assign(other.c_str());
}
NodeText::NodeText(NodeText &&other) noexcept : handle_(other.handle_)
{
	other.handle_ = kEmpty;
}
NodeText::~NodeText() { clear(); }
NodeText &NodeText::operator=(const NodeText &other)
{
	if (this != &other) {
		if (other.empty())
			clear();
		else
			assign(other.c_str());
	}
	return *this;
}
NodeText &NodeText::operator=(NodeText &&other) noexcept
{
	if (this != &other) {
		clear();
		handle_ = other.handle_;
		other.handle_ = kEmpty;
	}
	return *this;
}
const std::string &NodeText::str() const
{
	return empty() ? g_emptyNodeText : nodeTextPool().at(handle_).text;
}
void NodeText::clear()
{
	if (empty())
		return;
	auto &pool = nodeTextPool();
	auto &entry = pool.at(handle_);
	// Release long text allocations on removal, rather than retaining the
	// largest label ever seen in every reusable slot.
	entry.text.~basic_string();
	entry.nextFree = pool.freeHead;
	pool.freeHead = handle_;
	handle_ = kEmpty;
}
void NodeText::assign(const char *text)
{
	if (!text || !*text) {
		clear();
		return;
	}
	auto &pool = nodeTextPool();
	if (empty()) {
		if (pool.freeHead == kEmpty) {
			const auto index = pool.size;
			pool.append();
			handle_ = static_cast<std::uint32_t>(index);
		} else {
			handle_ = pool.freeHead;
			pool.freeHead = pool.at(handle_).nextFree;
		}
		new (&pool.at(handle_).text) std::string;
	}
	pool.at(handle_).text.assign(text);
}

namespace {

bool isClassWhitespace(char c)
{
	return static_cast<unsigned char>(c) <= ' ';
}

bool validToken(const std::string &token)
{
	if (token.empty()) return false;
	for (char c : token) {
		if (isClassWhitespace(c)) return false;
	}
	return true;
}

#if GEA_UI_CLASS_OVERFLOW
std::deque<std::vector<CssAtomId>> &classListOverflowLists()
{
	static std::deque<std::vector<CssAtomId>> lists;
	return lists;
}

std::vector<std::uint16_t> &classListOverflowFreeList()
{
	static std::vector<std::uint16_t> handles;
	return handles;
}

std::vector<CssAtomId> *classListOverflow(std::uint16_t handle)
{
	auto &lists = classListOverflowLists();
	if (handle == NodeClassList::kNoOverflow || static_cast<std::size_t>(handle) >= lists.size()) return nullptr;
	return &lists[handle];
}

const std::vector<CssAtomId> *classListOverflowConst(std::uint16_t handle)
{
	const auto &lists = classListOverflowLists();
	if (handle == NodeClassList::kNoOverflow || static_cast<std::size_t>(handle) >= lists.size()) return nullptr;
	return &lists[handle];
}

std::vector<CssAtomId> *ensureClassListOverflow(NodeClassList &list)
{
	if (auto *existing = classListOverflow(list.overflowHandle)) return existing;
	auto &lists = classListOverflowLists();
	auto &freeList = classListOverflowFreeList();
	std::uint16_t handle = NodeClassList::kNoOverflow;
	if (!freeList.empty()) {
		handle = freeList.back();
		freeList.pop_back();
		lists[handle].clear();
	} else if (lists.size() < NodeClassList::kNoOverflow) {
		handle = static_cast<std::uint16_t>(lists.size());
		lists.emplace_back();
	}
	list.overflowHandle = handle;
	return classListOverflow(handle);
}

void releaseClassListOverflow(NodeClassList &list)
{
	if (list.overflowHandle == NodeClassList::kNoOverflow) return;
	if (auto *overflow = classListOverflow(list.overflowHandle)) overflow->clear();
	classListOverflowFreeList().push_back(list.overflowHandle);
	list.overflowHandle = NodeClassList::kNoOverflow;
}
#else
void releaseClassListOverflow(NodeClassList &) {}
#endif

bool appendClassAtom(NodeClassList &list, CssAtomId atom)
{
	if (atom == kInvalidCssAtom || list.count == 0xFFu) return false;
	if (list.count < NodeClassList::kInlineTokenCount) {
		list.inlineTokens[list.count++] = atom;
		return true;
	}
#if GEA_UI_CLASS_OVERFLOW
	auto *overflow = ensureClassListOverflow(list);
	if (!overflow || list.overflowHandle == NodeClassList::kNoOverflow) return false;
	overflow->push_back(atom);
	++list.count;
	return true;
#else
	// A violated source proof must fail visibly, never truncate a class list.
	std::abort();
#endif
}

void eraseClassAtomAt(NodeClassList &list, std::size_t index)
{
	if (index >= list.count) return;
	if (index < NodeClassList::kInlineTokenCount) {
		const std::size_t inlineEnd = std::min<std::size_t>(list.count, NodeClassList::kInlineTokenCount);
		for (std::size_t i = index + 1; i < inlineEnd; ++i)
			list.inlineTokens[i - 1] = list.inlineTokens[i];
#if GEA_UI_CLASS_OVERFLOW
		if (list.count > NodeClassList::kInlineTokenCount) {
			auto *overflow = classListOverflow(list.overflowHandle);
			if (overflow && !overflow->empty()) {
				list.inlineTokens[NodeClassList::kInlineTokenCount - 1] = overflow->front();
				overflow->erase(overflow->begin());
				if (overflow->empty()) releaseClassListOverflow(list);
			}
		} else
#endif
		if (list.count > 0) {
			list.inlineTokens[list.count - 1] = kInvalidCssAtom;
		}
		--list.count;
		return;
	}
#if GEA_UI_CLASS_OVERFLOW
	auto *overflow = classListOverflow(list.overflowHandle);
	if (!overflow) return;
	const std::size_t overflowIndex = index - NodeClassList::kInlineTokenCount;
	if (overflowIndex >= overflow->size()) return;
	overflow->erase(overflow->begin() + static_cast<std::ptrdiff_t>(overflowIndex));
	--list.count;
	if (overflow->empty()) releaseClassListOverflow(list);
#endif
}

bool classListsEqual(const NodeClassList &a, const NodeClassList &b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0, n = a.size(); i < n; ++i)
		if (a.at(i) != b.at(i)) return false;
	return true;
}

NodeClassList splitClassNameAtoms(const char *className, std::size_t length)
{
	NodeClassList out;
	const char *text = className ? className : "";
	std::size_t i = 0;
	while (i < length) {
		while (i < length && isClassWhitespace(text[i])) ++i;
		const std::size_t start = i;
		while (i < length && !isClassWhitespace(text[i])) ++i;
		if (i <= start) continue;
		const CssAtomId token = internCssAtom(text + start, i - start);
		if (token == kInvalidCssAtom) continue;
		bool seen = false;
		for (std::size_t existingIndex = 0, count = out.size(); existingIndex < count; ++existingIndex) {
			if (out.at(existingIndex) == token) {
				seen = true;
				break;
			}
		}
		if (!seen) appendClassAtom(out, token);
	}
	return out;
}

}  // namespace

NodeClassList::NodeClassList(const NodeClassList &other)
{
	for (std::size_t i = 0, n = other.size(); i < n; ++i)
		appendClassAtom(*this, other.at(i));
}

NodeClassList &NodeClassList::operator=(const NodeClassList &other)
{
	if (this == &other) return *this;
	clear();
	for (std::size_t i = 0, n = other.size(); i < n; ++i)
		appendClassAtom(*this, other.at(i));
	return *this;
}

NodeClassList::NodeClassList(NodeClassList &&other) noexcept
{
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		inlineTokens[i] = other.inlineTokens[i];
#if GEA_UI_CLASS_OVERFLOW
	overflowHandle = other.overflowHandle;
	other.overflowHandle = kNoOverflow;
#endif
	count = other.count;
	other.count = 0;
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		other.inlineTokens[i] = kInvalidCssAtom;
}

NodeClassList &NodeClassList::operator=(NodeClassList &&other) noexcept
{
	if (this == &other) return *this;
	clear();
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		inlineTokens[i] = other.inlineTokens[i];
#if GEA_UI_CLASS_OVERFLOW
	overflowHandle = other.overflowHandle;
	other.overflowHandle = kNoOverflow;
#endif
	count = other.count;
	other.count = 0;
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		other.inlineTokens[i] = kInvalidCssAtom;
	return *this;
}

NodeClassList::~NodeClassList()
{
	releaseClassListOverflow(*this);
}

void NodeClassList::clear()
{
	releaseClassListOverflow(*this);
	for (std::uint8_t i = 0; i < kInlineTokenCount; ++i)
		inlineTokens[i] = kInvalidCssAtom;
	count = 0;
}

bool NodeClassList::set(const std::string &className)
{
	return set(className.c_str());
}

bool NodeClassList::set(const char *className)
{
	NodeClassList next = splitClassNameAtoms(className, className ? std::strlen(className) : 0);
	if (classListsEqual(*this, next)) return false;
	*this = std::move(next);
	return true;
}

bool NodeClassList::add(const std::string &token)
{
	if (!validToken(token)) return false;
	const CssAtomId atom = internCssAtom(token);
	if (containsAtom(atom)) return false;
	return appendClassAtom(*this, atom);
}

bool NodeClassList::remove(const std::string &token)
{
	if (!validToken(token)) return false;
	const CssAtomId atom = findCssAtom(token);
	if (atom == kInvalidCssAtom) return false;
	for (std::size_t i = 0, n = size(); i < n; ++i) {
		if (at(i) != atom) continue;
		eraseClassAtomAt(*this, i);
		return true;
	}
	return false;
}

bool NodeClassList::toggle(const std::string &token)
{
	if (contains(token)) {
		remove(token);
		return false;
	}
	if (!add(token)) return false;
	return true;
}

bool NodeClassList::toggle(const std::string &token, bool force)
{
	return force ? add(token) || contains(token) : !(remove(token) || !contains(token));
}

bool NodeClassList::contains(const std::string &token) const
{
	if (!validToken(token)) return false;
	return containsAtom(findCssAtom(token));
}

bool NodeClassList::containsAtom(CssAtomId token) const
{
	if (token == kInvalidCssAtom) return false;
	for (std::size_t i = 0, n = size(); i < n; ++i)
		if (at(i) == token) return true;
	return false;
}

CssAtomId NodeClassList::at(std::size_t index) const
{
	if (index >= count) return kInvalidCssAtom;
	if (index < kInlineTokenCount) return inlineTokens[index];
#if GEA_UI_CLASS_OVERFLOW
	const auto *overflow = classListOverflowConst(overflowHandle);
	if (!overflow) return kInvalidCssAtom;
	const std::size_t overflowIndex = index - kInlineTokenCount;
	if (overflowIndex >= overflow->size()) return kInvalidCssAtom;
	return (*overflow)[overflowIndex];
#else
	return kInvalidCssAtom;
#endif
}

std::string NodeClassList::value() const
{
	std::string out;
	for (std::size_t i = 0, n = size(); i < n; ++i) {
		const CssAtomId token = at(i);
		if (!out.empty()) out.push_back(' ');
		out += cssAtomText(token);
	}
	return out;
}

// Empty override stores own nothing. One allocation contains both metadata
// and values; computed styles remain ordinary aligned fields in Node.
struct NodeStyleOverrideStore::Block {
	// Entries are unique by Property; geometric growth fits this bound.
	static_assert(static_cast<unsigned>(Property::Count) <= 32768);
	std::uint16_t count = 0, capacity = 0;
	// Only CSS px overrides allocate unit metadata. Raw numeric styles keep
	// their compact entries and do not lose precision by reverse-scaling ints.
	std::unique_ptr<std::vector<std::pair<Property, float>>> cssPixels;
	NodeStyleOverride *values() { return reinterpret_cast<NodeStyleOverride *>(this + 1); }
	const NodeStyleOverride *values() const { return reinterpret_cast<const NodeStyleOverride *>(this + 1); }
};
namespace {
void appendStyleOverride(NodeStyleOverrideStore &store, NodeStyleOverride entry)
{
	auto *old = store.block;
	if (!old || old->count == old->capacity) {
		// Small inline styles commonly have three entries. Grow 2 -> 3 -> 4
		// before doubling, so they do not retain an unused fourth entry.
		const std::size_t capacity = old ? (old->capacity < 4 ? old->capacity + 1 : old->capacity * 2) : 2;
		auto *next = new (::operator new(sizeof(NodeStyleOverrideStore::Block) + capacity * sizeof(NodeStyleOverride))) NodeStyleOverrideStore::Block;
		next->capacity = static_cast<std::uint16_t>(capacity);
		if (old) {
			next->count = old->count;
			next->cssPixels = std::move(old->cssPixels);
			for (std::size_t i = 0; i < old->count; ++i) new (&next->values()[i]) NodeStyleOverride(old->values()[i]);
			old->~Block();
			::operator delete(old);
		}
		store.block = next;
	}
	new (&store.block->values()[store.block->count++]) NodeStyleOverride(entry);
}
}
NodeStyleOverrideStore::NodeStyleOverrideStore(const NodeStyleOverrideStore &other)
{
	for (std::size_t i = 0; i < other.size(); ++i) appendStyleOverride(*this, other.at(i));
	if (other.block && other.block->cssPixels)
		block->cssPixels = std::make_unique<std::vector<std::pair<Property, float>>>(*other.block->cssPixels);
}
NodeStyleOverrideStore &NodeStyleOverrideStore::operator=(const NodeStyleOverrideStore &other)
{
	if (this != &other) {
		NodeStyleOverrideStore copy(other);
		*this = std::move(copy);
	}
	return *this;
}
NodeStyleOverrideStore::NodeStyleOverrideStore(NodeStyleOverrideStore &&other) noexcept : block(other.block)
{
	other.block = nullptr;
}
NodeStyleOverrideStore &NodeStyleOverrideStore::operator=(NodeStyleOverrideStore &&other) noexcept
{
	if (this != &other) {
		clear();
		block = other.block;
		other.block = nullptr;
	}
	return *this;
}
NodeStyleOverrideStore::~NodeStyleOverrideStore() { clear(); }
void NodeStyleOverrideStore::clear()
{
	if (!block) return;
	block->~Block();
	::operator delete(block);
	block = nullptr;
}
std::size_t NodeStyleOverrideStore::size() const { return block ? block->count : 0; }
StorageUsage NodeStyleOverrideStore::storageUsage() const
{
    StorageUsage usage;
    if (!block) return usage;
    usage.addAllocation(block, sizeof(Block) + block->capacity * sizeof(NodeStyleOverride));
    if (block->cssPixels) {
        usage.addAllocation(block->cssPixels.get(), sizeof(*block->cssPixels));
        usage.addVector(*block->cssPixels);
    }
    return usage;
}

void NodeStyleOverrideStore::set(Property property, int value)
{
	for (std::size_t i = 0; i < size(); ++i) {
		auto &entry = block->values()[i];
		if (entry.property == property) {
			entry.value = value;
			if (block->cssPixels) {
				auto &units = *block->cssPixels;
				units.erase(std::remove_if(units.begin(), units.end(), [property](const auto &item) { return item.first == property; }), units.end());
			}
			return;
		}
	}
	appendStyleOverride(*this, {property, value});
}
void NodeStyleOverrideStore::setCssPixels(Property property, float value)
{
	if (!block) return;
	bool present = false;
	for (std::size_t i = 0; i < size(); ++i) if (at(i).property == property) { present = true; break; }
	if (!present) return;
	if (!block->cssPixels) block->cssPixels = std::make_unique<std::vector<std::pair<Property, float>>>();
	for (auto &entry : *block->cssPixels) if (entry.first == property) { entry.second = value; return; }
	block->cssPixels->emplace_back(property, value);
}
bool NodeStyleOverrideStore::getCssPixels(Property property, float &value) const
{
	if (!block || !block->cssPixels) return false;
	for (const auto &entry : *block->cssPixels) if (entry.first == property) { value = entry.second; return true; }
	return false;
}
bool NodeStyleOverrideStore::remove(Property property)
{
	for (std::size_t i = 0; i < size(); ++i) {
		if (at(i).property != property) continue;
		if (block->cssPixels) {
			auto &units = *block->cssPixels;
			units.erase(std::remove_if(units.begin(), units.end(), [property](const auto &item) { return item.first == property; }), units.end());
		}
		for (std::size_t j = i + 1; j < block->count; ++j) block->values()[j - 1] = block->values()[j];
		if (--block->count == 0) clear();
		return true;
	}
	return false;
}
const NodeStyleOverride &NodeStyleOverrideStore::at(std::size_t index) const { return block->values()[index]; }

#if GEA_CSS_CUSTOM_PROPERTIES
void NodeCustomPropertyStore::clear()
{
	values.clear();
}

void NodeCustomPropertyStore::set(const std::string &name, const std::string &value)
{
	if (name.empty()) return;
	set(internCssAtom(name), value);
}

void setCustomPropertyStorage(NodeCustomProperty &entry, const std::string &value)
{
	const CssAtomId atom = value.empty() ? kInvalidCssAtom : internCssAtom(value);
	// Form the replacement before releasing the old string: callers may pass
	// the value returned by get(), including an uninterned overflow value.
	auto storage = atom != kInvalidCssAtom || value.empty()
	    ? std::shared_ptr<const std::string>(std::shared_ptr<const std::string>{}, &cssAtomString(atom))
	    : std::make_shared<const std::string>(value);
	entry.value = std::move(storage);
	entry.valueAtom = atom;
}

void NodeCustomPropertyStore::set(CssAtomId nameId, const std::string &value)
{
	if (nameId == kInvalidCssAtom) return;
	for (auto &entry : values) {
		if (entry.nameId != nameId) continue;
		setCustomPropertyStorage(entry, value);
		entry.flags = 0;
		return;
	}
	NodeCustomProperty entry;
	entry.nameId = nameId;
	setCustomPropertyStorage(entry, value);
	values.push_back(std::move(entry));
}

void NodeCustomPropertyStore::setColor(CssAtomId nameId,
                                       const std::string &value,
                                       std::int32_t styleColor,
                                       std::int32_t nativeColor,
                                       std::uint8_t alpha)
{
	if (nameId == kInvalidCssAtom) return;
	for (auto &entry : values) {
		if (entry.nameId != nameId) continue;
		setCustomPropertyStorage(entry, value);
		entry.colorStyle = styleColor;
		entry.colorNative = nativeColor;
		entry.colorAlpha = alpha;
		entry.flags = static_cast<std::uint8_t>((entry.flags & ~2u) | 1u);
		return;
	}
	NodeCustomProperty entry;
	entry.nameId = nameId;
	setCustomPropertyStorage(entry, value);
	entry.colorStyle = styleColor;
	entry.colorNative = nativeColor;
	entry.colorAlpha = alpha;
	entry.flags = 1;
	values.push_back(std::move(entry));
}

void NodeCustomPropertyStore::setLength(CssAtomId nameId,
                                        const std::string &value,
                                        float lengthValue,
                                        std::uint8_t lengthUnit)
{
#if GEA_CSS_CUSTOM_PROPERTY_LENGTHS
	if (nameId == kInvalidCssAtom) return;
	for (auto &entry : values) {
		if (entry.nameId != nameId) continue;
		setCustomPropertyStorage(entry, value);
		entry.lengthValue = lengthValue;
		entry.lengthUnit = lengthUnit;
		entry.flags = static_cast<std::uint8_t>((entry.flags & ~1u) | 2u);
		return;
	}
	NodeCustomProperty entry;
	entry.nameId = nameId;
	setCustomPropertyStorage(entry, value);
	entry.lengthValue = lengthValue;
	entry.lengthUnit = lengthUnit;
	entry.flags = 2;
	values.push_back(std::move(entry));
#else
	(void)lengthValue; (void)lengthUnit;
	set(nameId, value);
#endif
}

const std::string *NodeCustomPropertyStore::get(const std::string &name) const
{
	if (name.empty()) return nullptr;
	return get(findCssAtom(name));
}

const std::string *NodeCustomPropertyStore::get(CssAtomId nameId) const
{
	if (nameId == kInvalidCssAtom) return nullptr;
	if (const NodeCustomProperty *entry = getEntry(nameId)) return entry->value.get();
	return nullptr;
}

const NodeCustomProperty *NodeCustomPropertyStore::getEntry(CssAtomId nameId) const
{
	if (nameId == kInvalidCssAtom) return nullptr;
	for (const auto &entry : values)
		if (entry.nameId == nameId) return &entry;
	return nullptr;
}

#endif

// File-scope lazy pointer rather than a function-local static. On this Xtensa
// toolchain the static-local guard is NOT inlined, so a Meyers singleton pays a
// __cxa_guard_acquire CALL on every access — and treeState() sits on the hottest
// style-recompute path (called per rule, per node). UI state is single-threaded
// (the mount task and the frame task never run concurrently), so the guard is
// unnecessary; a zero-initialized pointer + null check is one load and a branch.
static TreeState *g_treeState = nullptr;
StorageUsage treeStorageUsage()
{
    StorageUsage usage;
    usage.staticBytes = sizeof(g_treeState);
    usage.addAllocation(g_treeState, sizeof(TreeState));
    return usage;
}


TreeState &treeState()
{
	if (!g_treeState) {
#if defined(ESP_PLATFORM)
		// The tree pool belongs in PSRAM (it's CPU/cache-only, never DMA), so it
		// doesn't eat the scarce internal SRAM the radios need. esp_psram is brought
		// up before app code runs (CONFIG_SPIRAM_BOOT_INIT), so the first access
		// here lands in PSRAM directly.
		void *mem = heap_caps_malloc(sizeof(TreeState), MALLOC_CAP_SPIRAM);
		g_treeState = mem ? new (mem) TreeState() : new TreeState();
#else
		g_treeState = new TreeState();
#endif
	}
	return *g_treeState;
}

} // namespace gea::embedded::ui
