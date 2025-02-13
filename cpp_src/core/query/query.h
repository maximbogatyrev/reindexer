#pragma once

#include <functional>
#include <initializer_list>
#include "core/keyvalue/geometry.h"
#include "queryentry.h"
#include "tools/errors.h"
#include "tools/stringstools.h"

/// @namespace reindexer
/// The base namespace
namespace reindexer {

class WrSerializer;
class Serializer;
class JoinedQuery;

constexpr std::string_view kAggregationWithSelectFieldsMsgError =
	"Not allowed to combine aggregation functions and fields' filter in a single query";

/// @class Query
/// Allows to select data from DB.
/// Analog to ansi-sql select query.
class Query {
	template <typename Q>
	class OnHelperTempl;
	template <typename Q>
	class OnHelperGroup {
	public:
		OnHelperGroup &&Not() &&noexcept {
			op_ = OpNot;
			return std::move(*this);
		}
		OnHelperGroup &&Or() &&noexcept {
			op_ = OpOr;
			return std::move(*this);
		}
		OnHelperGroup &&On(std::string index, CondType cond, std::string joinIndex) &&;
		Q CloseBracket() &&noexcept { return std::forward<Q>(q_); }

	private:
		OnHelperGroup(Q q, JoinedQuery &jq) noexcept : q_{std::forward<Q>(q)}, jq_{jq} {}
		Q q_;
		JoinedQuery &jq_;
		OpType op_{OpAnd};
		friend class OnHelperTempl<Q>;
	};
	template <typename Q>
	class OnHelperTempl {
	public:
		OnHelperTempl &&Not() &&noexcept {
			op_ = OpNot;
			return std::move(*this);
		}
		Q On(std::string index, CondType cond, std::string joinIndex) &&;
		OnHelperGroup<Q> OpenBracket() &&noexcept { return {std::forward<Q>(q_), jq_}; }

	private:
		OnHelperTempl(Q q, JoinedQuery &jq) noexcept : q_{std::forward<Q>(q)}, jq_{jq} {}
		Q q_;
		JoinedQuery &jq_;
		OpType op_{OpAnd};
		friend class Query;
	};
	using OnHelper = OnHelperTempl<Query &>;
	using OnHelperR = OnHelperTempl<Query &&>;

public:
	/// Creates an object for certain namespace with appropriate settings.
	/// @param nsName - name of the namespace the data to be selected from.
	/// @param start - number of the first row to get from selected set. Analog to sql OFFSET Offset.
	/// @param count - number of rows to get from result set. Analog to sql LIMIT RowsCount.
	/// @param calcTotal - calculation mode.
	template <typename T>
	explicit Query(T nsName, unsigned start = QueryEntry::kDefaultOffset, unsigned count = QueryEntry::kDefaultLimit,
				   CalcTotalMode calcTotal = ModeNoTotal)
		: namespace_(std::forward<T>(nsName)), start_(start), count_(count), calcTotal_(calcTotal) {}

	/// Creates an empty object.
	Query() = default;

	/// Allows to compare 2 Query objects.
	bool operator==(const Query &) const;

	/// Parses pure sql select query and initializes Query object data members as a result.
	/// @param q - sql query.
	void FromSQL(std::string_view q);

	/// Logs query in 'Select field1, ... field N from namespace ...' format.
	/// @param ser - serializer to store SQL string
	/// @param stripArgs - replace condition values with '?'
	WrSerializer &GetSQL(WrSerializer &ser, bool stripArgs = false) const;

	/// Logs query in 'Select field1, ... field N from namespace ...' format.
	/// @param stripArgs - replace condition values with '?'
	/// @return Query in SQL format
	std::string GetSQL(bool stripArgs = false) const;

	/// Logs query in 'Select field1, ... field N from namespace ...' format.
	/// @param realType - replaces original query's type
	/// @return Query in SQL format
	std::string GetSQL(QueryType realType) const;

	/// Parses JSON dsl set.
	/// @param dsl - dsl set.
	/// @return always returns errOk or throws an exception.
	Error FromJSON(const std::string &dsl);

	/// returns structure of a query in JSON dsl format
	std::string GetJSON() const;

	/// Enable explain query
	/// @param on - signaling on/off
	/// @return Query object ready to be executed
	Query &Explain(bool on = true) & {
		explain_ = on;
		return *this;
	}
	Query &&Explain(bool on = true) && { return std::move(Explain(on)); }

	/// Adds a condition with a single value. Analog to sql Where clause.
	/// @param field - field used in condition clause.
	/// @param cond - type of condition.
	/// @param val - value of index to be compared with.
	/// @return Query object ready to be executed.
	template <typename Str, typename Input>
	Query &Where(Str &&field, CondType cond, Input val) & {
		return Where(std::forward<Str>(field), cond, {std::forward<Input>(val)});
	}
	template <typename Str, typename Input>
	Query &&Where(Str &&field, CondType cond, Input val) && {
		return std::move(Where<Str, Input>(std::forward<Str>(field), cond, {std::move(val)}));
	}

	/// Adds a condition with several values. Analog to sql Where clause.
	/// @param field - field used in condition clause.
	/// @param cond - type of condition.
	/// @param l - list of index values to be compared with.
	/// @return Query object ready to be executed.
	template <typename Str, typename T>
	Query &Where(Str &&field, CondType cond, std::initializer_list<T> l) & {
		VariantArray values;
		values.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) values.emplace_back(*it);
		entries.Append<QueryEntry>(nextOp_, std::forward<Str>(field), cond, std::move(values));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str, typename T>
	Query &&Where(Str &&field, CondType cond, std::initializer_list<T> l) && {
		return std::move(Where<Str, T>(std::forward<Str>(field), cond, std::move(l)));
	}

	/// Adds a condition with several values. Analog to sql Where clause.
	/// @param field - field used in condition clause.
	/// @param cond - type of condition.
	/// @param l - vector of index values to be compared with.
	/// @return Query object ready to be executed.
	template <typename Str, typename T>
	Query &Where(Str &&field, CondType cond, const std::vector<T> &l) & {
		VariantArray values;
		values.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) values.emplace_back(*it);
		entries.Append<QueryEntry>(nextOp_, std::forward<Str>(field), cond, std::move(values));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str, typename T>
	Query &&Where(Str &&field, CondType cond, const std::vector<T> &l) && {
		return std::move(Where<Str, T>(std::forward<Str>(field), cond, l));
	}

	/// Adds a condition with several values. Analog to sql Where clause.
	/// @param field - field used in condition clause.
	/// @param cond - type of condition.
	/// @param l - vector of index values to be compared with.
	/// @return Query object ready to be executed.
	template <typename Str>
	Query &Where(Str &&field, CondType cond, VariantArray l) & {
		entries.Append<QueryEntry>(nextOp_, std::forward<Str>(field), cond, std::move(l));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str>
	Query &&Where(Str &&field, CondType cond, VariantArray l) && {
		return std::move(Where(std::forward<Str>(field), cond, std::move(l)));
	}

	/// Adds a condition with several values to a composite index.
	/// @param idx - composite index name.
	/// @param cond - type of condition.
	/// @param l - list of values to be compared according to the order
	/// of indexes in composite index name.
	/// There can be maximum 2 VariantArray objects in l: in case of CondRange condition,
	/// in all other cases amount of elements in l would be striclty equal to 1.
	/// For example, composite index name is "bookid+price", so l[0][0] (and l[1][0]
	/// in case of CondRange) belongs to "bookid" and l[0][1] (and l[1][1] in case of CondRange)
	/// belongs to "price" indexes.
	/// @return Query object ready to be executed.
	template <typename Str>
	Query &WhereComposite(Str &&idx, CondType cond, std::initializer_list<VariantArray> l) & {
		VariantArray values;
		values.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) {
			values.emplace_back(*it);
		}
		entries.Append<QueryEntry>(nextOp_, std::forward<Str>(idx), cond, std::move(values));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str>
	Query &&WhereComposite(Str &&idx, CondType cond, std::initializer_list<VariantArray> l) && {
		return std::move(WhereComposite(std::forward<Str>(idx), cond, std::move(l)));
	}
	template <typename Str>
	Query &WhereComposite(Str &&idx, CondType cond, const std::vector<VariantArray> &v) & {
		VariantArray values;
		values.reserve(v.size());
		for (auto it = v.begin(); it != v.end(); it++) {
			values.emplace_back(*it);
		}
		entries.Append<QueryEntry>(nextOp_, std::forward<Str>(idx), cond, std::move(values));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str>
	Query &&WhereComposite(Str &&idx, CondType cond, const std::vector<VariantArray> &v) && {
		return std::move(WhereComposite(std::forward<Str>(idx), cond, v));
	}
	template <typename Str1, typename Str2>
	Query &WhereBetweenFields(Str1 &&firstIdx, CondType cond, Str2 &&secondIdx) & {
		entries.Append<BetweenFieldsQueryEntry>(nextOp_, std::forward<Str1>(firstIdx), cond, std::forward<Str2>(secondIdx));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str1, typename Str2>
	Query &&WhereBetweenFields(Str1 &&firstIdx, CondType cond, Str2 &&secondIdx) && {
		return std::move(WhereBetweenFields(std::forward<Str1>(firstIdx), cond, std::forward<Str2>(secondIdx)));
	}

	template <typename Str>
	Query &DWithin(Str &&field, Point p, double distance) & {
		entries.Append<QueryEntry>(nextOp_, std::forward<Str>(field), CondDWithin, VariantArray::Create(p, distance));
		nextOp_ = OpAnd;
		return *this;
	}
	template <typename Str>
	Query &&DWithin(Str &&field, Point p, double distance) && {
		return std::move(DWithin(std::forward<Str>(field), p, distance));
	}

	/// Sets a new value for a field.
	/// @param field - field name.
	/// @param value - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str, typename ValueType>
	Query &Set(Str &&field, ValueType value, bool hasExpressions = false) & {
		return Set(std::forward<Str>(field), {value}, hasExpressions);
	}
	template <typename Str, typename ValueType>
	Query &&Set(Str &&field, ValueType value, bool hasExpressions = false) && {
		return std::move(Set<Str, ValueType>(std::forward<Str>(field), std::move(value), hasExpressions));
	}
	/// Sets a new value for a field.
	/// @param field - field name.
	/// @param l - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str, typename ValueType>
	Query &Set(Str &&field, std::initializer_list<ValueType> l, bool hasExpressions = false) & {
		VariantArray value;
		value.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) value.emplace_back(*it);
		return Set(std::forward<Str>(field), std::move(value), hasExpressions);
	}
	template <typename Str, typename ValueType>
	Query &&Set(Str &&field, std::initializer_list<ValueType> l, bool hasExpressions = false) && {
		return std::move(Set<Str, ValueType>(std::forward<Str>(field), std::move(l), hasExpressions));
	}
	/// Sets a new value for a field.
	/// @param field - field name.
	/// @param l - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str, typename T>
	Query &Set(Str &&field, const std::vector<T> &l, bool hasExpressions = false) & {
		VariantArray value;
		value.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) value.emplace_back(*it);
		return Set(std::forward<Str>(field), std::move(value.MarkArray()), hasExpressions);
	}
	template <typename Str, typename T>
	Query &&Set(Str &&field, const std::vector<T> &l, bool hasExpressions = false) && {
		return std::move(Set<Str, T>(std::forward<Str>(field), l, hasExpressions));
	}
	/// Sets a new value for a field.
	/// @param field - field name.
	/// @param value - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str>
	Query &Set(Str &&field, VariantArray value, bool hasExpressions = false) & {
		updateFields_.emplace_back(std::forward<Str>(field), std::move(value), FieldModeSet, hasExpressions);
		return *this;
	}
	template <typename Str>
	Query &&Set(Str &&field, VariantArray value, bool hasExpressions = false) && {
		return std::move(Set(std::forward<Str>(field), std::move(value), hasExpressions));
	}
	/// Sets a value for a field as an object.
	/// @param field - field name.
	/// @param value - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str, typename ValueType>
	Query &SetObject(Str &&field, ValueType value, bool hasExpressions = false) & {
		return SetObject(std::forward<Str>(field), {value}, hasExpressions);
	}
	template <typename Str, typename ValueType>
	Query &&SetObject(Str &&field, ValueType value, bool hasExpressions = false) && {
		return std::move(SetObject<Str, ValueType>(std::forward<Str>(field), std::move(value), hasExpressions));
	}
	/// Sets a new value for a field as an object.
	/// @param field - field name.
	/// @param l - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str, typename ValueType>
	Query &SetObject(Str &&field, std::initializer_list<ValueType> l, bool hasExpressions = false) & {
		VariantArray value;
		value.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) value.emplace_back(*it);
		return SetObject(std::forward<Str>(field), std::move(value), hasExpressions);
	}
	template <typename Str, typename ValueType>
	Query &&SetObject(Str &&field, std::initializer_list<ValueType> l, bool hasExpressions = false) && {
		return std::move(SetObject<Str, ValueType>(std::forward<Str>(field), std::move(l), hasExpressions));
	}
	/// Sets a new value for a field as an object.
	/// @param field - field name.
	/// @param l - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str, typename T>
	Query &SetObject(Str &&field, const std::vector<T> &l, bool hasExpressions = false) & {
		VariantArray value;
		value.reserve(l.size());
		for (auto it = l.begin(); it != l.end(); it++) value.emplace_back(Variant(*it));
		return SetObject(std::forward<Str>(field), std::move(value.MarkArray()), hasExpressions);
	}
	template <typename Str, typename T>
	Query &&SetObject(Str &&field, const std::vector<T> &l, bool hasExpressions = false) && {
		return std::move(SetObject<Str, T>(std::forward<Str>(field), l, hasExpressions));
	}
	/// Sets a value for a field as an object.
	/// @param field - field name.
	/// @param value - new value.
	/// @param hasExpressions - true: value has expresions in it
	template <typename Str>
	Query &SetObject(Str &&field, VariantArray value, bool hasExpressions = false) & {
		for (auto &it : value) {
			if (!it.Type().Is<KeyValueType::String>()) {
				throw Error(errLogic, "Unexpected variant type in SetObject: %s. Expecting KeyValueType::String with JSON-content",
							it.Type().Name());
			}
		}
		updateFields_.emplace_back(std::forward<Str>(field), std::move(value), FieldModeSetJson, hasExpressions);
		return *this;
	}
	template <typename Str>
	Query &&SetObject(Str &&field, VariantArray value, bool hasExpressions = false) && {
		return std::move(SetObject(std::forward<Str>(field), std::move(value), hasExpressions));
	}
	/// Drops a value for a field.
	/// @param field - field name.
	template <typename Str>
	Query &Drop(Str &&field) & {
		updateFields_.emplace_back(std::forward<Str>(field), VariantArray(), FieldModeDrop);
		return *this;
	}
	template <typename Str>
	Query &&Drop(Str &&field) && {
		return std::move(Drop(std::forward<Str>(field)));
	}

	/// Add sql-function to query.
	/// @param function - function declaration.
	template <typename Str>
	void AddFunction(Str &&function) {
		selectFunctions_.emplace_back(std::forward<Str>(function));
	}

	/// Adds equal position fields to arrays queries.
	/// @param equalPosition - list of fields with equal array index position.
	Query &AddEqualPosition(h_vector<std::string> equalPosition) & {
		auto *const bracket = entries.LastOpenBracket();
		auto &eqPos = (bracket ? bracket->equalPositions : entries.equalPositions);
		eqPos.emplace_back(std::make_move_iterator(equalPosition.begin()), std::make_move_iterator(equalPosition.end()));
		return *this;
	}
	Query &AddEqualPosition(std::vector<std::string> equalPosition) & {
		auto *const bracket = entries.LastOpenBracket();
		auto &eqPos = (bracket ? bracket->equalPositions : entries.equalPositions);
		eqPos.emplace_back(std::make_move_iterator(equalPosition.begin()), std::make_move_iterator(equalPosition.end()));
		return *this;
	}
	Query &AddEqualPosition(std::initializer_list<std::string> l) & {
		auto *const bracket = entries.LastOpenBracket();
		auto &eqPos = (bracket ? bracket->equalPositions : entries.equalPositions);
		eqPos.emplace_back(l);
		return *this;
	}
	Query &&AddEqualPosition(h_vector<std::string> equalPosition) && { return std::move(AddEqualPosition(std::move(equalPosition))); }
	Query &&AddEqualPosition(std::vector<std::string> equalPosition) && { return std::move(AddEqualPosition(std::move(equalPosition))); }
	Query &&AddEqualPosition(std::initializer_list<std::string> l) && { return std::move(AddEqualPosition(l)); }

	/// Joins namespace with another namespace. Analog to sql JOIN.
	/// @param joinType - type of Join (Inner, Left or OrInner).
	/// @param leftField - name of the field in the namespace of this Query object.
	/// @param rightField - name of the field in the namespace of qr Query object.
	/// @param cond - condition type (Eq, Leq, Geq, etc).
	/// @param op - operation type (and, or, not).
	/// @param qr - query of the namespace that is going to be joined with this one.
	/// @return Query object ready to be executed.
	Query &Join(JoinType joinType, std::string leftField, std::string rightField, CondType cond, OpType op, Query &&qr) &;
	template <typename StrL, typename StrR>
	Query &&Join(JoinType joinType, StrL &&leftField, StrR &&rightField, CondType cond, OpType op, Query &&qr) && {
		return std::move(Join(joinType, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, op, std::move(qr)));
	}
	Query &Join(JoinType joinType, std::string leftField, std::string rightField, CondType cond, OpType op, const Query &qr) &;
	template <typename StrL, typename StrR>
	Query &&Join(JoinType joinType, StrL &&leftField, StrR &&rightField, CondType cond, OpType op, const Query &qr) && {
		return std::move(Join(joinType, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, op, qr));
	}

	OnHelper Join(JoinType joinType, Query &&q) &;
	OnHelper Join(JoinType joinType, const Query &q) &;
	OnHelperR Join(JoinType joinType, Query &&q) &&;
	OnHelperR Join(JoinType joinType, const Query &q) &&;

	/// @public
	/// Inner Join of this namespace with another one.
	/// @param leftField - name of the field in the namespace of this Query object.
	/// @param rightField - name of the field in the namespace of qr Query object.
	/// @param cond - condition type (Eq, Leq, Geq, etc).
	/// @param qr - query of the namespace that is going to be joined with this one.
	/// @return Query object ready to be executed.
	template <typename StrL, typename StrR>
	Query &InnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, Query &&qr) & {  // -V1071
		return Join(JoinType::InnerJoin, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, OpAnd, std::move(qr));
	}
	template <typename StrL, typename StrR>
	Query &&InnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, Query &&qr) && {
		return std::move(InnerJoin(std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, std::move(qr)));
	}
	template <typename StrL, typename StrR>
	Query &InnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, const Query &qr) & {
		return Join(JoinType::InnerJoin, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, OpAnd, qr);
	}
	template <typename StrL, typename StrR>
	Query &&InnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, const Query &qr) && {
		return std::move(InnerJoin(std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, qr));
	}

	/// Left Join of this namespace with another one.
	/// @param leftField - name of the field in the namespace of this Query object.
	/// @param rightField - name of the field in the namespace of qr Query object.
	/// @param cond - condition type (Eq, Leq, Geq, etc).
	/// @param qr - query of the namespace that is going to be joined with this one.
	/// @return Query object ready to be executed.
	template <typename StrL, typename StrR>
	Query &LeftJoin(StrL &&leftField, StrR &&rightField, CondType cond, Query &&qr) & {
		return Join(JoinType::LeftJoin, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, OpAnd, std::move(qr));
	}
	template <typename StrL, typename StrR>
	Query &&LeftJoin(StrL &&leftField, StrR &&rightField, CondType cond, Query &&qr) && {
		return std::move(LeftJoin(std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, std::move(qr)));
	}
	template <typename StrL, typename StrR>
	Query &LeftJoin(StrL &&leftField, StrR &&rightField, CondType cond, const Query &qr) & {
		return Join(JoinType::LeftJoin, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, OpAnd, qr);
	}
	template <typename StrL, typename StrR>
	Query &&LeftJoin(StrL &&leftField, StrR &&rightField, CondType cond, const Query &qr) && {
		return std::move(LeftJoin(std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, qr));
	}

	/// OrInnerJoin of this namespace with another one.
	/// @param leftField - name of the field in the namespace of this Query object.
	/// @param rightField - name of the field in the namespace of qr Query object.
	/// @param cond - condition type (Eq, Leq, Geq, etc).
	/// @param qr - query of the namespace that is going to be joined with this one.
	/// @return a reference to a query object ready to be executed.
	template <typename StrL, typename StrR>
	Query &OrInnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, Query &&qr) & {
		return Join(JoinType::OrInnerJoin, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, OpAnd, std::move(qr));
	}
	template <typename StrL, typename StrR>
	Query &&OrInnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, Query &&qr) && {
		return std::move(OrInnerJoin(std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, std::move(qr)));
	}
	template <typename StrL, typename StrR>
	Query &OrInnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, const Query &qr) & {
		return Join(JoinType::OrInnerJoin, std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, OpAnd, qr);
	}
	template <typename StrL, typename StrR>
	Query &&OrInnerJoin(StrL &&leftField, StrR &&rightField, CondType cond, const Query &qr) && {
		return std::move(OrInnerJoin(std::forward<StrL>(leftField), std::forward<StrR>(rightField), cond, qr));
	}
	Query &Merge(const Query &q) &;
	Query &&Merge(const Query &q) && { return std::move(Merge(q)); }
	Query &Merge(Query &&q) &;
	Query &&Merge(Query &&q) && { return std::move(Merge(std::move(q))); }

	/// Changes debug level.
	/// @param level - debug level.
	/// @return Query object.
	Query &Debug(int level) &noexcept {
		debugLevel = level;
		return *this;
	}
	Query &&Debug(int level) &&noexcept { return std::move(Debug(level)); }

	/// Changes strict mode.
	/// @param mode - strict mode.
	/// @return Query object.
	Query &Strict(StrictMode mode) &noexcept {
		strictMode = mode;
		return *this;
	}
	Query &&Strict(StrictMode mode) &&noexcept { return std::move(Strict(mode)); }

	/// Performs sorting by certain column. Same as sql 'ORDER BY'.
	/// @param sort - sorting column name.
	/// @param desc - is sorting direction descending or ascending.
	/// @return Query object.
	template <typename Str>
	Query &Sort(Str &&sort, bool desc) & {	// -V1071
		if (!strEmpty(sort)) sortingEntries_.emplace_back(std::forward<Str>(sort), desc);
		return *this;
	}
	template <typename Str>
	Query &&Sort(Str &&sort, bool desc) && {
		return std::move(Sort(std::forward<Str>(sort), desc));
	}

	/// Performs sorting by ST_Distance() expressions for geometry index. Sorting function will use distance between field and target point.
	/// @param field - field's name. This field must contain Point.
	/// @param p - target point.
	/// @param desc - is sorting direction descending or ascending.
	/// @return Query object.
	Query &SortStDistance(std::string_view field, reindexer::Point p, bool desc) &;
	Query &&SortStDistance(std::string_view field, reindexer::Point p, bool desc) && { return std::move(SortStDistance(field, p, desc)); }
	/// Performs sorting by ST_Distance() expressions for geometry index. Sorting function will use distance 2 fields.
	/// @param field1 - first field name. This field must contain Point.
	/// @param field2 - second field name.This field must contain Point.
	/// @param desc - is sorting direction descending or ascending.
	/// @return Query object.
	Query &SortStDistance(std::string_view field1, std::string_view field2, bool desc) &;
	Query &&SortStDistance(std::string_view field1, std::string_view field2, bool desc) && {
		return std::move(SortStDistance(field1, field2, desc));
	}

	/// Performs sorting by certain column. Analog to sql ORDER BY.
	/// @param sort - sorting column name.
	/// @param desc - is sorting direction descending or ascending.
	/// @param forcedSortOrder - list of values for forced sort order.
	/// @return Query object.
	template <typename Str, typename T>
	Query &Sort(Str &&sort, bool desc, std::initializer_list<T> forcedSortOrder) & {
		if (!sortingEntries_.empty() && !std::empty(forcedSortOrder))
			throw Error(errParams, "Forced sort order is allowed for the first sorting entry only");
		sortingEntries_.emplace_back(std::forward<Str>(sort), desc);
		for (const T &v : forcedSortOrder) forcedSortOrder_.emplace_back(v);
		return *this;
	}
	template <typename Str, typename T>
	Query &&Sort(Str &&sort, bool desc, std::initializer_list<T> forcedSortOrder) && {
		return std::move(Sort<Str, T>(std::forward<Str>(sort), desc, std::move(forcedSortOrder)));
	}

	/// Performs sorting by certain column. Analog to sql ORDER BY.
	/// @param sort - sorting column name.
	/// @param desc - is sorting direction descending or ascending.
	/// @param forcedSortOrder - list of values for forced sort order.
	/// @return Query object.
	template <typename Str, typename T>
	Query &Sort(Str &&sort, bool desc, const T &forcedSortOrder) & {
		if (!sortingEntries_.empty() && !forcedSortOrder.empty())
			throw Error(errParams, "Forced sort order is allowed for the first sorting entry only");
		sortingEntries_.emplace_back(std::forward<Str>(sort), desc);
		for (const auto &v : forcedSortOrder) forcedSortOrder_.emplace_back(v);
		return *this;
	}
	template <typename Str, typename T>
	Query &&Sort(Str &&sort, bool desc, const T &forcedSortOrder) && {
		return std::move(Sort<Str, T>(std::forward<Str>(sort), desc, forcedSortOrder));
	}

	/// Performs distinct for a certain index.
	/// @param indexName - name of index for distict operation.
	template <typename Str>
	Query &Distinct(Str &&indexName) & {
		if (!strEmpty(indexName)) {
			aggregations_.emplace_back(AggDistinct, h_vector<std::string, 1>{std::forward<Str>(indexName)});
		}
		return *this;
	}
	template <typename Str>
	Query &&Distinct(Str &&indexName) && {
		return std::move(Distinct(std::forward<Str>(indexName)));
	}

	/// Sets list of columns in this namespace to be finally selected.
	/// @param l - list of columns to be selected.
	Query &Select(std::initializer_list<const char *> l) & {
		if (!CanAddSelectFilter()) {
			throw Error(errConflict, kAggregationWithSelectFieldsMsgError);
		}
		selectFilter_.insert(selectFilter_.begin(), l.begin(), l.end());
		return *this;
	}
	Query &&Select(std::initializer_list<const char *> l) && { return std::move(Select(l)); }

	/// Adds an aggregate function for certain column.
	/// Analog to sql aggregate functions (min, max, avg, etc).
	/// @param type - aggregation function type (Sum, Avg).
	/// @param fields - names of the fields to be aggregated.
	/// @param sort - vector of sorting column names and descending (if true) or ascending (otherwise) flags.
	/// Use column name 'count' to sort by facet's count value.
	/// @param limit - number of rows to get from result set.
	/// @param offset - index of the first row to get from result set.
	/// @return Query object ready to be executed.
	Query &Aggregate(AggType type, h_vector<std::string, 1> fields, const std::vector<std::pair<std::string, bool>> &sort = {},
					 unsigned limit = QueryEntry::kDefaultLimit, unsigned offset = QueryEntry::kDefaultOffset) & {
		if (!CanAddAggregation(type)) {
			throw Error(errConflict, kAggregationWithSelectFieldsMsgError);
		}
		SortingEntries sorting;
		sorting.reserve(sort.size());
		for (const auto &s : sort) {
			sorting.emplace_back(s.first, s.second);
		}
		aggregations_.emplace_back(type, std::move(fields), std::move(sorting), limit, offset);
		return *this;
	}
	Query &&Aggregate(AggType type, h_vector<std::string, 1> fields, const std::vector<std::pair<std::string, bool>> &sort = {},
					  unsigned limit = QueryEntry::kDefaultLimit, unsigned offset = QueryEntry::kDefaultOffset) && {
		return std::move(Aggregate(type, std::move(fields), sort, limit, offset));
	}

	/// Sets next operation type to Or.
	/// @return Query object.
	Query &Or() & {
		nextOp_ = OpOr;
		return *this;
	}
	Query &&Or() && { return std::move(Or()); }

	/// Sets next operation type to Not.
	/// @return Query object.
	Query &Not() & {
		nextOp_ = OpNot;
		return *this;
	}
	Query &&Not() && { return std::move(Not()); }

	/// Insert open bracket to order logic operations.
	/// @return Query object.
	Query &OpenBracket() & {
		entries.OpenBracket(nextOp_);
		nextOp_ = OpAnd;
		return *this;
	}
	Query &&OpenBracket() && { return std::move(OpenBracket()); }

	/// Insert close bracket to order logic operations.
	/// @return Query object.
	Query &CloseBracket() & {
		entries.CloseBracket();
		return *this;
	}
	Query &&CloseBracket() && { return std::move(CloseBracket()); }

	/// Sets the limit of selected rows.
	/// Analog to sql LIMIT rowsNumber.
	/// @param limit - number of rows to get from result set.
	/// @return Query object.
	Query &Limit(unsigned limit) &noexcept {
		count_ = limit;
		return *this;
	}
	Query &&Limit(unsigned limit) &&noexcept { return std::move(Limit(limit)); }

	/// Sets the number of the first selected row from result query.
	/// Analog to sql LIMIT OFFSET.
	/// @param offset - index of the first row to get from result set.
	/// @return Query object.
	Query &Offset(unsigned offset) &noexcept {
		start_ = offset;
		return *this;
	}
	Query &&Offset(unsigned offset) &&noexcept { return std::move(Offset(offset)); }

	/// Set the total count calculation mode to Accurate
	/// @return Query object
	Query &ReqTotal() &noexcept {
		calcTotal_ = ModeAccurateTotal;
		return *this;
	}
	Query &&ReqTotal() &&noexcept { return std::move(ReqTotal()); }

	/// Set the total count calculation mode to Cached.
	/// It will be use LRUCache for total count result
	/// @return Query object
	Query &CachedTotal() &noexcept {
		calcTotal_ = ModeCachedTotal;
		return *this;
	}
	Query &&CachedTotal() &&noexcept { return std::move(CachedTotal()); }

	/// Output fulltext rank
	/// Allowed only with fulltext query
	/// @return Query object
	Query &WithRank() &noexcept {
		withRank_ = true;
		return *this;
	}
	Query &&WithRank() &&noexcept { return std::move(WithRank()); }
	bool IsWithRank() const noexcept { return withRank_; }

	/// Can we add aggregation functions
	/// or new select fields to a current query?
	bool CanAddAggregation(AggType type) const noexcept { return type == AggDistinct || (selectFilter_.empty()); }
	bool CanAddSelectFilter() const noexcept {
		return aggregations_.empty() || (aggregations_.size() == 1 && aggregations_.front().Type() == AggDistinct);
	}

	/// Serializes query data to stream.
	/// @param ser - serializer object for write.
	/// @param mode - serialization mode.
	void Serialize(WrSerializer &ser, uint8_t mode = Normal) const;

	/// Deserializes query data from stream.
	/// @param ser - serializer object.
	void Deserialize(Serializer &ser);

	void WalkNested(bool withSelf, bool withMerged, const std::function<void(const Query &q)> &visitor) const;

	bool HasLimit() const noexcept { return count_ != QueryEntry::kDefaultLimit; }
	bool HasOffset() const noexcept { return start_ != QueryEntry::kDefaultOffset; }
	bool IsWALQuery() const noexcept;
	const std::vector<UpdateEntry> &UpdateFields() const noexcept { return updateFields_; }
	QueryType Type() const noexcept { return type_; }
	const std::string &NsName() const &noexcept { return namespace_; }
	template <typename T>
	void SetNsName(T &&nsName) &noexcept {
		namespace_ = std::forward<T>(nsName);
	}
	unsigned Limit() const noexcept { return count_; }
	unsigned Offset() const noexcept { return start_; }
	CalcTotalMode CalcTotal() const noexcept { return calcTotal_; }
	void CalcTotal(CalcTotalMode calcTotal) noexcept { calcTotal_ = calcTotal; }

	int debugLevel = 0;							/// Debug level.
	StrictMode strictMode = StrictModeNotSet;	/// Strict mode.
	bool explain_ = false;						/// Explain query if true
	QueryType type_ = QuerySelect;				/// Query type
	OpType nextOp_ = OpAnd;						/// Next operation constant.
	SortingEntries sortingEntries_;				/// Sorting data.
	std::vector<Variant> forcedSortOrder_;		/// Keys that always go first - before any ordered values.
	std::vector<JoinedQuery> joinQueries_;		/// List of queries for join.
	std::vector<JoinedQuery> mergeQueries_;		/// List of merge queries.
	h_vector<std::string, 1> selectFilter_;		/// List of columns in a final result set.
	std::vector<std::string> selectFunctions_;	/// List of sql functions

	QueryEntries entries;

	std::vector<AggregateEntry> aggregations_;

	auto NsName() const && = delete;

private:
	void deserialize(Serializer &ser, bool &hasJoinConditions);

	std::string namespace_;						   /// Name of the namespace.
	unsigned start_ = QueryEntry::kDefaultOffset;  /// First row index from result set.
	unsigned count_ = QueryEntry::kDefaultLimit;   /// Number of rows from result set.
	CalcTotalMode calcTotal_ = ModeNoTotal;		   /// Calculation mode.
	std::vector<UpdateEntry> updateFields_;		   /// List of fields (and values) for update.
	bool withRank_ = false;
	friend class SQLParser;
};

class JoinedQuery : public Query {
public:
	JoinedQuery() = default;
	JoinedQuery(JoinType jt, const Query &q) : Query(q), joinType{jt} {}
	JoinedQuery(JoinType jt, Query &&q) : Query(std::move(q)), joinType{jt} {}
	using Query::Query;
	bool operator==(const JoinedQuery &obj) const;

	JoinType joinType{JoinType::LeftJoin};	   /// Default join type.
	h_vector<QueryJoinEntry, 1> joinEntries_;  /// Condition for join. Filled in each subqueries, empty in  root query
};

template <typename Q>
Q Query::OnHelperTempl<Q>::On(std::string index, CondType cond, std::string joinIndex) && {
	jq_.joinEntries_.emplace_back(op_, cond, std::move(index), std::move(joinIndex));
	return std::forward<Q>(q_);
}

template <typename Q>
Query::OnHelperGroup<Q> &&Query::OnHelperGroup<Q>::On(std::string index, CondType cond, std::string joinIndex) && {
	jq_.joinEntries_.emplace_back(op_, cond, std::move(index), std::move(joinIndex));
	op_ = OpAnd;
	return std::move(*this);
}

}  // namespace reindexer
