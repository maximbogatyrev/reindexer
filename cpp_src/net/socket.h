#pragma once

#include <stdlib.h>
#include "estl/chunk_buf.h"
#include "tools/ssize_t.h"

struct addrinfo;
namespace reindexer {
namespace net {

class socket {
public:
	socket(const socket &other) = default;
	socket &operator=(const socket &other) = default;
	socket(int fd = -1) : fd_(fd) {}

	int bind(std::string_view addr);
	int connect(std::string_view addr) noexcept;
	socket accept();
	int listen(int backlog);
	ssize_t recv(span<char> buf);
	ssize_t send(const span<char> buf);
	ssize_t send(span<chunk> chunks);
	int close();
	std::string addr() const;

	int set_nonblock();
	int set_nodelay();
	int fd() const noexcept { return fd_; }
	bool valid() const noexcept { return fd_ >= 0; }
	bool has_pending_data() const noexcept;

	static int last_error();
	static bool would_block(int error);

protected:
	int create(std::string_view addr, struct addrinfo **pres);

	int fd_;
};
}  // namespace net
}  // namespace reindexer
