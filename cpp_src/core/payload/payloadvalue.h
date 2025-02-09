#pragma once

#include <stddef.h>
#include <atomic>
#include <iosfwd>
#include "tools/assertrx.h"

namespace reindexer {

// The full item's payload object. It must be speed & size optimized
class PayloadValue {
public:
	typedef std::atomic<int32_t> refcounter;
	struct dataHeader {
		dataHeader() noexcept : refcount(1), cap(0), lsn(-1) {}

		~dataHeader() { assertrx(refcount.load(std::memory_order_acquire) == 0); }
		refcounter refcount;
		unsigned cap;
		int64_t lsn;
	};

	PayloadValue() noexcept : p_(nullptr) {}
	PayloadValue(const PayloadValue &other) noexcept : p_(other.p_) {
		if (p_) {
			header()->refcount.fetch_add(1, std::memory_order_relaxed);
		}
	}
	// Alloc payload store with size, and copy data from another array
	PayloadValue(size_t size, const uint8_t *ptr = nullptr, size_t cap = 0);
	~PayloadValue() { release(); }
	PayloadValue &operator=(const PayloadValue &other) noexcept {
		if (&other != this) {
			release();
			p_ = other.p_;
			if (p_) header()->refcount.fetch_add(1, std::memory_order_relaxed);
		}
		return *this;
	}
	PayloadValue(PayloadValue &&other) noexcept : p_(other.p_) { other.p_ = nullptr; }
	PayloadValue &operator=(PayloadValue &&other) noexcept {
		if (&other != this) {
			release();
			p_ = other.p_;
			other.p_ = nullptr;
		}

		return *this;
	}

	// Clone if data is shared for copy-on-write.
	void Clone(size_t size = 0);
	// Resize
	void Resize(size_t oldSize, size_t newSize);
	// Get data pointer
	uint8_t *Ptr() const noexcept { return p_ + sizeof(dataHeader); }
	void SetLSN(int64_t lsn) { header()->lsn = lsn; }
	int64_t GetLSN() const { return p_ ? header()->lsn : 0; }
	bool IsFree() const noexcept { return bool(p_ == nullptr); }
	void Free() noexcept { release(); }
	size_t GetCapacity() const noexcept { return header()->cap; }
	const uint8_t *get() const noexcept { return p_; }

protected:
	uint8_t *alloc(size_t cap);
	void release() noexcept;

	dataHeader *header() noexcept { return reinterpret_cast<dataHeader *>(p_); }
	const dataHeader *header() const noexcept { return reinterpret_cast<dataHeader *>(p_); }
	friend std::ostream &operator<<(std::ostream &os, const PayloadValue &);
	// Data of elements, shared
	uint8_t *p_;
};

}  // namespace reindexer
