#pragma once

#include <chrono>
#include "client/item.h"
#include "client/resultserializer.h"

namespace reindexer {
class TagsMatcher;
namespace net {
namespace cproto {
class ClientConnection;
}
}  // namespace net

namespace client {

using std::chrono::seconds;
using std::chrono::milliseconds;

class Namespace;
using NsArray = h_vector<Namespace*, 1>;

class QueryResults {
public:
	typedef std::function<void(const Error& err)> Completion;
	QueryResults(int fetchFlags = 0);
	QueryResults(const QueryResults&) = delete;
	QueryResults(QueryResults&&) noexcept;
	~QueryResults();
	QueryResults& operator=(const QueryResults&) = delete;
	QueryResults& operator=(QueryResults&& obj) noexcept;

	class Iterator {
	public:
		Error GetJSON(WrSerializer& wrser, bool withHdrLen = true);
		Error GetCJSON(WrSerializer& wrser, bool withHdrLen = true);
		Error GetMsgPack(WrSerializer& wrser, bool withHdrLen = true);
		Item GetItem();
		int64_t GetLSN();
		bool IsRaw();
		std::string_view GetRaw();
		Iterator& operator++();
		Error Status() { return qr_->status_; }
		bool operator!=(const Iterator&) const;
		bool operator==(const Iterator&) const;
		Iterator& operator*() { return *this; }
		void readNext();
		void getJSONFromCJSON(std::string_view cjson, WrSerializer& wrser, bool withHdrLen = true);

		const QueryResults* qr_;
		int idx_, pos_, nextPos_;
		ResultSerializer::ItemParams itemParams_;
	};

	Iterator begin() const { return Iterator{this, 0, 0, 0, {}}; }
	Iterator end() const { return Iterator{this, queryParams_.qcount, 0, 0, {}}; }

	size_t Count() const { return queryParams_.qcount; }
	int TotalCount() const { return queryParams_.totalcount; }
	bool HaveRank() const { return queryParams_.flags & kResultsWithRank; }
	bool NeedOutputRank() const { return queryParams_.flags & kResultsNeedOutputRank; }
	const std::string& GetExplainResults() const { return queryParams_.explainResults; }
	const std::vector<AggregationResult>& GetAggregationResults() const { return queryParams_.aggResults; }
	Error Status() { return status_; }
	h_vector<std::string_view, 1> GetNamespaces() const;
	bool IsCacheEnabled() const { return queryParams_.flags & kResultsWithItemID; }

	TagsMatcher getTagsMatcher(int nsid) const;

private:
	friend class RPCClient;
	friend class RPCClientMock;
	QueryResults(net::cproto::ClientConnection* conn, NsArray&& nsArray, Completion cmpl, int fetchFlags, int fetchAmount, seconds timeout);
	QueryResults(net::cproto::ClientConnection* conn, NsArray&& nsArray, Completion cmpl, std::string_view rawResult, int queryID,
				 int fetchFlags, int fetchAmount, seconds timeout);
	void Bind(std::string_view rawResult, int queryID);
	void fetchNextResults();
	void completion(const Error& err) {
		if (cmpl_) {
			auto cmpl = std::move(cmpl_);
			cmpl(err);
		}
	}

	net::cproto::ClientConnection* conn_;

	NsArray nsArray_;
	h_vector<char, 0x100> rawResult_;
	int queryID_;
	int fetchOffset_;
	int fetchFlags_;
	int fetchAmount_;
	seconds requestTimeout_;

	ResultSerializer::QueryParams queryParams_;
	Error status_;
	Completion cmpl_;
};
}  // namespace client
}  // namespace reindexer
