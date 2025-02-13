#pragma once
#include <memory>
#include <unordered_map>
#include "core/ft/areaholder.h"
#include "core/ft/config/ftfastconfig.h"
#include "core/ft/filters/itokenfilter.h"
#include "core/ft/idrelset.h"
#include "core/ft/stemmer.h"
#include "core/ft/typos.h"
#include "core/ft/usingcontainer.h"
#include "core/index/ft_preselect.h"
#include "core/index/indextext/ftkeyentry.h"
#include "estl/fast_hash_map.h"
#include "estl/flat_str_map.h"
#include "estl/suffix_map.h"
#include "indextexttypes.h"

namespace reindexer {

class RdxContext;

// unique document in the namespace (if different rows contain the same text document, then it will correspond to one vdoc)
struct VDocEntry {
#ifdef REINDEX_FT_EXTRA_DEBUG
	std::string keyDoc;
#endif

	const FtKeyEntryData* keyEntry;
	RVector<float, 3> wordsCount;
	RVector<float, 3> mostFreqWordCount;
};

// documents for the word
template <typename IdCont>
class PackedWordEntry {
public:
	IdCont vids_;  // IdCont - std::vector or packed_vector
	// document offset, for the last step.
	// Necessary for correct rebuilding of the last step
	size_t cur_step_pos_ = 0;
};
class WordEntry {
public:
	IdRelSet vids_;
	bool virtualWord = false;
};
enum ProcessStatus { FullRebuild, RecommitLast, CreateNew };

struct WordTypo {
	WordTypo() = default;
	explicit WordTypo(WordIdType w) noexcept : word(w) {}
	explicit WordTypo(WordIdType w, const typos_context::TyposVec& p) noexcept : word(w), positions(p) {}
	uint32_t operator&(uint32_t v) const noexcept { return uint32_t(word) & v; }
	WordTypo& operator=(uint32_t v) noexcept {
		word = v;
		return *this;
	}

	WordIdType word;
	typos_context::TyposVec positions;
};
static_assert(sizeof(WordTypo) <= 16, "This size is matter for overall size of the typos map");

class IDataHolder {
public:
	struct CommitStep {
		CommitStep() : wordOffset_(0) {}

		CommitStep(const CommitStep&) = delete;
		CommitStep& operator=(const CommitStep&) = delete;
		CommitStep(CommitStep&& /*rhs*/) noexcept = default;
		CommitStep& operator=(CommitStep&& /*rhs*/) = default;

		// Suffix map. suffix <-> original word id
		suffix_map<char, WordIdType> suffixes_;
		// Typos maps. typo string <-> original word id
		// typosHalf_ contains words with <=maxTypos/2 typos
		flat_str_multimap<char, WordTypo> typosHalf_;
		// typosMax_ contains words with MaxTyposInWord() typos if MaxTyposInWord() != maxTypos/2
		flat_str_multimap<char, WordTypo> typosMax_;
		// word offset for given step in DataHolder::words_
		uint32_t wordOffset_;

		void clear() {
			suffixes_.clear();
			typosHalf_.clear();
			typosMax_.clear();
		}
	};

	// Intermediate information about found document in current merge step. Used only for queries with 2 or more terms
	struct MergedIdRel {
		explicit MergedIdRel(IdRelType&& c, int r, int q) : next(std::move(c)), rank(r), qpos(q) {}
		explicit MergedIdRel(int r, int q) : rank(r), qpos(q) {}
		MergedIdRel(MergedIdRel&&) = default;
		IdRelType cur;	 // Ids & pos of matched document of current step
		IdRelType next;	 // Ids & pos of matched document of next step
		int32_t rank;	 // Rank of curent matched document
		int32_t qpos;	 // Position in query
	};

	struct MergedIdRelEx : public MergedIdRel {
		explicit MergedIdRelEx(IdRelType&& c, int r, int q) : MergedIdRel(r, q), posTmp(std::move(c)) {}
		MergedIdRelEx(MergedIdRelEx&&) = default;
		IdRelType posTmp;  // For group only. Collect all positions for subpatterns and the index in the vector with which we merged
	};

	struct MergedIdRelExArea : public MergedIdRel {
		MergedIdRelExArea(IdRelType&& c, int r, int q, RVector<std::pair<IdRelType::PosType, int>, 4>&& p)
			: MergedIdRel(std::move(c), r, q), posTmp(std::move(p)) {}
		MergedIdRelExArea(MergedIdRelExArea&&) = default;

		RVector<std::pair<IdRelType::PosType, int>, 4>
			posTmp;	 // For group only. Collect all positions for subpatterns and the index in the vector with which we merged
		h_vector<RVector<std::pair<IdRelType::PosType, int>, 4>, 2> wordPosForChain;
	};

	using MergedOffsetT = uint16_t;
	static_assert(std::numeric_limits<MergedOffsetT>::max() > kMaxMergeLimitValue,
				  "Merged offset type must be able to hold any value up to kMaxMergeLimitValue");
	// Final information about found document
	struct MergeInfo {
		IdType id;	   // Virtual id of merged document (index in vdocs)
		int32_t proc;  // Rank of document
		uint32_t areaIndex = std::numeric_limits<uint32_t>::max();
		int8_t field;														 // Field index, where was match
		MergedOffsetT indexAdd = std::numeric_limits<MergedOffsetT>::max();	 // index in merged_rd
	};

	struct MergeData : public std::vector<MergeInfo> {
		int maxRank = 0;
		std::vector<AreaHolder> vectorAreas;
	};

	virtual ~IDataHolder() = default;
	virtual MergeData Select(FtDSLQuery&&, size_t fieldSize, bool needArea, int maxAreasInDoc, bool inTransaction,
							 FtMergeStatuses::Statuses&&, FtUseExternStatuses, const RdxContext&) = 0;
	virtual void Process(size_t fieldSize, bool multithread) = 0;
	virtual size_t GetMemStat() = 0;
	virtual void Clear() = 0;
	virtual void StartCommit(bool complte_updated) = 0;
	void SetConfig(FtFastConfig* cfg);
	CommitStep& GetStep(WordIdType id);
	const CommitStep& GetStep(WordIdType id) const;
	bool NeedRebuild(bool complte_updated);
	bool NeedRecomitLast();
	void SetWordsOffset(uint32_t word_offset);
	bool NeedClear(bool complte_updated);
	suffix_map<char, WordIdType>& GetSuffix() noexcept { return steps.back().suffixes_; }
	flat_str_multimap<char, WordTypo>& GetTyposHalf() noexcept { return steps.back().typosHalf_; }
	flat_str_multimap<char, WordTypo>& GetTyposMax() noexcept { return steps.back().typosMax_; }
	WordIdType findWord(std::string_view word);
	uint32_t GetSuffixWordId(WordIdType id) const noexcept { return GetSuffixWordId(id, steps.back()); }
	uint32_t GetSuffixWordId(WordIdType id, const CommitStep& step) const noexcept;
	uint32_t GetWordsOffset();
	// returns id and found or not found
	WordIdType BuildWordId(uint32_t id);
	std::string Dump();

	// language and corresponding stemmer object
	std::unordered_map<std::string, stemmer> stemmers_;

	// translit generator for russian and english (returns word + weight)
	ITokenFilter::Ptr translit_;
	ITokenFilter::Ptr kbLayout_;
	ITokenFilter::Ptr synonyms_;

	std::vector<CommitStep> steps;
	// array of unique documents
	std::vector<VDocEntry> vdocs_;
	size_t cur_vdoc_pos_ = 0;
	ProcessStatus status_{CreateNew};
	std::vector<double> avgWordsCount_;
	// Virtual documents, merged. Addresable by VDocIdType
	// Temp data for build
	std::vector<RVector<std::pair<std::string_view, uint32_t>, 8>> vdocsTexts;
	std::vector<std::unique_ptr<std::string>> bufStrs_;
	size_t vdocsOffset_{0};
	size_t szCnt{0};
	FtFastConfig* cfg_{nullptr};
	// index - rowId, value vdocId (index in array vdocs_)
	std::vector<size_t> rowId2Vdoc_;
};

template <typename IdCont>
class DataHolder : public IDataHolder {
public:
	virtual MergeData Select(FtDSLQuery&&, size_t fieldSize, bool needArea, int maxAreasInDoc, bool inTransaction,
							 FtMergeStatuses::Statuses&&, FtUseExternStatuses, const RdxContext&) override final;
	void Process(size_t fieldSize, bool multithread) final;
	size_t GetMemStat() override final;
	void StartCommit(bool complte_updated) override final;
	void Clear() override final;
	std::vector<PackedWordEntry<IdCont>>& GetWords() noexcept { return words_; }
	PackedWordEntry<IdCont>& getWordById(WordIdType id) noexcept;
	const PackedWordEntry<IdCont>& getWordById(WordIdType id) const noexcept;
	std::vector<PackedWordEntry<IdCont>> words_;
};

extern template class DataHolder<PackedIdRelVec>;
extern template class DataHolder<IdRelVec>;

}  // namespace reindexer
