#include <gtest/gtest.h>
#include "core/reindexer.h"

using reindexer::Error;
using reindexer::Item;

TEST(JSONParsingTest, EmptyDocument) {
	reindexer::Reindexer rx;
	constexpr std::string_view kNsName("json_empty_doc_test");
	Error err = rx.OpenNamespace(kNsName);
	ASSERT_TRUE(err.ok()) << err.what();

	Item item(rx.NewItem(kNsName));
	ASSERT_TRUE(item.Status().ok()) << item.Status().what();

	err = item.FromJSON("\n");
	EXPECT_EQ(err.code(), errParseJson);
	ASSERT_TRUE(item.Status().ok()) << item.Status().what();

	err = item.FromJSON("\t");
	EXPECT_EQ(err.code(), errParseJson);
	ASSERT_TRUE(item.Status().ok()) << item.Status().what();

	err = item.FromJSON(" ");
	EXPECT_EQ(err.code(), errParseJson);
	ASSERT_TRUE(item.Status().ok()) << item.Status().what();
}

TEST(JSONParsingTest, Strings) {
	const std::vector<unsigned> lens = {0, 100, 8 < 10, 2 << 20, 8 << 20, 16 << 20, 32 << 20, 60 << 20};
	for (auto len : lens) {
		std::string strs[2];
		strs[0].resize(len / 2);
		std::fill(strs[0].begin(), strs[0].end(), 'a');
		strs[1].resize(len);
		std::fill(strs[1].begin(), strs[1].end(), 'b');

		std::string d("{\"id\":1,\"str0\":\"" + strs[0] + "\",\"str1\":\"" + strs[1] + "\",\"val\":999}");
		reindexer::span<char> data(d);
		try {
			gason::JsonParser parser;
			auto root = parser.Parse(data, nullptr);
			EXPECT_EQ(root["id"].As<int>(), 1) << len;
			auto rstr = root["str0"].As<std::string_view>();
			EXPECT_EQ(rstr, strs[0]) << len;
			rstr = root["str1"].As<std::string_view>();
			EXPECT_EQ(rstr, strs[1]) << len;
			EXPECT_EQ(root["val"].As<int>(), 999) << len;
		} catch (gason::Exception& e) {
			EXPECT_TRUE(false) << e.what();
		}
	}
}
