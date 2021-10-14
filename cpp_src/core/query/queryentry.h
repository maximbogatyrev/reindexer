#pragma once

#include <climits>
#include <string>
#include <vector>
#include "core/expressiontree.h"
#include "core/keyvalue/variant.h"
#include "core/type_consts_helpers.h"
#include "estl/h_vector.h"
#include "tools/serializer.h"

namespace reindexer {

class Query;
template <typename T>
class PayloadIface;
using ConstPayload = PayloadIface<const PayloadValue>;
class TagsMatcher;

struct QueryEntry {
	const static int kNoJoins = -1;

	QueryEntry(int joinIdx) : joinIndex(joinIdx) {}
	QueryEntry(CondType cond, const std::string &idx, int idxN, bool dist = false)
		: index(idx), idxNo(idxN), condition(cond), distinct(dist) {}
	QueryEntry() = default;

	bool operator==(const QueryEntry &) const noexcept;
	bool operator!=(const QueryEntry &other) const noexcept { return !operator==(other); }

	std::string index;
	int idxNo = IndexValueType::NotSet;
	CondType condition = CondType::CondAny;
	bool distinct = false;
	VariantArray values;
	int joinIndex = kNoJoins;

	template <typename JS>
	std::string Dump(const std::vector<JS> &joinedSelectors) const {
		WrSerializer ser;
		if (joinIndex != kNoJoins) {
			const auto &js = joinedSelectors.at(joinIndex);
			const auto &q = js.JoinQuery();
			ser << js.Type() << " (" << q.GetSQL() << ") ON ";
			ser << '(';
			for (const auto &jqe : q.joinEntries_) {
				if (&jqe != &q.joinEntries_.front()) {
					ser << ' ' << jqe.op_ << ' ';
				} else {
					assert(jqe.op_ == OpAnd);
				}
				ser << q._namespace << '.' << jqe.joinIndex_ << ' ' << InvertJoinCondition(jqe.condition_) << ' ' << jqe.index_;
			}
			ser << ')';
		} else if (distinct) {
			ser << "Distinct index: " << index;
		} else {
			ser << index << ' ' << condition << ' ';
			const bool severalValues = (values.size() > 1);
			if (severalValues) ser << '(';
			for (auto &v : values) {
				if (&v != &*values.begin()) ser << ',';
				ser << '\'' << v.As<std::string>() << '\'';
			}
			if (severalValues) ser << ')';
		}
		return std::string{ser.Slice()};
	}
};

struct EqualPosition : public h_vector<unsigned, 2> {};

class JsonBuilder;

class QueryEntries : public ExpressionTree<OpType, Bracket, 4, QueryEntry> {
	using Base = ExpressionTree<OpType, Bracket, 4, QueryEntry>;
	QueryEntries(Base &&b) : Base{std::move(b)} {}

public:
	QueryEntries() = default;
	QueryEntries(QueryEntries &&) = default;
	QueryEntries(const QueryEntries &) = default;
	QueryEntries &operator=(QueryEntries &&) = default;
	QueryEntries MakeLazyCopy() & { return {makeLazyCopy()}; }

	const QueryEntry &operator[](size_t i) const {
		assert(i < container_.size());
		return container_[i].Value();
	}
	QueryEntry &Entry(size_t i) {
		assert(i < container_.size());
		return container_[i].Value();
	}

	template <typename T>
	std::pair<unsigned, EqualPosition> DetermineEqualPositionIndexes(const T &fields) const;
	template <typename T>
	EqualPosition DetermineEqualPositionIndexes(unsigned start, const T &fields) const;
	void ToDsl(const Query &parentQuery, JsonBuilder &builder) const { return toDsl(cbegin(), cend(), parentQuery, builder); }
	void WriteSQLWhere(const Query &parentQuery, WrSerializer &, bool stripArgs) const;
	void Serialize(WrSerializer &ser) const { serialize(cbegin(), cend(), ser); }
	bool CheckIfSatisfyConditions(const ConstPayload &pl, TagsMatcher &tm) const;
	template <typename JS>
	std::string Dump(const std::vector<JS> &joinedSelectors) const {
		WrSerializer ser;
		dump(0, cbegin(), cend(), joinedSelectors, ser);
		return std::string{ser.Slice()};
	}

private:
	static void toDsl(const_iterator it, const_iterator to, const Query &parentQuery, JsonBuilder &);
	static void writeSQL(const Query &parentQuery, const_iterator from, const_iterator to, WrSerializer &, bool stripArgs);
	static void serialize(const_iterator it, const_iterator to, WrSerializer &);
	static bool checkIfSatisfyConditions(const_iterator begin, const_iterator end, const ConstPayload &, TagsMatcher &);
	static bool checkIfSatisfyCondition(const QueryEntry &, const ConstPayload &, TagsMatcher &);
	template <typename JS>
	static void dump(size_t level, const_iterator begin, const_iterator end, const std::vector<JS> &joinedSelectors, WrSerializer &ser) {
		for (const_iterator it = begin; it != end; ++it) {
			for (size_t i = 0; i < level; ++i) {
				ser << "   ";
			}
			if (it != begin || it->operation != OpAnd) {
				ser << it->operation << ' ';
			}
			it->InvokeAppropriate<void>(
				[&](const Bracket &) {
					ser << "(\n";
					dump(level + 1, it.cbegin(), it.cend(), joinedSelectors, ser);
					for (size_t i = 0; i < level; ++i) {
						ser << "   ";
					}
					ser << ")\n";
				},
				[&joinedSelectors, &ser](const QueryEntry &qe) { ser << qe.Dump(joinedSelectors) << '\n'; });
		}
	}
};

extern template EqualPosition QueryEntries::DetermineEqualPositionIndexes<std::vector<std::string>>(
	unsigned start, const std::vector<std::string> &fields) const;
extern template std::pair<unsigned, EqualPosition> QueryEntries::DetermineEqualPositionIndexes<std::vector<std::string>>(
	const std::vector<std::string> &fields) const;
extern template std::pair<unsigned, EqualPosition> QueryEntries::DetermineEqualPositionIndexes<h_vector<std::string, 4>>(
	const h_vector<std::string, 4> &fields) const;
extern template std::pair<unsigned, EqualPosition> QueryEntries::DetermineEqualPositionIndexes<std::initializer_list<std::string>>(
	const std::initializer_list<std::string> &fields) const;

struct UpdateEntry {
	UpdateEntry() {}
	UpdateEntry(std::string c, VariantArray v, FieldModifyMode m = FieldModeSet, bool e = false)
		: column(std::move(c)), values(std::move(v)), mode(m), isExpression(e) {}
	bool operator==(const UpdateEntry &) const;
	bool operator!=(const UpdateEntry &) const;
	std::string column;
	VariantArray values;
	FieldModifyMode mode = FieldModeSet;
	bool isExpression = false;
	bool isArray = false;
};

struct QueryJoinEntry {
	QueryJoinEntry() = default;
	QueryJoinEntry(OpType op, CondType cond, std::string idx, std::string jIdx)
		: op_{op}, condition_{cond}, index_{std::move(idx)}, joinIndex_{std::move(jIdx)} {}
	bool operator==(const QueryJoinEntry &) const noexcept;
	OpType op_ = OpAnd;
	CondType condition_ = CondEq;
	std::string index_;
	std::string joinIndex_;
	int idxNo = -1;
	bool reverseNamespacesOrder = false;
};

struct SortingEntry {
	SortingEntry() {}
	SortingEntry(const std::string &e, bool d) : expression(e), desc(d) {}
	bool operator==(const SortingEntry &) const;
	bool operator!=(const SortingEntry &) const;
	std::string expression;
	bool desc = false;
	int index = IndexValueType::NotSet;
};

struct SortingEntries : public h_vector<SortingEntry, 1> {};

struct AggregateEntry {
	AggregateEntry() = default;
	AggregateEntry(AggType type, const h_vector<std::string, 1> &fields, unsigned limit = UINT_MAX, unsigned offset = 0)
		: type_(type), fields_(fields), limit_(limit), offset_(offset) {}
	bool operator==(const AggregateEntry &) const;
	bool operator!=(const AggregateEntry &) const;
	AggType type_;
	h_vector<std::string, 1> fields_;
	SortingEntries sortingEntries_;
	unsigned limit_ = UINT_MAX;
	unsigned offset_ = 0;
};

}  // namespace reindexer
